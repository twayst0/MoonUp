#include "motion.h"

namespace sw {

namespace {
struct FlowCB {
    int size[2];
    float invSize[2];
    int srcSize[2];
    float invSrcSize[2];
    int hasCoarse;
    int radius;
    float lambda;
    float zeroBias;
    int hasPrev;
    float prevScale;
    float meanRemoval;
    float pad0;
};
struct InterpCB {
    float outSize[2], invOutSize[2], flowScale[2];
    float t, sigma, zeroBias, sceneThreshold;
    int sceneCut, hudProtect;
    float flowSize[2];
    float selSize[2];
    float staticEps, staticFrames;
    float refineStep, pad0;
};
const UINT kRW = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
}  // namespace

bool Motion::Init(Gpu& gpu, ShaderLibrary& lib, CommonStates& states) {
    gpu_ = &gpu;
    lib_ = &lib;
    states_ = &states;
    Reset();
    return cb_.Create(gpu.device.Get(), 256);
}

void Motion::Reset() {
    for (auto& p : pyr_) p.seq = UINT64_MAX;
    for (auto& p : pairs_) p.seqB = UINT64_MAX;
}

void Motion::Run(ID3D11ComputeShader* cs, UINT w, UINT h, const void* cb, size_t cbSize) {
    auto* ctx = gpu_->ctx.Get();
    cb_.Update(ctx, cb, cbSize);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cb_.Addr());
    ctx->Dispatch(DivUp(w, 8), DivUp(h, 8), 1);
}

bool Motion::BuildPyramid(Pyramid& p, ID3D11ShaderResourceView* src, UINT w, UINT h) {
    auto* dev = gpu_->device.Get();
    auto* ctx = gpu_->ctx.Get();
    UINT lw = std::max(1u, w / 2), lh = std::max(1u, h / 2);
    p.levels = 0;
    while (p.levels < kMaxLevels) {
        p.level[p.levels].Ensure(dev, lw, lh, DXGI_FORMAT_R16_FLOAT, kRW);
        p.levels++;
        if (std::min(lw, lh) / 2 < 12) break;
        lw /= 2;
        lh /= 2;
    }
    ID3D11ComputeShader* luma = lib_->CS("flow.hlsl", "CSLuma");
    ID3D11ComputeShader* down = lib_->CS("flow.hlsl", "CSDown2");
    if (!luma || !down) {
        error_ = lib_->LastError();
        return false;
    }
    FlowCB cb{};
    // level 0 from RGB (bound at t3)
    Texture& l0 = p.level[0];
    cb.size[0] = (int)l0.width;
    cb.size[1] = (int)l0.height;
    cb.invSize[0] = 1.0f / l0.width;
    cb.invSize[1] = 1.0f / l0.height;
    cb.srcSize[0] = (int)w;
    cb.srcSize[1] = (int)h;
    cb.invSrcSize[0] = 1.0f / w;
    cb.invSrcSize[1] = 1.0f / h;
    ctx->CSSetShaderResources(3, 1, &src);
    ctx->CSSetUnorderedAccessViews(0, 1, l0.uav.GetAddressOf(), nullptr);
    Run(luma, l0.width, l0.height, &cb, sizeof(cb));
    ClearCS(ctx);
    for (int i = 1; i < p.levels; i++) {
        Texture& a = p.level[i - 1];
        Texture& b = p.level[i];
        cb.size[0] = (int)b.width;
        cb.size[1] = (int)b.height;
        cb.invSize[0] = 1.0f / b.width;
        cb.invSize[1] = 1.0f / b.height;
        cb.srcSize[0] = (int)a.width;
        cb.srcSize[1] = (int)a.height;
        cb.invSrcSize[0] = 1.0f / a.width;
        cb.invSrcSize[1] = 1.0f / a.height;
        ctx->CSSetShaderResources(0, 1, a.srv.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0, 1, b.uav.GetAddressOf(), nullptr);
        Run(down, b.width, b.height, &cb, sizeof(cb));
        ClearCS(ctx);
    }
    return true;
}

