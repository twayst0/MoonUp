// In-game statistics overlay rendered with Direct2D / DirectWrite into a D3D11 texture.
#pragma once
#include <d2d1_1.h>
#include <dwrite.h>

#include <deque>

#include "d3d.h"

namespace sw {

struct HudData {
    double baseFps = 0;
    double outFps = 0;
    double gpuMs = 0;
    double latencyMs = 0;
    int srcW = 0, srcH = 0, outW = 0, outH = 0;
    std::wstring upscaler;
    std::wstring frameGen;
    std::wstring hint;  // short warning, e.g. "Cap game at 72 fps"
    std::deque<float> frameTimes;  // ms, most recent last
};

class Hud {
public:
    bool Init(Gpu& gpu);
    void Shutdown();
    // Redraws the texture (call a few times per second).
    void Render(const HudData& d, bool graph);
    ID3D11ShaderResourceView* Srv() const { return tex_.srv.Get(); }
    UINT Width() const { return tex_.width; }
    UINT Height() const { return tex_.height; }

private:
    Gpu* gpu_ = nullptr;
    Texture tex_;
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> big_, small_, label_;
    ComPtr<ID2D1SolidColorBrush> brush_;
};

}  // namespace sw
