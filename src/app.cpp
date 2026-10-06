#include "app.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include "engine/d3d.h"
#include "engine/neural_weights.h"
#include <thread>
#include "res/resource.h"
#include "sysinfo.h"
#include "upgraph/upgraph.h"
#include <shobjidl.h>

namespace sw {

namespace {
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_POST = WM_APP + 6;  // lParam: new std::string, posted to the UI as is
constexpr UINT_PTR TIMER_COUNTDOWN = 2;
constexpr UINT_PTR TIMER_SPLASH_FAILSAFE = 3;
constexpr UINT_PTR TIMER_START_TARGET = 4;
constexpr UINT_PTR TIMER_FPS_MONITOR = 5;
constexpr UINT_PTR TIMER_RESIZE_CHECK = 6;
constexpr int HOTKEY_SCALE = 1;
constexpr UINT TRAY_ID = 1;
constexpr int kMinW = 1060, kMinH = 680;

bool IsOwnWindow(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

UINT VkFromKey(const std::string& k) {
    if (k.size() == 1) {
        char c = (char)toupper((unsigned char)k[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (UINT)c;
    }
    if (k.size() >= 2 && (k[0] == 'F' || k[0] == 'f')) {
        int n = atoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (UINT)(n - 1);
    }
    if (k == "Home") return VK_HOME;
    if (k == "End") return VK_END;
    if (k == "Insert") return VK_INSERT;
    if (k == "PageUp") return VK_PRIOR;
    if (k == "PageDown") return VK_NEXT;
    return 0;
}

HWND g_main = nullptr;

// The window's client area covers its whole monitor (borderless / fullscreen-window games).
bool FillsMonitor(HWND h, int* mw = nullptr, int* mh = nullptr) {
    RECT c;
    if (!GetClientRect(h, &c)) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    int w = mi.rcMonitor.right - mi.rcMonitor.left, hh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (mw) *mw = w;
    if (mh) *mh = hh;
    return c.right >= w - 4 && c.bottom >= hh - 4;
}

int RenderScaleOf(const json& p) {
    auto it = p.find("renderScale");
    if (it == p.end()) return 100;
    if (it->is_number()) return it->get<int>();
    if (it->is_string()) return atoi(it->get<std::string>().c_str());
    return 100;
}
}  // namespace

// ============================================================================ startup
int App::Run(HINSTANCE inst, bool startMinimized) {
    inst_ = inst;
    settings_ = LoadSettings();
    startMinimized_ = startMinimized;  // set by the autostart entry (--minimized)
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    if (!CreateMainWindow()) return 1;
    fpsMon_.Start();
    SetTimer(hwnd_, TIMER_FPS_MONITOR, 250, nullptr);

    // Splash: logo centred where the main window will appear.
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    UINT dpi = GetDpiForWindow(hwnd_);
    int logoPx = MulDiv(168, dpi, 96);
    if (!startMinimized_ && settings_.value("intro", true)) splash_.Show((wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2, logoPx);
    SetTimer(hwnd_, TIMER_SPLASH_FAILSAFE, 9000, nullptr);

    bool devTools = GetFileAttributesW((ExeDir() + L"\\devtools.flag").c_str()) != INVALID_FILE_ATTRIBUTES;
    web_.Create(hwnd_, ExeDir() + L"\\ui", CacheDir() + L"\\WebView2",
                [this](const std::wstring& m) { OnWebMessage(m); },
                [this](bool ok, HRESULT hr) { OnWebReady(ok, hr); }, devTools);

    TrayAdd();
    ApplyHotkey();
    // Compile shaders in the background so the first Scale starts instantly.
    std::thread([] { ShaderLibrary::WarmCache(); }).detach();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

bool App::CreateMainWindow() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst_;
    wc.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(7, 8, 11));
    wc.lpszClassName = L"MoonUpMain";
    RegisterClassExW(&wc);

    // Size in DIPs, centred on the primary work area.
    POINT pt{0, 0};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT(WINAPI * Fn)(HMONITOR, int, UINT*, UINT*);
        if (auto fn = (Fn)GetProcAddress(shcore, "GetDpiForMonitor")) fn(mon, 0, &dpiX, &dpiY);
    }
    int w = MulDiv(1240, dpiX, 96), h = MulDiv(800, dpiX, 96);
    RECT work = mi.rcWork;
    w = std::min<int>(w, work.right - work.left - 40);
    h = std::min<int>(h, work.bottom - work.top - 40);
    int x = work.left + (work.right - work.left - w) / 2;
    int y = work.top + (work.bottom - work.top - h) / 2;

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, L"MoonUpMain", L"MoonUp",
                            WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                            x, y, w, h, nullptr, nullptr, inst_, this);
    if (!hwnd_) return false;
    g_main = hwnd_;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    COLORREF border = RGB(38, 41, 48);
    DwmSetWindowAttribute(hwnd_, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    MARGINS m{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd_, &m);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    return true;
}

void App::OnWebReady(bool ok, HRESULT hr) {
    if (ok) return;
    splash_.Destroy();
    SW_LOG("WebView2 failed: %s", HrToString(hr).c_str());
    int r = MessageBoxW(nullptr,
                        L"MoonUp needs the Microsoft Edge WebView2 Runtime for its interface.\n\n"
                        L"It is built into Windows 11 and most Windows 10 installations. "
                        L"Open the download page now?",
                        L"MoonUp", MB_ICONWARNING | MB_YESNO);
    if (r == IDYES)
        ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/microsoft-edge/webview2/", nullptr, nullptr, SW_SHOWNORMAL);
    Quit();
}

void App::ShowMain() {
    if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOW);
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    web_.Focus();
}

void App::HideMain() { ShowWindow(hwnd_, SW_HIDE); }

void App::Quit() {
    if (quitting_) return;
    quitting_ = true;
    KillTimer(hwnd_, TIMER_FPS_MONITOR);
    fpsMon_.Stop();
    engine_.Stop();
    RestoreRenderScale();
    UnregisterHotKey(hwnd_, HOTKEY_SCALE);
    TrayRemove();
    splash_.Destroy();
    web_.Close();
    DestroyWindow(hwnd_);
}

// ============================================================================ bridge
void App::Reply(const json& id, bool ok, const json& data) {
    json r{{"id", id}, {"ok", ok}};
    if (ok)
        r["data"] = data;
    else
        r["error"] = data;
    web_.Post(r.dump());
}

void App::Emit(const json& e) { web_.Post(e.dump()); }

void App::OnWebMessage(const std::wstring& wmsg) {
    json m = json::parse(Utf8(wmsg), nullptr, false);
    if (m.is_discarded() || !m.is_object()) return;
    json id = m.value("id", json());
    std::string cmd = m.value("cmd", "");
    json args = m.value("args", json::object());
    try {
        HandleCommand(id, cmd, args);
    } catch (const std::exception& e) {
        SW_LOG("Command %s failed: %s", cmd.c_str(), e.what());
        Reply(id, false, e.what());
    }
}

void App::HandleCommand(const json& id, const std::string& cmd, const json& args) {
    if (cmd == "init") {
        wchar_t loc[LOCALE_NAME_MAX_LENGTH] = L"en-US";
        GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH);
        json data{{"settings", settings_},
                  {"system", CollectSystemInfo()},
                  {"version", kVersion},
                  {"locale", Utf8(loc)},
                  {"neuralBundled", neural::kWeightCount > 0},
                  {"engine", {{"running", engine_.Running()}, {"target", scalingTarget_ ? WindowTitle(scalingTarget_) : ""}}},
                  {"maximized", IsZoomed(hwnd_) != FALSE},
                  {"startMinimized", startMinimized_}};
        Reply(id, true, data);
    } else if (cmd == "uiReady") {
        KillTimer(hwnd_, TIMER_SPLASH_FAILSAFE);
        if (!uiReady_) {
            uiReady_ = true;
            if (!startMinimized_) {
                ShowWindow(hwnd_, SW_SHOW);
                SetForegroundWindow(hwnd_);
                UpdateWindow(hwnd_);
                web_.Focus();
            }
            splash_.FadeOut();
        }
        Reply(id, true, nullptr);
    } else if (cmd == "saveSettings") {
        if (args.contains("settings") && args["settings"].is_object()) {
            bool autostartChanged = args["settings"].value("startWithWindows", false) != settings_.value("startWithWindows", false);
            settings_ = MergeDefaults(DefaultSettings(), args["settings"]);
            SaveSettings(settings_);
            if (autostartChanged) SetAutostart(settings_.value("startWithWindows", false));
            ApplyHotkey();
            ApplyLiveSettings();
        }
        Reply(id, true, nullptr);
    } else if (cmd == "listWindows") {
        Reply(id, true, ListWindows(hwnd_));
    } else if (cmd == "systemInfo") {
        Reply(id, true, CollectSystemInfo());
    } else if (cmd == "scale") {
        std::string h = args.value("hwnd", "");
        if (!h.empty()) {
            HWND target = (HWND)(uintptr_t)strtoull(h.c_str(), nullptr, 10);
            if (!IsWindow(target)) {
                Reply(id, false, "window_gone");
                return;
            }
            if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
            SetForegroundWindow(target);
            scalingTarget_ = target;
            // Give the window a moment to come to the front before capture starts.
            SetTimer(hwnd_, TIMER_START_TARGET, 350, nullptr);
        } else {
            BeginCountdown(std::clamp(args.value("delay", settings_.value("scaleDelay", 5)), 0, 30));
        }
        Reply(id, true, nullptr);
    } else if (cmd == "cancelCountdown") {
        countdown_ = 0;
        KillTimer(hwnd_, TIMER_COUNTDOWN);
        Emit({{"event", "countdown"}, {"remaining", -1}});
        Reply(id, true, nullptr);
    } else if (cmd == "stop") {
        StopScaling();
        Reply(id, true, nullptr);
    } else if (cmd == "window") {
        std::string a = args.value("action", "");
        if (a == "minimize") {
            if (settings_.value("minimizeToTray", true))
                HideMain();
            else
                ShowWindow(hwnd_, SW_MINIMIZE);
        } else if (a == "maximize") {
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        } else if (a == "close") {
            if (settings_.value("closeToTray", false))
                HideMain();
            else
                Quit();
        } else if (a == "drag") {
            ReleaseCapture();
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        } else if (a == "resize") {
            std::string e = args.value("edge", "");
            WPARAM ht = e == "l" ? HTLEFT : e == "r" ? HTRIGHT : e == "t" ? HTTOP : e == "b" ? HTBOTTOM
                      : e == "tl" ? HTTOPLEFT : e == "tr" ? HTTOPRIGHT : e == "bl" ? HTBOTTOMLEFT : HTBOTTOMRIGHT;
            if (!IsZoomed(hwnd_)) {
                ReleaseCapture();
                SendMessageW(hwnd_, WM_NCLBUTTONDOWN, ht, 0);
            }
        }
        if (id.is_number()) Reply(id, true, nullptr);
    } else if (cmd == "openExternal") {
        std::string url = args.value("url", "");
        if (url.rfind("https://", 0) == 0 || url.rfind("ms-settings:", 0) == 0)
            ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        Reply(id, true, nullptr);
    } else if (cmd == "openLogs") {
        ShellExecuteW(nullptr, L"open", LogDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        Reply(id, true, nullptr);
    } else if (cmd == "trayStrings") {
        trayOpen_ = Wide(args.value("open", "Open MoonUp"));
        trayStop_ = Wide(args.value("stop", "Stop scaling"));
        trayQuit_ = Wide(args.value("quit", "Quit"));
        fpsMon_.SetLabel(Wide(args.value("display", "display")));
        Reply(id, true, nullptr);
    } else if (cmd == "resetSettings") {
        settings_ = DefaultSettings();
        settings_["firstRun"] = false;
        SaveSettings(settings_);
        ApplyHotkey();
        Reply(id, true, settings_);
    } else if (cmd.rfind("upgraph", 0) == 0) {
        HandleUpgraph(id, cmd, args);
    } else if (cmd == "quit") {
        Reply(id, true, nullptr);
        Quit();
    } else {
        Reply(id, false, "unknown_command");
    }
}

// ============================================================================ Upgraph
namespace {
std::wstring PickPath(HWND owner, bool folder, const wchar_t* filterName, const wchar_t* filterSpec) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder && filterSpec) {
        COMDLG_FILTERSPEC spec{filterName, filterSpec};
        dlg->SetFileTypes(1, &spec);
    }
    if (FAILED(dlg->Show(owner))) return {};
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return {};
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        out = path;
        CoTaskMemFree(path);
    }
    return out;
}
}  // namespace

