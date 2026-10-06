#include "upgraph.h"

#include <shlobj.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <mutex>
#include <set>
#include <sstream>

#include "upgraph_assets.h"
#include "../third_party/miniz/miniz.h"
#include <dxgi.h>

namespace fs = std::filesystem;

namespace sw::upgraph {

namespace {

// ============================================================================ small helpers
std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
std::string U8(const fs::path& p) { return Utf8(p.wstring()); }

struct Fail : std::runtime_error {
    std::string code;
    Fail(const std::string& c, const std::string& msg) : std::runtime_error(msg), code(c) {}
};

bool ReadAll(const fs::path& p, std::string& out, size_t limit = (size_t)-1) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    size_t n = (size_t)f.tellg();
    if (n > limit) n = limit;
    f.seekg(0);
    out.resize(n);
    f.read(out.data(), (std::streamsize)n);
    return true;
}

bool WriteAll(const fs::path& p, const void* data, size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char*)data, (std::streamsize)n);
    return (bool)f;
}

std::wstring RegString(HKEY root, const std::wstring& key, const wchar_t* value) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

std::vector<std::wstring> RegSubkeys(HKEY root, const std::wstring& key) {
    std::vector<std::wstring> out;
    HKEY h;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS) return out;
    wchar_t name[512];
    for (DWORD i = 0;; i++) {
        DWORD n = 512;
        if (RegEnumKeyExW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(key + L"\\" + name);
    }
    RegCloseKey(h);
    return out;
}

bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// ============================================================================ PE inspection
struct Pe {
    int bits = 0;  // 32 / 64, 0 = not a PE image
    std::vector<std::string> imports;
};

Pe ReadPe(const fs::path& file) {
    Pe pe;
    std::ifstream f(file, std::ios::binary);
    if (!f) return pe;
    auto rd = [&](uint64_t pos, void* buf, size_t n) {
        f.clear();
        f.seekg((std::streamoff)pos);
        f.read((char*)buf, (std::streamsize)n);
        return (size_t)f.gcount() == n;
    };
    uint8_t dos[64];
    if (!rd(0, dos, 64) || dos[0] != 'M' || dos[1] != 'Z') return pe;
    uint32_t peOff = *(uint32_t*)(dos + 0x3c);
    uint8_t coff[24];
    if (!rd(peOff, coff, 24) || *(uint32_t*)coff != 0x00004550) return pe;
    uint16_t nsec = *(uint16_t*)(coff + 6), optSize = *(uint16_t*)(coff + 20);
    std::vector<uint8_t> opt(optSize);
    if (optSize < 2 || !rd(peOff + 24, opt.data(), optSize)) return pe;
    uint16_t magic = *(uint16_t*)opt.data();
    bool is64 = magic == 0x20b;
    pe.bits = is64 ? 64 : 32;
    size_t dd = is64 ? 112 : 96;
    auto dir = [&](int i, uint32_t& rva, uint32_t& size) {
        rva = size = 0;
        if (dd + i * 8 + 8 <= opt.size()) {
            rva = *(uint32_t*)&opt[dd + i * 8];
            size = *(uint32_t*)&opt[dd + i * 8 + 4];
        }
    };
    struct Sec {
        uint32_t vsize, va, rsize, raw;
    };
    std::vector<Sec> secs(nsec);
    std::vector<uint8_t> st(nsec * 40u);
    if (nsec && !rd(peOff + 24 + optSize, st.data(), st.size())) return pe;
    for (int i = 0; i < nsec; i++) {
        uint8_t* s = &st[i * 40];
        secs[i] = {*(uint32_t*)(s + 8), *(uint32_t*)(s + 12), *(uint32_t*)(s + 16), *(uint32_t*)(s + 20)};
    }
    auto toOff = [&](uint32_t rva) -> int64_t {
        for (auto& s : secs) {
            uint32_t span = std::max(s.vsize, s.rsize);
            if (rva >= s.va && rva < s.va + span) return (int64_t)s.raw + (rva - s.va);
        }
        return -1;
    };
    auto names = [&](int index, size_t stride, size_t nameOff) {
        uint32_t rva, size;
        dir(index, rva, size);
        if (!rva) return;
        int64_t off = toOff(rva);
        if (off < 0) return;
        size_t n = std::min<size_t>(size ? size : 4096, 64 * 1024);
        std::vector<uint8_t> t(n);
        f.clear();
        f.seekg(off);
        f.read((char*)t.data(), (std::streamsize)n);
        t.resize((size_t)f.gcount());
        for (size_t o = 0; o + stride <= t.size(); o += stride) {
            uint32_t nameRva = *(uint32_t*)&t[o + nameOff];
            if (!nameRva) break;
            int64_t no = toOff(nameRva);
            if (no < 0) continue;
            char buf[256] = {};
            rd((uint64_t)no, buf, 255);
            pe.imports.push_back(Lower(std::string(buf, strnlen(buf, 255))));
        }
    };
    names(1, 20, 12);   // import table
    names(13, 32, 4);   // delay-load table
    return pe;
}

// Which of 'needles' appear anywhere in the file (games that load Direct3D with LoadLibrary).
std::set<std::string> FindMarkers(const fs::path& file, const std::vector<std::string>& needles, size_t maxBytes = 512u << 20) {
    std::set<std::string> found;
    std::ifstream f(file, std::ios::binary);
    if (!f) return found;
    size_t longest = 0;
    for (auto& n : needles) longest = std::max(longest, n.size());
    const size_t chunk = 4u << 20;
    std::string buf, carry;
    size_t total = 0;
    while (f && total < maxBytes && found.size() < needles.size()) {
        buf.resize(chunk);
        f.read(buf.data(), (std::streamsize)chunk);
        buf.resize((size_t)f.gcount());
        if (buf.empty()) break;
        total += buf.size();
        std::string view = carry + buf;
        for (auto& n : needles)
            if (!found.count(n) && view.find(n) != std::string::npos) found.insert(n);
        carry = view.size() > longest ? view.substr(view.size() - longest) : view;
    }
    return found;
}

std::string VersionInfo(const fs::path& file) {
    DWORD h = 0, size = GetFileVersionInfoSizeW(file.c_str(), &h);
    if (!size) return {};
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return {};
    struct Lang {
        WORD lang, cp;
    }* langs = nullptr;
    UINT len = 0;
    std::string out;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &len) && len >= sizeof(Lang)) {
        for (const wchar_t* key : {L"ProductName", L"FileDescription", L"OriginalFilename", L"ProductVersion", L"CompanyName"}) {
            wchar_t q[128];
            swprintf(q, 128, L"\\StringFileInfo\\%04x%04x\\%ls", langs[0].lang, langs[0].cp, key);
            wchar_t* v = nullptr;
            UINT vl = 0;
            if (VerQueryValueW(data.data(), q, (void**)&v, &vl) && v) out += Utf8(v) + " | ";
        }
    }
    VS_FIXEDFILEINFO* fi = nullptr;
    if (VerQueryValueW(data.data(), L"\\", (void**)&fi, &len) && fi) {
        char b[64];
        snprintf(b, sizeof(b), "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
                 LOWORD(fi->dwFileVersionLS));
        out += b;
    }
    return out;
}

std::string FileVersion(const fs::path& file) {
    std::string v = VersionInfo(file);
    size_t p = v.rfind(" | ");
    return p == std::string::npos ? v : v.substr(p + 3);
}

bool Mentions(const fs::path& file, const char* what) { return Lower(VersionInfo(file)).find(Lower(what)) != std::string::npos; }

// ============================================================================ graphics API detection
struct Api {
    std::string api, label;  // api: dxgi, d3d9, opengl, vulkan, d3d8, ddraw, d3d10
};

bool ApiFromImports(const std::vector<std::string>& im, Api& a) {
    auto has = [&](const char* n) { return std::find(im.begin(), im.end(), n) != im.end(); };
    if (has("d3d12.dll")) a = {"dxgi", "DirectX 12"};
    else if (has("d3d11.dll")) a = {"dxgi", "DirectX 11"};
    else if (has("d3d10.dll") || has("d3d10_1.dll")) a = {"dxgi", "DirectX 10"};
    else if (has("dxgi.dll")) a = {"dxgi", "DirectX (DXGI)"};
    else if (has("vulkan-1.dll")) a = {"vulkan", "Vulkan"};
    else if (has("d3d9.dll")) a = {"d3d9", "DirectX 9"};
    else if (has("d3d8.dll")) a = {"d3d8", "DirectX 8"};
    else if (has("ddraw.dll")) a = {"ddraw", "DirectDraw"};
    else if (has("opengl32.dll")) a = {"opengl", "OpenGL"};
    else return false;
    return true;
}

bool ApiFromMarkers(const fs::path& file, Api& a) {
    static const std::vector<std::string> kMarkers = {"D3D12CreateDevice", "D3D12SDKVersion", "D3D11CreateDevice", "D3D10CreateDevice",
                                                      "CreateDXGIFactory", "Direct3DCreate9", "Direct3DCreate8", "vkCreateInstance",
                                                      "wglCreateContext"};
    auto m = FindMarkers(file, kMarkers);
    if (m.count("D3D12CreateDevice") || m.count("D3D12SDKVersion")) a = {"dxgi", "DirectX 12"};
    else if (m.count("D3D11CreateDevice")) a = {"dxgi", "DirectX 11"};
    else if (m.count("D3D10CreateDevice")) a = {"dxgi", "DirectX 10"};
    else if (m.count("CreateDXGIFactory")) a = {"dxgi", "DirectX (DXGI)"};
    else if (m.count("Direct3DCreate9")) a = {"d3d9", "DirectX 9"};
    else if (m.count("Direct3DCreate8")) a = {"d3d8", "DirectX 8"};
    else if (m.count("vkCreateInstance")) a = {"vulkan", "Vulkan"};
    else if (m.count("wglCreateContext")) a = {"opengl", "OpenGL"};
    else return false;
    return true;
}

bool ApiFromName(const std::string& lowerName, Api& a) {
    auto has = [&](const char* re) { return std::regex_search(lowerName, std::regex(re)); };
    if (has("(^|[_-])(d3d|dx)12([_.-]|$)")) a = {"dxgi", "DirectX 12"};
    else if (has("(^|[_-])(d3d|dx)11([_.-]|$)")) a = {"dxgi", "DirectX 11"};
    else if (has("(^|[_-])(d3d|dx)9([_.-]|$)")) a = {"d3d9", "DirectX 9"};
    else if (has("(^|[_-])vulkan([_.-]|$)")) a = {"vulkan", "Vulkan"};
    else return false;
    return true;
}

