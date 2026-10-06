#include "processor.h"

#include "neural.h"
#include "render.h"
#include "../third_party/nis/NIS_Config.h"

namespace sw {

namespace {
struct UpscaleCB {
    float srcSize[2], invSrcSize[2], dstSize[2], invDstSize[2];
    float params[4];
};
struct PostCB {
    float size[2], invSize[2];
    float p0[4];
    float p1[4];
    float srcSize[2], invSrcSize[2];
};
}  // namespace

Processor::Processor() = default;
Processor::~Processor() { Shutdown(); }

bool Processor::Init(Gpu& gpu, ShaderLibrary& lib, CommonStates& states) {
    gpu_ = &gpu;
    lib_ = &lib;
    states_ = &states;
    if (!cb_.Create(gpu.device.Get(), 256)) return false;
    neural_ = new NeuralUpscaler();
    if (!neural_->Init(gpu, lib)) {
        SW_LOG("Neural upscaler unavailable: %s", neural_->Error().c_str());
    }
    if (!CreateNisTables(gpu.device.Get())) SW_LOG("NIS coefficient tables could not be created");
    render_ = new NeuralRender();
    if (!render_->Init(gpu, lib)) {
        SW_LOG("Neural Render unavailable: %s", render_->Error().c_str());
    }
    return true;
}

void Processor::Shutdown() {
    delete neural_;
    neural_ = nullptr;
    delete render_;
    render_ = nullptr;
    rendered_.Reset();
    vision_.Reset();
    quarterA_.Reset();
    quarterB_.Reset();
    up_.Reset();
    nisScaler_.Reset();
    nisUsm_.Reset();
}

// NVIDIA Image Scaling filter banks: 64 phases x 8 taps, read as 2 x 64 RGBA32F textures.
bool Processor::CreateNisTables(ID3D11Device* dev) {
    auto make = [&](const float* data, ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kFilterSize / 4;
        d.Height = kPhaseCount;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{data, (UINT)(kFilterSize * sizeof(float)), 0};
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(dev->CreateTexture2D(&d, &init, &t))) return false;
        return SUCCEEDED(dev->CreateShaderResourceView(t.Get(), nullptr, &srv));
    };
    return make(&coef_scale[0][0], nisScaler_) && make(&coef_usm[0][0], nisUsm_);
}

bool Processor::NeuralAvailable() const { return neural_ && neural_->Ready(); }
bool Processor::RenderAvailable() const { return render_ && render_->Ready(); }

bool Processor::Dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs, UINT nSrv,
                         ID3D11UnorderedAccessView* uav, UINT w, UINT h, const void* cb, size_t cbSize) {
    if (!cs) {
        error_ = lib_->LastError();
        return false;
    }
    auto* ctx = gpu_->ctx.Get();
    cb_.Update(ctx, cb, cbSize);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cb_.Addr());
    ctx->CSSetShaderResources(0, nSrv, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->Dispatch(DivUp(w, 8), DivUp(h, 8), 1);
    ClearCS(ctx);
    return true;
}

static const char* UpscalerFile(Upscaler u) { return u == Upscaler::Fsr ? "fsr.hlsl" : "upscale.hlsl"; }

static const char* UpscalerEntry(Upscaler u) {
    switch (u) {
        case Upscaler::Edge: return "CSEdge";
        case Upscaler::Lanczos: return "CSLanczos";
        case Upscaler::Bicubic: return "CSBicubic";
        case Upscaler::Bilinear: return "CSBilinear";
        case Upscaler::Nearest: return "CSNearest";
        case Upscaler::PixelArt: return "CSPixel";
        case Upscaler::Fsr: return "CSEasu";
        default: return "CSEdge";
    }
}