void App::HandleUpgraph(const json& id, const std::string& cmd, const json& args) {
    HWND hwnd = hwnd_;
    // Long operations run on a worker thread; the reply is posted back to the UI thread.
    auto async = [hwnd, id](std::function<json()> work) {
        std::thread([hwnd, id, work] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            json r{{"id", id}};
            try {
                r["ok"] = true;
                r["data"] = work();
            } catch (const std::exception& e) {
                std::string code = upgraph::ErrorCode(e);
                SW_LOG("Upgraph error %s: %s", code.c_str(), e.what());
                r["ok"] = false;
                r["error"] = (code.empty() ? std::string("error") : code) + "|" + e.what();
            }
            PostMessageW(hwnd, WM_APP_POST, 0, (LPARAM) new std::string(r.dump()));
            CoUninitialize();
        }).detach();
    };
    std::wstring dir = Wide(args.value("dir", ""));
    if (cmd == "upgraphLibrary") {
        json folders = settings_.contains("upgraph") ? settings_["upgraph"].value("folders", json::array()) : json::array();
        async([folders] { return upgraph::ScanLibrary(folders); });
    } else if (cmd == "upgraphScan") {
        async([dir] { return upgraph::ScanGame(dir); });
    } else if (cmd == "upgraphInstall") {
        json opts = args.value("options", json::object());
        std::string dirU = args.value("dir", "");
        async([dir, opts, hwnd, dirU] {
            return upgraph::Install(dir, opts, [hwnd, dirU](const std::string& stage, double f) {
                json e{{"event", "upgraph"}, {"dir", dirU}, {"stage", stage}, {"fraction", f}};
                PostMessageW(hwnd, WM_APP_POST, 0, (LPARAM) new std::string(e.dump()));
            });
        });
    } else if (cmd == "upgraphRemove") {
        async([dir] { return upgraph::Remove(dir); });
    } else if (cmd == "upgraphPreset") {
        json preset = args.value("preset", json::object());
        async([dir, preset] { return upgraph::UpdatePreset(dir, preset); });
    } else if (cmd == "upgraphPoster") {
        Reply(id, true, upgraph::PosterDataUrl(Wide(args.value("file", ""))));
    } else if (cmd == "upgraphPickFolder") {
        Reply(id, true, Utf8(PickPath(hwnd_, true, nullptr, nullptr)));
    } else if (cmd == "upgraphPickReShade") {
        std::wstring f = PickPath(hwnd_, false, L"ReShade Setup", L"ReShade_Setup*.exe;*.exe");
        if (f.empty()) {
            Reply(id, true, nullptr);
            return;
        }
        async([f] { return upgraph::UseReShadeSetup(f); });
    } else if (cmd == "upgraphPickDlssNr") {
        std::wstring f = PickPath(hwnd_, false, L"nvngx_dlssnr.dll", L"nvngx_dlssnr*.dll;*.dll");
        if (f.empty()) {
            Reply(id, true, nullptr);
            return;
        }
        async([f] { return upgraph::UseDlssNr(f); });
    } else if (cmd == "upgraphClearDlssNr") {
        async([] { return upgraph::ClearDlssNr(); });
    } else if (cmd == "upgraphOpenFolder") {
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        Reply(id, true, nullptr);
    } else if (cmd == "upgraphInfo") {
        Reply(id, true, json{{"cache", Utf8(upgraph::CacheRoot())}});
    } else {
        Reply(id, false, "unknown_command");
    }
}