const std::regex kNotAGame(
    "^(unins|setup|install|vcredist|vc_redist|dxsetup|dxwebsetup|oalinst|uninstall|crashreport|crashhandler|easyanticheat|eac|"
    "battleye|be_service|launcher|activation|patch|update|dotnetfx|touchup|autorun|readme|config|benchmark|report|helper|"
    "service|cleanup|unitycrashhandler|ue4prereq|ueprereq|steamerrorreporter|reshade_setup|cefprocess|qtwebengine)",
    std::regex::icase);

const std::set<std::string> kSkipDirs = {"_moonup_upgraph", "reshade-shaders", "moonup-upgraph", "node_modules", ".git", "paks",
                                         "movies", "screenshots", "saved", "logs", "mods", "_redist", "prerequisites", "directx",
                                         "redist", "redistributable", "_commonredist", "dotnet", "installer", "installers", "support",
                                         "vcredist", "easyanticheat", "battleye", "eaanticheat", "backup", "backups", "content",
                                         "optiscaler", "engine"};

bool IsAntiCheatName(const std::string& lower) {
    return std::regex_search(lower, std::regex("easyanticheat|battleye|(^|[-_])(eac|be)launcher|eaanticheat|vanguard|ricochet"));
}

struct ExeCandidate {
    fs::path path;
    std::string name;
    uint64_t size = 0;
    int depth = 0;
    int bits = 0;
    Api api;
    bool detected = false;
    std::string via;
};

int RoleScore(const fs::path& p) {
    std::string s = Lower(U8(p));
    int score = 0;
    if (s.find("-win64-shipping.exe") != std::string::npos || s.find("-wingdk-shipping.exe") != std::string::npos) score += 6;
    if (s.find("\\binaries\\win64\\") != std::string::npos) score += 2;
    if (std::regex_search(s, std::regex("(^|[\\\\/])(multiplayer|online)([\\\\/]|$)"))) score -= 4;
    if (std::regex_search(s, std::regex("mp([_-][^\\\\/]*)?\\.exe$"))) score -= 2;
    if (std::regex_search(s, std::regex("(dedicated|server|editor|tool|crash)[^\\\\/]*\\.exe$"))) score -= 5;
    return score;
}

// ============================================================================ network
fs::path Cache();

// A regular browser identity: some download hosts (Cloudflare) refuse unknown agents.
std::string g_userAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";

// Second path for downloads: the curl.exe that ships with Windows 10 1803+ (Schannel, system proxy
// settings via the environment). Used when WinHTTP fails. Returns false when curl is missing or fails.
bool CurlGet(const std::wstring& url, const fs::path& toFile) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    fs::path curl = fs::path(sys) / L"curl.exe";
    if (!Exists(curl)) return false;
    std::error_code ec;
    fs::create_directories(toFile.parent_path(), ec);
    std::wstring cmd = L"\"" + curl.wstring() + L"\" -L -f -s -S --retry 2 --connect-timeout 20 -A \"" + Wide(g_userAgent) +
                       L"\" -o \"" + toFile.wstring() + L"\" \"" + url + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 180000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    bool ok = code == 0 && Exists(toFile) && fs::file_size(toFile, ec) > 0;
    SW_LOG("Upgraph: curl fallback %s -> %s", Utf8(url).c_str(), ok ? "ok" : ("failed, exit " + std::to_string(code)).c_str());
    return ok;
}

// GET into memory (or into 'toFile' when given). Follows redirects. Throws Fail("network", ...).
std::string HttpGetWinHttp(const std::wstring& url, const fs::path* toFile, const Progress& progress, const char* stage,
                           const wchar_t* accept) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) throw Fail("network", "bad url " + Utf8(url));
    HINTERNET s = WinHttpOpen(Wide(g_userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(Wide(g_userAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) throw Fail("network", "WinHttpOpen failed");
    struct Closer {
        HINTERNET h;
        ~Closer() {
            if (h) WinHttpCloseHandle(h);
        }
    } cs{s};
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
    if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    if (!c) throw Fail("network", "connect " + Utf8(host));
    Closer cc{c};
    HINTERNET r = WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!r) throw Fail("network", "request");
    Closer cr{r};
    std::wstring headers = L"Accept: ";
    headers += accept ? accept : L"*/*";
    headers += L"\r\n";
    if (!WinHttpSendRequest(r, headers.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r, nullptr))
        throw Fail("network", "no response from " + Utf8(host) + " (" + std::to_string(GetLastError()) + ")");
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
    if (status != 200) throw Fail("network", Utf8(host) + " answered HTTP " + std::to_string(status));
    uint64_t length = 0;
    wchar_t lenBuf[32];
    DWORD lsz = sizeof(lenBuf);
    if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenBuf, &lsz, nullptr)) length = _wtoi64(lenBuf);
    std::string out;
    std::ofstream file;
    if (toFile) {
        std::error_code ec;
        fs::create_directories(toFile->parent_path(), ec);
        file.open(*toFile, std::ios::binary | std::ios::trunc);
        if (!file) throw Fail("disk", "cannot write " + U8(*toFile));
    }
    uint64_t got = 0;
    std::vector<char> buf(256 * 1024);
    double lastReport = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(r, &avail)) throw Fail("network", "download interrupted");
        if (!avail) break;
        DWORD read = 0;
        if (!WinHttpReadData(r, buf.data(), std::min<DWORD>(avail, (DWORD)buf.size()), &read) || !read) break;
        if (toFile)
            file.write(buf.data(), read);
        else
            out.append(buf.data(), read);
        got += read;
        if (progress && length && NowSeconds() - lastReport > 0.2) {
            lastReport = NowSeconds();
            progress(stage, (double)got / (double)length);
        }
    }
    if (length && got != length) throw Fail("network", "download incomplete");
    if (toFile && !file) throw Fail("disk", "write failed " + U8(*toFile));
    return out;
}

std::string HttpGet(const std::wstring& url, const fs::path* toFile = nullptr, const Progress& progress = nullptr,
                    const char* stage = "download", const wchar_t* accept = nullptr) {
    try {
        return HttpGetWinHttp(url, toFile, progress, stage, accept);
    } catch (const Fail& f) {
        if (std::string(f.code) != "network") throw;
        SW_LOG("Upgraph: WinHTTP %s failed (%s), trying curl", Utf8(url).c_str(), f.what());
        fs::path tmp = toFile ? *toFile : Cache() / L"_http.tmp";
        if (!CurlGet(url, tmp)) throw;
        if (toFile) return {};
        std::string out;
        ReadAll(tmp, out);
        std::error_code ec;
        fs::remove(tmp, ec);
        return out;
    }
}

// ============================================================================ archives
// Zip archives are read with miniz (MIT). Anything else (OptiScaler's .7z) goes through the bsdtar
// that Windows 10 1803+ ships as tar.exe (7z support from Windows 11 23H2).
size_t ZipWrite(void* opaque, mz_uint64, const void* buf, size_t n) {
    auto* f = (std::ofstream*)opaque;
    f->write((const char*)buf, (std::streamsize)n);
    return *f ? n : 0;
}

// Extracts every entry (or those whose lower-case path matches 'filter') below 'dest'.
int UnzipTo(const fs::path& zipPath, const fs::path& dest, const std::regex* filter = nullptr) {
    FILE* fp = _wfopen(zipPath.c_str(), L"rb");
    if (!fp) throw Fail("extract", "cannot open " + U8(zipPath.filename()));
    _fseeki64(fp, 0, SEEK_END);
    mz_uint64 size = (mz_uint64)_ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
    mz_zip_archive z{};
    if (!mz_zip_reader_init_cfile(&z, fp, size, 0)) {
        fclose(fp);
        throw Fail("extract", "not a zip archive: " + U8(zipPath.filename()));
    }
    int count = 0;
    std::string err;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st) || mz_zip_reader_is_file_a_directory(&z, i)) continue;
        std::string name = st.m_filename;
        for (auto& c : name)
            if (c == '\\') c = '/';
        if (name.find("..") != std::string::npos || (!name.empty() && name[0] == '/')) continue;
        if (filter && !std::regex_search(Lower(name), *filter)) continue;
        fs::path out = dest / fs::u8path(name);
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!f || !mz_zip_reader_extract_to_callback(&z, i, ZipWrite, &f, 0)) {
            err = name;
            break;
        }
        count++;
    }
    mz_zip_reader_end(&z);
    fclose(fp);
    if (!err.empty()) throw Fail("extract", "could not unpack " + err + " from " + U8(zipPath.filename()));
    return count;
}

void Extract(const fs::path& archive, const fs::path& dest) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (LowerW(archive.extension().wstring()) == L".zip") {
        UnzipTo(archive, dest);
        return;
    }
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    if (!Exists(tar)) throw Fail("extract", "tar.exe not found (Windows 10 1803 or newer is required)");
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + archive.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        throw Fail("extract", "tar.exe could not start");
    WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0)
        throw Fail("extract", "could not unpack " + U8(archive.filename()) +
                                  (archive.extension() == ".7z" ? " (7z needs Windows 11 23H2 or newer)" : ""));
}

// ============================================================================ cache / components
fs::path Cache() {
    static fs::path root = [] {
        // Next to MoonUp.exe (the user keeps it off the system drive), else %LOCALAPPDATA%.
        fs::path p = fs::path(ExeDir()) / L"_cache" / L"upgraph";
        std::error_code ec;
        fs::create_directories(p, ec);
        fs::path probe = p / L".write_test";
        if (!ec && WriteAll(probe, "x", 1)) {
            fs::remove(probe, ec);
            return p;
        }
        p = fs::path(CacheDir()) / L"upgraph";
        fs::create_directories(p, ec);
        return p;
    }();
    return root;
}

// Index of the folders Upgraph is installed in (makes the library scan instant: no folder walks).
std::mutex g_indexMutex;
std::set<std::string> LoadIndex() {
    std::set<std::string> out;
    std::string t;
    if (ReadAll(Cache() / L"installed.json", t)) {
        json j = json::parse(t, nullptr, false);
        if (j.is_array())
            for (auto& e : j)
                if (e.is_string()) out.insert(e.get<std::string>());
    }
    return out;
}
void SaveIndex(const std::set<std::string>& idx) {
    json j = json::array();
    for (auto& e : idx) j.push_back(e);
    std::string t = j.dump();
    WriteAll(Cache() / L"installed.json", t.data(), t.size());
}
void IndexSet(const fs::path& exeDir, bool installed) {
    std::lock_guard<std::mutex> lock(g_indexMutex);
    auto idx = LoadIndex();
    std::string k = Lower(U8(exeDir));
    bool had = idx.count(k) > 0;
    if (installed == had) return;
    if (installed) idx.insert(k);
    else idx.erase(k);
    SaveIndex(idx);
}

