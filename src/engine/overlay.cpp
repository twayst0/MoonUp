#include "overlay.h"

namespace sw {

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

double MonitorRefreshRate(HMONITOR mon) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return 60.0;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return (double)dm.dmDisplayFrequency;
    return 60.0;
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool Overlay::Create(Gpu& gpu, const RECT& area, bool allowTearing, int maxLatency, bool layered, bool excludeFromCapture) {
    Destroy();
    gpu_ = &gpu;
    area_ = area;
    width_ = (UINT)(area.right - area.left);
    height_ = (UINT)(area.bottom - area.top);
    if (guard_ >= 2 && height_ > 2) height_ -= 1;
    excludeRequested_ = excludeFromCapture;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MoonUpOverlay";
        wc.hCursor = nullptr;
        RegisterClassExW(&wc);
        registered = true;
    }
    // Attempts, best first: 0 flip-discard + waitable (+tearing), 1 flip-discard, 2 flip-sequential,
    // 3 blit model. Each is tried with the layered window first, then without.
    for (int attempt = 0; attempt < 4; attempt++) {
        tearing_ = attempt == 0 && allowTearing && gpu.tearingSupported;
        if (layered && CreateWindowAndChain(gpu, true, attempt, maxLatency)) return true;
        if (CreateWindowAndChain(gpu, false, attempt, maxLatency)) return true;
    }
    SW_LOG("Overlay: every swap chain configuration failed");
    return false;
}

bool Overlay::CreateWindowAndChain(Gpu& gpu, bool layered, int attempt, int maxLatency) {
    Destroy();
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    if (attempt < 3) ex |= WS_EX_NOREDIRECTIONBITMAP;
    if (layered) ex |= WS_EX_LAYERED;
    hwnd_ = CreateWindowExW(ex, L"MoonUpOverlay", L"MoonUp Output", WS_POPUP, area_.left, area_.top, (int)width_,
                            (int)height_, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        SW_LOG("Overlay window creation failed (%lu)", GetLastError());
        return false;
    }
    if (layered) SetLayeredWindowAttributes(hwnd_, 0, guard_ >= 1 ? 254 : 255, LWA_ALPHA);
    // Desktop Duplication would capture our own output: hide the overlay from capture in that case.
    // With window capture the output stays visible to screenshots and recording software.
    if (excludeRequested_) {
        excluded_ = SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE) != FALSE;
        if (!excluded_) SW_LOG("SetWindowDisplayAffinity(EXCLUDEFROMCAPTURE) not supported on this system");
    }

    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = width_;
    d.Height = height_;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    switch (attempt) {
        case 0:
            d.BufferCount = 3;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
            break;
        case 1:
            d.BufferCount = 3;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            break;
        case 2:
            d.BufferCount = 2;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            break;
        default:
            d.BufferCount = 1;
            d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            break;
    }
    HRESULT hr = gpu.factory->CreateSwapChainForHwnd(gpu.device.Get(), hwnd_, &d, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        SW_LOG("Overlay: swap chain attempt %d (layered %d) failed: %s", attempt, (int)layered, HrToString(hr).c_str());
        Destroy();
        return false;
    }
    gpu.factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    if (attempt == 0 && SUCCEEDED(swap_.As(&swap2_))) {
        swap2_->SetMaximumFrameLatency((UINT)std::clamp(maxLatency, 1, 3));
        waitable_ = swap2_->GetFrameLatencyWaitableObject();
    } else {
        ComPtr<IDXGIDevice1> dxgi;
        if (SUCCEEDED(gpu.device.As(&dxgi))) dxgi->SetMaximumFrameLatency((UINT)std::clamp(maxLatency, 1, 3));
    }

    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&bb))) || FAILED(gpu.device->CreateRenderTargetView(bb.Get(), nullptr, &rtv_))) {
        SW_LOG("Overlay: back buffer view failed (attempt %d)", attempt);
        Destroy();
        return false;
    }
    refresh_ = MonitorRefreshRate(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
    SW_LOG("Overlay %ux%u @ %.1f Hz, attempt %d, tearing %d, layered %d, waitable %d, guard %d", width_, height_, refresh_,
           attempt, (int)tearing_, (int)layered, waitable_ ? 1 : 0, guard_);
    return true;
}

void Overlay::Show(bool show) {
    if (!hwnd_ || show == visible_) return;
    ShowWindow(hwnd_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    visible_ = show;
}

bool Overlay::WaitReady(DWORD timeoutMs) {
    if (!waitable_) return true;
    return WaitForSingleObjectEx(waitable_, timeoutMs, TRUE) == WAIT_OBJECT_0;
}

HRESULT Overlay::Present(bool vsync) {
    if (!swap_) return E_FAIL;
    UINT flags = (!vsync && tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = swap_->Present(vsync ? 1 : 0, flags);
    // Flip model keeps the same back buffer index 0 for the RTV after Present.
    return hr;
}

void Overlay::Destroy() {
    rtv_.Reset();
    if (waitable_) {
        CloseHandle(waitable_);
        waitable_ = nullptr;
    }
    swap2_.Reset();
    if (swap_) swap_->SetFullscreenState(FALSE, nullptr);
    swap_.Reset();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    visible_ = false;
}

}  // namespace sw
