#include "render.h"

#include "render_weights.h"

namespace sw {

namespace {
struct RenderCB {
    int inSize[2];
    int outSize[2];
    int cin, cout, stride, relu;
    int wOff, bOff, hasPrev;
    float temporal;
    int offCcm, offCcmb, offSlopes, offMix;
    int offMixb, offF1w, offF1b, offF2w;
    int offF2b, offOw, offOb, dilation;
    float tone, color, structure, pad1;
    float srcSize[2], invSrcSize[2];
};
constexpr UINT kLowW = 256, kLowH = 144;
constexpr UINT kGW = 16, kGH = 9, kGD = 8;
constexpr int kC1 = 16, kC2 = 32, kC3 = 64, kSC = 12;

bool CreateArray(ID3D11Device* dev, UINT w, UINT h, UINT slices, Texture& t) {
    if (t.tex && t.width == w && t.height == h) return true;
    t.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = slices;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t.tex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = d.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = slices;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = d.Format;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    ud.Texture2DArray.ArraySize = slices;
    if (FAILED(dev->CreateShaderResourceView(t.tex.Get(), &sd, &t.srv)) || FAILED(dev->CreateUnorderedAccessView(t.tex.Get(), &ud, &t.uav)))
        return false;
    t.width = w;
    t.height = h;
    t.format = d.Format;
    return true;
}

void BaseCB(RenderCB& c) {
    using namespace render;
    c.offCcm = kOff_ccm;
    c.offCcmb = kOff_ccmb;
    c.offSlopes = kOff_slopes;
    c.offMix = kOff_mix;
    c.offMixb = kOff_mixb;
    c.offF1w = kOff_f1w;
    c.offF1b = kOff_f1b;
    c.offF2w = kOff_f2w;
    c.offF2b = kOff_f2b;
    c.offOw = kOff_ow;
    c.offOb = kOff_ob;
}
}  // namespace

bool NeuralRender::Init(Gpu& gpu, ShaderLibrary& lib) {
    using namespace render;
    gpu_ = &gpu;
    lib_ = &lib;
    ready_ = false;
    auto* dev = gpu.device.Get();
    if (!cb_.Create(dev, 256)) return false;
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = sizeof(float) * render::kWeightCount;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{render::kWeights, 0, 0};
    if (FAILED(dev->CreateBuffer(&d, &init, &weights_))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = render::kWeightCount;
    if (FAILED(dev->CreateShaderResourceView(weights_.Get(), &sd, &weightsSrv_))) return false;

    // Guide weights and every structure layer as constant buffers (constant-register operands).
    {
        static_assert(kOff_mixb - kOff_ccm == 63, "guide block must be 64 floats");
        auto makeCb = [&](const float* data, int n, UINT floats, ComPtr<ID3D11Buffer>& cb) {
            std::vector<float> v(floats, 0.0f);
            std::copy(data, data + n, v.begin());
            D3D11_BUFFER_DESC cd{};
            cd.ByteWidth = floats * sizeof(float);
            cd.Usage = D3D11_USAGE_IMMUTABLE;
            cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA ci{v.data(), 0, 0};
            return SUCCEEDED(dev->CreateBuffer(&cd, &ci, &cb));
        };
        if (!makeCb(kWeights + kOff_ccm, 64, 64, netConsts_)) return false;
        const int sw[6] = {kOff_s1w, kOff_s2w, kOff_s3w, kOff_s4w, kOff_s5w, kOff_s6w};
        const int sb[6] = {kOff_s1b, kOff_s2b, kOff_s3b, kOff_s4b, kOff_s5b, kOff_s6b};
        const int cout[6] = {kSC, kSC, kSC, kSC, kSC, 2};
        for (int i = 0; i < 6; i++) {
            // weights immediately followed by biases, exactly as the shader indexes them
            int n = sb[i] + cout[i] - sw[i];
            if (n > 328 * 4 || (i < 5 && sb[i] + cout[i] != sw[i + 1]) || !makeCb(kWeights + sw[i], n, 328 * 4, structCb_[i]))
                return false;
        }
    }

    D3D11_BUFFER_DESC gd{};
    gd.ByteWidth = sizeof(float) * 64;
    gd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateBuffer(&gd, nullptr, &global_))) return false;
    sd.Buffer.NumElements = 64;
    if (FAILED(dev->CreateShaderResourceView(global_.Get(), &sd, &globalSrv_))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 64;
    if (FAILED(dev->CreateUnorderedAccessView(global_.Get(), &ud, &globalUav_))) return false;

    for (auto& g : grid_) {
        for (int i = 0; i < 3; i++) {
            D3D11_TEXTURE3D_DESC td{};
            td.Width = kGW;
            td.Height = kGH;
            td.Depth = kGD;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(dev->CreateTexture3D(&td, nullptr, &g.tex[i]))) return false;
            if (FAILED(dev->CreateShaderResourceView(g.tex[i].Get(), nullptr, &g.srv[i]))) return false;
            if (FAILED(dev->CreateUnorderedAccessView(g.tex[i].Get(), nullptr, &g.uav[i]))) return false;
        }
    }
    const UINT rw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (auto& l : low_)
        if (!l.Create(dev, kLowW, kLowH, DXGI_FORMAT_R16G16B16A16_FLOAT, rw)) return false;
    if (!CreateArray(dev, 128, 72, kC1 / 4, f1_) || !CreateArray(dev, 64, 36, kC2 / 4, f2_) ||
        !CreateArray(dev, 32, 18, kC3 / 4, f3_) || !CreateArray(dev, kGW, kGH, kC3 / 4, f4_) ||
        !CreateArray(dev, kGW, kGH, kC3 / 4, f5_) || !CreateArray(dev, kGW, kGH, kC3 / 4, f6_))
        return false;
    ready_ = lib.CS("render.hlsl", "CSDownIn") && lib.CS("render.hlsl", "CSConv") && lib.CS("render.hlsl", "CSGlobal") &&
             lib.CS("render.hlsl", "CSFuse") && lib.CS("render.hlsl", "CSApply") && lib.CS("render.hlsl", "CSDownHalf") &&
             lib.CS("render.hlsl", "CSStructBlend") && lib.CS("render.hlsl", "CSStructIn") &&
             lib.CS("render.hlsl", "CSStructMid") && lib.CS("render.hlsl", "CSStructOut");
    if (!ready_) error_ = lib.LastError();
    return ready_;
}