// ReShade_Setup.exe carries a zip archive (ReShade32.dll, ReShade64.dll, ...) at its end, starting
// on a 512 byte boundary (see ReShade setup/MainWindow.xaml.cs).
void ExtractReShadeSetup(const fs::path& setup, const fs::path& dest) {
    std::string data;
    if (!ReadAll(setup, data)) throw Fail("reshade", "cannot read " + U8(setup));
    size_t at = std::string::npos;
    for (size_t o = 0; o + 512 <= data.size(); o += 512) {
        if (data.compare(o, 4, "PK\x03\x04", 4) != 0) continue;
        bool nonzero = false;
        for (size_t k = 4; k < 30; k++) nonzero |= data[o + k] != 0;
        if (nonzero) {
            at = o;
            break;
        }
    }
    if (at == std::string::npos) throw Fail("reshade", "not a ReShade setup file");
    fs::path zip = Cache() / L"reshade_payload.zip";
    if (!WriteAll(zip, data.data() + at, data.size() - at)) throw Fail("disk", "cannot write " + U8(zip));
    std::error_code ec;
    fs::remove_all(dest, ec);
    Extract(zip, dest);
    fs::remove(zip, ec);
    if (!Exists(dest / L"ReShade64.dll") || !Exists(dest / L"ReShade32.dll")) throw Fail("reshade", "ReShade DLLs missing in setup");
}

// Two ReShade builds: the standard one (MoonUp Upgraph alone) and the add-on build (needed by
// every DLSS 5 route: DLSS5-Feeder and the RenoDX neural consumer are ReShade add-ons).
fs::path ReShadeDir(bool addon = false) { return Cache() / (addon ? L"reshade-addon" : L"reshade"); }

std::string ReShadeVersion(bool addon = false) {
    fs::path d = ReShadeDir(addon) / L"ReShade64.dll";
    return Exists(d) ? FileVersion(d) : "";
}

void EnsureReShade(const Progress& progress, bool addon = false) {
    fs::path dir = ReShadeDir(addon);
    if (Exists(dir / L"ReShade64.dll") && Exists(dir / L"ReShade32.dll")) return;
    progress("reshade_find", 0);
    std::string best;
    try {
        std::string page = HttpGet(L"https://reshade.me/", nullptr, nullptr, "reshade_find", L"text/html");
        std::regex re(addon ? "downloads/(ReShade_Setup_[0-9.]+_Addon\\.exe)" : "downloads/(ReShade_Setup_[0-9.]+\\.exe)");
        std::smatch m;
        if (std::regex_search(page, m, re)) best = m[1];
    } catch (const Fail& f) {
        SW_LOG("Upgraph: reshade.me not readable (%s), using the known build", f.what());
    }
    // reshade.me could not be read: known builds (DLSS5-Feeder's installer uses the same fallback).
    if (best.empty()) best = addon ? "ReShade_Setup_6.8.0_Addon.exe" : "ReShade_Setup_6.8.0.exe";
    if (best.empty()) throw Fail("reshade_download", "ReShade download link not found on reshade.me");
    fs::path setup = Cache() / Wide(best);
    if (!Exists(setup)) {
        fs::path part = setup;
        part += L".part";
        try {
            HttpGet(L"https://reshade.me/downloads/" + Wide(best), &part, progress, "reshade_download");
        } catch (const Fail& f) {
            throw Fail("reshade_download", std::string(f.what()));
        }
        std::error_code ec;
        fs::rename(part, setup, ec);
        if (ec) throw Fail("disk", "cannot store " + U8(setup));
    }
    progress("reshade_extract", 0);
    ExtractReShadeSetup(setup, dir);
    SW_LOG("Upgraph: ReShade %s (%s) ready", ReShadeVersion(addon).c_str(), addon ? "add-on build" : "standard");
}

// OptiScaler (GPL-3.0, https://github.com/optiscaler/OptiScaler): latest GitHub release.
fs::path FindOptiDir(const fs::path& root) {
    std::error_code ec;
    if (Exists(root / L"OptiScaler.dll")) return root;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (LowerW(it->path().filename().wstring()) == L"optiscaler.dll") return it->path().parent_path();
    }
    return {};
}

fs::path EnsureOptiScaler(const Progress& progress, std::string& version) {
    fs::path base = Cache() / L"optiscaler";
    std::error_code ec;
    // a release that was already unpacked
    fs::path known;
    if (Exists(base))
        for (auto& e : fs::directory_iterator(base, ec))
            if (e.is_directory() && !FindOptiDir(e.path()).empty()) {
                known = e.path();
                version = U8(e.path().filename());
            }
    json rel;
    try {
        progress("optiscaler_find", 0);
        std::string body = HttpGet(L"https://api.github.com/repos/optiscaler/OptiScaler/releases/latest", nullptr, nullptr,
                                   "optiscaler_find", L"application/vnd.github+json");
        rel = json::parse(body, nullptr, false);
    } catch (const Fail&) {
        if (!known.empty()) return FindOptiDir(known);
        throw;
    }
    if (!rel.is_object() || !rel.contains("assets")) {
        if (!known.empty()) return FindOptiDir(known);
        throw Fail("optiscaler_download", "OptiScaler release information unavailable");
    }
    std::string tag = rel.value("tag_name", "latest");
    fs::path dir = base / Wide(tag);
    if (!FindOptiDir(dir).empty()) {
        version = tag;
        return FindOptiDir(dir);
    }
    std::string url, name;
    for (auto& a : rel["assets"]) {
        std::string n = a.value("name", "");
        std::string ln = Lower(n);
        if ((ln.size() > 3 && (ln.substr(ln.size() - 3) == ".7z" || ln.substr(ln.size() - 4) == ".zip")) &&
            ln.find("source") == std::string::npos && ln.find("debug") == std::string::npos &&
            ln.find("pdb") == std::string::npos) {
            url = a.value("browser_download_url", "");
            name = n;
            if (ln.find("optiscaler") != std::string::npos) break;
        }
    }
    if (url.empty()) throw Fail("optiscaler_download", "no OptiScaler archive in release " + tag);
    fs::path archive = base / Wide(name);
    if (!Exists(archive)) {
        fs::path part = archive;
        part += L".part";
        HttpGet(Wide(url), &part, progress, "optiscaler_download");
        fs::rename(part, archive, ec);
    }
    progress("optiscaler_extract", 0);
    fs::remove_all(dir, ec);
    Extract(archive, dir);
    fs::path found = FindOptiDir(dir);
    if (found.empty()) throw Fail("optiscaler_download", "OptiScaler.dll not found in " + name);
    version = tag;
    SW_LOG("Upgraph: OptiScaler %s ready", tag.c_str());
    return found;
}

// ============================================================================ INI editing
// Sets key=value inside [section] (adds the section/key when missing). Keeps everything else.
std::string IniSet(const std::string& text, const std::string& section, const std::string& key, const std::string& value) {
    std::istringstream in(text);
    std::string line, out, cur;
    bool done = false, inSec = false, secSeen = false;
    std::vector<std::string> lines;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    for (size_t i = 0; i < lines.size(); i++) {
        std::string& l = lines[i];
        std::string t = l;
        t.erase(0, t.find_first_not_of(" \t"));
        if (!t.empty() && t[0] == '[') {
            if (inSec && !done) {
                out += key + "=" + value + "\r\n";
                done = true;
            }
            cur = t.substr(1, t.find(']') - 1);
            inSec = Lower(cur) == Lower(section);
            secSeen |= inSec;
        } else if (inSec && !done) {
            size_t eq = t.find('=');
            if (eq != std::string::npos && t[0] != ';' && t[0] != '#') {
                std::string k = t.substr(0, eq);
                while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
                if (Lower(k) == Lower(key)) {
                    out += key + "=" + value + "\r\n";
                    done = true;
                    continue;
                }
            }
        }
        out += l + "\r\n";
    }
    if (!done) {
        if (inSec)
            out += key + "=" + value + "\r\n";
        else
            out += "\r\n[" + section + "]\r\n" + key + "=" + value + "\r\n";
    }
    (void)secSeen;
    return out;
}

// ============================================================================ install journal
struct Journal {
    fs::path gameDir, exeDir, dir;  // dir = <exeDir>\_MoonUp_Upgraph
    json m;

    void Load(const fs::path& exeDirIn) {
        exeDir = exeDirIn;
        dir = exeDir / L"_MoonUp_Upgraph";
        std::string text;
        if (ReadAll(dir / L"manifest.json", text)) m = json::parse(text, nullptr, false);
        if (!m.is_object()) m = json::object();
        if (!m.contains("added")) m["added"] = json::array();
        if (!m.contains("backups")) m["backups"] = json::array();
    }
    void Save() {
        std::string t = m.dump(2);
        if (!WriteAll(dir / L"manifest.json", t.data(), t.size())) throw Fail("disk", "cannot write manifest");
        IndexSet(exeDir, true);
    }
    std::string Rel(const fs::path& p) const { return U8(fs::relative(p, exeDir)); }
    bool Added(const fs::path& p) const {
        std::string r = Lower(Rel(p));
        for (auto& a : m["added"])
            if (Lower(a.get<std::string>()) == r) return true;
        return false;
    }
    // Moves an existing file that is not ours into the backup folder before it is overwritten.
    void Protect(const fs::path& dest) {
        if (!Exists(dest) || Added(dest)) return;
        fs::path b = dir / L"backup" / fs::relative(dest, exeDir);
        std::error_code ec;
        fs::create_directories(b.parent_path(), ec);
        fs::rename(dest, b, ec);
        if (ec) {
            fs::copy_file(dest, b, fs::copy_options::overwrite_existing, ec);
            if (ec) throw Fail("disk", "cannot back up " + U8(dest));
        }
        m["backups"].push_back(Rel(dest));
        Save();
    }
    void Record(const fs::path& dest) {
        if (!Added(dest)) m["added"].push_back(Rel(dest));
    }
    void Write(const fs::path& dest, const void* data, size_t n) {
        Protect(dest);
        if (!WriteAll(dest, data, n)) throw Fail("disk", "cannot write " + U8(dest));
        Record(dest);
    }
    void Copy(const fs::path& src, const fs::path& dest) {
        Protect(dest);
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) throw Fail("disk", "cannot copy to " + U8(dest) + ": " + ec.message());
        Record(dest);
    }
};

