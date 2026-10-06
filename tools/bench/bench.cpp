// MoonUp bench: runs the real engine pipeline (same sources, same shaders) without capture
// or overlay, on PNG sequences. Used to verify the GPU passes and to measure frame generation
// quality against ground truth (interpolate frame i from i-1 and i+1).
//
//   bench startup
//   bench process <in.png> <outW> <outH> <upscaler 0-9> <out.png> [sharpness]   (SW_RENDER=v or tone,color,structure)
//   bench interp  <dir> <outdir> [quality 0-2]     dir holds 0000.png, 0001.png, ...
#include <windows.h>
#include <wincodec.h>

#include <cmath>
#include <tuple>
#include <cstdio>
#include <string>
#include <mutex>
#include <vector>

#include "../../src/common.h"
#include "../../src/engine/cursor.h"
#include "../../src/engine/d3d.h"
#include "../../src/engine/gpu_timer.h"
#include "../../src/engine/hud.h"
#include "../../src/engine/motion.h"
#include "../../src/engine/processor.h"
#include "../../src/engine/engine.h"
#include "../../src/engine_client.h"

using namespace sw;

static ComPtr<IWICImagingFactory> g_wic;

static bool LoadPng(const std::wstring& path, std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> fr;
    dec->GetFrame(0, &fr);
    ComPtr<IWICFormatConverter> conv;
    g_wic->CreateFormatConverter(&conv);
    conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    conv->GetSize(&w, &h);
    bgra.resize((size_t)w * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data()));
}

bool BenchLoadPng(const std::wstring& path, std::vector<uint8_t>& bgra, UINT& w, UINT& h);
bool BenchSavePng(const std::wstring& path, const uint8_t* bgra, UINT w, UINT h);
int UpgraphTest(int argc, wchar_t** argv, bool compare);
int UpgraphInstallTest(int argc, wchar_t** argv);

static bool SavePng(const std::wstring& path, const uint8_t* bgra, UINT w, UINT h) {
    ComPtr<IWICStream> st;
    g_wic->CreateStream(&st);
    if (FAILED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(st.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> fr;
    enc->CreateNewFrame(&fr, nullptr);
    fr->Initialize(nullptr);
    fr->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&fmt);
    fr->WritePixels(h, w * 4, w * h * 4, (BYTE*)bgra);
    fr->Commit();
    return SUCCEEDED(enc->Commit());
}

static bool Upload(Gpu& gpu, const std::vector<uint8_t>& px, UINT w, UINT h, Texture& t) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA s{px.data(), w * 4, 0};
    t.Reset();
    if (FAILED(gpu.device->CreateTexture2D(&d, &s, &t.tex))) return false;
    gpu.device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
    t.width = w;
    t.height = h;
    t.format = d.Format;
    return true;
}

// Reads back an RGBA8 or BGRA8 texture as BGRA.
static std::vector<uint8_t> Readback(Gpu& gpu, Texture& t) {
    D3D11_TEXTURE2D_DESC d;
    t.tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    gpu.device->CreateTexture2D(&d, nullptr, &st);
    gpu.ctx->CopyResource(st.Get(), t.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    std::vector<uint8_t> out((size_t)d.Width * d.Height * 4);
    if (SUCCEEDED(gpu.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < d.Height; y++) memcpy(&out[(size_t)y * d.Width * 4], (uint8_t*)m.pData + (size_t)y * m.RowPitch, d.Width * 4);
        gpu.ctx->Unmap(st.Get(), 0);
    }
    if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
        for (size_t i = 0; i < out.size(); i += 4) std::swap(out[i], out[i + 2]);
    return out;
}

static double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double se = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < 3; c++) {
            double d = (double)a[i + c] - b[i + c];
            se += d * d;
            n++;
        }
    double mse = se / std::max<size_t>(1, n);
    return mse <= 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

struct Ctx {
    Gpu gpu;
    ShaderLibrary lib;
    CommonStates states;
    bool Init() {
        if (!gpu.Create(-1)) {
            printf("device failed\n");
            return false;
        }
        printf("device: %s FL %x\n", gpu.info.name.c_str(), gpu.featureLevel);
        lib.Init(gpu.device.Get());
        states.Create(gpu.device.Get());
        return true;
    }
};