// ============================================================================ scaling
void App::BeginCountdown(int seconds) {
    countdown_ = seconds;
    Emit({{"event", "countdown"}, {"remaining", countdown_}});
    if (seconds <= 0) {
        HWND fg = GetForegroundWindow();
        if (fg && !IsOwnWindow(fg))
            StartScaling(fg);
        else
            Emit({{"event", "engine"}, {"state", "error"}, {"code", "pick_window"}, {"message", ""}});
        return;
    }
    SetTimer(hwnd_, TIMER_COUNTDOWN, 1000, nullptr);
}

void App::StartScaling(HWND target) {
    if (!IsWindow(target) || IsOwnWindow(target)) {
        Emit({{"event", "engine"}, {"state", "error"}, {"code", "pick_window"}, {"message", ""}});
        return;
    }
    std::string exe = WindowExeName(target);
    json profile = ProfileForExe(settings_, exe, &scalingProfile_);
    if (scalingTarget_ && scalingTarget_ != target) RestoreRenderScale();
    scalingTarget_ = target;
    engineOverlay_ = false;
    int mw = 0, mh = 0;
    bool fills = FillsMonitor(target, &mw, &mh);
    int pct = EffectiveRenderScale(profile, target);
    ApplyRenderScale(target, pct);
    engine_.Start(hwnd_, target, profile);
    // Same resolution as the screen and no render scale: MoonUp can only pass the image through.
    if (fills && (pct <= 0 || pct >= 100) && profile.value("scaleMode", std::string("auto")) != "off")
        Emit({{"event", "notice"}, {"code", "same_res"}, {"w", mw}, {"h", mh}});
    std::string title = WindowTitle(target);
    settings_["lastSession"] = {{"exe", exe}, {"title", title}};
    SaveSettings(settings_);
    Emit({{"event", "scaling"}, {"title", title}, {"exe", exe}, {"profile", scalingProfile_}});
    TrayTooltip(L"MoonUp • " + Wide(title));
    SW_LOG("Scaling '%s' (%s) with profile %s", title.c_str(), exe.c_str(), scalingProfile_.c_str());
}