bool ExeRunning(const fs::path& exe) {
    HANDLE h = CreateFileW(exe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
}

std::string F(double v) {
    char b[32];
    snprintf(b, sizeof(b), "%.6f", v);
    return b;
}

// Preset: the technique list (DLSS 5 feed chain and/or MoonUp Upgraph) and Upgraph's sliders.
std::string PresetText(const json& p, bool shader = true, bool feeder = false) {
    auto g = [&](const char* k, double d) { return p.is_object() && p.contains(k) && p[k].is_number() ? p[k].get<double>() : d; };
    std::vector<std::string> tech, sort;
    if (feeder) {
        // the motion-vector provider must run before DLSS5_Feed
        tech = {"Lumenite_Kernel@lumenite_Kernel.fx", "DLSS5_Feed@DLSS5_Feed.fx"};
        sort = tech;
        sort.push_back("DLSS5_Feed_Debug@DLSS5_Feed.fx");
    }
    if (shader) {
        tech.push_back("MoonUpUpgraph@MoonUpUpgraph.fx");
        sort.push_back("MoonUpUpgraph@MoonUpUpgraph.fx");
    }
    auto join = [](const std::vector<std::string>& v) {
        std::string r;
        for (auto& x : v) r += (r.empty() ? "" : ",") + x;
        return r;
    };
    std::string s;
    s += "Techniques=" + join(tech) + "\r\n";
    s += "TechniqueSorting=" + join(sort) + "\r\n\r\n";
    if (feeder) {
        s += "[DLSS5_Feed.fx]\r\nDEBUG_VIEW=0\r\nMV_SCALE=1.000000\r\nMV_SIGN=1.000000,1.000000\r\n";
        s += "PreprocessorDefinitions=DLSS5_MV_PROVIDER=3\r\n\r\n";
    }
    s += "[MoonUpUpgraph.fx]\r\n";
    s += "Color=" + F(g("color", 0.8)) + "\r\n";
    s += "DebugView=0\r\n";
    s += "DepthMode=" + std::to_string((int)g("depthMode", 0)) + "\r\n";
    s += "LightRadius=" + F(g("lightRadius", 1.0)) + "\r\n";
    s += "LightStrength=" + F(g("light", 0.6)) + "\r\n";
    s += "Sharpness=" + F(g("sharpness", 0.35)) + "\r\n";
    s += "Structure=" + F(g("structure", 1.0)) + "\r\n";
    s += "Temporal=" + F(g("temporal", 0.7)) + "\r\n";
    s += "Tone=" + F(g("tone", 1.0)) + "\r\n";
    return s;
}

// ReShade.ini. 'dlss5' adds what the add-on route needs (add-on path, framework shader folders).
std::string ReShadeIni(bool unreal, bool dlss5 = false, bool depthClears = false) {
    std::string s;
    if (dlss5) {
        s += "[ADDON]\r\nAddonPath=.\\\r\n\r\n";
        s += std::string("[DEPTH]\r\nDepthCopyBeforeClears=") + (depthClears ? "1" : "0") + "\r\n\r\n";
    }
    s += "[GENERAL]\r\n";
    if (dlss5) {
        s += "EffectSearchPaths=.\\reshade-shaders\\Shaders\\**,.\\moonup-upgraph\\Shaders\r\n";
        s += "TextureSearchPaths=.\\reshade-shaders\\Textures\\**,.\\moonup-upgraph\\Textures\r\n";
        s += "NoDebugInfo=1\r\n";
    } else {
        s += "EffectSearchPaths=.\\moonup-upgraph\\Shaders\r\n";
        s += "TextureSearchPaths=.\\moonup-upgraph\\Textures\r\n";
    }
    s += "IntermediateCachePath=.\\moonup-upgraph\\Cache\r\n";
    s += "PresetPath=.\\MoonUpUpgraph.ini\r\n";
    s += "PerformanceMode=0\r\n";
    s += "NoReloadOnInit=0\r\n";
    s += "SkipLoadingDisabledEffects=" + std::string(dlss5 ? "0" : "1") + "\r\n";
    // Unreal Engine (and most modern engines) use reversed depth; the effect also detects it.
    s += std::string("PreprocessorDefinitions=RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0,") +
         "RESHADE_DEPTH_INPUT_IS_REVERSED=" + (unreal ? "1" : "0") + ",RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0\r\n\r\n";
    s += "[INPUT]\r\n";
    s += "ForceShortcutModifiers=1\r\n";
    s += "KeyOverlay=36,0,0,0\r\n";   // Home
    s += "KeyEffects=145,0,0,0\r\n";  // Scroll Lock: effects on/off (before / after)
    s += "KeyReload=0,0,0,0\r\n\r\n";
    s += "[OVERLAY]\r\n";
    s += "TutorialProgress=4\r\n";
    s += "ShowFPS=0\r\n";
    s += "ShowClock=0\r\n";
    s += "AutoSavePreset=1\r\n\r\n";
    s += "[SCREENSHOT]\r\n";
    s += "SavePath=.\\moonup-upgraph\\Screenshots\r\n";
    return s;
}

const char* HookFor(const std::string& api) {
    if (api == "dxgi" || api == "d3d10") return "dxgi.dll";
    if (api == "d3d9") return "d3d9.dll";
    if (api == "opengl") return "opengl32.dll";
    return nullptr;
}

// ============================================================================ GPU
// NVIDIA's own DLSS 5 model (nvngx_dlssnr.dll) runs on GeForce RTX 50 (Blackwell). The modified
// runtime on RHI (310.8.Lecram), the one DLSS5-Swapper's ReShade/Feeder routes ship, is reported by
// its author to also run on RTX 20/30/40. A GPU-specific build picked by the user overrides it.
json GpuInfo() {
    json out = json::array();
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return out;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++, a.Reset()) {
        DXGI_ADAPTER_DESC1 d;
        if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        std::string name = Utf8(d.Description);
        std::string ln = Lower(name);
        bool nvidia = d.VendorId == 0x10de;
        std::string tier = "other";
        if (nvidia && ln.find(" ada") == std::string::npos && (std::regex_search(ln, std::regex("rtx\\s*50[5-9]0|rtx\\s*5[0-9]{3}")) ||
                       std::regex_search(ln, std::regex("rtx pro [0-9]+ blackwell|blackwell"))))
            tier = "rtx50";
        else if (nvidia && std::regex_search(ln, std::regex("rtx|quadro rtx|titan rtx")))
            tier = "rtx";
        // Driver: 32.0.15.6164 -> 561.64 (last digit of the third field + the fourth field)
        std::string driver;
        LARGE_INTEGER umd{};
        if (nvidia && SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            unsigned c = HIWORD(umd.LowPart), dd = LOWORD(umd.LowPart);
            unsigned n = (c % 10) * 10000 + dd;
            char b[32];
            snprintf(b, sizeof(b), "%u.%02u", n / 100, n % 100);
            driver = b;
        }
        out.push_back({{"name", name}, {"vendor", d.VendorId}, {"tier", tier}, {"driver", driver}});
    }
    return out;
}

// ============================================================================ DLSS 5 components
// Everything is fetched at install time from the projects' own GitHub releases (and reshade.me),
// exactly the sources DLSS5-Feeder's installer uses; nothing is bundled with MoonUp.
//   DLSS5-Feeder (MIT)             https://github.com/jlrouzies-fr/DLSS5-Feeder
//   RenoDX DLSS 5 add-on, NGX DLLs https://github.com/RankFTW/rhi-repo (RHI release mirror)
//   ReShade add-on build           https://reshade.me
//   LumeniteFX (motion vectors)    https://github.com/umar-afzaal/LumeniteFX
//   dgVoodoo2 (DirectX 8/9)        https://github.com/dege-diosg/dgVoodoo2
fs::path D5() { return Cache() / L"dlss5"; }

json GithubJson(const std::wstring& url) {
    std::string body = HttpGet(url, nullptr, nullptr, "dlss5_find", L"application/vnd.github+json");
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded()) throw Fail("network", "bad answer from GitHub");
    return j;
}

fs::path FindFile(const fs::path& root, const std::string& lowerName) {
    std::error_code ec;
    if (!Exists(root)) return {};
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && Lower(U8(it->path().filename())) == lowerName) return it->path();
    }
    return {};
}

// Newest unpacked copy under 'base' that contains 'lowerName' (offline fallback).
fs::path NewestCached(const fs::path& base, const std::string& lowerName) {
    std::error_code ec;
    fs::path best;
    fs::file_time_type bestT{};
    if (!Exists(base)) return {};
    for (auto& e : fs::directory_iterator(base, ec)) {
        if (!e.is_directory()) continue;
        fs::path f = FindFile(e.path(), lowerName);
        if (f.empty()) continue;
        auto t = fs::last_write_time(f, ec);
        if (best.empty() || t > bestT) {
            best = f;
            bestT = t;
        }
    }
    return best;
}

// Downloads 'url' (a zip) once into base/<tag>/ and unpacks it there.
fs::path FetchZip(const std::wstring& url, const fs::path& dir, const Progress& progress, const char* stage) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path zip = dir / L"_download.zip";
    fs::path part = zip;
    part += L".part";
    HttpGet(url, &part, progress, stage);
    fs::remove(zip, ec);
    fs::rename(part, zip, ec);
    progress(stage, 1.0);
    UnzipTo(zip, dir);
    fs::remove(zip, ec);
    return dir;
}

struct Dlss5Parts {
    fs::path feederDir, renodx, dlssnr, dlss, headersDir, lumeniteDir, dgvDir;
    std::string feederVersion, renodxVersion, dlssnrVersion, dlssVersion;
    bool customNr = false;
};

fs::path CustomNr() { return D5() / L"custom" / L"nvngx_dlssnr.dll"; }

json g_rhi;  // RHI release list, fetched once per install

// One piece from RHI's releases: the newest release whose tag matches 'tagRe', inner file 'inner'.
fs::path RhiPiece(const std::string& tagRe, const std::string& inner, const std::wstring& fallbackUrl, const std::wstring& sub,
                  const Progress& progress, const char* stage, std::string& version) {
    fs::path base = D5() / L"rhi" / sub;
    try {
        if (g_rhi.is_null()) g_rhi = GithubJson(L"https://api.github.com/repos/RankFTW/rhi-repo/releases?per_page=100");
    } catch (const Fail&) {
        g_rhi = json::array();
    }
    std::string tag, url, published;
    if (g_rhi.is_array()) {
        std::regex re(tagRe);
        for (auto& r : g_rhi) {
            if (!r.is_object() || r.value("draft", false)) continue;
            std::string t = r.value("tag_name", "");
            if (!std::regex_search(t, re)) continue;
            std::string p = r.value("published_at", "");
            for (auto& a : r.value("assets", json::array())) {
                std::string n = Lower(a.value("name", ""));
                if (n.size() > 4 && n.substr(n.size() - 4) == ".zip" && p > published) {
                    tag = t;
                    url = a.value("browser_download_url", "");
                    published = p;
                    break;
                }
            }
        }
    }
    if (tag.empty()) {
        fs::path cached = NewestCached(base, inner);
        if (!cached.empty()) {
            version = U8(cached.parent_path().filename());
            return cached;
        }
        url = Utf8(fallbackUrl);
        tag = U8(fs::path(fallbackUrl).stem());
    }
    fs::path dir = base / Wide(tag);
    fs::path have = FindFile(dir, inner);
    if (have.empty()) {
        FetchZip(Wide(url), dir, progress, stage);
        have = FindFile(dir, inner);
    }
    if (have.empty()) throw Fail("dlss5_download", inner + " not found in " + tag);
    version = tag;
    return have;
}