bool NeuralRender::Conv(ID3D11ShaderResourceView* in, UINT inW, UINT inH, Texture& out, UINT outW, UINT outH, int cin,
                        int cout, int stride, bool relu, int wOff, int bOff, int dilation) {
    auto* ctx = gpu_->ctx.Get();
    RenderCB c{};
    BaseCB(c);
    c.inSize[0] = (int)inW;
    c.inSize[1] = (int)inH;
    c.outSize[0] = (int)outW;
    c.outSize[1] = (int)outH;
    c.cin = cin;
    c.cout = cout;
    c.stride = stride;
    c.relu = relu ? 1 : 0;
    c.wOff = wOff;
    c.bOff = bOff;
    c.dilation = dilation;
    cb_.Update(ctx, &c, sizeof(c));
    ctx->CSSetShader(lib_->CS("render.hlsl", "CSConv"), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cb_.Addr());
    ID3D11ShaderResourceView* srvs[7] = {nullptr, in, nullptr, nullptr, nullptr, nullptr, weightsSrv_.Get()};
    ctx->CSSetShaderResources(0, 7, srvs);
    ID3D11UnorderedAccessView* uavs[2] = {nullptr, out.uav.Get()};
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ctx->Dispatch(DivUp(outW, 8), DivUp(outH, 8), (UINT)(cout + 3) / 4);
    ClearCS(ctx);
    return true;
}

void NeuralRender::ConvS(ID3D11ShaderResourceView* in, Texture& out, UINT w, UINT h, int layer, int dilation) {
    auto* ctx = gpu_->ctx.Get();
    RenderCB c{};
    BaseCB(c);
    c.inSize[0] = c.outSize[0] = (int)w;
    c.inSize[1] = c.outSize[1] = (int)h;
    c.dilation = dilation;
    cb_.Update(ctx, &c, sizeof(c));
    const char* entry = layer == 0 ? "CSStructIn" : layer == 5 ? "CSStructOut" : "CSStructMid";
    ctx->CSSetShader(lib_->CS("render.hlsl", entry), nullptr, 0);
    ID3D11Buffer* cbs[3] = {cb_.Get(), netConsts_.Get(), structCb_[layer].Get()};
    ctx->CSSetConstantBuffers(0, 3, cbs);
    ID3D11ShaderResourceView* srvs[2] = {nullptr, in};
    ctx->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uavs[2] = {nullptr, out.uav.Get()};
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ctx->Dispatch(DivUp(w, 8), DivUp(h, 8), 1);
    ClearCS(ctx);
}