void App::StopScaling() {
    engine_.Stop();
    RestoreRenderScale();
    engineOverlay_ = false;
    scalingTarget_ = nullptr;
    TrayTooltip(L"MoonUp");
}

void App::ApplyLiveSettings() {
    if (!engine_.Running() || !scalingTarget_) return;
    json profile = ProfileForExe(settings_, WindowExeName(scalingTarget_), &scalingProfile_);
    if (engine_.NeedsRestart(profile)) {
        StartScaling(scalingTarget_);
    } else {
        ApplyRenderScale(scalingTarget_, EffectiveRenderScale(profile, scalingTarget_));
        engine_.UpdateConfig(profile);
    }
}

void App::ApplyRenderScale(HWND h, int pct) {
    if (pct <= 0 || pct >= 100 || !IsWindow(h)) {
        RestoreRenderScale();
        return;
    }
    if (resized_ == h && resizedPct_ == pct) return;
    if (resized_ != h) {
        RestoreRenderScale();
        WINDOWPLACEMENT wp{sizeof(wp)};
        if (!GetWindowPlacement(h, &wp)) return;
        resizedPlacement_ = wp;
        resized_ = h;
    }
    if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return;
    const int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    const int cw = (mw * pct / 100) & ~1, ch = (mh * pct / 100) & ~1;
    // Non-client margins measured from the real rectangles (works for any DPI awareness of the game).
    RECT wr, c;
    POINT tl{0, 0};
    if (!GetWindowRect(h, &wr) || !GetClientRect(h, &c) || !ClientToScreen(h, &tl)) return;
    const int ml = tl.x - wr.left, mt = tl.y - wr.top;
    const int mr = wr.right - (tl.x + c.right), mb = wr.bottom - (tl.y + c.bottom);
    const int cx = mi.rcMonitor.left + (mw - cw) / 2, cy = mi.rcMonitor.top + (mh - ch) / 2;
    BOOL ok = SetWindowPos(h, nullptr, cx - ml, cy - mt, cw + ml + mr, ch + mt + mb,
                           SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    resizedPct_ = pct;
    expectW_ = cw;
    expectH_ = ch;
    SW_LOG("Render scale %d%%: game window client %dx%d (%s)", pct, cw, ch, ok ? "ok" : "refused");
    // Some games put their window back (fullscreen-window modes): check once it settled.
    SetTimer(hwnd_, TIMER_RESIZE_CHECK, 1500, nullptr);
}

int App::EffectiveRenderScale(const json& p, HWND h) {
    int pct = RenderScaleOf(p);
    if (pct > 0 && pct < 100) return pct;
    if (!IsWindow(h) || (resized_ != h && !FillsMonitor(h))) return 100;
    if (p.value("scaleMode", std::string("auto")) == "custom") {
        double f = 1.0;
        auto it = p.find("factor");
        if (it != p.end() && it->is_number()) f = it->get<double>();
        if (f > 1.05) return std::clamp((int)std::lround(100.0 / f), 25, 90);
    }
    return 100;
}

void App::CheckRenderScale() {
    if (!resized_ || !IsWindow(resized_)) return;
    RECT c;
    if (!GetClientRect(resized_, &c)) return;
    int mw = 0, mh = 0;
    if (FillsMonitor(resized_, &mw, &mh)) {
        SW_LOG("Render scale: the game kept its size (%ldx%ld)", c.right, c.bottom);
        Emit({{"event", "notice"}, {"code", "resize_refused"}});
        RestoreRenderScale();
        return;
    }
    SW_LOG("Render scale: game renders %ldx%ld (asked %dx%d)", c.right, c.bottom, expectW_, expectH_);
    Emit({{"event", "notice"}, {"code", "resize_ok"}, {"w", c.right}, {"h", c.bottom}, {"mw", mw}, {"mh", mh}});
}

void App::RestoreRenderScale() {
    if (!resized_) return;
    if (IsWindow(resized_)) {
        WINDOWPLACEMENT wp = resizedPlacement_;
        wp.length = sizeof(wp);
        if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_MINIMIZE) wp.showCmd = SW_SHOWNORMAL;
        SetWindowPlacement(resized_, &wp);
        SW_LOG("Render scale: game window restored");
    }
    resized_ = nullptr;
    resizedPct_ = 0;
}

