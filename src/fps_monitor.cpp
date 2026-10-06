#include "fps_monitor.h"

#include <dwmapi.h>
#include <roapi.h>
#include <shellscalingapi.h>

#include <cmath>
#include <memory>

#include "engine/capture.h"
#include "engine/d3d.h"
#include "engine/overlay.h"
#include <dxgi1_2.h>

namespace sw {

namespace {

const wchar_t* kClass = L"MoonUpFpsCounter";
const int kW = 150, kH = 56;  // at 96 dpi

LRESULT CALLBACK CounterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

bool AdapterForMonitor(HMONITOR mon, LUID& luid) {
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++, a.Reset()) {
        ComPtr<IDXGIOutput> o;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++, o.Reset()) {
            DXGI_OUTPUT_DESC d;
            o->GetDesc(&d);
            if (d.Monitor == mon) {
                DXGI_ADAPTER_DESC1 ad;
                a->GetDesc1(&ad);
                luid = ad.AdapterLuid;
                return true;
            }
        }
    }
    return false;
}

bool FillsMonitor(HWND h) {
    RECT c;
    POINT tl{0, 0};
    if (!GetClientRect(h, &c) || !ClientToScreen(h, &tl)) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return tl.x <= mi.rcMonitor.left && tl.y <= mi.rcMonitor.top && tl.x + c.right >= mi.rcMonitor.right &&
           tl.y + c.bottom >= mi.rcMonitor.bottom;
}

bool ClientOnScreen(HWND h, RECT& r) {
    RECT c;
    if (!GetClientRect(h, &c)) return false;
    POINT tl{0, 0};
    if (!ClientToScreen(h, &tl)) return false;
    r = {tl.x, tl.y, tl.x + c.right, tl.y + c.bottom};
    return c.right > 0 && c.bottom > 0;
}

// Draws the counter into a premultiplied BGRA bitmap and hands it to the layered window.
class CounterWindow {
public:
    bool Create(HINSTANCE inst) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = CounterProc;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass,
                                L"MoonUp FPS", WS_POPUP, 0, 0, kW, kH, nullptr, nullptr, inst, nullptr);
        return hwnd_ != nullptr;
    }
    ~CounterWindow() {
        FreeBitmap();
        if (hwnd_) DestroyWindow(hwnd_);
    }

    void Hide() {
        if (visible_) ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
    }

    void Draw(const RECT& client, int corner, double fps, double ms, UINT dpi, double hz, const std::wstring& display) {
        float s = dpi ? dpi / 96.0f : 1.0f;
        int w = (int)std::lround(kW * s), h = (int)std::lround(kH * s);
        if (w != bw_ || h != bh_ || s != scale_) {
            FreeBitmap();
            Alloc(w, h, s);
        }
        if (!bits_) return;

        // 1. text, white on black (coverage)
        memset(bits_, 0, (size_t)w * h * 4);
        wchar_t num[16], sub[64];
        swprintf(num, 16, L"%.0f", fps);
        // Faster than the screen: say so, the screen only shows 'hz' of them.
        if (ms > 0 && hz > 20 && fps > hz * 1.05) swprintf(sub, 64, L"%.1f ms \u00b7 %ls %.0f Hz", ms, display.c_str(), hz);
        else if (ms > 0) swprintf(sub, 64, L"%.1f ms", ms);
        else wcscpy(sub, L"-- ms");
        SetBkMode(dc_, TRANSPARENT);
        SetTextColor(dc_, RGB(255, 255, 255));
        HGDIOBJ old = SelectObject(dc_, big_);
        int x0 = (int)std::lround(12 * s);
        TextOutW(dc_, x0, (int)std::lround(3 * s), num, (int)wcslen(num));
        SIZE ext{};
        GetTextExtentPoint32W(dc_, num, (int)wcslen(num), &ext);
        SelectObject(dc_, small_);
        TextOutW(dc_, x0 + ext.cx + (int)std::lround(5 * s), (int)std::lround(17 * s), L"FPS", 3);
        SelectObject(dc_, label_);
        TextOutW(dc_, x0 + 1, (int)std::lround(36 * s), sub, (int)wcslen(sub));
        SelectObject(dc_, old);
        GdiFlush();

        // 2. compose: rounded translucent panel + text, premultiplied alpha
        // accent colour of the number: green >= 60, yellow >= 30, red below
        float tr = 1, tg = 1, tb = 1;
        if (fps > 0 && fps < 30) tr = 1.0f, tg = 0.45f, tb = 0.40f;
        else if (fps > 0 && fps < 60) tr = 1.0f, tg = 0.82f, tb = 0.40f;
        const float pr = 10 / 255.f, pg = 11 / 255.f, pb = 14 / 255.f, pa = 0.74f;
        const float rad = 10 * s;
        int numBottom = (int)std::lround(34 * s);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint8_t* p = bits_ + ((size_t)y * w + x) * 4;
                float c = std::max({p[0], p[1], p[2]}) / 255.f;
                // rounded rectangle coverage
                float dx = std::max({rad - (x + 0.5f), (x + 0.5f) - (w - rad), 0.0f});
                float dy = std::max({rad - (y + 0.5f), (y + 0.5f) - (h - rad), 0.0f});
                float d = std::sqrt(dx * dx + dy * dy) - rad;
                float m = std::clamp(0.5f - d, 0.0f, 1.0f);
                float a = pa * m;
                float r = pr * a, g = pg * a, b = pb * a;
                float cr = 1, cg = 1, cb = 1;
                if (y < numBottom) cr = tr, cg = tg, cb = tb;
                else cr = 0.72f, cg = 0.80f, cb = 0.92f;
                c *= m;
                r = cr * c + r * (1 - c);
                g = cg * c + g * (1 - c);
                b = cb * c + b * (1 - c);
                a = c + a * (1 - c);
                p[0] = (uint8_t)std::lround(b * 255);
                p[1] = (uint8_t)std::lround(g * 255);
                p[2] = (uint8_t)std::lround(r * 255);
                p[3] = (uint8_t)std::lround(a * 255);
            }
        }

        int pad = (int)std::lround(12 * s);
        int x = (corner == 1 || corner == 3) ? client.right - w - pad : client.left + pad;
        int y = (corner == 2 || corner == 3) ? client.bottom - h - pad : client.top + pad;
        POINT pos{x, y}, src{0, 0};
        SIZE size{w, h};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        HDC screen = GetDC(nullptr);
        UpdateLayeredWindow(hwnd_, screen, &pos, &size, dc_, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
        // Stay above the game (it may be topmost as well); re-asserted now and then.
        double now = NowSeconds();
        if (!visible_ || now - lastTop_ > 1.0) {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            lastTop_ = now;
        }
        visible_ = true;
    }