bool Motion::Estimate(const Pyramid& a, const Pyramid& b, int finest, Texture& result, Texture* scene, const Texture* prev) {
    auto* dev = gpu_->device.Get();
    auto* ctx = gpu_->ctx.Get();
    ID3D11ComputeShader* flow = lib_->CS("flow.hlsl", "CSFlow");
    ID3D11ComputeShader* prop = lib_->CS("flow.hlsl", "CSPropagate");
    ID3D11ComputeShader* refine = lib_->CS("flow.hlsl", "CSRefine");
    ID3D11ComputeShader* median = lib_->CS("flow.hlsl", "CSMedian");
    ID3D11ComputeShader* sceneCs = lib_->CS("flow.hlsl", "CSScene");
    if (!flow || !prop || !refine || !median || !sceneCs) {
        error_ = lib_->LastError();
        return false;
    }
    int levels = std::min(a.levels, b.levels);
    int coarsest = levels - 1;
    finest = std::min(finest, coarsest);
    const Texture& c0 = a.level[coarsest];
    cost_.Ensure(dev, c0.width, c0.height, DXGI_FORMAT_R16_FLOAT, kRW);

    auto pass = [&](ID3D11ComputeShader* cs, FlowCB& cb, ID3D11ShaderResourceView* flowIn, ID3D11UnorderedAccessView* out,
                    ID3D11UnorderedAccessView* cost, const Texture& la, const Texture& lb, ID3D11ShaderResourceView* coarse) {
        ID3D11ShaderResourceView* srvs[7] = {la.srv.Get(), lb.srv.Get(), coarse, nullptr, flowIn, nullptr,
                                             prev ? prev->srv.Get() : nullptr};
        ctx->CSSetShaderResources(0, 7, srvs);
        ID3D11UnorderedAccessView* uavs[3] = {nullptr, out, cost};
        ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        Run(cs, la.width, la.height, &cb, sizeof(cb));
        ClearCS(ctx);
    };

    for (int L = coarsest; L >= finest; L--) {
        const Texture& la = a.level[L];
        const Texture& lb = b.level[L];
        raw_[L].Ensure(dev, la.width, la.height, DXGI_FORMAT_R16G16_FLOAT, kRW);
        tmp_[L].Ensure(dev, la.width, la.height, DXGI_FORMAT_R16G16_FLOAT, kRW);
        Texture& dst = (L == finest) ? result : filtered_[L];
        if (L != finest) filtered_[L].Ensure(dev, la.width, la.height, DXGI_FORMAT_R16G16_FLOAT, kRW);

        FlowCB cb{};
        cb.size[0] = (int)la.width;
        cb.size[1] = (int)la.height;
        cb.invSize[0] = 1.0f / la.width;
        cb.invSize[1] = 1.0f / la.height;
        bool hasCoarse = L < coarsest;
        if (hasCoarse) {
            cb.srcSize[0] = (int)filtered_[L + 1].width;
            cb.srcSize[1] = (int)filtered_[L + 1].height;
            cb.invSrcSize[0] = 1.0f / filtered_[L + 1].width;
            cb.invSrcSize[1] = 1.0f / filtered_[L + 1].height;
        } else {
            cb.srcSize[0] = cb.srcSize[1] = 1;
            cb.invSrcSize[0] = cb.invSrcSize[1] = 1;
        }
        cb.hasCoarse = hasCoarse ? 1 : 0;
        cb.radius = levels >= 5 ? 4 : 6;
        cb.lambda = params_.lambda;
        cb.zeroBias = params_.zeroBias;
        cb.meanRemoval = params_.meanRemoval;
        cb.hasPrev = prev && *prev ? 1 : 0;
        cb.prevScale = prev && *prev ? (float)la.width / (float)prev->width : 0.0f;
        ID3D11ShaderResourceView* coarse = hasCoarse ? filtered_[L + 1].srv.Get() : nullptr;
        ID3D11UnorderedAccessView* cost = (L == coarsest) ? cost_.uav.Get() : nullptr;

        pass(flow, cb, nullptr, raw_[L].uav.Get(), nullptr, la, lb, coarse);
        Texture* cur = &raw_[L];
        Texture* other = &tmp_[L];
        for (int i = 0; i < params_.propagate; i++) {
            cb.radius = (i & 1) ? 3 : 1;
            pass(prop, cb, cur->srv.Get(), other->uav.Get(), nullptr, la, lb, coarse);
            std::swap(cur, other);
        }
        pass(refine, cb, cur->srv.Get(), other->uav.Get(), cost, la, lb, coarse);
        std::swap(cur, other);
        pass(median, cb, cur->srv.Get(), dst.uav.Get(), nullptr, la, lb, coarse);
    }

    if (scene) {
        FlowCB cb{};
        cb.size[0] = (int)cost_.width;
        cb.size[1] = (int)cost_.height;
        ctx->CSSetShaderResources(5, 1, cost_.srv.GetAddressOf());
        ID3D11UnorderedAccessView* su[4] = {nullptr, nullptr, nullptr, scene->uav.Get()};
        ctx->CSSetUnorderedAccessViews(0, 4, su, nullptr);
        cb_.Update(ctx, &cb, sizeof(cb));
        ctx->CSSetShader(sceneCs, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cb_.Addr());
        ctx->Dispatch(1, 1, 1);
        ClearCS(ctx);
    }
    return true;
}