void EnsureDlss5(Dlss5Parts& p, bool feeder, bool needDlss, bool dgvoodoo, const Progress& progress) {
    g_rhi = json();
    std::error_code ec;
    // --- neural model: the user's own GPU-specific build first, else RHI's newest public build
    if (Exists(CustomNr())) {
        p.dlssnr = CustomNr();
        p.dlssnrVersion = FileVersion(p.dlssnr) + " (custom)";
        p.customNr = true;
    } else {
        p.dlssnr = RhiPiece("^dlssnr-", "nvngx_dlssnr.dll",
                            L"https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.Lecram/nvngx_dlssnr_310.8.Lecram.zip",
                            L"dlssnr", progress, "dlss5_model", p.dlssnrVersion);
    }
    // --- neural consumer: RenoDX DLSS 5 add-on (v6+ survives driver 616.64 and newer)
    p.renodx = RhiPiece("^renodx-dlss5-[0-9]", "renodx-dlss5.addon64",
                        L"https://github.com/RankFTW/rhi-repo/releases/download/renodx-dlss5-7.0.0-rc8/renodx-dlss5_7.0.0-rc8.zip",
                        L"renodx", progress, "dlss5_consumer", p.renodxVersion);
    // --- DLSS runtime (games without DLSS of their own)
    if (needDlss)
        p.dlss = RhiPiece("^dlss-[0-9]", "nvngx_dlss.dll",
                          L"https://github.com/RankFTW/rhi-repo/releases/download/dlss-310.9.1/nvngx_dlss_310.9.1.zip", L"dlss",
                          progress, "dlss5_runtime", p.dlssVersion);
    if (feeder) {
        // --- DLSS5-Feeder release
        fs::path base = D5() / L"feeder";
        std::string tag, url;
        try {
            json rel = GithubJson(L"https://api.github.com/repos/jlrouzies-fr/DLSS5-Feeder/releases/latest");
            tag = rel.value("tag_name", "");
            for (auto& a : rel.value("assets", json::array())) {
                std::string n = a.value("name", "");
                if (std::regex_search(n, std::regex("^DLSS5-Feeder-.*\\.zip$", std::regex::icase))) {
                    url = a.value("browser_download_url", "");
                    break;
                }
            }
        } catch (const Fail&) {
        }
        if (url.empty()) {
            fs::path cached = NewestCached(base, "dlss5_feed.fx");
            if (cached.empty()) throw Fail("dlss5_download", "DLSS5-Feeder release not reachable on GitHub");
            p.feederDir = cached.parent_path();
            while (p.feederDir.parent_path() != base && p.feederDir.has_parent_path()) p.feederDir = p.feederDir.parent_path();
            p.feederVersion = U8(p.feederDir.filename());
        } else {
            p.feederDir = base / Wide(tag);
            if (FindFile(p.feederDir, "dlss5_feed.fx").empty()) FetchZip(Wide(url), p.feederDir, progress, "dlss5_feeder");
            p.feederVersion = tag;
        }
        // --- ReShade framework headers (the feeder's shader includes them)
        p.headersDir = D5() / L"headers";
        for (const char* h : {"ReShade.fxh", "ReShadeUI.fxh", "DrawText.fxh"}) {
            fs::path dst = p.headersDir / h;
            if (Exists(dst)) continue;
            fs::path part = dst;
            part += L".part";
            HttpGet(L"https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders/" + Wide(h), &part, nullptr, "dlss5_feeder");
            fs::rename(part, dst, ec);
        }
        // --- LumeniteFX: optical-flow motion vectors for the feed (DLSS5_MV_PROVIDER=3)
        p.lumeniteDir = D5() / L"lumenite";
        if (FindFile(p.lumeniteDir, "lumenite_kernel.fx").empty()) {
            try {
                FetchZip(L"https://codeload.github.com/umar-afzaal/LumeniteFX/zip/refs/heads/mainline", p.lumeniteDir, progress,
                         "dlss5_motion");
            } catch (const Fail&) {
                if (FindFile(p.lumeniteDir, "lumenite_kernel.fx").empty()) throw;
            }
        }
    }
    if (dgvoodoo) {
        fs::path base = D5() / L"dgvoodoo";
        std::string tag, url;
        try {
            json rel = GithubJson(L"https://api.github.com/repos/dege-diosg/dgVoodoo2/releases/latest");
            tag = rel.value("tag_name", "");
            for (auto& a : rel.value("assets", json::array()))
                if (std::regex_search(a.value("name", ""), std::regex("^dgVoodoo2_[0-9]+_[0-9]+\\.zip$", std::regex::icase))) {
                    url = a.value("browser_download_url", "");
                    break;
                }
        } catch (const Fail&) {
        }
        if (url.empty()) {
            fs::path cached = NewestCached(base, "dgvoodoo.conf");
            if (cached.empty()) throw Fail("dlss5_download", "dgVoodoo2 not reachable on GitHub");
            p.dgvDir = cached.parent_path();
        } else {
            p.dgvDir = base / Wide(tag);
            if (FindFile(p.dgvDir, "dgvoodoo.conf").empty()) FetchZip(Wide(url), p.dgvDir, progress, "dlss5_dgvoodoo");
            p.dgvDir = FindFile(p.dgvDir, "dgvoodoo.conf").parent_path();
        }
    }
}

// ReShade's own compatibility list: games that ban ReShade are refused.
bool ReShadeBanned(const std::string& exeName) {
    try {
        std::string ini = HttpGet(L"https://raw.githubusercontent.com/crosire/reshade-shaders/list/Compatibility.ini", nullptr, nullptr,
                                  "dlss5_find", L"text/plain");
        std::string sec = "[" + Lower(exeName) + "]";
        std::string low = Lower(ini);
        size_t at = low.find(sec);
        if (at == std::string::npos) return false;
        size_t end = low.find("\n[", at + sec.size());
        std::string body = low.substr(at, end == std::string::npos ? std::string::npos : end - at);
        return std::regex_search(body, std::regex("\\nbanned\\s*=\\s*1"));
    } catch (const Fail&) {
        return false;
    }
}

// Every other neural consumer / NGX interposer must go: each turns the others inert.
bool IsOtherConsumer(const fs::path& p) {
    std::string n = Lower(U8(p.filename()));
    if (n == "renodx-dlss5.addon64") return false;
    if (std::regex_search(n, std::regex("^(deep-fried-chicken.*|alexs-toolkit\\.addon64|dlss5-dx11-bridge\\.addon64|renodx-dlss.*\\.addon64)$")))
        return true;
    static const std::set<std::string> optiNames = {"winmm.dll", "version.dll", "dbghelp.dll", "winhttp.dll", "wininet.dll", "d3d12.dll",
                                                    "optiscaler.dll", "optiscaler.asi"};
    return optiNames.count(n) && Lower(VersionInfo(p)).find("optiscaler") != std::string::npos;
}

json Status(const fs::path& gameDir, const fs::path& exeDir);

}  // namespace

// ============================================================================ public API
std::wstring CacheRoot() { return Cache().wstring(); }

