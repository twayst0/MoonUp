#include "hud.h"

namespace sw {

static const UINT kHudW = 300, kHudH = 132;

bool Hud::Init(Gpu& gpu) {
    gpu_ = &gpu;
    if (!tex_.Create(gpu.device.Get(), kHudW, kHudH, DXGI_FORMAT_B8G8R8A8_UNORM,
                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
        return false;
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 (void**)factory_.GetAddressOf())))
        return false;
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(gpu.device.As(&dxgi))) return false;
    if (FAILED(factory_->CreateDevice(dxgi.Get(), &d2dDevice_))) return false;
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
    ComPtr<IDXGISurface> surface;
    if (FAILED(tex_.tex.As(&surface))) return false;
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    bp.dpiX = bp.dpiY = 96.0f;
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target_))) return false;
    dc_->SetTarget(target_.Get());
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dwrite_.GetAddressOf())))
        return false;
    dwrite_->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, 30.0f, L"en-us", &big_);
    dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &small_);
    dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, 10.0f, L"en-us", &label_);
    if (!big_ || !small_ || !label_) return false;
    dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_);
    return brush_ != nullptr;
}

void Hud::Shutdown() {
    if (dc_) dc_->SetTarget(nullptr);
    brush_.Reset();
    target_.Reset();
    dc_.Reset();
    d2dDevice_.Reset();
    factory_.Reset();
    tex_.Reset();
}

void Hud::Render(const HudData& d, bool graph) {
    if (!dc_) return;
    const float W = (float)kHudW, H = (float)kHudH;
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));

    D2D1_ROUNDED_RECT bg{D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), 10, 10};
    brush_->SetColor(D2D1::ColorF(0.035f, 0.04f, 0.05f, 0.78f));
    dc_->FillRoundedRectangle(bg, brush_.Get());
    brush_->SetColor(D2D1::ColorF(0.86f, 0.89f, 0.95f, 0.28f));
    dc_->DrawRoundedRectangle(bg, brush_.Get(), 1.0f);

    wchar_t buf[256];
    // Header label.
    brush_->SetColor(D2D1::ColorF(0.84f, 0.87f, 0.92f, 1.0f));
    dc_->DrawText(L"MOONUP", 8, label_.Get(), D2D1::RectF(12, 8, 140, 22), brush_.Get());

    // Output FPS big, base FPS small.
    swprintf(buf, 256, L"%.0f", d.outFps);
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 1));
    dc_->DrawText(buf, (UINT32)wcslen(buf), big_.Get(), D2D1::RectF(12, 18, 150, 58), brush_.Get());
    swprintf(buf, 256, L"FPS   base %.0f", d.baseFps);
    brush_->SetColor(D2D1::ColorF(0.72f, 0.75f, 0.80f, 1.0f));
    dc_->DrawText(buf, (UINT32)wcslen(buf), small_.Get(), D2D1::RectF(14, 56, 200, 72), brush_.Get());

    if (!d.hint.empty()) {
        brush_->SetColor(D2D1::ColorF(1.0f, 0.74f, 0.35f, 1.0f));
        dc_->DrawText(d.hint.c_str(), (UINT32)d.hint.size(), small_.Get(), D2D1::RectF(118, 56, W - 10, 72), brush_.Get());
        brush_->SetColor(D2D1::ColorF(0.72f, 0.75f, 0.80f, 1.0f));
    }
    swprintf(buf, 256, L"%ls\n%ls", d.upscaler.c_str(), d.frameGen.c_str());
    dc_->DrawText(buf, (UINT32)wcslen(buf), small_.Get(), D2D1::RectF(150, 22, W - 10, 56), brush_.Get());
    swprintf(buf, 256, L"%d×%d → %d×%d   GPU %.1f ms", d.srcW, d.srcH, d.outW, d.outH, d.gpuMs);
    dc_->DrawText(buf, (UINT32)wcslen(buf), small_.Get(), D2D1::RectF(14, 74, W - 10, 90), brush_.Get());

    if (graph && d.frameTimes.size() > 2) {
        const float gx = 12, gy = 96, gw = W - 24, gh = 26;
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.06f));
        dc_->FillRectangle(D2D1::RectF(gx, gy, gx + gw, gy + gh), brush_.Get());
        float maxMs = 1.0f;
        for (float v : d.frameTimes) maxMs = std::max(maxMs, v);
        maxMs = std::max(maxMs * 1.15f, 20.0f);
        ComPtr<ID2D1PathGeometry> path;
        factory_->CreatePathGeometry(&path);
        ComPtr<ID2D1GeometrySink> sink;
        if (path && SUCCEEDED(path->Open(&sink))) {
            size_t n = d.frameTimes.size();
            for (size_t i = 0; i < n; i++) {
                float x = gx + gw * (float)i / (float)(n - 1);
                float y = gy + gh - gh * std::min(d.frameTimes[i] / maxMs, 1.0f);
                if (i == 0)
                    sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
                else
                    sink->AddLine(D2D1::Point2F(x, y));
            }
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            brush_->SetColor(D2D1::ColorF(0.90f, 0.93f, 1.0f, 1.0f));
            dc_->DrawGeometry(path.Get(), brush_.Get(), 1.4f);
        }
    }
    dc_->EndDraw();
}

}  // namespace sw
