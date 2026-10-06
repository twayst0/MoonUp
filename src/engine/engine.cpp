#include "engine.h"

#include <roapi.h>

#include <cmath>
#include <deque>

#include "../third_party/json.hpp"
#include "capture.h"
#include "cursor.h"
#include "d3d.h"
#include "gpu_timer.h"
#include "perf.h"
#include "hud.h"
#include "motion.h"
#include "overlay.h"
#include "processor.h"

namespace sw {

using json = nlohmann::json;

namespace {

struct PresentCB {
    float rect[4];
    float bbSize[2];
    float useLinear;
    float opacity;
};

bool ClientRectOnScreen(HWND h, RECT& r) {
    RECT c;
    if (!GetClientRect(h, &c)) return false;
    POINT tl{0, 0};
    if (!ClientToScreen(h, &tl)) return false;
    r = {tl.x, tl.y, tl.x + c.right, tl.y + c.bottom};
    return c.right > 0 && c.bottom > 0;
}

// Output rectangle in overlay (monitor) coordinates.
RECT ComputeOutputRect(const RECT& client, const RECT& monitor, const EngineConfig& cfg) {
    const int srcW = client.right - client.left, srcH = client.bottom - client.top;
    const int monW = monitor.right - monitor.left, monH = monitor.bottom - monitor.top;
    double fit = std::min((double)monW / srcW, (double)monH / srcH);
    double s = 1.0;
    switch (cfg.scaleMode) {
        case ScaleMode::Off: {
            RECT r{client.left - monitor.left, client.top - monitor.top, 0, 0};
            r.right = r.left + srcW;
            r.bottom = r.top + srcH;
            return r;
        }
        case ScaleMode::Fullscreen: return RECT{0, 0, monW, monH};
        case ScaleMode::Auto: s = fit; break;
        case ScaleMode::Integer: s = std::max(1.0, std::floor(fit + 1e-6)); break;
        case ScaleMode::Custom: s = std::min<double>(std::max(0.25f, cfg.customFactor), fit); break;
    }
    int w = std::max(1, (int)std::lround(srcW * s));
    int h = std::max(1, (int)std::lround(srcH * s));
    w = std::min(w, monW);
    h = std::min(h, monH);
    RECT r;
    r.left = (monW - w) / 2;
    r.top = (monH - h) / 2;
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

const wchar_t* UpscalerLabel(Upscaler u, bool neuralReady) {
    switch (u) {
        case Upscaler::Neural: return neuralReady ? L"Neural SR" : L"Edge";
        case Upscaler::Edge: return L"Edge";
        case Upscaler::Lanczos: return L"Lanczos";
        case Upscaler::Bicubic: return L"Bicubic";
        case Upscaler::Bilinear: return L"Bilinear";
        case Upscaler::Nearest: return L"Integer / Nearest";
        case Upscaler::PixelArt: return L"Pixel Art";
        case Upscaler::Fsr: return L"FSR 1";
        case Upscaler::Nis: return L"NIS";
        case Upscaler::Anime: return neuralReady ? L"ArtCNN" : L"Edge";
    }
    return L"";
}

const char* UpscalerId(Upscaler u) {
    switch (u) {
        case Upscaler::Neural: return "neural";
        case Upscaler::Edge: return "edge";
        case Upscaler::Lanczos: return "lanczos";
        case Upscaler::Bicubic: return "bicubic";
        case Upscaler::Bilinear: return "bilinear";
        case Upscaler::Nearest: return "nearest";
        case Upscaler::PixelArt: return "pixel";
        case Upscaler::Fsr: return "fsr";
        case Upscaler::Nis: return "nis";
        case Upscaler::Anime: return "anime";
    }
    return "edge";
}

bool FindAdapterForMonitor(HMONITOR mon, LUID& luid) {
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        ComPtr<IDXGIOutput> o;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC d;
            o->GetDesc(&d);
            if (d.Monitor == mon) {
                DXGI_ADAPTER_DESC1 ad;
                a->GetDesc1(&ad);
                luid = ad.AdapterLuid;
                return true;
            }
            o.Reset();
        }
        a.Reset();
    }
    return false;
}

void EnableMmcss() {
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    if (!avrt) return;
    typedef HANDLE(WINAPI * Fn)(LPCWSTR, LPDWORD);
    Fn fn = (Fn)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
    DWORD idx = 0;
    if (fn) fn(L"Games", &idx);
}

}  // namespace

Engine::Engine() = default;
Engine::~Engine() { Stop(); }

bool Engine::Start(HWND target, const EngineConfig& cfg, EventFn events) {
    Stop();
    if (!IsWindow(target)) return false;
    target_ = target;
    startCfg_ = cfg;
    pendingCfg_ = cfg;
    events_ = std::move(events);
    stop_ = false;
    running_ = true;
    thread_ = std::thread(&Engine::ThreadMain, this);
    return true;
}

void Engine::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void Engine::UpdateConfig(const EngineConfig& cfg) {
    std::lock_guard<std::mutex> lock(cfgMutex_);
    pendingCfg_ = cfg;
    cfgDirty_ = true;
}

bool Engine::NeedsRestart(const EngineConfig& n) const {
    const EngineConfig& o = startCfg_;
    return n.capture != o.capture || n.adapterIndex != o.adapterIndex || n.vsync != o.vsync ||
           n.allowTearing != o.allowTearing || n.maxFrameLatency != o.maxFrameLatency;
}