// Compiles every shader with d3dcompiler_47 and writes the DXBC blobs + an index for
// tools/embed_shader_blobs.py (precompiled shaders shipped inside MoonUp.exe).
static int CompileAll(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    std::wstring dir = argv[2];
    CreateDirectoryW(dir.c_str(), nullptr);
    ShaderLibrary lib;
    lib.Init(nullptr);
    std::vector<std::tuple<std::string, std::string, std::string>> all;
    for (auto& e : AllComputeShaders()) all.emplace_back(e.file, e.entry, "cs_5_0");
    all.emplace_back("present.hlsl", "VSFull", "vs_5_0");
    all.emplace_back("present.hlsl", "PSFrame", "ps_5_0");
    all.emplace_back("present.hlsl", "VSQuad", "vs_5_0");
    all.emplace_back("present.hlsl", "PSQuad", "ps_5_0");
    FILE* idx = _wfopen((dir + L"\\index.txt").c_str(), L"w");
    int n = 0, fails = 0;
    double total = 0;
    for (auto& [file, entry, profile] : all) {
        std::string blob;
        uint64_t hash = 0;
        double t0 = NowSeconds();
        bool ok = lib.Precompile(file.c_str(), entry.c_str(), profile.c_str(), blob, hash);
        double ms = (NowSeconds() - t0) * 1000;
        total += ms;
        printf("%-14s %-16s %s %8.0f ms %7zu bytes\n", file.c_str(), entry.c_str(), ok ? "ok  " : "FAIL", ms, blob.size());
        if (!ok) {
            fails++;
            printf("%s\n", lib.LastError().c_str());
            continue;
        }
        char name[32];
        snprintf(name, sizeof(name), "%03d.cso", n++);
        FILE* f = _wfopen((dir + L"\\" + Wide(name)).c_str(), L"wb");
        fwrite(blob.data(), 1, blob.size(), f);
        fclose(f);
        fprintf(idx, "%s:%s:%s %016llx %s\n", file.c_str(), entry.c_str(), profile.c_str(), (unsigned long long)hash, name);
    }
    fclose(idx);
    printf("compiled %d shaders, %d failed, %.1f s\n", n, fails, total / 1000);
    return fails ? 1 : 0;
}

static int Startup() {
    Ctx c;
    if (!c.Init()) return 1;
    for (auto& e : AllComputeShaders()) {
        bool ok = c.lib.CS(e.file, e.entry) != nullptr;
        printf("CS %s %s: %s %s\n", e.file, e.entry, ok ? "ok" : "FAIL", ok ? "" : c.lib.LastError().c_str());
    }
    printf("VS/PS: %d %d %d %d\n", !!c.lib.VS("present.hlsl", "VSFull"), !!c.lib.PS("present.hlsl", "PSFrame"),
           !!c.lib.VS("present.hlsl", "VSQuad"), !!c.lib.PS("present.hlsl", "PSQuad"));
    Processor proc;
    printf("proc.Init %d neural %d\n", proc.Init(c.gpu, c.lib, c.states), proc.NeuralAvailable());
    Motion motion;
    printf("motion.Init %d\n", motion.Init(c.gpu, c.lib, c.states));
    GpuTimer timer;
    printf("timer %d\n", timer.Init(c.gpu.device.Get()));
    Hud hud;
    printf("hud %d\n", hud.Init(c.gpu));
    // Wine has no Magnification API; the cursor path is Windows-only.
    if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
        CursorRenderer cursor;
        printf("cursor %d\n", (int)cursor.Init(c.gpu));
    }
    fflush(stdout);
    return 0;
}

