// Per-frame image pipeline: Neural Render -> Vision -> upscaler -> adaptive sharpening.
#pragma once
#include "config.h"
#include "d3d.h"
#include "gpu_timer.h"

namespace sw {

class NeuralUpscaler;
class NeuralRender;

class Processor {
public:
    Processor();
    ~Processor();
    bool Init(Gpu& gpu, ShaderLibrary& lib, CommonStates& states);
    void Shutdown();

    // Processes 'src' (srcW x srcH). Writes the output-resolution result into 'out'.
    // 'visionOut' receives the pre-upscale frame (used for motion estimation).
    bool Run(ID3D11ShaderResourceView* src, UINT srcW, UINT srcH, Texture& out, const EngineConfig& cfg,
             GpuTimer* timer);

    // The source-resolution image after Vision (or the raw source when Vision is off).
    ID3D11ShaderResourceView* SourceView() const { return sourceView_; }
    const std::string& Error() const { return error_; }
    bool NeuralAvailable() const;
    bool RenderAvailable() const;

private:
    bool Dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs, UINT nSrv,
                  ID3D11UnorderedAccessView* uav, UINT w, UINT h, const void* cb, size_t cbSize);
    bool CreateNisTables(ID3D11Device* dev);
    Gpu* gpu_ = nullptr;
    ShaderLibrary* lib_ = nullptr;
    CommonStates* states_ = nullptr;
    ConstantBuffer cb_;
    Texture vision_, quarterA_, quarterB_, up_, rendered_;
    ComPtr<ID3D11ShaderResourceView> nisScaler_, nisUsm_;
    ID3D11ShaderResourceView* sourceView_ = nullptr;
    NeuralUpscaler* neural_ = nullptr;
    NeuralRender* render_ = nullptr;
    bool renderWasOn_ = false;
    bool fsrWarned_ = false;
    std::string error_;
};

}  // namespace sw
