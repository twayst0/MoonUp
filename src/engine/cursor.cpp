#include "cursor.h"

namespace sw {

bool CursorRenderer::Init(Gpu& gpu) {
    gpu_ = &gpu;
    // Wine ships Magnification.dll with unimplemented stubs that abort the process.
    bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    if (!mag_ && !wine) {
        mag_ = LoadLibraryW(L"Magnification.dll");
        if (mag_) {
            magInit_ = (BOOL(WINAPI*)())GetProcAddress(mag_, "MagInitialize");
            magUninit_ = (BOOL(WINAPI*)())GetProcAddress(mag_, "MagUninitialize");
            magShow_ = (BOOL(WINAPI*)(BOOL))GetProcAddress(mag_, "MagShowSystemCursor");
            if (magInit_ && magShow_) magReady_ = magInit_() != FALSE;
        }
    }
    return true;
}

void CursorRenderer::Shutdown() {
    HideSystemCursor(false);
    ClipTo(nullptr);
    if (magReady_ && magUninit_) magUninit_();
    magReady_ = false;
    if (mag_) {
        FreeLibrary(mag_);
        mag_ = nullptr;
    }
    tex_.Reset();
    current_ = nullptr;
}

void CursorRenderer::HideSystemCursor(bool hide) {
    if (hide == systemHidden_) return;
    if (magReady_ && magShow_) magShow_(hide ? FALSE : TRUE);
    systemHidden_ = hide;
}

void CursorRenderer::ClipTo(const RECT* r) {
    if (r) {
        ClipCursor(r);
        clipped_ = true;
    } else if (clipped_) {
        ClipCursor(nullptr);
        clipped_ = false;
    }
}

// Renders the cursor on black and on white to recover per pixel alpha (works for colour,
// monochrome and inverting cursors) and uploads a premultiplied BGRA texture.
bool CursorRenderer::Rebuild(HCURSOR c) {
    ICONINFO ii{};
    if (!GetIconInfo(c, &ii)) return false;
    BITMAP bm{};
    int w = 32, h = 32;
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm)) {
        w = bm.bmWidth;
        h = bm.bmHeight;
    } else if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(bm), &bm)) {
        w = bm.bmWidth;
        h = bm.bmHeight / 2;
    }
    hotX_ = (int)ii.xHotspot;
    hotY_ = (int)ii.yHotspot;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    void* bitsBlack = nullptr;
    void* bitsWhite = nullptr;
    HBITMAP bmpBlack = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bitsBlack, nullptr, 0);
    HBITMAP bmpWhite = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bitsWhite, nullptr, 0);
    bool ok = bmpBlack && bmpWhite;
    if (ok) {
        HGDIOBJ old = SelectObject(dc, bmpBlack);
        RECT rc{0, 0, w, h};
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        DrawIconEx(dc, 0, 0, c, w, h, 0, nullptr, DI_NORMAL);
        SelectObject(dc, bmpWhite);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        DrawIconEx(dc, 0, 0, c, w, h, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
        GdiFlush();

        std::vector<uint32_t> px((size_t)w * h);
        const uint8_t* b = (const uint8_t*)bitsBlack;
        const uint8_t* wt = (const uint8_t*)bitsWhite;
        for (int i = 0; i < w * h; i++) {
            // On black the result is a*C, on white a*C + (1-a): the difference gives alpha.
            // Inverting pixels come out with a negative difference -> clamped to opaque white.
            int a = 255 - ((int)wt[i * 4 + 1] - (int)b[i * 4 + 1]);
            a = std::clamp(a, 0, 255);
            uint8_t B = b[i * 4 + 0], G = b[i * 4 + 1], R = b[i * 4 + 2];
            px[i] = (uint32_t)B | ((uint32_t)G << 8) | ((uint32_t)R << 16) | ((uint32_t)a << 24);
        }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = (UINT)w;
        d.Height = (UINT)h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{px.data(), (UINT)w * 4, 0};
        tex_.Reset();
        ok = SUCCEEDED(gpu_->device->CreateTexture2D(&d, &init, &tex_.tex)) &&
             SUCCEEDED(gpu_->device->CreateShaderResourceView(tex_.tex.Get(), nullptr, &tex_.srv));
        tex_.width = (UINT)w;
        tex_.height = (UINT)h;
        cw_ = w;
        ch_ = h;
    }
    if (bmpBlack) DeleteObject(bmpBlack);
    if (bmpWhite) DeleteObject(bmpWhite);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return ok;
}

bool CursorRenderer::Update(const RECT& src, const RECT& dst, POINT origin, bool scaled) {
    CURSORINFO ci{sizeof(ci)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return false;
    POINT p = ci.ptScreenPos;
    if (p.x < src.left || p.y < src.top || p.x >= src.right || p.y >= src.bottom) return false;
    if (ci.hCursor != current_) {
        if (!Rebuild(ci.hCursor)) return false;
        current_ = ci.hCursor;
    }
    float sx = (float)(dst.right - dst.left) / std::max(1L, src.right - src.left);
    float sy = (float)(dst.bottom - dst.top) / std::max(1L, src.bottom - src.top);
    float cs = scaled ? std::clamp((sx + sy) * 0.5f, 1.0f, 3.0f) : 1.0f;
    float lx = dst.left + (p.x - src.left) * sx;
    float ly = dst.top + (p.y - src.top) * sy;
    (void)origin;
    x_ = lx - hotX_ * cs;
    y_ = ly - hotY_ * cs;
    w_ = cw_ * cs;
    h_ = ch_ * cs;
    return tex_.srv != nullptr;
}

}  // namespace sw
