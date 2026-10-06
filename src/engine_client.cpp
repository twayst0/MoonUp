#include "engine_client.h"

#include "settings.h"

namespace sw {

using json = nlohmann::json;

namespace {

void PostEvent(HWND notify, UINT session, const json& j) {
    PostMessageW(notify, WM_APP_ENGINE, session, (LPARAM) new std::string(j.dump()));
}

struct ExitWaitCtx {
    HWND notify;
    DWORD pid;
    HANDLE process;
};

VOID CALLBACK OnExitWait(PVOID param, BOOLEAN) {
    auto* c = (ExitWaitCtx*)param;
    DWORD code = 0;
    GetExitCodeProcess(c->process, &code);
    PostMessageW(c->notify, WM_APP_ENGINE_EXIT, (WPARAM)c->pid, (LPARAM)code);
}

ExitWaitCtx g_waitCtx;

bool SendJson(HWND to, HWND from, ULONG_PTR kind, const std::string& s, UINT timeoutMs) {
    COPYDATASTRUCT cds{kind, (DWORD)s.size(), (PVOID)s.data()};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(to, WM_COPYDATA, (WPARAM)from, (LPARAM)&cds, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, timeoutMs,
                               &result) != 0;
}

}  // namespace

bool EngineClient::Start(HWND notify, HWND target, const json& profile) {
    Stop();
    session_++;
    notify_ = notify;
    startCfg_ = ProfileToConfig(profile);
    sawStopped_ = false;
    stopping_ = false;
    host_ = nullptr;
    pending_.clear();

    // Hand the start parameters over in a file (no quoting issues, any size).
    std::wstring startFile = CacheDir() + L"\\engine_start.json";
    json start = {{"parent", (uint64_t)(uintptr_t)notify}, {"target", (uint64_t)(uintptr_t)target}, {"profile", profile}};
    bool written = false;
    if (FILE* f = _wfopen(startFile.c_str(), L"wb")) {
        std::string s = start.dump();
        written = fwrite(s.data(), 1, s.size(), f) == s.size();
        fclose(f);
    }

    std::wstring exe = ExeDir() + L"\\MoonUp.exe";
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(nullptr, self, MAX_PATH)) exe = self;
    std::wstring cmd = L"\"" + exe + L"\" --engine-host \"" + startFile + L"\"";
    STARTUPINFOW si{sizeof(si)};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (written && CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi_)) {
        inProcess_ = false;
        g_waitCtx = {notify, pi_.dwProcessId, pi_.hProcess};
        if (!RegisterWaitForSingleObject(&wait_, pi_.hProcess, OnExitWait, &g_waitCtx, INFINITE, WT_EXECUTEONLYONCE))
            wait_ = nullptr;
        SW_LOG("Engine host started (pid %lu)", pi_.dwProcessId);
        return true;
    }
    SW_LOG("Engine host could not be started (%lu); running the engine in process", GetLastError());
    inProcess_ = true;
    UINT session = session_;
    return local_.Start(target, startCfg_, [notify, session](const std::string& j) {
        PostMessageW(notify, WM_APP_ENGINE, session, (LPARAM) new std::string(j));
    });
}

void EngineClient::Send(const json& cmd) {
    std::string s = cmd.dump();
    if (!host_) {
        pending_.push_back(s);
        return;
    }
    SendJson(host_, notify_, kCopyEngineCommand, s, 1500);
}

void EngineClient::UpdateConfig(const json& profile) {
    if (inProcess_) {
        local_.UpdateConfig(ProfileToConfig(profile));
        return;
    }
    if (pi_.hProcess) Send({{"cmd", "config"}, {"profile", profile}});
}

bool EngineClient::NeedsRestart(const json& profile) const {
    EngineConfig n = ProfileToConfig(profile);
    const EngineConfig& o = startCfg_;
    return n.capture != o.capture || n.adapterIndex != o.adapterIndex || n.vsync != o.vsync ||
           n.allowTearing != o.allowTearing || n.maxFrameLatency != o.maxFrameLatency;
}

bool EngineClient::Running() const {
    if (inProcess_) return local_.Running();
    return pi_.hProcess && WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT;
}

void EngineClient::Stop() {
    if (inProcess_) {
        local_.Stop();
        inProcess_ = false;
        return;
    }
    if (!pi_.hProcess) return;
    stopping_ = true;
    // If the host has not announced itself yet, the stop is delivered when it does.
    if (host_)
        SendJson(host_, notify_, kCopyEngineCommand, json{{"cmd", "stop"}}.dump(), 1500);
    else
        pending_.push_back(json{{"cmd", "stop"}}.dump());
    // Wait for a clean exit while still answering messages sent to us (the host forwards its
    // final events with SendMessage).
    double deadline = NowSeconds() + 4.0;
    while (WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT && NowSeconds() < deadline) {
        DWORD r = MsgWaitForMultipleObjectsEx(1, &pi_.hProcess, 50, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0 + 1) {
            MSG m;
            PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
    }
    if (WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT) {
        SW_LOG("Engine host did not stop in time, terminating it");
        TerminateProcess(pi_.hProcess, 1);
        WaitForSingleObject(pi_.hProcess, 2000);
    }
    Cleanup();
    ClipCursor(nullptr);
    if (!sawStopped_) PostEvent(notify_, session_, {{"event", "engine"}, {"state", "stopped"}, {"reason", "user"}});
}

void EngineClient::Cleanup() {
    if (wait_) {
        UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE);
        wait_ = nullptr;
    }
    if (pi_.hThread) CloseHandle(pi_.hThread);
    if (pi_.hProcess) CloseHandle(pi_.hProcess);
    pi_ = PROCESS_INFORMATION{};
    host_ = nullptr;
    pending_.clear();
}

