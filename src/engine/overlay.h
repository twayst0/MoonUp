// Click-through topmost output window. Flip model swap chain with a waitable object when the
// system supports it, with progressively simpler fallbacks down to a blit model swap chain.
#pragma once
#include "d3d.h"

namespace sw {

class Overlay {
public:
    ~Overlay() { Destroy(); }
    bool Create(Gpu& gpu, const RECT& area, bool allowTearing, int maxLatency, bool layered, bool excludeFromCapture);
    void Destroy();
    void Show(bool show);
    bool Visible() const { return visible_; }

    // Waits for the swap chain to accept a new frame (pacing). Returns false on timeout.
    bool WaitReady(DWORD timeoutMs);
    ID3D11RenderTargetView* BackBuffer() { return rtv_.Get(); }
    HRESULT Present(bool vsync);
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }
    const RECT& Area() const { return area_; }
    HWND Hwnd() const { return hwnd_; }
    double RefreshRate() const { return refresh_; }
    bool ExcludedFromCapture() const { return excluded_; }
    // Occlusion guard (call before Create). Windows and many engines (Unreal for example) slow a
    // game down to a few frames per second when its window is completely covered by an opaque
    // window. 0 = opaque, 1 = 254/255 alpha (DWM no longer treats the game as covered),
    // 2 = 254/255 alpha and the bottom pixel row left uncovered.
    void SetGuard(int mode) { guard_ = mode; }
    int Guard() const { return guard_; }

private:
    HWND hwnd_ = nullptr;
    RECT area_{};
    UINT width_ = 0, height_ = 0;
    bool tearing_ = false;
    bool visible_ = false;
    bool excluded_ = false;
    double refresh_ = 60.0;
    Gpu* gpu_ = nullptr;
    bool CreateWindowAndChain(Gpu& gpu, bool layered, int attempt, int maxLatency);
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<IDXGISwapChain2> swap2_;
    bool excludeRequested_ = false;
    int guard_ = 1;
    ComPtr<ID3D11RenderTargetView> rtv_;
    HANDLE waitable_ = nullptr;
};

double MonitorRefreshRate(HMONITOR mon);

}  // namespace sw