// The FPS counter of the foreground window, whenever MoonUp' own overlay is not showing one.
void App::UpdateFpsMonitor() {
    HWND fg = GetForegroundWindow();
    HWND show = nullptr;
    int corner = 0;
    auto candidate = [&](HWND h) {
        if (!h || !IsWindowVisible(h) || IsIconic(h) || IsOwnWindow(h)) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (engine_.Running() && pid == engine_.HostPid()) return false;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return false;
        wchar_t cls[96] = L"";
        GetClassNameW(h, cls, 96);
        static const wchar_t* shell[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                         L"NotifyIconOverflowWindow", L"Windows.UI.Core.CoreWindow",
                                         L"XamlExplorerHostIslandWindow", L"ForegroundStaging", L"MultitaskingViewFrame",
                                         L"TopLevelWindowForOverflowXamlIsland", L"#32768"};
        for (auto* s : shell)
            if (!wcscmp(cls, s)) return false;
        RECT c;
        if (!GetClientRect(h, &c) || c.right < 320 || c.bottom < 200) return false;
        return true;
    };
    if (candidate(fg)) {
        // MoonUp' own HUD is on screen for this window.
        bool ownHud = engine_.Running() && engineOverlay_ && fg == scalingTarget_;
        bool allowed = fpsMon_.AnyWindow() || FillsMonitor(fg) || (engine_.Running() && fg == scalingTarget_);
        if (!ownHud && allowed) {
            json prof = ProfileForExe(settings_, WindowExeName(fg), nullptr);
            if (prof.value("showFps", true)) {
                show = fg;
                std::string hp = prof.value("hudPosition", "tl");
                corner = hp == "tr" ? 1 : hp == "bl" ? 2 : hp == "br" ? 3 : 0;
            }
        }
    }
    fpsMon_.SetTarget(show, corner);
}