void Engine::Emit(const std::string& j) {
    if (j.find("\"stopped\"") != std::string::npos) stoppedSent_ = true;
    if (events_) events_(j);
}

void Engine::ThreadMain() {
    stoppedSent_ = false;
    try {
        Run();
    } catch (const std::exception& e) {
        SW_LOG("Engine exception: %s", e.what());
        Emit(json{{"event", "engine"}, {"state", "error"}, {"code", "generic"}, {"message", e.what()}}.dump());
    } catch (...) {
        SW_LOG("Engine exception: unknown");
        Emit(json{{"event", "engine"}, {"state", "error"}, {"code", "generic"}, {"message", "unknown"}}.dump());
    }
    running_ = false;
    // Every way out of Run() (including early setup failures) ends with a "stopped" event, so
    // the app and the engine host always know the session is over.
    if (!stoppedSent_) Emit(json{{"event", "engine"}, {"state", "stopped"}, {"reason", "error"}}.dump());
}

void Engine::Run() {
    TimerResolution timerRes;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    // No MMCSS boost: the game's threads come first.
    RoInitialize(RO_INIT_MULTITHREADED);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    EngineConfig cfg = startCfg_;
    auto fail = [&](const std::string& code, const std::string& msg) {
        SW_LOG("Engine error %s: %s", code.c_str(), msg.c_str());
        Emit(json{{"event", "engine"}, {"state", "error"}, {"code", code}, {"message", msg}}.dump());
    };
    Emit(json{{"event", "engine"}, {"state", "starting"}}.dump());

    HMONITOR monitor = MonitorFromWindow(target_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    RECT monRect = mi.rcMonitor;

    // ------------------------------------------------------------------ device
    Gpu gpu;
    LUID displayLuid{};
    bool haveDisplayLuid = FindAdapterForMonitor(monitor, displayLuid);
    bool ok = (cfg.capture == CaptureApi::DDA && haveDisplayLuid) ? gpu.Create(-1, &displayLuid) : gpu.Create(cfg.adapterIndex);
    if (!ok) {
        fail("device", "Direct3D 11 device could not be created");
        running_ = false;
        RoUninitialize();
        return;
    }

    ShaderLibrary lib;
    lib.Init(gpu.device.Get());
    CommonStates states;
    states.Create(gpu.device.Get());
    ConstantBuffer presentCb;
    presentCb.Create(gpu.device.Get(), 64);

    // Compile everything up front so the first frame does not hitch.
    {
        for (auto& e : AllComputeShaders()) {
            // Optional stages (neural networks, FSR, NIS) are compiled when first used and fall
            // back to MoonUp Edge if they cannot be built on this system.
            if (!strcmp(e.file, "neural.hlsl") || !strcmp(e.file, "render.hlsl") || !strcmp(e.file, "fsr.hlsl") ||
                !strcmp(e.file, "nis.hlsl"))
                continue;
            if (!lib.CS(e.file, e.entry)) {
                fail("shader", lib.LastError());
                running_ = false;
                RoUninitialize();
                return;
            }
        }
        if (!lib.VS("present.hlsl", "VSFull") || !lib.PS("present.hlsl", "PSFrame") || !lib.VS("present.hlsl", "VSQuad") ||
            !lib.PS("present.hlsl", "PSQuad")) {
            fail("shader", lib.LastError());
            running_ = false;
            RoUninitialize();
            return;
        }
    }

    SW_LOG("Engine: shaders ready");
    double tInit = NowSeconds();
    Processor proc;
    proc.Init(gpu, lib, states);
    SW_LOG("Engine: processor ready (%.2f s, neural %d, render %d)", NowSeconds() - tInit, (int)proc.NeuralAvailable(),
           (int)proc.RenderAvailable());
    Motion motion;
    motion.Init(gpu, lib, states);
    GpuTimer timer;    // process + motion search (per processed frame)
    GpuTimer timerI;   // interpolation (per generated frame)
    GpuTimer timerC;   // compose + present
    bool timerOk = timer.Init(gpu.device.Get()) && timerI.Init(gpu.device.Get()) && timerC.Init(gpu.device.Get());
    SW_LOG("Engine: timers %d", (int)timerOk);
    PerfGovernor gov;
    int gameCount = 0, procCount = 0;
    double govT0 = NowSeconds();
    double lastProc = 0;
    double busyMs = 0;
    bool wasBypass = false;
    bool stickyNoticed = false;
    bool captureParked = false;
    bool fgIdle = false;
    // Native frame rate probe (overlay hidden) and occlusion guard state.
    bool probing = true;
    double probeStart = 0;
    int probeFrames = 0;
    double nativeFps = 0;
    double shownSince = 0;
    int guardBad = 0;
    bool guardWarned = false;
    EngineConfig prevEff = startCfg_;
    double lastBusyPct = 0, lastGameFps = 0;
    Hud hud;
    bool hudOk = hud.Init(gpu);
    SW_LOG("Engine: hud %d", (int)hudOk);
    CursorRenderer cursor;
    cursor.Init(gpu);
    SW_LOG("Engine: cursor ready");

    // ------------------------------------------------------------------ capture
    std::unique_ptr<Capture> capture;
    auto makeCapture = [&](CaptureApi api) -> bool {
        capture.reset(api == CaptureApi::WGC ? CreateWgcCapture() : api == CaptureApi::DDA ? CreateDdaCapture() : CreateGdiCapture());
        SW_LOG("Engine: starting %s", capture->Name());
        if (capture->Init(gpu, target_, monitor)) return true;
        SW_LOG("Capture %s failed: %s", capture->Name(), capture->Error().c_str());
        return false;
    };
    // Requested API first, then the other modern one, then GDI as the last resort.
    CaptureApi order[3] = {cfg.capture, cfg.capture == CaptureApi::WGC ? CaptureApi::DDA : CaptureApi::WGC, CaptureApi::GDI};
    if (cfg.capture == CaptureApi::GDI) order[1] = order[2] = CaptureApi::GDI;
    std::string captureErrors;
    bool captureOk = false;
    for (CaptureApi api : order) {
        if (makeCapture(api)) {
            captureOk = true;
            break;
        }
        captureErrors += std::string(captureErrors.empty() ? "" : " / ") + capture->Name() + ": " + capture->Error();
    }
    if (!captureOk) {
        fail("capture", captureErrors);
        running_ = false;
        RoUninitialize();
        return;
    }

    // ------------------------------------------------------------------ overlay
    SW_LOG("Engine: capture ready (%s)", capture->Name());
    Overlay overlay;
    auto createOverlay = [&]() {
        return overlay.Create(gpu, monRect, cfg.allowTearing && !cfg.vsync, cfg.maxFrameLatency, true,
                              std::string(capture->Name()).find("Duplication") != std::string::npos ||
                                  std::string(capture->Name()) == "GDI");
    };
    if (!createOverlay()) {
        fail("overlay", "Output window could not be created");
        running_ = false;
        RoUninitialize();
        return;
    }
    SW_LOG("Engine: overlay ready (monitor %.0f Hz, vsync %d, tearing %d)", overlay.RefreshRate(), (int)cfg.vsync,
           (int)(cfg.allowTearing && !cfg.vsync));
    UINT dpi = GetDpiForWindow(overlay.Hwnd());
    float uiScale = dpi ? dpi / 96.0f : 1.0f;

    // ------------------------------------------------------------------ state
    Texture srcTex;
    const UINT rw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    struct Slot {
        Texture tex;
        double time = 0;
        uint64_t seq = UINT64_MAX;
    };
    Slot slots[3];
    Texture genTex;
    uint64_t nextSeq = 0;
    RECT lastClient{};
    RECT dst{};
    double baseInterval = 1.0 / 60.0;
    double lastFrameTime = 0;

    // statistics
    int baseCount = 0, outCount = 0;
    double statT0 = NowSeconds();
    double lastHud = 0;
    double lastStatLog = NowSeconds();
    double lastPresent = 0;
    POINT lastCursor{-1, -1};
    uint64_t genSeq = UINT64_MAX;  // pair and t of the frame currently in genTex
    float genT = -1;
    int fgWhy[4] = {};  // frame selection outcomes (diagnostics): past pair, before pair, no flow, interpolable
    double gpuMs = 0;
    double lastDisplayTime = NowSeconds();
    std::deque<float> frameTimes;
    uint64_t lastShownSeq = UINT64_MAX;
    float lastShownT = -1;
    double presentDelay = 0;
    bool announced = false;
    bool loggedFirst = false;
    bool paused = false;
    std::string stopReason = "user";
    HudData hd;
    // Overlay visibility, reported to the app (its own FPS counter shows while ours is hidden).
    int shownReported = -1;
    auto setShown = [&](bool v) {
        overlay.Show(v);
        if (shownReported != (int)v) {
            shownReported = (int)v;
            Emit(json{{"event", "overlay"}, {"visible", v}}.dump());
        }
    };

    Emit(json{{"event", "engine"},
              {"state", "running"},
              {"capture", capture->Name()},
              {"gpu", gpu.info.name},
              {"refresh", overlay.RefreshRate()},
              {"neural", proc.NeuralAvailable()},
              {"render", proc.RenderAvailable()}}
             .dump());

    while (!stop_) {
        // Pump the overlay window's messages.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (cfgDirty_) {
            std::lock_guard<std::mutex> lock(cfgMutex_);
            EngineConfig n = pendingCfg_;
            cfgDirty_ = false;
            bool resetMotion = n.frameGen != cfg.frameGen || n.flowQuality != cfg.flowQuality;
            bool geomChange = n.scaleMode != cfg.scaleMode || n.customFactor != cfg.customFactor;
            cfg = n;
            genSeq = UINT64_MAX;  // settings may change how frames are generated
            if (resetMotion) motion.Reset();
            if (geomChange) lastClient = RECT{};
        }

        if (!IsWindow(target_)) {
            stopReason = "window_closed";
            break;
        }

        // ---------------------------------------------------------- focus / visibility
        HWND fgw = GetForegroundWindow();
        bool focused = fgw == target_ || fgw == overlay.Hwnd();
        bool minimized = IsIconic(target_) != FALSE;
        bool shouldPause = minimized || (cfg.pauseWhenUnfocused && !focused);
        if (shouldPause) {
            if (!paused) {
                setShown(false);
                cursor.ClipTo(nullptr);
                cursor.HideSystemCursor(false);
                paused = true;
                Emit(json{{"event", "engine"}, {"state", "paused"}}.dump());
            }
            RECT tmp;
            // Keep the capture drained, but only while the window still has the size of srcTex.
            if (srcTex && ClientRectOnScreen(target_, tmp) && (UINT)(tmp.right - tmp.left) == srcTex.width &&
                (UINT)(tmp.bottom - tmp.top) == srcTex.height)
            {
                capture->SetCopy(true);
                capture->Poll(tmp, srcTex.tex.Get());
            }
            Sleep(16);
            continue;
        }
        if (paused) {
            paused = false;
            gameCount = procCount = 0;
            busyMs = 0;
            govT0 = NowSeconds();
            Emit(json{{"event", "engine"}, {"state", "running"}}.dump());
        }

        // ---------------------------------------------------------- geometry
        RECT client;
        if (!ClientRectOnScreen(target_, client)) {
            Sleep(16);
            continue;
        }
        if (!EqualRect(&client, &lastClient)) {
            UINT w = (UINT)(client.right - client.left), h = (UINT)(client.bottom - client.top);
            srcTex.Ensure(gpu.device.Get(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
            dst = ComputeOutputRect(client, monRect, cfg);
            UINT ow = (UINT)(dst.right - dst.left), oh = (UINT)(dst.bottom - dst.top);
            for (auto& s : slots) {
                s.tex.Ensure(gpu.device.Get(), ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, rw);
                s.seq = UINT64_MAX;
            }
            genTex.Ensure(gpu.device.Get(), ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, rw);
            genSeq = UINT64_MAX;
            motion.Reset();
            lastClient = client;
            SW_LOG("Geometry: source %ux%u -> output %ux%u at (%ld,%ld)", w, h, ow, oh, dst.left, dst.top);
        }
        const UINT srcW = srcTex.width, srcH = srcTex.height;
        const bool scaled = (dst.right - dst.left) != (LONG)srcW || (dst.bottom - dst.top) != (LONG)srcH ||
                            dst.left != client.left - monRect.left || dst.top != client.top - monRect.top;

        // ---------------------------------------------------------- performance protection
        // 'eff' is what is really applied: the profile, lowered step by step when the GPU is too
        // busy for the game and MoonUp to share (see perf.h).
        auto* ctx = gpu.ctx.Get();
        if (timerOk) {
            timer.Collect(ctx);
            timerI.Collect(ctx);
            timerC.Collect(ctx);
        }
        EngineConfig eff = cfg;
        const double hzNow = overlay.RefreshRate();
        const double fgTarget = cfg.targetFps > 0 ? cfg.targetFps : hzNow;
        if (cfg.autoPerf) {
            // Frame rate first: lower the quality before ever limiting the processing rate.
            int tier = gov.tier();
            if (tier >= PerfGovernor::kLightMotion) eff.flowQuality = FlowQuality::Performance;
            if (tier >= PerfGovernor::kLightUpscale) {
                if (eff.upscaler == Upscaler::Neural || eff.upscaler == Upscaler::Anime) eff.upscaler = Upscaler::Fsr;
                else if (eff.upscaler == Upscaler::Lanczos) eff.upscaler = Upscaler::Edge;
            }
            if (tier >= PerfGovernor::kNoEffects) {
                eff.render.enabled = false;
                eff.vision.enabled = false;
            }
            if (tier >= PerfGovernor::kNoFrameGen) eff.frameGen = FrameGenMode::Off;
            // The game already fills the display: frame generation would only cost performance.
            // While we slow the game down, its measured rate says nothing about what it can reach:
            // use the uncovered rate then (frame generation cannot repair a slowdown we cause).
            double g = gov.gameFps();
            if (gov.native() > 0 && g < gov.native() * 0.75) g = gov.native();
            if (fgTarget > 0 && !captureParked) {
                if (!fgIdle && g >= fgTarget * 0.9) fgIdle = true;
                else if (fgIdle && g < fgTarget * 0.8) fgIdle = false;
            }
            if (fgIdle) eff.frameGen = FrameGenMode::Off;
        }
        if (eff.frameGen != prevEff.frameGen || eff.flowQuality != prevEff.flowQuality) {
            genSeq = UINT64_MAX;
            motion.Reset();
        }
        prevEff = eff;
        bool fgOn = eff.frameGen != FrameGenMode::Off;
        // Nothing to improve (no scaling, no frame generation, no effects): covering the game would
        // only cost frames, so it is shown directly (the app's FPS counter takes over).
        const bool passthrough = !scaled && !fgOn && !eff.render.Active() && !eff.vision.enabled;
        const bool bypass = (cfg.autoPerf && gov.bypass()) || passthrough;

        // ---------------------------------------------------------- capture + process
        // The loop polls every ~1 ms. Frames above the processing limit are released without any GPU
        // work; the image is only presented when it changes (new or generated frame, HUD, cursor).
        double nowG = NowSeconds();
        double ivl = cfg.autoPerf ? gov.interval() : 0.0;
        // Frames above the display rate can never be shown: do not spend GPU time on them.
        if (cfg.vsync && hzNow > 20) ivl = std::max(ivl, 1.0 / (hzNow * 1.04));
        if (probing && probeStart == 0) probeStart = nowG;
        bool gateOpen = !bypass && !probing && (ivl <= 0 || nowG - lastProc >= ivl - 0.0004);
        // Passthrough on a system that cannot hide the yellow capture border (Windows 10): stop
        // capturing so the game is shown clean; capture resumes when there is work again.
        {
            bool park = passthrough && !probing && !capture->BorderFree();
            if (park != captureParked) {
                captureParked = park;
                if (park) capture->Suspend();
                else if (!capture->Resume()) {
                    stopReason = "capture_lost";
                    break;
                }
            }
        }
        capture->SetCopy(gateOpen);
        CaptureResult cr = capture->Poll(client, srcTex.tex.Get());
        gameCount += cr.frames > 0 ? cr.frames : (cr.newFrame ? 1 : 0);
        if (probing) {
            // First 0.6 s: the game runs uncovered, we only count its frames (reference rate).
            probeFrames += cr.frames > 0 ? cr.frames : (cr.newFrame ? 1 : 0);
            if (nowG - probeStart >= 0.6) {
                probing = false;
                nativeFps = probeFrames / (nowG - probeStart);
                gov.SetNative(nativeFps);
                // The game alone already fills the display: never start frame generation for it.
                if (cfg.autoPerf && fgTarget > 0 && nativeFps >= fgTarget * 0.9) fgIdle = true;
                SW_LOG("Probe: game runs at %.0f fps uncovered", nativeFps);
            }
        }
        if (cr.lost) {
            std::string name = capture->Name();
            CaptureApi api = name.find("Duplication") != std::string::npos ? CaptureApi::DDA
                             : name == "GDI" ? CaptureApi::GDI : CaptureApi::WGC;
            if (!makeCapture(api)) {
                stopReason = "capture_lost";
                break;
            }
        }

        // Governor window.
        {
            double w = nowG - govT0;
            if (w >= 0.5) {
                // Estimated from the timed jobs x all jobs (query slots can be busy at high rates).
                if (timerOk) busyMs += timer.Estimate() + timerI.Estimate() + timerC.Estimate();
                const bool covering = overlay.Visible() && shownSince > 0 && nowG - shownSince > 1.0;
                double procFps = procCount / w;
                if (procCount > 0) gpuMs = gpuMs * 0.7 + (busyMs / procCount) * 0.3;
                if (cfg.autoPerf) {
                    bool heavyFx = cfg.render.Active() || cfg.vision.enabled ||
                                   (scaled && (cfg.upscaler == Upscaler::Neural || cfg.upscaler == Upscaler::Anime ||
                                               cfg.upscaler == Upscaler::Lanczos));
                    double fgMult = 1.0;
                    if (fgOn) {
                        fgMult = eff.frameGen == FrameGenMode::Adaptive
                                     ? std::clamp(std::round(fgTarget / std::max(gov.gameFps(), 1.0)), 1.0, 6.0)
                                     : (double)(int)eff.frameGen;
                    }
                    if (gov.Update(w, gameCount / w, procFps, busyMs / 1000.0 / w, cfg.frameGen != FrameGenMode::Off, heavyFx,
                                   covering, hzNow, fgMult)) {
                        SW_LOG("Perf: level %d (game %.0f fps, alone %.0f fps, MoonUp %.0f fps, GPU share %.0f%%, limit %.0f fps%s)",
                               gov.tier(), gov.gameFps(), gov.native(), procFps, busyMs / 10.0 / w, gov.cap(),
                               gov.collapsed() ? ", collapse" : "");
                    }
                    if (gov.sticky() && !stickyNoticed) {
                        stickyNoticed = true;
                        SW_LOG("Perf: the game collapses whenever it is covered (%.0f vs %.0f fps): staying bypassed",
                               gov.gameFps(), gov.native());
                        Emit(json{{"event", "notice"}, {"code", "slowdown"}, {"game", std::round(gov.gameFps())},
                                  {"native", std::round(gov.native())}}
                                 .dump());
                    }
                }
                lastBusyPct = busyMs / 10.0 / w;
                lastGameFps = gameCount / w;
                // Occlusion guard: the game got much slower once our window covered it -> make the
                // overlay "not covering" for the window manager (see Overlay::SetGuard).
                const double guardRef = std::min(gov.native() > 0 ? gov.native() : nativeFps, hzNow > 20 ? hzNow : 1e9);
                if (!probing && guardRef >= 30 && overlay.Visible() && shownSince > 0 && nowG - shownSince > 1.0 &&
                    lastGameFps < guardRef * (fgOn ? 0.45 : 0.6)) {
                    // A collapse is handled at once, a milder slowdown after 1.5 s.
                    if (++guardBad >= (lastGameFps < guardRef * 0.4 ? 1 : 3)) {
                        guardBad = 0;
                        if (overlay.Guard() < 2) {
                            int g = overlay.Guard() + 1;
                            SW_LOG("Occlusion: game %.0f fps covered vs %.0f fps uncovered, overlay guard %d", lastGameFps,
                                   nativeFps, g);
                            overlay.Destroy();
                            overlay.SetGuard(g);
                            if (!createOverlay()) {
                                fail("overlay", "Output window could not be created");
                                stopReason = "error";
                                break;
                            }
                            shownSince = 0;
                            lastShownSeq = UINT64_MAX;
                            lastShownT = -1;
                        } else if (!guardWarned) {
                            guardWarned = true;
                            SW_LOG("Occlusion: the game still slows down while covered (%.0f vs %.0f fps)", lastGameFps,
                                   nativeFps);
                        }
                    }
                } else {
                    guardBad = 0;
                }
                gameCount = procCount = 0;
                busyMs = 0;
                govT0 = nowG;
            }
        }

        if (bypass) {
            // MoonUp steps aside: the game is shown directly (no overlay) until the GPU has room again.
            if (!wasBypass) {
                wasBypass = true;
                setShown(false);
                cursor.ClipTo(nullptr);
                cursor.HideSystemCursor(false);
                lastShownSeq = UINT64_MAX;
                SW_LOG(passthrough ? "Passthrough: nothing to scale or generate, the game is shown directly"
                                   : "Perf: bypass, the game is shown directly");
            }
            // Keep the app informed: the game's own rate is what the user sees now.
            if (nowG - statT0 >= 0.5) {
                statT0 = nowG;
                baseCount = outCount = 0;
                Emit(json{{"event", "stats"},
                          {"baseFps", std::round(lastGameFps * 10) / 10},
                          {"outFps", std::round(lastGameFps * 10) / 10},
                          {"gpuMs", 0},
                          {"delayMs", 0},
                          {"src", {srcW, srcH}},
                          {"out", {srcW, srcH}},
                          {"upscaler", "native"},
                          {"gameFps", std::round(lastGameFps * 10) / 10},
                          {"perfLevel", gov.tier()},
                          {"passthrough", passthrough},
                          {"neural", proc.NeuralAvailable()},
                          {"capture", capture->Name()},
                          {"refresh", overlay.RefreshRate()},
                          {"frameTimes", json::array()}}
                         .dump());
            }
            PreciseSleepUntil(NowSeconds() + 0.004);
            continue;
        }
        if (wasBypass) {
            wasBypass = false;
            for (auto& sl : slots) sl.seq = UINT64_MAX;  // never show stale frames
            genSeq = UINT64_MAX;
            lastShownSeq = UINT64_MAX;
            lastShownT = -1;
            motion.Reset();
        }

        if (cr.newFrame && gateOpen) {
            lastProc = nowG;
            uint64_t seq = nextSeq++;
            Slot& s = slots[seq % 3];
            if (timerOk) timer.Begin(ctx);
            if (!proc.Run(srcTex.srv.Get(), srcW, srcH, s.tex, eff, timerOk ? &timer : nullptr)) {
                fail("process", proc.Error().empty() ? lib.LastError() : proc.Error());
                stopReason = "error";
                break;
            }
            s.time = cr.time;
            s.seq = seq;
            if (fgOn) motion.AddFrame(proc.SourceView(), srcW, srcH, eff.flowQuality, seq);
            if (timerOk) timer.End(ctx);
            if (lastFrameTime > 0) {
                double dt = cr.time - lastFrameTime;
                if (dt > 0.002 && dt < 0.1) baseInterval = baseInterval * 0.9 + dt * 0.1;
            }
            lastFrameTime = cr.time;
            baseCount++;
            procCount++;
        }

        // ---------------------------------------------------------- choose what to display
        Slot* latest = nextSeq > 0 ? &slots[(nextSeq - 1) % 3] : nullptr;
        ID3D11ShaderResourceView* show = nullptr;
        uint64_t showSeq = UINT64_MAX;
        float showT = 1.0f;
        if (latest && latest->seq == nextSeq - 1) {
            show = latest->tex.srv.Get();
            showSeq = latest->seq;
            if (fgOn && nextSeq >= 2) {
                double delay = std::clamp(baseInterval * 1.02 + 0.002, 0.004, 0.06);
                presentDelay = delay;
                double tau = NowSeconds() - delay;
                for (uint64_t k = nextSeq - 1; k >= 1 && k + 2 >= nextSeq; k--) {
                    Slot& B = slots[k % 3];
                    Slot& A = slots[(k - 1) % 3];
                    if (B.seq != k || A.seq != k - 1) break;
                    if (tau >= B.time) {  // newer than this pair: keep the later frame
                        fgWhy[0]++;
                        break;
                    }
                    if (tau < A.time) {
                        fgWhy[1]++;
                        show = A.tex.srv.Get();
                        showSeq = A.seq;
                        continue;
                    }
                    double span = B.time - A.time;
                    if (span <= 0 || span > 0.2 || !motion.HasPair(k)) {
                        fgWhy[2]++;
                        show = A.tex.srv.Get();
                        showSeq = A.seq;
                        break;
                    }
                    float t = (float)((tau - A.time) / span);
                    // Number of displayed steps per real frame: the fixed multiplier, or for
                    // Adaptive enough steps to reach the target rate (at most 6x).
                    float m = (float)(int)eff.frameGen;
                    if (eff.frameGen == FrameGenMode::Adaptive) {
                        double target = cfg.targetFps > 0 ? cfg.targetFps : overlay.RefreshRate();
                        m = (float)std::clamp(std::round(target * span), 1.0, 6.0);
                    }
                    t = std::floor(t * m + 1e-3f) / m;
                    fgWhy[3]++;
                    if (t <= 0.001f) {
                        show = A.tex.srv.Get();
                        showSeq = A.seq;
                    } else if (t >= 0.999f) {
                        show = B.tex.srv.Get();
                        showSeq = B.seq;
                    } else if (genSeq == k && genT == t) {
                        show = genTex.srv.Get();  // already generated: reuse it
                        showSeq = k;
                        showT = t;
                    } else {
                        if (timerOk) timerI.Begin(ctx);
                        bool okI = motion.Interpolate(k, A.tex.srv.Get(), B.tex.srv.Get(), t, genTex, eff);
                        if (timerOk) timerI.End(ctx);
                        if (okI) {
                            show = genTex.srv.Get();
                            showSeq = k;
                            showT = t;
                            genSeq = k;
                            genT = t;
                        }
                    }
                    break;
                }
            } else {
                presentDelay = 0;
            }
        }

        // ---------------------------------------------------------- present only on change
        double now = NowSeconds();
        bool changed = show && (showSeq != lastShownSeq || showT != lastShownT);
        bool needCursor = cfg.drawCursor && scaled;
        POINT cpos{};
        GetCursorPos(&cpos);
        bool cursorMoved = needCursor && focused && (cpos.x != lastCursor.x || cpos.y != lastCursor.y);
        bool hudDue = hudOk && cfg.showFps && now - lastHud > 0.25;
        if (!changed && !cursorMoved && !hudDue && !(show && !overlay.Visible()) && now - lastPresent < 0.5) {
            PreciseSleepUntil(now + 0.0012);
            continue;
        }
        lastCursor = cpos;
        // Waitable swap chain: wait until the previous frame left the queue (no blocking Present).
        if (cfg.vsync) overlay.WaitReady(100);
        if (timerOk) timerC.Begin(ctx);

        // ---------------------------------------------------------- compose
        if (!overlay.Visible() && show) {
            setShown(true);
            shownSince = now;
        }
        ID3D11RenderTargetView* rtv = overlay.BackBuffer();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{0, 0, (float)overlay.Width(), (float)overlay.Height(), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(states.noCull.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        states.BindSamplers(ctx);
        ctx->PSSetConstantBuffers(0, 1, presentCb.Addr());
        ctx->VSSetConstantBuffers(0, 1, presentCb.Addr());

        PresentCB pc{};
        pc.bbSize[0] = (float)overlay.Width();
        pc.bbSize[1] = (float)overlay.Height();
        pc.opacity = 1.0f;
        if (show) {
            pc.rect[0] = (float)dst.left;
            pc.rect[1] = (float)dst.top;
            pc.rect[2] = (float)(dst.right - dst.left);
            pc.rect[3] = (float)(dst.bottom - dst.top);
            presentCb.Update(ctx, &pc, sizeof(pc));
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx->VSSetShader(lib.VS("present.hlsl", "VSFull"), nullptr, 0);
            ctx->PSSetShader(lib.PS("present.hlsl", "PSFrame"), nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &show);
            ctx->Draw(3, 0);
        } else {
            const float black[4] = {0, 0, 0, 1};
            ctx->ClearRenderTargetView(rtv, black);
        }

        auto drawQuad = [&](ID3D11ShaderResourceView* srv, float x, float y, float w, float h, bool linear) {
            pc.rect[0] = x;
            pc.rect[1] = y;
            pc.rect[2] = w;
            pc.rect[3] = h;
            pc.useLinear = linear ? 1.0f : 0.0f;
            presentCb.Update(ctx, &pc, sizeof(pc));
            ctx->OMSetBlendState(states.premulAlpha.Get(), nullptr, 0xffffffff);
            ctx->VSSetShader(lib.VS("present.hlsl", "VSQuad"), nullptr, 0);
            ctx->PSSetShader(lib.PS("present.hlsl", "PSQuad"), nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ctx->Draw(4, 0);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        };

        // Cursor: only needed when the output geometry differs from the window.
        if (needCursor && focused) {
            RECT clip = client;
            if (cfg.clipCursor) cursor.ClipTo(&clip);
            cursor.HideSystemCursor(true);
            POINT origin{monRect.left, monRect.top};
            if (cursor.Update(client, dst, origin, scaled))
                drawQuad(cursor.Srv(), cursor.X(), cursor.Y(), cursor.W(), cursor.H(), true);
        } else {
            cursor.HideSystemCursor(false);
            cursor.ClipTo(nullptr);
        }

        if (hudOk && cfg.showFps) {
            if (hudDue) {
                hd.srcW = (int)srcW;
                hd.srcH = (int)srcH;
                hd.outW = dst.right - dst.left;
                hd.outH = dst.bottom - dst.top;
                hd.gpuMs = gpuMs;
                hd.upscaler = scaled ? UpscalerLabel(eff.upscaler, proc.NeuralAvailable()) : L"Native";
                if (eff.render.enabled && proc.RenderAvailable()) hd.upscaler += L" + NR";
                if (!fgOn)
                    hd.frameGen = L"Frame gen off";
                else if (eff.frameGen == FrameGenMode::Adaptive)
                    hd.frameGen = L"Motion adaptive";
                else
                    hd.frameGen = std::wstring(L"Motion x") + std::to_wstring((int)eff.frameGen);
                // Output can never exceed the monitor refresh: tell the user how far to cap the game.
                double hz = overlay.RefreshRate();
                double mult = eff.frameGen == FrameGenMode::Adaptive ? 2.0 : (double)(int)eff.frameGen;
                hd.hint.clear();
                if (fgOn && hz > 1 && hd.baseFps * mult > hz * 1.1)
                    hd.hint = L"Cap game at " + std::to_wstring((int)std::floor(hz / mult)) + L" fps";
                if (cfg.autoPerf) {
                    // Tell the user when the protection stepped in (and why).
                    int tr = gov.tier();
                    if (tr == PerfGovernor::kLightMotion) hd.hint = L"Auto: lighter motion search";
                    else if (tr == PerfGovernor::kLightUpscale) hd.hint = L"Auto: lighter upscaler (GPU busy)";
                    else if (tr == PerfGovernor::kNoEffects) hd.hint = L"Auto: effects off (GPU busy)";
                    else if (tr == PerfGovernor::kNoFrameGen) hd.hint = L"Auto: frame gen paused (GPU busy)";
                    else if (tr >= PerfGovernor::kRateLimit) hd.hint = L"GPU full: lower game resolution";
                    else if (fgIdle && cfg.frameGen != FrameGenMode::Off) hd.hint = L"Game already fast: frame gen idle";
                }
                hd.frameTimes = frameTimes;
                hud.Render(hd, cfg.showGraph);
                lastHud = now;
            }
            float hw = hud.Width() * uiScale, hh = hud.Height() * uiScale;
            float pad = 16 * uiScale;
            float x = (cfg.hudPosition == HudPosition::TopRight || cfg.hudPosition == HudPosition::BottomRight)
                          ? overlay.Width() - hw - pad
                          : pad;
            float y = (cfg.hudPosition == HudPosition::BottomLeft || cfg.hudPosition == HudPosition::BottomRight)
                          ? overlay.Height() - hh - pad
                          : pad;
            drawQuad(hud.Srv(), x, y, hw, hh, true);
        }
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);
        if (timerOk) timerC.End(ctx);

        HRESULT hr = overlay.Present(cfg.vsync);
        lastPresent = now;
        if (!loggedFirst) {
            loggedFirst = true;
            SW_LOG("Engine: first frame presented (hr %s, shown %d)", HrToString(hr).c_str(), show ? 1 : 0);
        }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            fail("device_lost", "The GPU was reset");
            stopReason = "error";
            break;
        }

        // ---------------------------------------------------------- stats
        if (changed) {
            outCount++;
            double ft = (now - lastDisplayTime) * 1000.0;
            lastDisplayTime = now;
            frameTimes.push_back((float)ft);
            while (frameTimes.size() > 90) frameTimes.pop_front();
            lastShownSeq = showSeq;
            lastShownT = showT;
        }
        double el = now - statT0;
        if (el >= 0.5) {
            double baseFps = baseCount / el, outFps = outCount / el;
            hd.baseFps = baseFps;
            hd.outFps = outFps;
            json st = {{"event", "stats"},
                       {"baseFps", std::round(baseFps * 10) / 10},
                       {"outFps", std::round(outFps * 10) / 10},
                       {"gpuMs", std::round(gpuMs * 100) / 100},
                       {"delayMs", std::round(presentDelay * 10000) / 10},
                       {"src", {srcW, srcH}},
                       {"out", {dst.right - dst.left, dst.bottom - dst.top}},
                       {"upscaler", scaled ? UpscalerId(eff.upscaler) : "native"},
                       {"gameFps", std::round(lastGameFps * 10) / 10},
                       {"perfLevel", gov.tier()},
                       {"neural", proc.NeuralAvailable()},
                       {"capture", capture->Name()},
                       {"refresh", overlay.RefreshRate()}};
            json ftj = json::array();
            for (size_t i = frameTimes.size() > 60 ? frameTimes.size() - 60 : 0; i < frameTimes.size(); i++)
                ftj.push_back(std::round(frameTimes[i] * 10) / 10);
            st["frameTimes"] = ftj;
            Emit(st.dump());
            if (now - lastStatLog >= 5.0) {
                // Diagnostics for test reports: base (game) rate, output rate and our GPU time.
                lastStatLog = now;
                SW_LOG("Stats: game %.1f fps, base %.1f fps, out %.1f fps, gpu %.2f ms/frame, share %.0f%%, level %d, limit %.0f, delay %.1f ms, "
                       "%ux%u -> %ldx%ld, %s, fg %d, nr %d",
                       lastGameFps, baseFps, outFps, gpuMs, lastBusyPct, gov.tier(), gov.cap(), presentDelay * 1000, srcW, srcH,
                       dst.right - dst.left, dst.bottom - dst.top, scaled ? UpscalerId(eff.upscaler) : "native",
                       (int)eff.frameGen, (int)(eff.render.Active() && proc.RenderAvailable()));
                SW_LOG("FG: past %d, early %d, noflow %d, interp %d", fgWhy[0], fgWhy[1], fgWhy[2], fgWhy[3]);
                fgWhy[0] = fgWhy[1] = fgWhy[2] = fgWhy[3] = 0;
            }
            baseCount = outCount = 0;
            statT0 = now;
            if (!announced) {
                announced = true;
                SW_LOG("Running: %ux%u -> %ldx%ld, %s", srcW, srcH, dst.right - dst.left, dst.bottom - dst.top,
                       capture->Name());
            }
        }

        // Without vsync, never present faster than the target rate.
        if (!cfg.vsync) {
            double target = cfg.targetFps > 0 ? cfg.targetFps : overlay.RefreshRate();
            PreciseSleepUntil(now + 1.0 / std::max(30.0, target) - 0.0005);
        }
    }

    // ------------------------------------------------------------------ teardown
    cursor.Shutdown();
    overlay.Destroy();
    capture.reset();
    hud.Shutdown();
    proc.Shutdown();
    gpu.Release();
    RoUninitialize();
    running_ = false;
    Emit(json{{"event", "engine"}, {"state", "stopped"}, {"reason", stopReason}}.dump());
}

}  // namespace sw