json ScanLibrary(const json& folders) {
    double t0 = NowSeconds();
    json games = json::array();
    std::set<std::wstring> seen;
    // Installed state: the index written by Install/Remove (manifest re-checked), plus a small
    // bounded look for installs made before the index existed (folders only, at most 3 levels).
    const bool haveIndex = Exists(Cache() / L"installed.json");
    const std::set<std::string> index = [] {
        std::lock_guard<std::mutex> lock(g_indexMutex);
        return LoadIndex();
    }();
    std::set<std::string> found;  // installs located by the one-time look (index migration)
    auto IsInstalledQuick = [&](const fs::path& dir) {
        std::string root = Lower(U8(dir));
        if (!root.empty() && root.back() != '\\') root += '\\';
        for (auto& e : index) {
            if ((e + "\\").rfind(root, 0) == 0 && Exists(fs::path(Wide(e)) / L"_MoonUp_Upgraph" / L"manifest.json"))
                return true;
        }
        if (haveIndex) return false;
        std::vector<std::pair<fs::path, int>> stack{{dir, 0}};
        int budget = 120;
        while (!stack.empty() && budget-- > 0) {
            auto [d, depth] = stack.back();
            stack.pop_back();
            std::error_code ec;
            for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), endIt; !ec && it != endIt;
                 it.increment(ec)) {
                std::error_code e2;
                if (!it->is_directory(e2)) continue;
                std::string n = Lower(U8(it->path().filename()));
                if (n == "_moonup_upgraph") {
                    found.insert(Lower(U8(d)));
                    return true;
                }
                if (depth < 3 && !kSkipDirs.count(n)) stack.push_back({it->path(), depth + 1});
            }
        }
        return false;
    };
    auto add = [&](const std::string& launcher, const std::string& id, const std::wstring& name, const fs::path& dir,
                   const fs::path& poster) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) return;
        std::wstring key = LowerW(fs::weakly_canonical(dir, ec).wstring());
        if (seen.count(key)) return;
        static const std::regex skipNames("^(steamworks common redistributables|proton|steam linux runtime|"
                                          "directx|vc redist|steamvr|wallpaper engine)");
        if (std::regex_search(Lower(Utf8(name)), skipNames))
            return;
        seen.insert(key);
        json g{{"launcher", launcher}, {"id", id}, {"name", Utf8(name)}, {"dir", U8(dir)}};
        g["poster"] = poster.empty() ? "" : U8(poster);
        g["installed"] = IsInstalledQuick(dir);
        games.push_back(g);
    };

    // ---- Steam
    std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (!steam.empty()) {
        for (auto& c : steam)
            if (c == L'/') c = L'\\';
        std::set<std::wstring> libs{steam};
        std::string vdf;
        if (ReadAll(fs::path(steam) / L"steamapps" / L"libraryfolders.vdf", vdf)) {
            std::regex re("\"path\"\\s+\"([^\"]+)\"");
            for (auto it = std::sregex_iterator(vdf.begin(), vdf.end(), re); it != std::sregex_iterator(); ++it) {
                std::string p = (*it)[1];
                std::string q;
                for (size_t i = 0; i < p.size(); i++) {
                    if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') i++;
                    q += p[i];
                }
                libs.insert(Wide(q));
            }
        }
        for (auto& lib : libs) {
            fs::path apps = fs::path(lib) / L"steamapps";
            std::error_code ec;
            for (auto& e : fs::directory_iterator(apps, ec)) {
                std::string fn = U8(e.path().filename());
                if (fn.rfind("appmanifest_", 0) != 0) continue;
                std::string acf;
                if (!ReadAll(e.path(), acf)) continue;
                static const std::regex reId("\"appid\"\\s+\"([^\"]*)\"", std::regex::icase),
                    reDir("\"installdir\"\\s+\"([^\"]*)\"", std::regex::icase),
                    reName("\"name\"\\s+\"([^\"]*)\"", std::regex::icase);
                auto kv = [&](const std::regex& re) {
                    std::smatch m;
                    return std::regex_search(acf, m, re) ? m[1].str() : std::string();
                };
                std::string appid = kv(reId), dirName = kv(reDir), name = kv(reName);
                if (appid.empty() || dirName.empty()) continue;
                fs::path cache = fs::path(steam) / L"appcache" / L"librarycache";
                fs::path poster;
                for (auto cand : {cache / Wide(appid) / L"library_600x900.jpg", cache / (Wide(appid) + L"_library_600x900.jpg"),
                                  cache / Wide(appid) / L"header.jpg", cache / (Wide(appid) + L"_header.jpg")})
                    if (Exists(cand)) {
                        poster = cand;
                        break;
                    }
                add("Steam", appid, Wide(name.empty() ? dirName : name), apps / L"common" / Wide(dirName), poster);
            }
        }
    }
    // ---- Epic Games
    {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(L"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests", ec)) {
            if (e.path().extension() != L".item") continue;
            std::string t;
            if (!ReadAll(e.path(), t)) continue;
            json j = json::parse(t, nullptr, false);
            if (!j.is_object()) continue;
            add("Epic Games", j.value("AppName", ""), Wide(j.value("DisplayName", "")), Wide(j.value("InstallLocation", "")), {});
        }
    }
    // ---- GOG
    for (auto& k : RegSubkeys(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games")) {
        std::wstring dir = RegString(HKEY_LOCAL_MACHINE, k, L"path"), name = RegString(HKEY_LOCAL_MACHINE, k, L"gameName");
        add("GOG", Utf8(k.substr(k.rfind(L'\\') + 1)), name.empty() ? fs::path(dir).filename().wstring() : name, dir, {});
    }
    // ---- Ubisoft Connect
    for (auto& k : RegSubkeys(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs")) {
        std::wstring dir = RegString(HKEY_LOCAL_MACHINE, k, L"InstallDir");
        for (auto& c : dir)
            if (c == L'/') c = L'\\';
        while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
        add("Ubisoft", Utf8(k.substr(k.rfind(L'\\') + 1)), fs::path(dir).filename().wstring(), dir, {});
    }
    // ---- folders the user added: the folder itself when it holds a game, else every subfolder
    if (folders.is_array()) {
        for (auto& f : folders) {
            if (!f.is_string()) continue;
            fs::path root = Wide(f.get<std::string>());
            std::error_code ec;
            bool hasExe = false;
            for (auto& e : fs::directory_iterator(root, ec))
                if (e.path().extension() == L".exe" || LowerW(e.path().filename().wstring()) == L"binaries") hasExe = true;
            if (hasExe) {
                add("Folder", "", root.filename().wstring(), root, {});
                continue;
            }
            for (auto& e : fs::directory_iterator(root, ec))
                if (e.is_directory(ec)) add("Folder", "", e.path().filename().wstring(), e.path(), {});
        }
    }
    std::sort(games.begin(), games.end(), [](const json& a, const json& b) { return Lower(a["name"]) < Lower(b["name"]); });
    if (!haveIndex) {
        std::lock_guard<std::mutex> lock(g_indexMutex);
        auto idx = LoadIndex();
        idx.insert(found.begin(), found.end());
        SaveIndex(idx);
    }
    SW_LOG("Upgraph: library %d games in %.2f s", (int)games.size(), NowSeconds() - t0);
    return games;
}

json ScanGame(const std::wstring& dirIn) {
    fs::path dir = dirIn;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw Fail("not_found", "folder not found");
    std::vector<ExeCandidate> exes, undetected;
    json upscalers = json::array();
    bool antiCheat = false;
    std::set<std::string> upscalerSeen;
    int visited = 0;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && visited < 60000; it.increment(ec)) {
        if (ec) break;
        visited++;
        std::string name = Lower(U8(it->path().filename()));
        if (it->is_directory(ec)) {
            if (IsAntiCheatName(name)) antiCheat = true;
            if (it.depth() >= 7 || kSkipDirs.count(name)) it.disable_recursion_pending();
            continue;
        }
        if (IsAntiCheatName(name)) antiCheat = true;
        // upscaler libraries (what OptiScaler can take over)
        const char* kind = nullptr;
        if (name == "nvngx_dlss.dll") kind = "DLSS";
        else if (name == "nvngx_dlssg.dll") kind = "DLSS-FG";
        else if (name == "libxess.dll") kind = "XeSS";
        else if (name.rfind("ffx_fsr2", 0) == 0 || name.rfind("amd_fidelityfx_dx12", 0) == 0 || name.rfind("amd_fidelityfx_vk", 0) == 0 ||
                 name.rfind("ffx_fsr3", 0) == 0 || name.rfind("amd_fidelityfx_upscaler", 0) == 0)
            kind = "FSR";
        else if (name.rfind("sl.dlss", 0) == 0)
            kind = "Streamline";
        if (kind && !upscalerSeen.count(kind)) {
            upscalerSeen.insert(kind);
            upscalers.push_back({{"kind", kind}, {"file", U8(fs::relative(it->path(), dir))}, {"version", FileVersion(it->path())}});
        }
        if (it->path().extension() != L".exe" || std::regex_search(name, kNotAGame)) continue;
        Pe pe = ReadPe(it->path());
        if (!pe.bits) continue;
        ExeCandidate c;
        c.path = it->path();
        c.name = U8(it->path().filename());
        c.size = it->file_size(ec);
        c.depth = it.depth();
        c.bits = pe.bits;
        if (ApiFromImports(pe.imports, c.api)) c.via = "imports";
        else if (c.size < (600ull << 20) && ApiFromMarkers(c.path, c.api)) c.via = "strings";
        else if (ApiFromName(name, c.api)) c.via = "filename";
        c.detected = !c.via.empty();
        (c.detected ? exes : undetected).push_back(c);
    }
    auto order = [](const ExeCandidate& a, const ExeCandidate& b) {
        int ra = RoleScore(a.path), rb = RoleScore(b.path);
        bool a12 = a.api.label == "DirectX 12", b12 = b.api.label == "DirectX 12";
        if (ra != rb) return ra > rb;
        if (a12 != b12) return a12;
        if (a.depth != b.depth) return a.depth < b.depth;
        return a.size > b.size;
    };
    std::sort(exes.begin(), exes.end(), order);
    std::sort(undetected.begin(), undetected.end(), order);
    // Unreal Engine: the small launcher in the root starts <Game>\Binaries\Win64\*-Shipping.exe.
    bool unreal = Exists(dir / L"Engine" / L"Binaries") || Exists(dir / L"Engine");
    for (auto& e : exes)
        if (Lower(e.name).find("-shipping.exe") != std::string::npos) unreal = true;
    if (exes.empty() && !undetected.empty() && (unreal || !upscalers.empty())) {
        // a game whose renderer is loaded dynamically: assume DXGI (covers DirectX 11 and 12)
        for (auto& u : undetected)
            if (Lower(u.name).find("-shipping.exe") != std::string::npos || exes.empty()) {
                u.api = {"dxgi", "DirectX 11/12"};
                u.via = "assumed";
                u.detected = true;
                exes.push_back(u);
                break;
            }
    }
    json cands = json::array();
    for (auto& e : exes)
        cands.push_back({{"path", U8(e.path)},
                         {"rel", U8(fs::relative(e.path, dir))},
                         {"name", e.name},
                         {"bits", e.bits},
                         {"api", e.api.api},
                         {"apiLabel", e.api.label},
                         {"via", e.via},
                         {"sizeMB", (double)e.size / 1048576.0}});
    json r{{"dir", U8(dir)},
           {"name", U8(dir.filename())},
           {"candidates", cands},
           {"upscalers", upscalers},
           {"antiCheat", antiCheat},
           {"unreal", unreal},
           {"reshadeCached", ReShadeVersion()},
           {"gpus", GpuInfo()},
           {"dlss5Custom", Exists(CustomNr()) ? FileVersion(CustomNr()) : ""}};
    if (!exes.empty()) {
        r["exe"] = cands[0];
        r["status"] = Status(dir, exes[0].path.parent_path());
    } else {
        r["exe"] = nullptr;
        r["status"] = json::object();
    }
    // An existing installation decides which executable folder is shown.
    for (auto& e : exes) {
        if (Exists(e.path.parent_path() / L"_MoonUp_Upgraph" / L"manifest.json")) {
            for (auto& c : cands)
                if (c["path"] == U8(e.path)) r["exe"] = c;
            r["status"] = Status(dir, e.path.parent_path());
            break;
        }
    }
    return r;
}

namespace {
json Status(const fs::path& gameDir, const fs::path& exeDir) {
    json s{{"installed", false}};
    Journal j;
    j.Load(exeDir);
    if (Exists(j.dir / L"manifest.json")) {
        s["installed"] = true;
        s["manifest"] = j.m;
    }
    // Other graphics hooks present next to the executable.
    json hooks = json::array();
    for (const wchar_t* h : {L"dxgi.dll", L"d3d11.dll", L"d3d12.dll", L"d3d9.dll", L"opengl32.dll", L"winmm.dll", L"version.dll",
                             L"dinput8.dll", L"ReShade64.dll", L"ReShade32.dll", L"renodx-dlss5.addon64", L"dlss5-feed.addon64",
                             L"dlss5-feed.addon32", L"deep-fried-chicken.addon64", L"nvngx_dlssnr.dll"}) {
        fs::path p = exeDir / h;
        if (!Exists(p)) continue;
        std::string vi = VersionInfo(p);
        std::string fn = Lower(Utf8(h));
        std::string kind = fn.find("renodx") != std::string::npos               ? "RenoDX DLSS 5"
                           : fn.find("dlss5-feed") != std::string::npos         ? "DLSS5-Feeder"
                           : fn.find("deep-fried") != std::string::npos         ? "Deep Fried Chicken"
                           : fn == "nvngx_dlssnr.dll"                           ? "DLSS 5 model"
                           : Lower(vi).find("reshade") != std::string::npos      ? "ReShade"
                           : Lower(vi).find("optiscaler") != std::string::npos ? "OptiScaler"
                           : Lower(vi).find("dxvk") != std::string::npos       ? "DXVK"
                                                                                  : "other";
        hooks.push_back({{"file", Utf8(h)}, {"kind", kind}, {"version", FileVersion(p)}, {"ours", j.Added(p)}});
    }
    s["hooks"] = hooks;
    (void)gameDir;
    return s;
}
}  // namespace

