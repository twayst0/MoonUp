#include "sysinfo.h"

#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>

#include "common.h"
#include "engine/capture.h"
#include "engine/d3d.h"
#include "engine/overlay.h"

namespace sw {

using json = nlohmann::json;

static std::string RegString(HKEY root, const wchar_t* path, const wchar_t* name) {
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return Utf8(buf);
    return {};
}

static DWORD RegDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD def) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS) return v;
    return def;
}

static void OsVersion(DWORD& major, DWORD& minor, DWORD& build) {
    typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi{sizeof(vi)};
    auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (fn) fn(&vi);
    major = vi.dwMajorVersion;
    minor = vi.dwMinorVersion;
    build = vi.dwBuildNumber;
}

static BOOL CALLBACK MonitorEnum(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    json& arr = *(json*)lp;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m, &mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    // Friendly name through the display device API.
    DISPLAY_DEVICEW dd{sizeof(dd)};
    std::string friendly;
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) friendly = Utf8(dd.DeviceString);
    // Highest refresh rate supported at the current resolution.
    DWORD maxHz = dm.dmDisplayFrequency;
    DEVMODEW m2{};
    m2.dmSize = sizeof(m2);
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &m2); i++) {
        if (m2.dmPelsWidth == dm.dmPelsWidth && m2.dmPelsHeight == dm.dmPelsHeight) maxHz = std::max(maxHz, m2.dmDisplayFrequency);
    }
    arr.push_back({{"name", friendly.empty() ? Utf8(mi.szDevice) : friendly},
                   {"device", Utf8(mi.szDevice)},
                   {"width", mi.rcMonitor.right - mi.rcMonitor.left},
                   {"height", mi.rcMonitor.bottom - mi.rcMonitor.top},
                   {"refresh", dm.dmDisplayFrequency},
                   {"maxRefresh", maxHz},
                   {"primary", (mi.dwFlags & MONITORINFOF_PRIMARY) != 0}});
    return TRUE;
}

json CollectSystemInfo() {
    json j;
    json gpus = json::array();
    for (auto& a : EnumerateAdapters()) {
        gpus.push_back({{"index", a.index},
                        {"name", a.name},
                        {"vendor", VendorName(a.vendorId)},
                        {"vendorId", a.vendorId},
                        {"deviceId", a.deviceId},
                        {"vramMB", (uint64_t)(a.dedicatedVideoMemory >> 20)},
                        {"sharedMB", (uint64_t)(a.sharedSystemMemory >> 20)},
                        {"integrated", a.integrated}});
    }
    j["gpus"] = gpus;

    json mons = json::array();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnum, (LPARAM)&mons);
    j["monitors"] = mons;

    j["cpu"] = RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    j["cpuThreads"] = si.dwNumberOfProcessors;
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    j["ramMB"] = (uint64_t)(ms.ullTotalPhys >> 20);

    DWORD maj, min, build;
    OsVersion(maj, min, build);
    j["os"] = {{"major", maj}, {"minor", min}, {"build", build}, {"windows11", build >= 22000}};
    j["wgc"] = WgcSupported();
    j["captureExclusion"] = build >= 19041;
    // Hardware accelerated GPU scheduling (HwSchMode 2 = on).
    j["hags"] = RegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", 0) == 2;
    // Windows "Game Mode".
    j["gameMode"] = RegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", 1) != 0;

    SYSTEM_POWER_STATUS ps{};
    GetSystemPowerStatus(&ps);
    j["battery"] = {{"present", ps.BatteryFlag != 128 && ps.BatteryFlag != 255},
                    {"onBattery", ps.ACLineStatus == 0},
                    {"percent", ps.BatteryLifePercent == 255 ? -1 : (int)ps.BatteryLifePercent}};
    j["version"] = kVersion;
    return j;
}

// ------------------------------------------------------------------------------- windows list
std::string WindowTitle(HWND h) {
    wchar_t t[512];
    int n = GetWindowTextW(h, t, 512);
    return n > 0 ? Utf8(std::wstring(t, n)) : std::string();
}