bool NeuralRender::Run(ID3D11ShaderResourceView* src, UINT w, UINT h, Texture& out, const NeuralRenderConfig& rc) {
    if (!ready_) return false;
    using namespace render;
    auto* ctx = gpu_->ctx.Get();
    auto* dev = gpu_->device.Get();
    RenderCB c{};
    BaseCB(c);
    c.srcSize[0] = (float)w;
    c.srcSize[1] = (float)h;
    c.invSrcSize[0] = 1.0f / w;
    c.invSrcSize[1] = 1.0f / h;

    // 1. low resolution copy (written into a texture that also has a 1-slice array view)
    lowCur_ ^= 1;
    Texture& low = low_[lowCur_];
    c.outSize[0] = kLowW;
    c.outSize[1] = kLowH;
    cb_.Update(ctx, &c, sizeof(c));
    ctx->CSSetShader(lib_->CS("render.hlsl", "CSDownIn"), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cb_.Addr());
    ctx->CSSetShaderResources(0, 1, &src);
    ctx->CSSetUnorderedAccessViews(0, 1, low.uav.GetAddressOf(), nullptr);
    ctx->Dispatch(DivUp(kLowW, 8), DivUp(kLowH, 8), 1);
    ClearCS(ctx);

    // A 1-slice array view of the low resolution input for the first convolution.
    if (!lowArray_[lowCur_]) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = 1;
        if (FAILED(dev->CreateShaderResourceView(low.tex.Get(), &sd, &lowArray_[lowCur_]))) return false;
    }

    // 2. tone network
    Conv(lowArray_[lowCur_].Get(), kLowW, kLowH, f1_, 128, 72, 3, kC1, 2, true, kOff_c1w, kOff_c1b);
    Conv(f1_.srv.Get(), 128, 72, f2_, 64, 36, kC1, kC2, 2, true, kOff_c2w, kOff_c2b);
    Conv(f2_.srv.Get(), 64, 36, f3_, 32, 18, kC2, kC3, 2, true, kOff_c3w, kOff_c3b);
    Conv(f3_.srv.Get(), 32, 18, f4_, kGW, kGH, kC3, kC3, 2, true, kOff_c4w, kOff_c4b);
    Conv(f4_.srv.Get(), kGW, kGH, f5_, kGW, kGH, kC3, kC3, 1, true, kOff_c5w, kOff_c5b);
    Conv(f5_.srv.Get(), kGW, kGH, f6_, kGW, kGH, kC3, kC3, 1, false, kOff_c6w, kOff_c6b);

    // 3. global features from c4
    c.inSize[0] = kGW;
    c.inSize[1] = kGH;
    c.cin = kC3;
    cb_.Update(ctx, &c, sizeof(c));
    ctx->CSSetShader(lib_->CS("render.hlsl", "CSGlobal"), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cb_.Addr());
    {
        ID3D11ShaderResourceView* srvs[7] = {nullptr, f4_.srv.Get(), nullptr, nullptr, nullptr, nullptr, weightsSrv_.Get()};
        ctx->CSSetShaderResources(0, 7, srvs);
        ID3D11UnorderedAccessView* uavs[3] = {nullptr, nullptr, globalUav_.Get()};
        ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        ctx->Dispatch(1, 1, 1);
        ClearCS(ctx);
    }

    // 4. fuse into the grid, blended with the previous grid (temporal state)
    Grid& prev = grid_[gridCur_];
    gridCur_ ^= 1;
    Grid& cur = grid_[gridCur_];
    c.hasPrev = hasPrev_ ? 1 : 0;
    c.temporal = std::clamp(rc.temporal, 0.0f, 1.0f);
    cb_.Update(ctx, &c, sizeof(c));
    ctx->CSSetShader(lib_->CS("render.hlsl", "CSFuse"), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cb_.Addr());
    {
        ID3D11ShaderResourceView* srvs[10] = {nullptr, nullptr, low.srv.Get(), low_[lowCur_ ^ 1].srv.Get(), prev.srv[0].Get(),
                                              prev.srv[1].Get(), weightsSrv_.Get(), prev.srv[2].Get(), f6_.srv.Get(), globalSrv_.Get()};
        ctx->CSSetShaderResources(0, 10, srvs);
        ID3D11UnorderedAccessView* uavs[6] = {nullptr, nullptr, nullptr, cur.uav[0].Get(), cur.uav[1].Get(), cur.uav[2].Get()};
        ctx->CSSetUnorderedAccessViews(0, 6, uavs, nullptr);
        ctx->Dispatch(DivUp(kGW, 4), DivUp(kGH, 4), 1);
        ID3D11ShaderResourceView* nulls[10] = {};
        ID3D11UnorderedAccessView* nullu[6] = {};
        ctx->CSSetShaderResources(0, 10, nulls);
        ctx->CSSetUnorderedAccessViews(0, 6, nullu, nullptr);
    }

    // 5. structure network at half resolution (+ temporal blend)
    const UINT hw = (w + 1) / 2, hh = (h + 1) / 2;
    if (!CreateArray(dev, hw, hh, 1, half_[0]) || !CreateArray(dev, hw, hh, 1, half_[1]) ||
        !CreateArray(dev, hw, hh, kSC / 4, sA_) || !CreateArray(dev, hw, hh, kSC / 4, sB_) ||
        !CreateArray(dev, hw, hh, 1, sRaw_) || !CreateArray(dev, hw, hh, 1, struct_[0]) ||
        !CreateArray(dev, hw, hh, 1, struct_[1]))
        return false;
    halfCur_ ^= 1;
    Texture& halfIn = half_[halfCur_];
    c.outSize[0] = (int)hw;
    c.outSize[1] = (int)hh;
    cb_.Update(ctx, &c, sizeof(c));
    ctx->CSSetShader(lib_->CS("render.hlsl", "CSDownHalf"), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cb_.Addr());
    {
        ctx->CSSetShaderResources(0, 1, &src);
        ID3D11UnorderedAccessView* uavs[2] = {nullptr, halfIn.uav.Get()};
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->Dispatch(DivUp(hw, 8), DivUp(hh, 8), 1);
        ClearCS(ctx);
    }
    ConvS(halfIn.srv.Get(), sA_, hw, hh, 0, 1);
    ConvS(sA_.srv.Get(), sB_, hw, hh, 1, 2);
    ConvS(sB_.srv.Get(), sA_, hw, hh, 2, 4);
    ConvS(sA_.srv.Get(), sB_, hw, hh, 3, 8);
    ConvS(sB_.srv.Get(), sA_, hw, hh, 4, 1);
    ConvS(sA_.srv.Get(), sRaw_, hw, hh, 5, 1);
    Texture& sPrev = struct_[structCur_];
    structCur_ ^= 1;
    Texture& sCur = struct_[structCur_];
    c.outSize[0] = (int)hw;
    c.outSize[1] = (int)hh;
    c.hasPrev = hasPrev_ ? 1 : 0;
    cb_.Update(ctx, &c, sizeof(c));
    ctx->CSSetShader(lib_->CS("render.hlsl", "CSStructBlend"), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cb_.Addr());
    {
        ID3D11ShaderResourceView* srvs[13] = {nullptr, sRaw_.srv.Get(), nullptr, nullptr, nullptr, nullptr, nullptr,
                                              nullptr, nullptr, nullptr, sPrev.srv.Get(), halfIn.srv.Get(),
                                              half_[halfCur_ ^ 1].srv.Get()};
        ctx->CSSetShaderResources(0, 13, srvs);
        ID3D11UnorderedAccessView* uavs[2] = {nullptr, sCur.uav.Get()};
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->Dispatch(DivUp(hw, 8), DivUp(hh, 8), 1);
        ID3D11ShaderResourceView* nulls[13] = {};
        ctx->CSSetShaderResources(0, 13, nulls);
        ClearCS(ctx);
    }
    hasPrev_ = true;

    // 6. apply at full resolution
    c.outSize[0] = (int)w;
    c.outSize[1] = (int)h;
    c.tone = std::clamp(rc.tone, 0.0f, 2.0f);
    c.color = std::clamp(rc.color, 0.0f, 2.0f);
    c.structure = std::clamp(rc.structure, 0.0f, 2.0f);
    cb_.Update(ctx, &c, sizeof(c));
    ctx->CSSetShader(lib_->CS("render.hlsl", "CSApply"), nullptr, 0);
    {
        ID3D11Buffer* cbs[2] = {cb_.Get(), netConsts_.Get()};
        ctx->CSSetConstantBuffers(0, 2, cbs);
    }
    {
        ID3D11ShaderResourceView* srvs[14] = {src,     nullptr, nullptr, nullptr, cur.srv[0].Get(), cur.srv[1].Get(), weightsSrv_.Get(),
                                              cur.srv[2].Get(), nullptr, nullptr, nullptr, nullptr, nullptr, sCur.srv.Get()};
        ctx->CSSetShaderResources(0, 14, srvs);
        ID3D11UnorderedAccessView* uavs[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, out.uav.Get()};
        ctx->CSSetUnorderedAccessViews(0, 7, uavs, nullptr);
        ctx->Dispatch(DivUp(w, 8), DivUp(h, 8), 1);
        ID3D11ShaderResourceView* nulls[14] = {};
        ID3D11UnorderedAccessView* nullu[7] = {};
        ctx->CSSetShaderResources(0, 14, nulls);
        ctx->CSSetUnorderedAccessViews(0, 7, nullu, nullptr);
    }
    return true;
}

}  // namespace sw
