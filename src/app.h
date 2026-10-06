// MoonUp application shell: main window, WebView UI bridge, tray icon, hotkeys and engine control.
#pragma once
#include "common.h"
#include "engine_client.h"
#include "fps_monitor.h"
#include "settings.h"
#include "splash.h"
#include "webview.h"

namespace sw {

class App {
public:
    int Run(HINSTANCE inst, bool startMinimized);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    bool CreateMainWindow();
    void OnWebReady(bool ok, HRESULT hr);
    void OnWebMessage(const std::wstring& msg);
    void Reply(const json& id, bool ok, const json& data);
    void Emit(const json& event);

    void HandleCommand(const json& id, const std::string& cmd, const json& args);
    void HandleUpgraph(const json& id, const std::string& cmd, const json& args);
    void StartScaling(HWND target);
    void StopScaling();
    void BeginCountdown(int seconds);
    void ApplyLiveSettings();
    void UpdateFpsMonitor();
    // Render resolution: shrinks the game's window so it renders fewer pixels (more fps) and
    // MoonUp scales it back up to the monitor. The original placement is restored afterwards.
    void ApplyRenderScale(HWND target, int percent);
    void RestoreRenderScale();
    // Render scale actually used: the profile's, or derived from a custom factor when the game
    // already fills its monitor (there is nothing to upscale otherwise).
    int EffectiveRenderScale(const json& profile, HWND target);
    void CheckRenderScale();
    void ApplyHotkey();
    void SetAutostart(bool enabled);
    void ShowMain();
    void HideMain();
    void Quit();

    void TrayAdd();
    void TrayRemove();
    void TrayMenu();
    void TrayTooltip(const std::wstring& text);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    WebView web_;
    Splash splash_;
    EngineClient engine_;
    FpsMonitor fpsMon_;
    bool engineOverlay_ = false;
    HWND resized_ = nullptr;
    int resizedPct_ = 0;
    WINDOWPLACEMENT resizedPlacement_{};
    int expectW_ = 0, expectH_ = 0;
    json settings_;
    bool startMinimized_ = false;
    bool uiReady_ = false;
    bool quitting_ = false;
    int countdown_ = 0;
    HWND scalingTarget_ = nullptr;
    std::string scalingProfile_;
    UINT taskbarCreatedMsg_ = 0;
    std::wstring trayOpen_ = L"Open MoonUp", trayStop_ = L"Stop scaling", trayQuit_ = L"Quit";
};

}  // namespace sw
