#include "neural.h"

#include "neural_weights.h"

namespace sw {

namespace {
struct NeuralCB {
    int size[2];
    int relu, addSkip, direct;
    int pad[3];
};
struct UpscaleCB {
    float srcSize[2], invSrcSize[2], dstSize[2], invDstSize[2];
    float params[4];
};
// Layer shapes (cin, cout): c0 1->16, c1..c5 16->16, c6 16->4. Floats per layer = cout*cin*9 + cout.
constexpr int kCin[7] = {1, 16, 16, 16, 16, 16, 16};
constexpr int kCout[7] = {16, 16, 16, 16, 16, 16, 4};
constexpr int LayerFloats(int i) { return kCout[i] * kCin[i] * 9 + kCout[i]; }
constexpr int TotalFloats() {
    int n = 0;
    for (int i = 0; i < 7; i++) n += LayerFloats(i);
    return n;
}
constexpr UINT kCbFloats = 580 * 4;  // matches LayerWeights in neural.hlsl
static_assert(TotalFloats() == neural::kWeightCount, "neural_weights.h does not match the network layout");
}  // namespace

bool NeuralUpscaler::Init(Gpu& gpu, ShaderLibrary& lib) {
    gpu_ = &gpu;
    lib_ = &lib;
    ready_ = false;
    if (!cb_.Create(gpu.device.Get(), 64) || !cbUp_.Create(gpu.device.Get(), 64)) return false;
    const float* models[2] = {neural::kMoonUp, neural::kArtCNN};
    for (int m = 0; m < 2; m++) {
        int off = 0;
        for (int i = 0; i < kLayers; i++) {
            std::vector<float> data(kCbFloats, 0.0f);
            std::copy(models[m] + off, models[m] + off + LayerFloats(i), data.begin());
            off += LayerFloats(i);
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = kCbFloats * sizeof(float);
            d.Usage = D3D11_USAGE_IMMUTABLE;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA init{data.data(), 0, 0};
            if (FAILED(gpu.device->CreateBuffer(&d, &init, &layerCb_[m][i]))) {
                error_ = "constant buffer";
                return false;
            }
        }
    }
    ready_ = lib.CS("neural.hlsl", "CSConvIn") && lib.CS("neural.hlsl", "CSConvMid") && lib.CS("neural.hlsl", "CSConvOut");
    if (!ready_) error_ = lib.LastError();
    return ready_;
}

bool NeuralUpscaler::Run(ID3D11ShaderResourceView* src, UINT w, UINT h, Texture& out, Model model) {
    if (!ready_) return false;
    auto* dev = gpu_->device.Get();
    auto* ctx = gpu_->ctx.Get();
    const UINT rw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (int i = 0; i < 4; i++) {
        if (!s_[i].Ensure(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, rw) || !a_[i].Ensure(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, rw) ||
            !b_[i].Ensure(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, rw))
            return false;
    }
    bool exact2x = out.width == w * 2 && out.height == h * 2;
    if (!exact2x) out2x_.Ensure(dev, w * 2, h * 2, DXGI_FORMAT_R8G8B8A8_UNORM, rw);
    Texture& target2x = exact2x ? out : out2x_;
    const int m = model == kArtCNN ? 1 : 0;

    auto layer = [&](int li, const char* entry, Texture* in, Texture* outF, bool relu, bool skip, Texture* final2x) {
        NeuralCB cb{};
        cb.size[0] = (int)w;
        cb.size[1] = (int)h;
        cb.relu = relu ? 1 : 0;
        cb.addSkip = skip ? 1 : 0;
        cb.direct = 1;  // both models predict luma directly (MoonUp Neural v3 is fine-tuned from ArtCNN)
        cb_.Update(ctx, &cb, sizeof(cb));
        ctx->CSSetShader(lib_->CS("neural.hlsl", entry), nullptr, 0);
        ID3D11Buffer* cbs[2] = {cb_.Get(), layerCb_[m][li].Get()};
        ctx->CSSetConstantBuffers(0, 2, cbs);
        ID3D11ShaderResourceView* srvs[9] = {src};
        if (in)
            for (int i = 0; i < 4; i++) srvs[1 + i] = in[i].srv.Get();
        if (skip)
            for (int i = 0; i < 4; i++) srvs[5 + i] = s_[i].srv.Get();
        ctx->CSSetShaderResources(0, 9, srvs);
        ID3D11UnorderedAccessView* uavs[5] = {};
        if (outF)
            for (int i = 0; i < 4; i++) uavs[i] = outF[i].uav.Get();
        if (final2x) uavs[4] = final2x->uav.Get();
        ctx->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
        ctx->Dispatch(DivUp(w, 8), DivUp(h, 8), 1);
        ID3D11ShaderResourceView* nullSrv[9] = {};
        ID3D11UnorderedAccessView* nullUav[5] = {};
        ctx->CSSetShaderResources(0, 9, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 5, nullUav, nullptr);
    };
    layer(0, "CSConvIn", nullptr, s_, false, false, nullptr);
    layer(1, "CSConvMid", s_, a_, true, false, nullptr);
    layer(2, "CSConvMid", a_, b_, true, false, nullptr);
    layer(3, "CSConvMid", b_, a_, true, false, nullptr);
    layer(4, "CSConvMid", a_, b_, true, false, nullptr);
    layer(5, "CSConvMid", b_, a_, false, true, nullptr);
    layer(6, "CSConvOut", a_, nullptr, false, false, &target2x);

    if (!exact2x) {
        // Resample the 2x result to the requested size: Edge when enlarging further, bilinear otherwise.
        bool larger = out.width > w * 2 || out.height > h * 2;
        UpscaleCB u{};
        u.srcSize[0] = (float)(w * 2);
        u.srcSize[1] = (float)(h * 2);
        u.invSrcSize[0] = 1.0f / (w * 2);
        u.invSrcSize[1] = 1.0f / (h * 2);
        u.dstSize[0] = (float)out.width;
        u.dstSize[1] = (float)out.height;
        u.invDstSize[0] = 1.0f / out.width;
        u.invDstSize[1] = 1.0f / out.height;
        u.params[0] = 1.0f;
        u.params[1] = 0.85f;
        cbUp_.Update(ctx, &u, sizeof(u));
        ctx->CSSetShader(lib_->CS("upscale.hlsl", larger ? "CSEdge" : "CSBilinear"), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cbUp_.Addr());
        ctx->CSSetShaderResources(0, 1, out2x_.srv.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0, 1, out.uav.GetAddressOf(), nullptr);
        ctx->Dispatch(DivUp(out.width, 8), DivUp(out.height, 8), 1);
        ClearCS(ctx);
    }
    return true;
}

}  // namespace sw