void App::ApplyHotkey() {
    UnregisterHotKey(hwnd_, HOTKEY_SCALE);
    json hk = settings_.value("hotkey", json::object());
    UINT vk = VkFromKey(hk.value("key", "S"));
    if (!vk) return;
    UINT mods = MOD_NOREPEAT;
    if (hk.value("ctrl", true)) mods |= MOD_CONTROL;
    if (hk.value("alt", true)) mods |= MOD_ALT;
    if (hk.value("shift", false)) mods |= MOD_SHIFT;
    if (!RegisterHotKey(hwnd_, HOTKEY_SCALE, mods, vk)) {
        SW_LOG("Hotkey registration failed (%lu)", GetLastError());
        Emit({{"event", "hotkey"}, {"ok", false}});
    }
}

void App::SetAutostart(bool enabled) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enabled) {
        wchar_t path[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\" --minimized";
        RegSetValueExW(key, L"MoonUp", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"MoonUp");
    }
    RegCloseKey(key);
}

// ============================================================================ tray
void App::TrayAdd() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(nid.szTip, L"MoonUp");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void App::TrayRemove() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void App::TrayTooltip(const std::wstring& text) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::TrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, trayOpen_.c_str());
    if (engine_.Running()) AppendMenuW(menu, MF_STRING, 2, trayStop_.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, trayQuit_.c_str());
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd_);
    int c = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (c == 1) ShowMain();
    if (c == 2) {
        StopScaling();
    }
    if (c == 3) Quit();
}