bool Motion::AddFrame(ID3D11ShaderResourceView* source, UINT w, UINT h, FlowQuality q, uint64_t seq) {
    states_->BindSamplers(gpu_->ctx.Get());
    Pyramid& cur = pyr_[seq & 1];
    Pyramid& prev = pyr_[(seq + 1) & 1];
    if (!BuildPyramid(cur, source, w, h)) return false;
    cur.seq = seq;
    if (prev.seq == UINT64_MAX || prev.seq + 1 != seq || prev.level[0].width != cur.level[0].width ||
        prev.level[0].height != cur.level[0].height)
        return true;

    int finest = q == FlowQuality::Quality ? 0 : (q == FlowQuality::Balanced ? 1 : 2);
    finest = std::min(finest, cur.levels - 1);
    const Texture& lvl = cur.level[finest];
    Pair& pr = pairs_[seq & 1];
    auto* dev = gpu_->device.Get();
    pr.flowAB.Ensure(dev, lvl.width, lvl.height, DXGI_FORMAT_R16G16_FLOAT, kRW);
    pr.flowBA.Ensure(dev, lvl.width, lvl.height, DXGI_FORMAT_R16G16_FLOAT, kRW);
    pr.scene.Ensure(dev, 1, 1, DXGI_FORMAT_R32_FLOAT, kRW);
    // Temporal prediction: the previous pair's motion, when it is the directly preceding pair.
    const Pair& last = pairs_[(seq + 1) & 1];
    bool haveLast = last.seqB + 1 == seq && last.flowW == lvl.width && last.flowH == lvl.height;
    if (!Estimate(prev, cur, finest, pr.flowAB, &pr.scene, haveLast ? &last.flowAB : nullptr)) return false;
    if (!Estimate(cur, prev, finest, pr.flowBA, nullptr, haveLast ? &last.flowBA : nullptr)) return false;
    pr.seqB = seq;
    pr.flowW = lvl.width;
    pr.flowH = lvl.height;
    return true;
}

bool Motion::HasPair(uint64_t seqB) const { return pairs_[seqB & 1].seqB == seqB; }

