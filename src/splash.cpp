#include "splash.h"

#include <wincodec.h>

#include <cmath>

#include "res/resource.h"

namespace sw {

static bool DecodeLogo(int size, std::vector<uint8_t>& out) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_SPLASH_PNG), RT_RCDATA);
    if (!res) return false;
    HGLOBAL h = LoadResource(nullptr, res);
    const void* data = LockResource(h);
    DWORD len = SizeofResource(nullptr, res);
    if (!data || !len) return false;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory((BYTE*)data, len))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    // The splash shows the first frame of the intro: the logo, smoothly scaled, at the size and
    // position the web intro starts from. The intro then settles it into its glass tile.
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), (UINT)size, (UINT)size, WICBitmapInterpolationModeFant)))
        return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    out.resize((size_t)size * size * 4);
    if (FAILED(conv->CopyPixels(nullptr, (UINT)size * 4, (UINT)out.size(), out.data()))) return false;
    return true;
}

bool Splash::Show(int cx, int cy, int logoPx) {
    std::vector<uint8_t> px;
    if (!DecodeLogo(logoPx, px)) return false;
    size_ = logoPx;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MoonUpSplash";
        wc.hCursor = LoadCursorW(nullptr, IDC_APPSTARTING);
        RegisterClassExW(&wc);
        registered = true;
    }
    pos_ = {cx - size_ / 2, cy - size_ / 2};
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"MoonUpSplash", L"MoonUp", WS_POPUP,
                            pos_.x, pos_.y, size_, size_, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size_;
    bi.bmiHeader.biHeight = -size_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    memDc_ = CreateCompatibleDC(screen);
    bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp_) {
        Destroy();
        return false;
    }
    memcpy(bits, px.data(), px.size());
    oldBmp_ = SelectObject(memDc_, bmp_);

    start_ = NowSeconds();
    Paint(0);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetTimer(hwnd_, 1, 15, nullptr);
    return true;
}

void Splash::Paint(BYTE alpha) {
    if (!hwnd_) return;
    POINT src{0, 0};
    SIZE sz{size_, size_};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &pos_, &sz, memDc_, &src, 0, &bf, ULW_ALPHA);
}

void Splash::Tick() {
    double now = NowSeconds();
    if (fading_) {
        double t = (now - fadeStart_) / 0.12;
        if (t >= 1.0) {
            Destroy();
            return;
        }
        Paint((BYTE)(255 * (1.0 - t) * (1.0 - t)));
        return;
    }
    double t = std::min(1.0, (now - start_) / 0.18);
    Paint((BYTE)(255 * t));
}

void Splash::FadeOut() {
    if (!hwnd_ || fading_) return;
    fading_ = true;
    fadeStart_ = NowSeconds();
}

void Splash::Destroy() {
    if (hwnd_) {
        KillTimer(hwnd_, 1);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (memDc_) {
        if (oldBmp_) SelectObject(memDc_, oldBmp_);
        DeleteDC(memDc_);
        memDc_ = nullptr;
    }
    if (bmp_) {
        DeleteObject(bmp_);
        bmp_ = nullptr;
    }
}

LRESULT CALLBACK Splash::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = (Splash*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_TIMER && self) {
        self->Tick();
        return 0;
    }
    if (m == WM_NCHITTEST) return HTCAPTION;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace sw
