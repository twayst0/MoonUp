// MoonUp entry point.
#include <objbase.h>
#include <shellapi.h>

#include "app.h"
#include "engine_client.h"

#include <dbghelp.h>
#include <exception>

static const wchar_t* g_dumpName = L"\\crash.dmp";

// Crash diagnostics: logs the faulting address (as a module offset) and writes a minidump.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    ClipCursor(nullptr);
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &mod);
    wchar_t modName[MAX_PATH] = L"?";
    if (mod) GetModuleFileNameW(mod, modName, MAX_PATH);
    SW_LOG("CRASH: code 0x%08lX at %p (%s + 0x%llx), thread %lu", ep->ExceptionRecord->ExceptionCode, addr,
           sw::Utf8(modName).c_str(), (unsigned long long)((char*)addr - (char*)mod), GetCurrentThreadId());
    if (HMODULE dbg = LoadLibraryW(L"dbghelp.dll")) {
        typedef BOOL(WINAPI * Fn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PVOID, PVOID);
        if (auto fn = (Fn)GetProcAddress(dbg, "MiniDumpWriteDump")) {
            std::wstring path = sw::CacheDir() + g_dumpName;
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
                fn(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &mei, nullptr, nullptr);
                CloseHandle(f);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool minimized = false;
    std::wstring engineHost;
    for (int i = 1; argv && i < argc; i++) {
        if (wcscmp(argv[i], L"--minimized") == 0) minimized = true;
        if (wcscmp(argv[i], L"--engine-host") == 0 && i + 1 < argc) engineHost = argv[i + 1];
    }
    if (argv) LocalFree(argv);

    // Engine host process: runs capture, processing and output for the app (see engine_client.h).
    if (!engineHost.empty()) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        sw::LogInit(L"moonup-engine");
        g_dumpName = L"\\engine-crash.dmp";
        SetUnhandledExceptionFilter(CrashFilter);
        std::set_terminate([] {
            SW_LOG("CRASH: std::terminate (uncaught C++ exception)");
            ClipCursor(nullptr);
            abort();
        });
        SW_LOG("MoonUp engine host starting");
        int rc = sw::EngineHostMain(engineHost);
        ClipCursor(nullptr);
        return rc;
    }

    // Single instance: bring the running window to the front instead.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MoonUpSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"MoonUpMain", nullptr);
        if (existing) {
            COPYDATASTRUCT cds{1, 0, nullptr};
            SendMessageW(existing, WM_COPYDATA, 0, (LPARAM)&cds);
        }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    sw::LogInit();
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate([] {
        SW_LOG("CRASH: std::terminate (uncaught C++ exception)");
        ClipCursor(nullptr);
        abort();
    });
    SW_LOG("MoonUp %s starting", sw::kVersion);

    sw::App app;
    int rc = app.Run(inst, minimized);

    // Safety net: never leave the system cursor hidden or clipped.
    ClipCursor(nullptr);
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return rc;
}