static int Process(int argc, wchar_t** argv) {
    if (argc < 7) return 2;
    Ctx c;
    if (!c.Init()) return 1;
    std::vector<uint8_t> px;
    UINT w, h;
    if (!LoadPng(argv[2], px, w, h)) {
        printf("load failed\n");
        return 1;
    }
    Texture src;
    Upload(c.gpu, px, w, h, src);
    Processor proc;
    proc.Init(c.gpu, c.lib, c.states);
    EngineConfig cfg;
    cfg.upscaler = (Upscaler)_wtoi(argv[5]);
    cfg.sharpness = argc > 7 ? (float)_wtof(argv[7]) : 0.0f;
    if (const char* r = getenv("SW_RENDER")) {
        cfg.render.enabled = true;
        // SW_RENDER=v (tone = colour = structure = v) or SW_RENDER=tone,color,structure
        float v[3];
        int n = sscanf(r, "%f,%f,%f", &v[0], &v[1], &v[2]);
        cfg.render.tone = v[0];
        cfg.render.color = n == 3 ? v[1] : v[0];
        cfg.render.structure = n == 3 ? v[2] : v[0];
        cfg.render.temporal = 0.0f;
    }
    UINT ow = _wtoi(argv[3]), oh = _wtoi(argv[4]);
    Texture out;
    out.Create(c.gpu.device.Get(), ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    double t0 = NowSeconds();
    bool ok = proc.Run(src.srv.Get(), w, h, out, cfg, nullptr);
    auto res = Readback(c.gpu, out);
    printf("process %d (%s) neural %d render %d %.1f ms\n", ok, proc.Error().c_str(), proc.NeuralAvailable(), proc.RenderAvailable(),
           (NowSeconds() - t0) * 1000);
    SavePng(argv[6], res.data(), ow, oh);
    return ok ? 0 : 1;
}

static int Interp(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    Ctx c;
    if (!c.Init()) return 1;
    std::wstring dir = argv[2], outDir = argv[3];
    FlowQuality q = argc > 4 ? (FlowQuality)_wtoi(argv[4]) : FlowQuality::Balanced;
    std::vector<std::vector<uint8_t>> frames;
    UINT w = 0, h = 0;
    for (int i = 0;; i++) {
        wchar_t name[64];
        swprintf(name, 64, L"\\%04d.png", i);
        std::vector<uint8_t> px;
        UINT fw, fh;
        if (!LoadPng(dir + name, px, fw, fh)) break;
        w = fw;
        h = fh;
        frames.push_back(std::move(px));
    }
    printf("frames %zu %ux%u\n", frames.size(), w, h);
    Motion motion;
    motion.Init(c.gpu, c.lib, c.states);
    auto envf = [](const char* n, float d) { const char* v = getenv(n); return v ? (float)atof(v) : d; };
    auto& tp = motion.Tuning();
    tp.lambda = envf("SW_LAMBDA", tp.lambda);
    tp.zeroBias = envf("SW_ZB", tp.zeroBias);
    tp.meanRemoval = envf("SW_MR", tp.meanRemoval);
    tp.propagate = (int)envf("SW_PROP", (float)tp.propagate);
    tp.sigma = envf("SW_SIGMA", tp.sigma);
    tp.interpZeroBias = envf("SW_IZB", tp.interpZeroBias);
    tp.staticEps = envf("SW_SEPS", tp.staticEps);
    tp.staticFrames = envf("SW_SFR", tp.staticFrames);
    tp.refineStep = envf("SW_REF", tp.refineStep);
    EngineConfig cfg;
    cfg.flowQuality = q;
    const UINT rw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    Texture gen;
    gen.Create(c.gpu.device.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, rw);
    double sumI = 0, sumB = 0, sumR = 0;
    int n = 0;
    // The game frames are the even ones; they are fed in order exactly like the engine does,
    // and every odd frame is the ground truth for t = 0.5 between its neighbours.
    std::vector<Texture> src(frames.size());
    motion.Reset();
    uint64_t seq = 0;
    for (size_t i = 0; i < frames.size(); i += 2) {
        Upload(c.gpu, frames[i], w, h, src[i]);
        double t0 = NowSeconds();
        motion.AddFrame(src[i].srv.Get(), w, h, q, seq);
        if (i == 0) { seq++; continue; }
        bool ok = motion.Interpolate(seq, src[i - 2].srv.Get(), src[i].srv.Get(), 0.5f, gen, cfg);
        if (getenv("SW_DUMPFLOW")) {
            for (int dir = 0; dir < 2; dir++) {
                Texture* f = motion.DebugFlow(seq, dir == 0);
                if (!f) continue;
                D3D11_TEXTURE2D_DESC d;
                f->tex->GetDesc(&d);
                d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
                ComPtr<ID3D11Texture2D> st;
                c.gpu.device->CreateTexture2D(&d, nullptr, &st);
                c.gpu.ctx->CopyResource(st.Get(), f->tex.Get());
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(c.gpu.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                    wchar_t name[64];
                    swprintf(name, 64, L"\\flow%s_%04zu_%ux%u.bin", dir == 0 ? L"AB" : L"BA", i - 1, d.Width, d.Height);
                    FILE* fo = _wfopen((outDir + name).c_str(), L"wb");
                    for (UINT y = 0; y < d.Height; y++) fwrite((uint8_t*)m.pData + (size_t)y * m.RowPitch, 4, d.Width, fo);
                    fclose(fo);
                    c.gpu.ctx->Unmap(st.Get(), 0);
                }
            }
        }
        seq++;
        auto res = Readback(c.gpu, gen);
        double ms = (NowSeconds() - t0) * 1000;
        size_t m = i - 1;
        std::vector<uint8_t> blend(frames[m].size());
        for (size_t k = 0; k < blend.size(); k++) blend[k] = (uint8_t)((frames[m - 1][k] + frames[m + 1][k] + 1) / 2);
        double pi = Psnr(res, frames[m]), pb = Psnr(blend, frames[m]), pr = Psnr(frames[m - 1], frames[m]);
        printf("frame %zu: interp %d %.2f dB | blend %.2f | repeat %.2f | %.0f ms\n", m, ok, pi, pb, pr, ms);
        sumI += pi;
        sumB += pb;
        sumR += pr;
        n++;
        wchar_t name[64];
        swprintf(name, 64, L"\\gen_%04zu.png", m);
        SavePng(outDir + name, res.data(), w, h);
    }
    if (n) printf("MEAN interp %.2f dB | blend %.2f | repeat %.2f\n", sumI / n, sumB / n, sumR / n);
    return 0;
}


// ------------------------------------------------------------------ end-to-end engine test
// Opens a window that plays a PNG sequence at a fixed rate (the "game"), runs the real
// Engine on it (GDI capture under Wine), changes settings while it runs and prints events.
static std::vector<std::vector<uint8_t>> g_movie;
static UINT g_mw = 0, g_mh = 0;
static int g_frame = 0;

static LRESULT CALLBACK GameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (!g_movie.empty()) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = (LONG)g_mw;
            bi.bmiHeader.biHeight = -(LONG)g_mh;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            const auto& f = g_movie[(g_frame / 2 * 2) % g_movie.size()];  // even frames only: the "game" rate
            SetDIBitsToDevice(dc, 0, 0, g_mw, g_mh, 0, 0, 0, g_mh, f.data(), &bi, DIB_RGB_COLORS);
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER) {
        g_frame += 2;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int EngineTest(int argc, wchar_t** argv) {
    std::wstring dir = argc > 2 ? argv[2] : L"";
    double seconds = argc > 3 ? _wtof(argv[3]) : 6.0;
    for (int i = 0;; i++) {
        wchar_t name[64];
        swprintf(name, 64, L"\\%04d.png", i);
        std::vector<uint8_t> px;
        UINT fw, fh;
        if (!LoadPng(dir + name, px, fw, fh)) break;
        g_mw = fw;
        g_mh = fh;
        g_movie.push_back(std::move(px));
    }
    printf("movie %zu frames %ux%u\n", g_movie.size(), g_mw, g_mh);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = GameProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BenchGame";
    RegisterClassExW(&wc);
    RECT r{0, 0, (LONG)g_mw, (LONG)g_mh};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND game = CreateWindowExW(0, L"BenchGame", L"Bench Game", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    SetTimer(game, 1, 33, nullptr);  // ~30 fps game
    SetForegroundWindow(game);

    EngineConfig cfg;
    cfg.capture = CaptureApi::GDI;
    cfg.scaleMode = ScaleMode::Auto;
    cfg.upscaler = Upscaler::Edge;
    cfg.frameGen = FrameGenMode::Adaptive;
    if (const char* pm = getenv("SW_PACING")) {  // pacing test: fixed multiplier, no scaling
        cfg.frameGen = (FrameGenMode)atoi(pm);
        cfg.scaleMode = ScaleMode::Off;
        cfg.flowQuality = FlowQuality::Performance;
    }
    cfg.vsync = false;
    cfg.targetFps = 60;
    cfg.pauseWhenUnfocused = false;
    Engine engine;
    std::mutex mu;
    std::vector<std::string> events;
    engine.Start(game, cfg, [&](const std::string& j) {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(j);
    });
    double t0 = NowSeconds();
    int phase = 0;
    const char* phases[] = {"adaptive+edge", "x2+neural+render", "x3+anime+vision", "off+fsr", "x4+nis+quality"};
    while (NowSeconds() - t0 < seconds) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        int want = getenv("SW_PACING") ? 0 : (int)((NowSeconds() - t0) / (seconds / 5));
        if (want != phase && want < 5) {
            phase = want;
            EngineConfig n = cfg;
            if (phase == 1) { n.frameGen = FrameGenMode::X2; n.upscaler = Upscaler::Neural; n.render.enabled = true; }
            if (phase == 2) { n.frameGen = FrameGenMode::X3; n.upscaler = Upscaler::Anime; n.vision.enabled = true; }
            if (phase == 3) { n.frameGen = FrameGenMode::Off; n.upscaler = Upscaler::Fsr; }
            if (phase == 4) { n.frameGen = FrameGenMode::X4; n.flowQuality = FlowQuality::Quality; n.upscaler = Upscaler::Nis; }
            engine.UpdateConfig(n);
            printf("-- phase %s\n", phases[phase]);
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            for (auto& e : events) {
                if (e.find("\"stats\"") != std::string::npos) {
                    // compact: base/out fps and gpu ms
                    size_t a = e.find("\"baseFps\""), b = e.find("\"outFps\"");
                    printf("stats %s | %s\n", e.substr(a, 16).c_str(), e.substr(b, 15).c_str());
                } else {
                    printf("event %s\n", e.c_str());
                }
            }
            events.clear();
        }
        Sleep(20);
    }
    if (getenv("SW_SHOT")) system("cmd /c echo shot");
    engine.Stop();
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : events) printf("event %s\n", e.c_str());
    }
    printf("engine test done, running=%d\n", (int)engine.Running());
    DestroyWindow(game);
    return 0;
}