json Install(const std::wstring& dirIn, const json& o, const Progress& progress) {
    fs::path dir = dirIn;
    json scan = ScanGame(dirIn);
    // executable: the one chosen in the UI, else the detected one
    fs::path exe;
    std::string api;
    if (o.contains("exe") && o["exe"].is_string() && !o["exe"].get<std::string>().empty()) {
        exe = Wide(o["exe"].get<std::string>());
        if (exe.is_relative()) exe = dir / exe;
        for (auto& c : scan["candidates"])
            if (LowerW(Wide(c["path"].get<std::string>())) == LowerW(exe.wstring())) api = c["api"];
    } else if (scan["exe"].is_object()) {
        exe = Wide(scan["exe"]["path"].get<std::string>());
        api = scan["exe"]["api"];
    }
    if (o.contains("api") && o["api"].is_string() && !o["api"].get<std::string>().empty()) api = o["api"];
    if (exe.empty() || !Exists(exe)) throw Fail("no_exe", "no game executable found");
    if (scan.value("antiCheat", false) && !o.value("acceptAntiCheat", false))
        throw Fail("anticheat", "this game uses anti-cheat; confirm the risk first");
    if (ExeRunning(exe)) throw Fail("running", "close the game first");
    bool wantShader = o.value("shader", true);
    bool wantOpti = o.value("optiscaler", false);
    bool wantDlss5 = o.value("dlss5", false);
    bool frameGen = o.value("frameGen", true);
    int bits = ReadPe(exe).bits;
    if (wantOpti && wantDlss5)
        throw Fail("dlss5_opti", "DLSS 5 and OptiScaler FSR 3.1 cannot be installed together (OptiScaler takes the DLSS calls)");
    if (wantOpti && bits != 64) throw Fail("opti_32bit", "OptiScaler supports 64-bit games only");
    if (wantOpti && api != "dxgi" && api != "vulkan") throw Fail("opti_api", "OptiScaler supports DirectX 11/12 and Vulkan");

    // ---- DLSS 5 route planning
    bool gameHasDlss = false;
    for (auto& u : scan["upscalers"])
        if (u.value("kind", "") == "DLSS") gameHasDlss = true;
    bool feeder = false, useHost = false, dgv = false, isGL = api == "opengl";
    if (wantDlss5) {
        json gpus = GpuInfo();
        bool rtx50 = false, rtx = false;
        for (auto& g : gpus) {
            rtx50 |= g["tier"] == "rtx50";
            rtx |= g["tier"] == "rtx50" || g["tier"] == "rtx";
        }
        if (!rtx && !o.value("ignoreGpu", false)) throw Fail("dlss5_gpu", "DLSS 5 needs an NVIDIA GeForce RTX graphics card");
        // RTX 20/30/40: the modified runtime from RHI (310.8.Lecram) is reported by its author to run on
        // older series on the ReShade/Feeder routes (DLSS5-Swapper README: "RTX 20 / 30 / 40 / 50").
        // A GPU-specific build the user picked takes precedence.
        (void)rtx50;
        if (api == "vulkan") throw Fail("dlss5_vulkan", "Vulkan games need DLSS5-Feeder's own installer (machine-wide ReShade layer)");
        if (api != "dxgi" && api != "d3d10" && api != "opengl" && api != "d3d9" && api != "d3d8")
            throw Fail("api", "graphics API not supported for DLSS 5: " + api);
        // A 64-bit DirectX 10/11/12 game with DLSS of its own: the RenoDX add-on hooks the game's own
        // DLSS calls. Anything else: DLSS5-Feeder makes the DLSS calls itself from ReShade's frame.
        feeder = !(bits == 64 && (api == "dxgi" || api == "d3d10") && gameHasDlss) || o.value("forceFeeder", false);
        useHost = bits == 32;
        dgv = api == "d3d9" || api == "d3d8";
        if (ReShadeBanned(U8(exe.filename()))) throw Fail("reshade_banned", "ReShade's compatibility list says this game bans ReShade");
    } else if (wantShader && !HookFor(api)) {
        if (api == "vulkan") throw Fail("vulkan", "Vulkan games need ReShade's own setup (global Vulkan layer)");
        throw Fail("api", "graphics API not supported by ReShade from here: " + api);
    }
    bool useReShade = wantShader || wantDlss5;
    bool addonBuild = wantDlss5;

    fs::path exeDir = exe.parent_path();
    Journal j;
    j.Load(exeDir);
    if (Exists(j.dir / L"manifest.json")) {
        // reinstall: undo first so backups stay correct
        Remove(dirIn);
        j = Journal();
        j.Load(exeDir);
    }
    j.m["version"] = 1;
    j.m["exe"] = U8(exe.filename());
    j.m["api"] = api;
    j.m["bits"] = bits;
    j.m["installedAt"] = (int64_t)time(nullptr);
    std::error_code ec;
    fs::create_directories(j.dir, ec);
    j.Save();

    try {
        std::string optiVersion;
        fs::path optiDir;
        Dlss5Parts d5;
        if (wantOpti) optiDir = EnsureOptiScaler(progress, optiVersion);
        if (useReShade) EnsureReShade(progress, addonBuild);
        if (wantDlss5) {
            bool needDlss = feeder || !Exists(exeDir / L"nvngx_dlss.dll");
            EnsureDlss5(d5, feeder, needDlss, dgv, progress);
        }
        progress("install", 0.5);

        // ---- OptiScaler (FSR 3.1 upscaling + frame generation for games with DLSS / FSR 2+ / XeSS)
        std::string optiHook;
        if (wantOpti) {
            optiHook = api == "vulkan" ? "winmm.dll" : "dxgi.dll";
            for (auto it = fs::recursive_directory_iterator(optiDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file()) continue;
                fs::path rel = fs::relative(it->path(), optiDir);
                std::string ln = Lower(U8(rel.filename()));
                if (ln.size() > 4 && (ln.substr(ln.size() - 4) == ".bat" || ln.substr(ln.size() - 3) == ".sh" || ln.find("readme") != std::string::npos))
                    continue;
                fs::path dest = exeDir / rel;
                if (ln == "optiscaler.dll") dest = exeDir / Wide(optiHook);
                j.Copy(it->path(), dest);
            }
            j.Save();
            // configuration: FSR 3.1 for every API, FSR frame generation through OptiFG
            fs::path ini = exeDir / L"OptiScaler.ini";
            std::string t;
            ReadAll(ini, t);
            t = IniSet(t, "Upscalers", "Dx12Upscaler", "fsr31");
            t = IniSet(t, "Upscalers", "Dx11Upscaler", "fsr31_12");
            t = IniSet(t, "Upscalers", "VulkanUpscaler", "fsr31");
            t = IniSet(t, "FrameGen", "Enabled", frameGen ? "true" : "false");
            t = IniSet(t, "FrameGen", "FGInput", frameGen ? "upscaler" : "nofg");
            t = IniSet(t, "FrameGen", "FGOutput", frameGen ? "fsrfg" : "nofg");
            t = IniSet(t, "OptiFG", "HUDFix", "true");
            t = IniSet(t, "Menu", "ShortcutKey", "0x2D");  // Insert
            if (wantShader) t = IniSet(t, "Plugins", "LoadReshade", "true");
            j.Write(ini, t.data(), t.size());
            j.m["optiscaler"] = {{"version", optiVersion}, {"hook", optiHook}, {"frameGen", frameGen}};
            j.Save();
        }

        // ---- DLSS 5 (NVIDIA neural rendering through ReShade add-ons)
        if (wantDlss5) {
            fs::path consumerDir = useHost ? exeDir / L"host64" : exeDir;
            fs::path shaders = exeDir / L"reshade-shaders" / L"Shaders";
            fs::path textures = exeDir / L"reshade-shaders" / L"Textures";
            // exactly one neural consumer: every other one (and a stock OptiScaler) is set aside
            for (const fs::path& d : {exeDir, consumerDir}) {
                for (auto& e : fs::directory_iterator(d, ec))
                    if (e.is_regular_file(ec) && IsOtherConsumer(e.path())) j.Protect(e.path());
            }
            // a ReShade already installed under another name would load twice
            for (const wchar_t* n : {L"d3d11.dll", L"d3d12.dll", L"d3d9.dll", L"dinput8.dll", L"opengl32.dll", L"dxgi.dll"}) {
                fs::path p = exeDir / n;
                if (Exists(p) && Lower(VersionInfo(p)).find("reshade") != std::string::npos) j.Protect(p);
            }
            // DirectX 8/9: dgVoodoo2 turns the game into a DirectX 11 one (ReShade then goes in as dxgi.dll)
            if (dgv) {
                const wchar_t* wrap = api == "d3d8" ? L"D3D8.dll" : L"D3D9.dll";
                fs::path src = FindFile(d5.dgvDir / L"MS" / (bits == 64 ? L"x64" : L"x86"), Lower(Utf8(wrap)));
                if (src.empty()) throw Fail("dlss5_download", "dgVoodoo2 archive incomplete");
                j.Copy(src, exeDir / wrap);
                std::string conf;
                ReadAll(d5.dgvDir / L"dgVoodoo.conf", conf);
                conf = IniSet(conf, "General", "OutputAPI", "d3d11_fl11_0");
                conf = IniSet(conf, "DirectX", "DisableAndPassThru", "false");
                conf = IniSet(conf, "DirectX", "VideoCard", "internal3D");
                conf = IniSet(conf, "DirectX", "VRAM", "1GB");
                conf = IniSet(conf, "DirectX", "dgVoodooWatermark", "false");
                j.Write(exeDir / L"dgVoodoo.conf", conf.data(), conf.size());
                fs::path cpl = d5.dgvDir / L"dgVoodooCpl.exe";
                if (Exists(cpl)) j.Copy(cpl, exeDir / L"dgVoodooCpl.exe");
            }
            if (feeder) {
                const char* addon = bits == 32 ? "dlss5-feed.addon32" : "dlss5-feed.addon64";
                fs::path a = FindFile(d5.feederDir, addon), fx = FindFile(d5.feederDir, "dlss5_feed.fx");
                if (a.empty() || fx.empty()) throw Fail("dlss5_download", "DLSS5-Feeder release incomplete");
                j.Copy(a, exeDir / addon);
                j.Copy(fx, shaders / L"DLSS5_Feed.fx");
                fs::path verify = FindFile(d5.feederDir, "verify-dlss5feeder.ps1");
                if (!verify.empty()) j.Copy(verify, exeDir / L"Verify-DLSS5Feeder.ps1");
                if (useHost) {
                    fs::path host = FindFile(d5.feederDir, "dlss5-feed-host64.exe");
                    if (host.empty()) throw Fail("dlss5_download", "dlss5-feed-host64.exe missing in the release");
                    j.Copy(host, consumerDir / L"dlss5-feed-host64.exe");
                }
                for (const char* h : {"ReShade.fxh", "ReShadeUI.fxh", "DrawText.fxh"})
                    if (!Exists(shaders / h)) j.Copy(d5.headersDir / h, shaders / h);
                // LumeniteFX: Shaders/lumenite_*.fx, Shaders/include/*.fxh, Textures/*
                int n = 0;
                for (auto it = fs::recursive_directory_iterator(d5.lumeniteDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec) break;
                    if (!it->is_regular_file(ec)) continue;
                    std::string rel = Lower(U8(fs::relative(it->path(), d5.lumeniteDir)));
                    for (auto& c : rel)
                        if (c == '\\') c = '/';
                    std::smatch m;
                    if (std::regex_search(rel, m, std::regex("(^|/)shaders/(lumenite_[^/]+\\.fx)$")))
                        j.Copy(it->path(), shaders / it->path().filename()), n++;
                    else if (std::regex_search(rel, m, std::regex("(^|/)shaders/include/([^/]+\\.fxh)$")))
                        j.Copy(it->path(), shaders / L"include" / it->path().filename()), n++;
                    else if (std::regex_search(rel, m, std::regex("(^|/)textures/([^/]+)$")))
                        j.Copy(it->path(), textures / it->path().filename()), n++;
                }
                if (!n) throw Fail("dlss5_download", "LumeniteFX archive incomplete");
            }
            // neural consumer and NVIDIA runtimes where the 64-bit NGX code runs
            j.Copy(d5.renodx, consumerDir / L"renodx-dlss5.addon64");
            j.Copy(d5.dlssnr, consumerDir / L"nvngx_dlssnr.dll");
            if (!d5.dlss.empty()) j.Copy(d5.dlss, consumerDir / L"nvngx_dlss.dll");
            if (useHost) {
                j.Copy(ReShadeDir(true) / L"ReShade64.dll", consumerDir / L"dxgi.dll");
                std::string hostIni = "[GENERAL]\r\nEffectSearchPaths=.\\\r\nTextureSearchPaths=.\\\r\n";
                j.Write(consumerDir / L"ReShade.ini", hostIni.data(), hostIni.size());
            }
            // an old d3dcompiler_47.dll (6.3.x) next to the game cannot compile the feed shader
            for (const fs::path& d : {exeDir, consumerDir}) {
                fs::path dc = d / L"d3dcompiler_47.dll";
                if (Exists(dc) && FileVersion(dc).rfind("6.3.", 0) == 0) j.Protect(dc);
            }
            j.Record(exeDir / L"dlss5-feed.log");
            j.m["dlss5"] = {{"route", feeder ? "feeder" : "native"},
                            {"host64", useHost},
                            {"dgVoodoo", dgv},
                            {"feeder", d5.feederVersion},
                            {"consumer", d5.renodxVersion},
                            {"model", d5.dlssnrVersion},
                            {"runtime", d5.dlssVersion},
                            {"customModel", d5.customNr}};
            j.Save();
        }

        // ---- ReShade (+ MoonUp Upgraph effect)
        if (useReShade) {
            fs::path dll = ReShadeDir(addonBuild) / (bits == 64 ? L"ReShade64.dll" : L"ReShade32.dll");
            std::wstring hook;
            if (wantOpti && optiHook == "dxgi.dll")
                hook = L"ReShade64.dll";  // OptiScaler in dxgi.dll loads ReShade from ReShade64.dll
            else if (wantDlss5)
                hook = isGL ? L"opengl32.dll" : L"dxgi.dll";  // dgVoodoo games are DirectX 11 behind it
            else
                hook = Wide(HookFor(api));
            j.Copy(dll, exeDir / hook);
            std::string ini = ReShadeIni(scan.value("unreal", false), wantDlss5, dgv);
            j.Write(exeDir / L"ReShade.ini", ini.data(), ini.size());
            std::string preset = PresetText(o.value("preset", json::object()), wantShader, wantDlss5 && feeder);
            j.Write(exeDir / L"MoonUpUpgraph.ini", preset.data(), preset.size());
            if (wantShader) {
                j.Write(exeDir / L"moonup-upgraph" / L"Shaders" / L"MoonUpUpgraph.fx", upgraph_assets::kFx, sizeof(upgraph_assets::kFx));
                j.Write(exeDir / L"moonup-upgraph" / L"Textures" / L"moonup_upgraph_weights.png", upgraph_assets::kWeightsPng,
                        sizeof(upgraph_assets::kWeightsPng));
            }
            // ReShade writes its log and cache here; recorded so removal cleans them up.
            j.Record(exeDir / L"ReShade.log");
            j.Record(exeDir / L"moonup-upgraph");
            j.m["reshade"] = {{"version", ReShadeVersion(addonBuild)}, {"hook", Utf8(hook)}, {"addon", addonBuild}};
            j.m["shader"] = wantShader;
        }
        j.Save();
    } catch (...) {
        // leave the game as it was
        try {
            Remove(dirIn);
        } catch (...) {
        }
        throw;
    }
    progress("done", 1);
    SW_LOG("Upgraph: installed into %s (api %s, shader %d, optiscaler %d, dlss5 %d%s)", U8(exeDir).c_str(), api.c_str(), (int)wantShader,
           (int)wantOpti, (int)wantDlss5, wantDlss5 ? (feeder ? " feeder" : " native") : "");
    return ScanGame(dirIn);
}