bool Motion::Interpolate(uint64_t seqB, ID3D11ShaderResourceView* frameA, ID3D11ShaderResourceView* frameB, float t,
                         Texture& out, const EngineConfig& cfg) {
    const Pair& pr = pairs_[seqB & 1];
    if (pr.seqB != seqB) return false;
    ID3D11ComputeShader* sel = lib_->CS("interp.hlsl", "CSSelect");
    ID3D11ComputeShader* comp = lib_->CS("interp.hlsl", "CSCompose");
    ID3D11ComputeShader* stat = lib_->CS("interp.hlsl", "CSStatic");
    if (!sel || !comp || !stat) {
        error_ = lib_->LastError();
        return false;
    }
    auto* dev = gpu_->device.Get();
    auto* ctx = gpu_->ctx.Get();
    states_->BindSamplers(ctx);
    const UINT sw = (out.width + 1) / 2, sh = (out.height + 1) / 2;
    sel_.Ensure(dev, sw, sh, DXGI_FORMAT_R16G16B16A16_FLOAT, kRW);
    selW_.Ensure(dev, sw, sh, DXGI_FORMAT_R16_FLOAT, kRW);
    if (!static_[0] || static_[0].width != out.width || static_[0].height != out.height) {
        const float zero[4] = {0, 0, 0, 0};
        for (auto& s : static_) {
            s.Create(dev, out.width, out.height, DXGI_FORMAT_R16_FLOAT, kRW);
            ctx->ClearUnorderedAccessViewFloat(s.uav.Get(), zero);
        }
        staticSeq_ = UINT64_MAX;
    }

    InterpCB cb{};
    cb.outSize[0] = (float)out.width;
    cb.outSize[1] = (float)out.height;
    cb.invOutSize[0] = 1.0f / out.width;
    cb.invOutSize[1] = 1.0f / out.height;
    cb.flowScale[0] = (float)out.width / (float)pr.flowW;
    cb.flowScale[1] = (float)out.height / (float)pr.flowH;
    cb.t = t;
    cb.sigma = params_.sigma;
    cb.zeroBias = params_.interpZeroBias;
    cb.sceneThreshold = 0.11f;
    cb.sceneCut = cfg.sceneCut ? 1 : 0;
    cb.hudProtect = cfg.hudProtect ? 1 : 0;
    cb.flowSize[0] = (float)pr.flowW;
    cb.flowSize[1] = (float)pr.flowH;
    cb.selSize[0] = (float)sw;
    cb.selSize[1] = (float)sh;
    cb.staticEps = params_.staticEps;
    cb.staticFrames = params_.staticFrames;
    cb.refineStep = params_.refineStep;

    // Static detector: once per pair (consecutive pairs only, otherwise the count restarts).
    if (staticSeq_ != seqB) {
        Texture& src = static_[staticCur_];
        Texture& dst = static_[staticCur_ ^ 1];
        if (staticSeq_ != UINT64_MAX && staticSeq_ + 1 != seqB) {
            const float zero[4] = {0, 0, 0, 0};
            ctx->ClearUnorderedAccessViewFloat(src.uav.Get(), zero);
        }
        ID3D11ShaderResourceView* srvs[8] = {frameA, frameB, nullptr, nullptr, nullptr, nullptr, nullptr, src.srv.Get()};
        ctx->CSSetShaderResources(0, 8, srvs);
        ID3D11UnorderedAccessView* uavs[4] = {nullptr, nullptr, nullptr, dst.uav.Get()};
        ctx->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
        Run(stat, out.width, out.height, &cb, sizeof(cb));
        ClearCS(ctx);
        staticCur_ ^= 1;
        staticSeq_ = seqB;
    }

    ID3D11ShaderResourceView* srvs[5] = {frameA, frameB, pr.flowAB.srv.Get(), pr.flowBA.srv.Get(), pr.scene.srv.Get()};
    ctx->CSSetShaderResources(0, 5, srvs);
    ID3D11UnorderedAccessView* su[3] = {nullptr, sel_.uav.Get(), selW_.uav.Get()};
    ctx->CSSetUnorderedAccessViews(0, 3, su, nullptr);
    Run(sel, sw, sh, &cb, sizeof(cb));
    ClearCS(ctx);

    ID3D11ShaderResourceView* cs[8] = {frameA, frameB, nullptr, nullptr, pr.scene.srv.Get(), sel_.srv.Get(), selW_.srv.Get(),
                                       static_[staticCur_].srv.Get()};
    ctx->CSSetShaderResources(0, 8, cs);
    ctx->CSSetUnorderedAccessViews(0, 1, out.uav.GetAddressOf(), nullptr);
    Run(comp, out.width, out.height, &cb, sizeof(cb));
    ClearCS(ctx);
    return true;
}

}  // namespace sw