private:
    void Alloc(int w, int h, float s) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc_ = CreateCompatibleDC(nullptr);
        bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, (void**)&bits_, nullptr, 0);
        if (!bmp_) {
            bits_ = nullptr;
            return;
        }
        oldBmp_ = SelectObject(dc_, bmp_);
        auto font = [&](int px, int weight) {
            return CreateFontW(-(int)std::lround(px * s), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        };
        big_ = font(28, FW_SEMIBOLD);
        small_ = font(12, FW_SEMIBOLD);
        label_ = font(12, FW_NORMAL);
        bw_ = w;
        bh_ = h;
        scale_ = s;
    }
    void FreeBitmap() {
        if (dc_ && oldBmp_) SelectObject(dc_, oldBmp_);
        if (bmp_) DeleteObject(bmp_);
        if (dc_) DeleteDC(dc_);
        for (HFONT* f : {&big_, &small_, &label_})
            if (*f) DeleteObject(*f), *f = nullptr;
        dc_ = nullptr;
        bmp_ = nullptr;
        oldBmp_ = nullptr;
        bits_ = nullptr;
        bw_ = bh_ = 0;
    }

    HWND hwnd_ = nullptr;
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    uint8_t* bits_ = nullptr;
    HFONT big_ = nullptr, small_ = nullptr, label_ = nullptr;
    int bw_ = 0, bh_ = 0;
    float scale_ = 0;
    bool visible_ = false;
    double lastTop_ = 0;
};

}  // namespace

// Windows 10 cannot hide the yellow capture border, which would frame every window in front.
static bool BorderlessCaptureSupported() {
    typedef LONG(WINAPI * Fn)(PRTL_OSVERSIONINFOW);
    auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    RTL_OSVERSIONINFOW v{sizeof(v)};
    return fn && fn(&v) == 0 && v.dwBuildNumber >= 22000;
}

void FpsMonitor::Start() {
    if (thread_.joinable()) return;
    anyWindow_ = BorderlessCaptureSupported();
    if (!anyWindow_) SW_LOG("FPS counter: Windows 10 capture border, counter only for the window being scaled");
    stop_ = false;
    thread_ = std::thread(&FpsMonitor::Thread, this);
}

void FpsMonitor::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void FpsMonitor::SetTarget(HWND target, int corner) {
    std::lock_guard<std::mutex> lock(m_);
    want_ = target;
    corner_ = corner;
}