// ============================================================================ window procedure
LRESULT CALLBACK App::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    App* self = nullptr;
    if (m == WM_NCCREATE) {
        self = (App*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
    } else {
        self = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) {
                // Remove the standard frame but keep resize/snap/shadow behaviour.
                if (IsZoomed(hwnd_)) {
                    auto* p = (NCCALCSIZE_PARAMS*)lp;
                    UINT dpi = GetDpiForWindow(hwnd_);
                    int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    p->rgrc[0].left += fx;
                    p->rgrc[0].right -= fx;
                    p->rgrc[0].top += fy;
                    p->rgrc[0].bottom -= fy;
                }
                return 0;
            }
            break;
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd_, msg, wp, -1);
        case WM_NCHITTEST: {
            // Thin resize band for the parts not covered by the web view.
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT r;
            GetWindowRect(hwnd_, &r);
            int b = MulDiv(6, GetDpiForWindow(hwnd_), 96);
            if (!IsZoomed(hwnd_)) {
                bool L = p.x < r.left + b, R = p.x >= r.right - b, T = p.y < r.top + b, B = p.y >= r.bottom - b;
                if (T && L) return HTTOPLEFT;
                if (T && R) return HTTOPRIGHT;
                if (B && L) return HTBOTTOMLEFT;
                if (B && R) return HTBOTTOMRIGHT;
                if (L) return HTLEFT;
                if (R) return HTRIGHT;
                if (T) return HTTOP;
                if (B) return HTBOTTOM;
            }
            return HTCLIENT;
        }
        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            web_.Resize(rc);
            if (uiReady_) Emit({{"event", "window"}, {"maximized", IsZoomed(hwnd_) != FALSE}});
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
            mm->ptMinTrackSize.x = MulDiv(kMinW, dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(kMinH, dpi, 96);
            return 0;
        }
        case WM_DPICHANGED: {
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            HBRUSH br = CreateSolidBrush(RGB(7, 8, 11));
            FillRect((HDC)wp, &rc, br);
            DeleteObject(br);
            return 1;
        }
        case WM_TIMER:
            if (wp == TIMER_SPLASH_FAILSAFE) {
                KillTimer(hwnd_, TIMER_SPLASH_FAILSAFE);
                if (!uiReady_) {
                    uiReady_ = true;
                    if (!startMinimized_) ShowWindow(hwnd_, SW_SHOW);
                    splash_.FadeOut();
                }
            } else if (wp == TIMER_COUNTDOWN) {
                countdown_--;
                Emit({{"event", "countdown"}, {"remaining", countdown_}});
                if (countdown_ <= 0) {
                    KillTimer(hwnd_, TIMER_COUNTDOWN);
                    HWND fg = GetForegroundWindow();
                    if (fg && !IsOwnWindow(fg))
                        StartScaling(fg);
                    else
                        Emit({{"event", "engine"}, {"state", "error"}, {"code", "pick_window"}, {"message", ""}});
                }
            } else if (wp == TIMER_RESIZE_CHECK) {
                KillTimer(hwnd_, TIMER_RESIZE_CHECK);
                CheckRenderScale();
            } else if (wp == TIMER_FPS_MONITOR) {
                UpdateFpsMonitor();
            } else if (wp == TIMER_START_TARGET) {
                KillTimer(hwnd_, TIMER_START_TARGET);
                if (scalingTarget_) StartScaling(scalingTarget_);
            }
            return 0;
        case WM_HOTKEY:
            if (wp == HOTKEY_SCALE) {
                if (engine_.Running()) {
                    StopScaling();
                } else {
                    HWND fg = GetForegroundWindow();
                    if (fg && !IsOwnWindow(fg)) StartScaling(fg);
                }
            }
            return 0;
        case WM_APP_ENGINE: {
            std::unique_ptr<std::string> s((std::string*)lp);
            if ((UINT)wp != engine_.Session()) return 0;  // from a session that was restarted
            json e = json::parse(*s, nullptr, false);
            if (!e.is_discarded()) {
                std::string ev = e.value("event", "");
                if (ev == "overlay") {
                    engineOverlay_ = e.value("visible", false);
                    UpdateFpsMonitor();
                    return 0;
                }
                if (ev == "engine" && e.value("state", "") == "stopped") {
                    RestoreRenderScale();
                    engineOverlay_ = false;
                    scalingTarget_ = nullptr;
                    TrayTooltip(L"MoonUp");
                }
                web_.Post(*s);
            }
            return 0;
        }
        case WM_APP_POST: {
            std::unique_ptr<std::string> s((std::string*)lp);
            web_.Post(*s);
            return 0;
        }
        case WM_APP_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) ShowMain();
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) TrayMenu();
            return 0;
        case WM_COPYDATA:
            // Engine host events, or a second instance asking us to show ourselves.
            if (engine_.OnCopyData((HWND)wp, (const COPYDATASTRUCT*)lp)) return TRUE;
            ShowMain();
            return TRUE;
        case WM_APP_ENGINE_EXIT:
            engine_.OnProcessExit((DWORD)wp, (DWORD)lp);
            return 0;
        case WM_CLOSE:
            Quit();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_) {
                TrayAdd();
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace sw