// Copies the user's GPU-specific nvngx_dlssnr.dll (RTX 20/30/40) into the cache.
json UseDlssNr(const std::wstring& file) {
    if (Lower(VersionInfo(file)).find("nvidia") == std::string::npos && Lower(U8(fs::path(file).filename())).find("dlssnr") == std::string::npos)
        throw Fail("dlss5_file", "this is not an nvngx_dlssnr.dll");
    if (ReadPe(file).bits != 64) throw Fail("dlss5_file", "nvngx_dlssnr.dll must be the 64-bit file");
    std::error_code ec;
    fs::create_directories(CustomNr().parent_path(), ec);
    fs::copy_file(file, CustomNr(), fs::copy_options::overwrite_existing, ec);
    if (ec) throw Fail("disk", "cannot copy the file");
    return {{"version", FileVersion(CustomNr())}};
}

json ClearDlssNr() {
    std::error_code ec;
    fs::remove(CustomNr(), ec);
    return json::object();
}

json Remove(const std::wstring& dirIn) {
    fs::path dir = dirIn;
    std::error_code ec;
    // every executable folder of this game that has a manifest
    std::vector<fs::path> roots;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_directory(ec)) continue;
        std::string n = Lower(U8(it->path().filename()));
        if (n == "_moonup_upgraph") {
            roots.push_back(it->path().parent_path());
            it.disable_recursion_pending();
        } else if (it.depth() >= 7 || (kSkipDirs.count(n) && n != "engine")) {
            it.disable_recursion_pending();
        }
    }
    int removed = 0, restored = 0;
    for (auto& exeDir : roots) {
        Journal j;
        j.Load(exeDir);
        if (ExeRunning(exeDir / Wide(j.m.value("exe", std::string("x.exe"))))) throw Fail("running", "close the game first");
        // added files (deepest first), then directories that became empty
        std::vector<fs::path> added;
        for (auto& a : j.m["added"]) added.push_back(exeDir / Wide(a.get<std::string>()));
        std::sort(added.begin(), added.end(), [](const fs::path& a, const fs::path& b) { return a.wstring().size() > b.wstring().size(); });
        for (auto& p : added) {
            if (fs::is_directory(p, ec))
                removed += (int)fs::remove_all(p, ec) > 0;
            else if (fs::remove(p, ec))
                removed++;
            // empty parent folders that we created (e.g. OptiScaler's subfolders)
            for (fs::path q = p.parent_path(); q != exeDir && q.wstring().size() > exeDir.wstring().size(); q = q.parent_path())
                if (fs::is_empty(q, ec)) fs::remove(q, ec);
        }
        for (auto& b : j.m["backups"]) {
            fs::path dest = exeDir / Wide(b.get<std::string>());
            fs::path src = j.dir / L"backup" / Wide(b.get<std::string>());
            if (!Exists(src)) continue;
            fs::remove(dest, ec);
            fs::rename(src, dest, ec);
            if (ec) fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
            if (!ec) restored++;
        }
        fs::remove_all(j.dir, ec);
        IndexSet(exeDir, false);
        SW_LOG("Upgraph: removed from %s (%d files removed, %d restored)", U8(exeDir).c_str(), removed, restored);
    }
    json r = ScanGame(dirIn);
    r["removed"] = removed;
    r["restored"] = restored;
    return r;
}

json UpdatePreset(const std::wstring& dirIn, const json& preset) {
    json scan = ScanGame(dirIn);
    if (!scan["status"].value("installed", false) || !scan["exe"].is_object()) throw Fail("not_installed", "Upgraph is not installed");
    fs::path exeDir = fs::path(Wide(scan["exe"]["path"].get<std::string>())).parent_path();
    std::string t = PresetText(preset);
    if (!WriteAll(exeDir / L"MoonUpUpgraph.ini", t.data(), t.size())) throw Fail("disk", "cannot write preset");
    return scan;
}

json UseReShadeSetup(const std::wstring& setupExe) {
    bool addon = LowerW(fs::path(setupExe).filename().wstring()).find(L"addon") != std::wstring::npos;
    ExtractReShadeSetup(setupExe, ReShadeDir(addon));
    // The add-on build also serves MoonUp Upgraph alone.
    if (addon && !Exists(ReShadeDir(false) / L"ReShade64.dll")) ExtractReShadeSetup(setupExe, ReShadeDir(false));
    return {{"version", ReShadeVersion(addon)}, {"addon", addon}};
}

std::string PosterDataUrl(const std::wstring& file) {
    std::string data;
    if (file.empty() || !ReadAll(file, data, 2u << 20)) return {};
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out = LowerW(fs::path(file).extension().wstring()) == L".png" ? "data:image/png;base64," : "data:image/jpeg;base64,";
    out.reserve(out.size() + data.size() * 4 / 3 + 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = ((uint8_t)data[i] << 16) | ((uint8_t)data[i + 1] << 8) | (uint8_t)data[i + 2];
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i < data.size()) {
        uint32_t v = (uint8_t)data[i] << 16;
        if (i + 1 < data.size()) v |= (uint8_t)data[i + 1] << 8;
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < data.size() ? tbl[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// Error code of an exception thrown by this module ("" for anything else).
std::string ErrorCode(const std::exception& e) {
    auto* f = dynamic_cast<const Fail*>(&e);
    return f ? f->code : "";
}

}  // namespace sw::upgraph