bool EngineClient::OnCopyData(HWND sender, const COPYDATASTRUCT* cds) {
    if (cds->dwData != kCopyEngineEvent && cds->dwData != kCopyHostReady) return false;
    if (!pi_.hProcess || !sender) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(sender, &pid);
    if (pid != pi_.dwProcessId) return true;  // not our current host
    if (cds->dwData == kCopyHostReady) {
        host_ = sender;
        for (auto& s : pending_) SendJson(host_, notify_, kCopyEngineCommand, s, 1500);
        pending_.clear();
        return true;
    }
    std::string s((const char*)cds->lpData, cds->cbData);
    if (s.find("\"stopped\"") != std::string::npos) sawStopped_ = true;
    PostMessageW(notify_, WM_APP_ENGINE, session_, (LPARAM) new std::string(s));
    return true;
}

void EngineClient::OnProcessExit(DWORD pid, DWORD code) {
    if (!pi_.hProcess || pid != pi_.dwProcessId) return;  // an older host, already handled
    bool clean = sawStopped_ || stopping_;
    Cleanup();
    ClipCursor(nullptr);
    if (!clean) {
        char hex[32];
        snprintf(hex, sizeof(hex), "0x%08lX", code);
        SW_LOG("Engine host exited unexpectedly (code %s)", hex);
        PostEvent(notify_, session_, {{"event", "engine"}, {"state", "error"}, {"code", "crash"}, {"message", std::string("exit code ") + hex}});
        PostEvent(notify_, session_, {{"event", "engine"}, {"state", "stopped"}, {"reason", "error"}});
    }
}

// ============================================================================ host process
namespace {

constexpr UINT WM_HOST_EVENT = WM_APP + 1;
constexpr UINT_PTR TIMER_PARENT = 1;

struct Host {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    Engine engine;
    bool quitting = false;
};
Host* g_host = nullptr;

LRESULT CALLBACK HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Host* host = g_host;
    switch (msg) {
        case WM_HOST_EVENT: {
            std::unique_ptr<std::string> s((std::string*)lp);
            if (host && IsWindow(host->parent)) SendJson(host->parent, h, kCopyEngineEvent, *s, 2000);
            if (s->find("\"stopped\"") != std::string::npos && host && !host->quitting) {
                host->quitting = true;
                PostQuitMessage(0);
            }
            return 0;
        }
        case WM_COPYDATA: {
            auto* cds = (const COPYDATASTRUCT*)lp;
            if (!host || cds->dwData != kCopyEngineCommand || (HWND)wp != host->parent) return FALSE;
            json cmd = json::parse(std::string((const char*)cds->lpData, cds->cbData), nullptr, false);
            if (cmd.is_discarded()) return FALSE;
            std::string c = cmd.value("cmd", "");
            if (c == "config" && cmd.contains("profile")) {
                host->engine.UpdateConfig(ProfileToConfig(cmd["profile"]));
            } else if (c == "stop") {
                // Stopping joins the engine thread, which posts its final "stopped" event here.
                PostMessageW(h, WM_APP + 2, 0, 0);
            }
            return TRUE;
        }
        case WM_APP + 2:
            if (host) host->engine.Stop();
            return 0;
        case WM_TIMER:
            if (wp == TIMER_PARENT && host && !IsWindow(host->parent)) {
                SW_LOG("Engine host: app window is gone, stopping");
                host->engine.Stop();
                host->quitting = true;
                PostQuitMessage(0);
            }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int EngineHostMain(const std::wstring& startFile) {
    std::string text;
    if (FILE* f = _wfopen(startFile.c_str(), L"rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
    }
    json start = json::parse(text, nullptr, false);
    if (start.is_discarded() || !start.contains("profile")) {
        SW_LOG("Engine host: invalid start file");
        return 2;
    }
    Host host;
    g_host = &host;
    host.parent = (HWND)(uintptr_t)start.value("parent", (uint64_t)0);
    HWND target = (HWND)(uintptr_t)start.value("target", (uint64_t)0);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MoonUpEngineHost";
    RegisterClassExW(&wc);
    host.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!host.hwnd || !IsWindow(host.parent)) {
        SW_LOG("Engine host: no window or no parent");
        return 3;
    }
    // Let the app's main window send us commands (UIPI: both run at the same integrity level).
    ChangeWindowMessageFilterEx(host.hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    SendJson(host.parent, host.hwnd, kCopyHostReady, "{}", 2000);
    SetTimer(host.hwnd, TIMER_PARENT, 1000, nullptr);

    HWND self = host.hwnd;
    SW_LOG("Engine host: starting engine for window %p", (void*)target);
    if (!host.engine.Start(target, ProfileToConfig(start["profile"]), [self](const std::string& j) {
            PostMessageW(self, WM_HOST_EVENT, 0, (LPARAM) new std::string(j));
        })) {
        SendJson(host.parent, host.hwnd, kCopyEngineEvent,
                 json{{"event", "engine"}, {"state", "error"}, {"code", "pick_window"}, {"message", ""}}.dump(), 2000);
        SendJson(host.parent, host.hwnd, kCopyEngineEvent, json{{"event", "engine"}, {"state", "stopped"}, {"reason", "error"}}.dump(), 2000);
        return 4;
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    host.engine.Stop();
    // Forward anything the engine posted while stopping.
    while (PeekMessageW(&m, host.hwnd, WM_HOST_EVENT, WM_HOST_EVENT, PM_REMOVE)) DispatchMessageW(&m);
    DestroyWindow(host.hwnd);
    g_host = nullptr;
    SW_LOG("Engine host: exit");
    return 0;
}

}  // namespace sw
