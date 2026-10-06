// Runs the engine in a separate MoonUp process ("--engine-host"), so a driver crash or
// a fault anywhere in capture / processing / presentation can never take the app down.
// The app keeps working, reports the failure and the user can simply start again.
// Falls back to the in-process engine when the host process cannot be started.
#pragma once
#include "common.h"
#include "engine/engine.h"
#include "third_party/json.hpp"

namespace sw {

constexpr UINT WM_APP_ENGINE = WM_APP + 1;       // wParam: session, lParam: new std::string (JSON event)
constexpr UINT WM_APP_ENGINE_EXIT = WM_APP + 7;  // wParam: process id, lParam: exit code

// WM_COPYDATA dwData values used between the app and the engine host.
constexpr ULONG_PTR kCopyShowInstance = 1;
constexpr ULONG_PTR kCopyEngineEvent = 2;
constexpr ULONG_PTR kCopyEngineCommand = 3;
constexpr ULONG_PTR kCopyHostReady = 4;

class EngineClient {
public:
    ~EngineClient() { Stop(); }
    // notify: window that receives WM_APP_ENGINE / WM_APP_ENGINE_EXIT.
    bool Start(HWND notify, HWND target, const nlohmann::json& profile);
    void Stop();
    bool Running() const;
    void UpdateConfig(const nlohmann::json& profile);
    bool NeedsRestart(const nlohmann::json& profile) const;

    // App hooks. OnCopyData returns true when the message belonged to the engine host.
    bool OnCopyData(HWND sender, const COPYDATASTRUCT* cds);
    void OnProcessExit(DWORD pid, DWORD code);
    DWORD HostPid() const { return pi_.dwProcessId; }
    // Events carry the session they belong to; events of an earlier session are stale.
    UINT Session() const { return session_; }

private:
    void Send(const nlohmann::json& cmd);
    void Cleanup();
    HWND notify_ = nullptr;
    PROCESS_INFORMATION pi_{};
    HANDLE wait_ = nullptr;
    HWND host_ = nullptr;
    bool sawStopped_ = false;
    bool stopping_ = false;
    std::vector<std::string> pending_;
    EngineConfig startCfg_;
    bool inProcess_ = false;
    UINT session_ = 0;
    Engine local_;
};

// Entry point of the engine host process.
int EngineHostMain(const std::wstring& startFile);

}  // namespace sw