bool Processor::Run(ID3D11ShaderResourceView* src, UINT srcW, UINT srcH, Texture& out, const EngineConfig& cfg,
                    GpuTimer* timer) {
    auto* dev = gpu_->device.Get();
    const UINT rwBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    states_->BindSamplers(gpu_->ctx.Get());

    // ---------------------------------------------------------------- Neural Render (source resolution)
    ID3D11ShaderResourceView* cur = src;
    bool renderOn = cfg.render.Active() && RenderAvailable();
    if (renderOn) {
        if (!renderWasOn_ || rendered_.width != srcW || rendered_.height != srcH) render_->Reset();
        rendered_.Ensure(dev, srcW, srcH, DXGI_FORMAT_R8G8B8A8_UNORM, rwBind);
        if (render_->Run(cur, srcW, srcH, rendered_, cfg.render)) cur = rendered_.srv.Get();
        states_->BindSamplers(gpu_->ctx.Get());
    }
    renderWasOn_ = renderOn;

    // ---------------------------------------------------------------- Vision (source resolution)
    if (cfg.vision.enabled) {
        UINT qw = std::max(1u, srcW / 4), qh = std::max(1u, srcH / 4);
        vision_.Ensure(dev, srcW, srcH, DXGI_FORMAT_R8G8B8A8_UNORM, rwBind);
        quarterA_.Ensure(dev, qw, qh, DXGI_FORMAT_R8G8B8A8_UNORM, rwBind);
        quarterB_.Ensure(dev, qw, qh, DXGI_FORMAT_R8G8B8A8_UNORM, rwBind);
        PostCB p{};
        p.size[0] = (float)qw;
        p.size[1] = (float)qh;
        p.invSize[0] = 1.0f / qw;
        p.invSize[1] = 1.0f / qh;
        p.srcSize[0] = (float)srcW;
        p.srcSize[1] = (float)srcH;
        p.invSrcSize[0] = 1.0f / srcW;
        p.invSrcSize[1] = 1.0f / srcH;
        if (!Dispatch(lib_->CS("post.hlsl", "CSDown4"), &cur, 1, quarterA_.uav.Get(), qw, qh, &p, sizeof(p))) return false;
        p.p0[0] = 1;
        p.p0[1] = 0;
        Dispatch(lib_->CS("post.hlsl", "CSBlur"), quarterA_.srv.GetAddressOf(), 1, quarterB_.uav.Get(), qw, qh, &p, sizeof(p));
        p.p0[0] = 0;
        p.p0[1] = 1;
        Dispatch(lib_->CS("post.hlsl", "CSBlur"), quarterB_.srv.GetAddressOf(), 1, quarterA_.uav.Get(), qw, qh, &p, sizeof(p));

        PostCB v{};
        v.size[0] = (float)srcW;
        v.size[1] = (float)srcH;
        v.invSize[0] = 1.0f / srcW;
        v.invSize[1] = 1.0f / srcH;
        v.p0[0] = cfg.vision.clarity;
        v.p0[1] = cfg.vision.detail;
        v.p0[2] = cfg.vision.vibrance;
        v.p0[3] = cfg.vision.contrast;
        v.p1[0] = cfg.vision.warmth;
        v.p1[1] = cfg.vision.brightness;
        ID3D11ShaderResourceView* vs[2] = {cur, quarterA_.srv.Get()};
        if (!Dispatch(lib_->CS("post.hlsl", "CSVision"), vs, 2, vision_.uav.Get(), srcW, srcH, &v, sizeof(v))) return false;
        cur = vision_.srv.Get();
    }
    sourceView_ = cur;
    if (timer) timer->Mark(gpu_->ctx.Get());

    // ---------------------------------------------------------------- upscale
    const UINT outW = out.width, outH = out.height;
    bool sameSize = outW == srcW && outH == srcH;
    bool downscale = outW < srcW || outH < srcH;
    Upscaler algo = cfg.upscaler;
    if ((algo == Upscaler::Neural || algo == Upscaler::Anime) && !(NeuralAvailable() && outW > srcW)) algo = Upscaler::Edge;
    // NIS covers 1x..2x (its own adaptive sharpening included); above that FSR takes over.
    if (algo == Upscaler::Nis && (sameSize || downscale || outW > srcW * 2 || outH > srcH * 2 || !nisScaler_))
        algo = Upscaler::Fsr;
    if (algo == Upscaler::Fsr && !sameSize && !downscale && !lib_->CS("fsr.hlsl", "CSEasu")) {
        if (!fsrWarned_) SW_LOG("FSR unavailable, using Edge: %s", lib_->LastError().c_str());
        fsrWarned_ = true;
        algo = Upscaler::Edge;
    }

    if (algo == Upscaler::Nis && !sameSize) {
        NISConfig nc{};
        NVScalerUpdateConfig(nc, std::clamp(cfg.sharpness, 0.0f, 1.0f), 0, 0, srcW, srcH, srcW, srcH, 0, 0, outW, outH,
                             outW, outH);
        ID3D11ComputeShader* cs = lib_->CS("nis.hlsl", "CSNis");
        if (cs) {
            auto* ctx = gpu_->ctx.Get();
            cb_.Update(ctx, &nc, sizeof(nc));
            ctx->CSSetShader(cs, nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, cb_.Addr());
            ID3D11ShaderResourceView* srvs[3] = {cur, nisScaler_.Get(), nisUsm_.Get()};
            ctx->CSSetShaderResources(0, 3, srvs);
            ID3D11UnorderedAccessView* uav = out.uav.Get();
            ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            ctx->Dispatch(DivUp(outW, 32), DivUp(outH, 24), 1);
            ClearCS(ctx);
            if (timer) timer->Mark(gpu_->ctx.Get());
            return true;
        }
        SW_LOG("NIS unavailable, using FSR: %s", lib_->LastError().c_str());
        algo = Upscaler::Fsr;
        if (!lib_->CS("fsr.hlsl", "CSEasu")) algo = Upscaler::Edge;
    }

    bool sharpen = cfg.sharpness > 0.01f;
    Texture& upTarget = sharpen ? up_ : out;
    if (sharpen) up_.Ensure(dev, outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM, rwBind);

    UpscaleCB u{};
    u.srcSize[0] = (float)srcW;
    u.srcSize[1] = (float)srcH;
    u.invSrcSize[0] = 1.0f / srcW;
    u.invSrcSize[1] = 1.0f / srcH;
    u.dstSize[0] = (float)outW;
    u.dstSize[1] = (float)outH;
    u.invDstSize[0] = 1.0f / outW;
    u.invDstSize[1] = 1.0f / outH;
    u.params[0] = 1.0f;   // edge sensitivity
    u.params[1] = 0.85f;  // anti-ringing

    bool done = false;
    if (sameSize) {
        done = Dispatch(lib_->CS("upscale.hlsl", "CSCopy"), &cur, 1, upTarget.uav.Get(), outW, outH, &u, sizeof(u));
    } else if (algo == Upscaler::Neural || algo == Upscaler::Anime) {
        done = neural_->Run(cur, srcW, srcH, upTarget,
                            algo == Upscaler::Anime ? NeuralUpscaler::kArtCNN : NeuralUpscaler::kMoonUp);
        if (!done) algo = Upscaler::Edge;
    }
    if (!done && !sameSize) {
        // Downscaling: bilinear sampling of the 4x4 footprint is enough.
        bool bil = downscale && algo != Upscaler::Nearest;
        const char* file = bil ? "upscale.hlsl" : UpscalerFile(algo);
        const char* entry = bil ? "CSBilinear" : UpscalerEntry(algo);
        done = Dispatch(lib_->CS(file, entry), &cur, 1, upTarget.uav.Get(), outW, outH, &u, sizeof(u));
    }
    if (!done) return false;
    if (timer) timer->Mark(gpu_->ctx.Get());

    // ---------------------------------------------------------------- sharpen
    if (sharpen) {
        PostCB p{};
        p.size[0] = (float)outW;
        p.size[1] = (float)outH;
        p.invSize[0] = 1.0f / outW;
        p.invSize[1] = 1.0f / outH;
        p.p0[0] = cfg.sharpness;
        if (algo == Upscaler::Fsr && lib_->CS("fsr.hlsl", "CSRcas")) {
            // RCAS: sharpness 1 = 0 stops (maximum), 0 = 2 stops of reduction.
            UpscaleCB r = u;
            r.srcSize[0] = r.dstSize[0];
            r.srcSize[1] = r.dstSize[1];
            r.params[0] = std::exp2(-2.0f * (1.0f - std::clamp(cfg.sharpness, 0.0f, 1.0f)));
            if (!Dispatch(lib_->CS("fsr.hlsl", "CSRcas"), up_.srv.GetAddressOf(), 1, out.uav.Get(), outW, outH, &r, sizeof(r)))
                return false;
        } else if (!Dispatch(lib_->CS("post.hlsl", "CSSharpen"), up_.srv.GetAddressOf(), 1, out.uav.Get(), outW, outH, &p,
                             sizeof(p))) {
            return false;
        }
    }
    return true;
}

}  // namespace sw