static std::wstring WindowExePath(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t path[MAX_PATH * 2];
    DWORD size = MAX_PATH * 2;
    std::wstring out;
    if (QueryFullProcessImageNameW(p, 0, path, &size)) out.assign(path, size);
    CloseHandle(p);
    return out;
}

std::string WindowExeName(HWND h) {
    std::wstring p = WindowExePath(h);
    size_t s = p.find_last_of(L"\\/");
    return Utf8(s == std::wstring::npos ? p : p.substr(s + 1));
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string Base64(const uint8_t* d, size_t n) {
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += kB64[(v >> 18) & 63];
        o += kB64[(v >> 12) & 63];
        o += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        o += i + 2 < n ? kB64[v & 63] : '=';
    }
    return o;
}

std::string IconToDataUrl(HICON icon, int size) {
    if (!icon) return {};
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return {};
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmapFromHICON(icon, &bmp))) return {};
    ComPtr<IWICBitmapScaler> scaler;
    UINT w = 0, h = 0;
    bmp->GetSize(&w, &h);
    IWICBitmapSource* src = bmp.Get();
    if ((int)w != size && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(bmp.Get(), size, size, (WICBitmapInterpolationMode)4 /* HighQualityCubic */)))
        src = scaler.Get();
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return {};
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return {};
    enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr))) return {};
    frame->Initialize(nullptr);
    UINT fw = 0, fh = 0;
    src->GetSize(&fw, &fh);
    frame->SetSize(fw, fh);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WriteSource(src, nullptr))) return {};
    frame->Commit();
    enc->Commit();
    HGLOBAL hg = nullptr;
    GetHGlobalFromStream(stream.Get(), &hg);
    STATSTG st{};
    stream->Stat(&st, STATFLAG_NONAME);
    size_t n = (size_t)st.cbSize.QuadPart;
    const uint8_t* data = (const uint8_t*)GlobalLock(hg);
    std::string out = "data:image/png;base64," + Base64(data, n);
    GlobalUnlock(hg);
    return out;
}

static HICON WindowIcon(HWND h) {
    HICON icon = nullptr;
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, &r) && r) icon = (HICON)r;
    if (!icon && SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 50, &r) && r) icon = (HICON)r;
    if (!icon) icon = (HICON)GetClassLongPtrW(h, GCLP_HICON);
    if (!icon) icon = (HICON)GetClassLongPtrW(h, GCLP_HICONSM);
    return icon;
}

struct EnumCtx {
    HWND exclude;
    json* out;
};

static BOOL CALLBACK EnumWin(HWND h, LPARAM lp) {
    EnumCtx* c = (EnumCtx*)lp;
    if (h == c->exclude || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    std::string title = WindowTitle(h);
    if (title.empty()) return TRUE;
    RECT rc;
    GetClientRect(h, &rc);
    if (rc.right < 64 || rc.bottom < 64) return TRUE;
    wchar_t cls[128];
    GetClassNameW(h, cls, 128);
    std::wstring cs(cls);
    if (cs == L"Progman" || cs == L"WorkerW" || cs == L"Shell_TrayWnd" || cs == L"MoonUpOverlay") return TRUE;

    std::wstring path = WindowExePath(h);
    HICON icon = WindowIcon(h);
    HICON owned = nullptr;
    if (!icon && !path.empty()) {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON)) owned = icon = sfi.hIcon;
    }
    char hs[32];
    snprintf(hs, sizeof(hs), "%llu", (unsigned long long)(uintptr_t)h);
    size_t slash = path.find_last_of(L"\\/");
    c->out->push_back({{"hwnd", hs},
                       {"title", title},
                       {"exe", Utf8(slash == std::wstring::npos ? path : path.substr(slash + 1))},
                       {"width", rc.right},
                       {"height", rc.bottom},
                       {"minimized", IsIconic(h) != FALSE},
                       {"icon", IconToDataUrl(icon, 32)}});
    if (owned) DestroyIcon(owned);
    return TRUE;
}

json ListWindows(HWND exclude) {
    json out = json::array();
    EnumCtx c{exclude, &out};
    EnumWindows(EnumWin, (LPARAM)&c);
    return out;
}

}  // namespace sw
