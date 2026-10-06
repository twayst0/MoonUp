// MoonUp Neural Render v2 runtime (weights in render_weights.h, trained by tools/train_render.py).
#pragma once
#include "config.h"
#include "d3d.h"

namespace sw {

class NeuralRender {
public:
    bool Init(Gpu& gpu, ShaderLibrary& lib);
    bool Ready() const { return ready_; }
    // Drops the temporal state (scene change, resize, settings change).
    void Reset() { hasPrev_ = false; }
    // Renders 'src' (w x h) into 'out' (same size).
    bool Run(ID3D11ShaderResourceView* src, UINT w, UINT h, Texture& out, const NeuralRenderConfig& rc);
    const std::string& Error() const { return error_; }

private:
    bool Conv(ID3D11ShaderResourceView* in, UINT inW, UINT inH, Texture& out, UINT outW, UINT outH, int cin, int cout,
              int stride, bool relu, int wOff, int bOff, int dilation = 1);
    void ConvS(ID3D11ShaderResourceView* in, Texture& out, UINT w, UINT h, int layer, int dilation);
    Gpu* gpu_ = nullptr;
    ShaderLibrary* lib_ = nullptr;
    ConstantBuffer cb_;
    ComPtr<ID3D11Buffer> weights_;
    ComPtr<ID3D11ShaderResourceView> weightsSrv_;
    ComPtr<ID3D11Buffer> netConsts_;
    ComPtr<ID3D11Buffer> structCb_[6];
    ComPtr<ID3D11Buffer> global_;
    ComPtr<ID3D11ShaderResourceView> globalSrv_;
    ComPtr<ID3D11UnorderedAccessView> globalUav_;
    Texture low_[2];
    ComPtr<ID3D11ShaderResourceView> lowArray_[2];
    int lowCur_ = 0;
    Texture f1_, f2_, f3_, f4_, f5_, f6_;
    Texture half_[2], sA_, sB_, sRaw_, struct_[2];  // structure branch (half resolution)
    int halfCur_ = 0, structCur_ = 0;
    struct Grid {
        ComPtr<ID3D11Texture3D> tex[3];
        ComPtr<ID3D11ShaderResourceView> srv[3];
        ComPtr<ID3D11UnorderedAccessView> uav[3];
    } grid_[2];
    int gridCur_ = 0;
    bool hasPrev_ = false;
    bool ready_ = false;
    std::string error_;
};

}  // namespace sw