// ------------------------------------------------------------------ engine host process test
static std::vector<std::string> g_hostEvents;
static sw::EngineClient* g_client = nullptr;
static LRESULT CALLBACK ParentProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA && g_client && g_client->OnCopyData((HWND)w, (const COPYDATASTRUCT*)l)) return TRUE;
    if (m == sw::WM_APP_ENGINE) {
        std::unique_ptr<std::string> s((std::string*)l);
        g_hostEvents.push_back(*s);
        return 0;
    }
    if (m == sw::WM_APP_ENGINE_EXIT) {
        printf("host exit pid %lu code 0x%lx\n", (DWORD)w, (DWORD)l);
        if (g_client) g_client->OnProcessExit((DWORD)w, (DWORD)l);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void Pump(double seconds) {
    double end = NowSeconds() + seconds;
    while (NowSeconds() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        for (auto& e : g_hostEvents) {
            if (e.find("\"stats\"") != std::string::npos) {
                size_t a = e.find("\"outFps\"");
                printf("stats %s\n", e.substr(a, 14).c_str());
            } else
                printf("event %s\n", e.c_str());
        }
        g_hostEvents.clear();
        Sleep(15);
    }
}

static int HostTest(int argc, wchar_t** argv) {
    std::wstring dir = argc > 2 ? argv[2] : L"";
    for (int i = 0;; i++) {
        wchar_t name[64];
        swprintf(name, 64, L"\\%04d.png", i);
        std::vector<uint8_t> px;
        UINT fw, fh;
        if (!LoadPng(dir + name, px, fw, fh)) break;
        g_mw = fw;
        g_mh = fh;
        g_movie.push_back(std::move(px));
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = GameProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BenchGame";
    RegisterClassExW(&wc);
    WNDCLASSEXW pc{sizeof(pc)};
    pc.lpfnWndProc = ParentProc;
    pc.hInstance = wc.hInstance;
    pc.lpszClassName = L"BenchParent";
    RegisterClassExW(&pc);
    HWND parent = CreateWindowExW(0, L"BenchParent", L"Bench Parent", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    HWND game = CreateWindowExW(0, L"BenchGame", L"Bench Game", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, g_mw + 16, g_mh + 39,
                                nullptr, nullptr, wc.hInstance, nullptr);
    SetTimer(game, 1, 33, nullptr);
    sw::EngineClient client;
    g_client = &client;
    nlohmann::json profile = {{"capture", "gdi"}, {"frameGen", "adaptive"}, {"upscaler", "edge"}, {"vsync", false},
                              {"targetFps", 60}, {"pauseWhenUnfocused", false}};
    printf("== start\n");
    client.Start(parent, game, profile);
    Pump(6);
    printf("== live config change (x2, neural)\n");
    profile["frameGen"] = "x2";
    profile["upscaler"] = "neural";
    client.UpdateConfig(profile);
    Pump(4);
    printf("== stop, running=%d\n", (int)client.Running());
    client.Stop();
    Pump(1);
    printf("== restart and simulate a crash of the host\n");
    client.Start(parent, game, profile);
    Pump(4);
    HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, client.HostPid());
    if (p) {
        TerminateProcess(p, 0xC0000005);
        CloseHandle(p);
    }
    Pump(2);
    printf("== after crash running=%d\n", (int)client.Running());
    g_client = nullptr;
    DestroyWindow(game);
    DestroyWindow(parent);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 2 && std::wstring(argv[1]) == L"--engine-host") {
        LogInit(L"moonup-engine");
        return sw::EngineHostMain(argv[2]);
    }
    LogInit();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    if (argc < 2) return 2;
    std::wstring mode = argv[1];
    if (mode == L"startup") return Startup();
    if (mode == L"compileall") return CompileAll(argc, argv);
    if (mode == L"process") return Process(argc, argv);
    if (mode == L"interp") return Interp(argc, argv);
    if (mode == L"engine") return EngineTest(argc, argv);
    if (mode == L"hosttest") return HostTest(argc, argv);
    if (mode == L"upgraphinstall") return UpgraphInstallTest(argc, argv);
    if (mode == L"upgraph") return UpgraphTest(argc, argv, false);
    if (mode == L"upgraphcmp") return UpgraphTest(argc, argv, true);
    return 2;
}

bool BenchLoadPng(const std::wstring& path, std::vector<uint8_t>& bgra, UINT& w, UINT& h) { return LoadPng(path, bgra, w, h); }
bool BenchSavePng(const std::wstring& path, const uint8_t* bgra, UINT w, UINT h) { return SavePng(path, bgra, w, h); }

#include "../../src/upgraph/upgraph.h"
// bench upgraphinstall <gamedir> [optiscaler]
int UpgraphInstallTest(int argc, wchar_t** argv) {
    using namespace sw::upgraph;
    std::wstring dir = argv[2];
    bool opti = argc > 3;
    try {
        json lib = ScanLibrary(json::array({Utf8(dir)}));
        printf("library: %s\n", lib.dump().c_str());
        json s = ScanGame(dir);
        printf("scan: %s\n", s.dump(1).c_str());
        json o{{"shader", true}, {"optiscaler", opti && !_wgetenv(L"UG_DLSS5")}, {"frameGen", true}, {"dlss5", _wgetenv(L"UG_DLSS5") != nullptr}, {"ignoreGpu", true}};
        json r = Install(dir, o, [](const std::string& st, double f) { printf("  progress %s %.2f\n", st.c_str(), f); });
        printf("installed: %s\n", r["status"].dump(1).c_str());
        if (argc > 4) return 0;
        json rm = Remove(dir);
        printf("removed %d restored %d, status %s\n", rm.value("removed", 0), rm.value("restored", 0), rm["status"].dump().c_str());
    } catch (const std::exception& e) {
        printf("ERROR %s: %s\n", ErrorCode(e).c_str(), e.what());
        return 1;
    }
    return 0;
}