void FpsMonitor::Thread() {
    RoInitialize(RO_INIT_MULTITHREADED);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    {
        CounterWindow win;
        if (!win.Create(GetModuleHandleW(nullptr))) {
            SW_LOG("FPS counter: window could not be created");
            RoUninitialize();
            return;
        }
        Gpu gpu;
        bool gpuOk = false;
        std::unique_ptr<Capture> cap;
        HWND cur = nullptr, failed = nullptr;
        bool borderBlocked = false, usingDda = false, curFull = false;
        double failedAt = 0;
        int frames = 0;
        double t0 = NowSeconds(), lastDraw = 0;
        double fps = 0, ms = 0;
        bool timerHi = false;
        TimerResolution* res = nullptr;

        while (!stop_) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            HWND want;
            int corner;
            std::wstring display;
            {
                std::lock_guard<std::mutex> lock(m_);
                want = want_;
                corner = corner_;
                display = display_;
            }
            if (want && !IsWindow(want)) want = nullptr;
            if (want && want == cur && FillsMonitor(want) != curFull) {
                cap.reset();
                cur = nullptr;  // switched between full-screen and windowed: choose the capture again
            }
            if (want == failed && NowSeconds() - failedAt < 3.0) want = nullptr;

            if (want != cur) {
                cap.reset();
                cur = nullptr;
                fps = ms = 0;
                frames = 0;
                t0 = NowSeconds();
                if (want) {
                    // Full-screen windows (most games): Desktop Duplication counts what reaches the
                    // screen and never draws a capture border. Other windows: window capture, when
                    // Windows lets us hide its border.
                    HMONITOR mon = MonitorFromWindow(want, MONITOR_DEFAULTTONEAREST);
                    bool full = FillsMonitor(want);
                    LUID luid{};
                    bool haveLuid = AdapterForMonitor(mon, luid);
                    if (!gpuOk || (full && haveLuid && memcmp(&gpu.info.luid, &luid, sizeof(luid)) != 0)) {
                        gpu.Release();
                        gpuOk = full && haveLuid ? gpu.Create(-1, &luid) : gpu.Create(-1);
                        if (!gpuOk) SW_LOG("FPS counter: no Direct3D device");
                    }
                    if (gpuOk) {
                        bool ok = false;
                        if (full) {
                            cap.reset(CreateDdaCapture());
                            ok = cap->Init(gpu, want, mon);
                            if (ok) usingDda = true;
                        }
                        if (!ok && !borderBlocked) {
                            cap.reset(CreateWgcCapture());
                            ok = cap->Init(gpu, want, mon);
                            if (ok && !cap->BorderFree()) {
                                // Windows would frame the window in yellow: not for a counter.
                                SW_LOG("FPS counter: capture border cannot be hidden, window capture off");
                                ok = false;
                                borderBlocked = true;
                                anyWindow_ = false;
                            }
                            usingDda = false;
                        }
                        if (ok) {
                            cap->SetCopy(false);
                            cur = want;
                            curFull = full;
                        } else {
                            cap.reset();
                            failed = want;
                            failedAt = NowSeconds();
                        }
                    }
                }
                // High timer resolution only while measuring (accurate polling of the capture).
                if (cur && !timerHi) {
                    res = new TimerResolution();
                    timerHi = true;
                } else if (!cur && timerHi) {
                    delete res;
                    res = nullptr;
                    timerHi = false;
                }
            }

            if (!cur) {
                win.Hide();
                Sleep(50);
                continue;
            }

            RECT dummy{};
            CaptureResult r = cap->Poll(dummy, nullptr);
            if (r.lost) {
                cap.reset();
                cur = nullptr;
                continue;
            }
            frames += r.frames;
            double now = NowSeconds();
            if (now - t0 >= 0.5) {
                double f = frames / (now - t0);
                fps = fps <= 0 ? f : fps * 0.5 + f * 0.5;
                ms = fps > 0.5 ? 1000.0 / fps : 0;
                frames = 0;
                t0 = now;
            }
            if (now - lastDraw >= 0.25) {
                lastDraw = now;
                RECT client;
                if (IsIconic(cur) || !IsWindowVisible(cur) || !ClientOnScreen(cur, client))
                    win.Hide();
                else
                    {
                    UINT dx = 96, dy = 96;
                    HMONITOR mon = MonitorFromWindow(cur, MONITOR_DEFAULTTONEAREST);
                    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
                    win.Draw(client, corner, fps, ms, dx, MonitorRefreshRate(mon), display);
                }
            }
            Sleep(1);
        }
        cap.reset();
        delete res;
        gpu.Release();
    }
    RoUninitialize();
}

}  // namespace sw
