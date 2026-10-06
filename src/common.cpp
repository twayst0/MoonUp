#include "common.h"

#include <shlobj.h>
#include <timeapi.h>
#include <cstdarg>
#include <ctime>

namespace sw {

std::string Utf8(const std::wstring& w) { return Utf8(w.c_str()); }

std::string Utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string HrToString(HRESULT hr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
    return buf;
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

static std::wstring KnownDir(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        out = std::wstring(raw) + L"\\MoonUp";
        CoTaskMemFree(raw);
    } else {
        out = ExeDir() + L"\\data";
    }
    CreateDirectoryW(out.c_str(), nullptr);
    return out;
}

std::wstring DataDir() {
    static std::wstring d = KnownDir(FOLDERID_RoamingAppData);
    return d;
}

std::wstring CacheDir() {
    static std::wstring d = KnownDir(FOLDERID_LocalAppData);
    return d;
}

static int64_t QpcFreq() {
    static int64_t f = [] {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        return (int64_t)li.QuadPart;
    }();
    return f;
}

int64_t QpcNow() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

double QpcToSeconds(int64_t ticks) { return (double)ticks / (double)QpcFreq(); }
double NowSeconds() { return QpcToSeconds(QpcNow()); }

// ---------------------------------------------------------------- logging
static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;

// Logs live next to the program (logs\ folder) so they are easy to find and send; the user data
// folder is the fallback when the program folder is not writable.
std::wstring LogDir() {
    static std::wstring d = [] {
        std::wstring p = ExeDir() + L"\\logs";
        CreateDirectoryW(p.c_str(), nullptr);
        std::wstring probe = p + L"\\.write_test";
        HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return p;
        }
        return CacheDir();
    }();
    return d;
}

void LogInit(const wchar_t* name) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) return;
    std::wstring path = LogDir() + L"\\" + name + L".log";
    // Keep one previous log for diagnostics.
    std::wstring old = LogDir() + L"\\" + name + L".previous.log";
    MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_logFile = _wfopen(path.c_str(), L"w");
}

void Log(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, msg);
    OutputDebugStringA(line);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        fputs(line, g_logFile);
        fflush(g_logFile);
    }
}

bool Check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    Log("FAILED %s: %s", what, HrToString(hr).c_str());
    return false;
}

TimerResolution::TimerResolution() { timeBeginPeriod(1); }
TimerResolution::~TimerResolution() { timeEndPeriod(1); }

void PreciseSleepUntil(double target) {
    static thread_local HANDLE timer = [] {
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x2 (Windows 10 1803+), fall back otherwise.
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        return h;
    }();
    for (;;) {
        double remaining = target - NowSeconds();
        if (remaining <= 0) return;
        // The high resolution waitable timer is accurate to ~0.5 ms: sleep, then spin only for
        // the last fraction (never burn a whole core: the game needs the CPU).
        if (remaining > 0.0006 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((remaining - 0.0004) * 1e7);
            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
                WaitForSingleObject(timer, 50);
                continue;
            }
        }
        if (remaining > 0.0002) {
            SwitchToThread();
        } else {
            YieldProcessor();
        }
    }
}

}  // namespace sw
