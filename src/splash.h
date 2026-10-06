// Native splash: shows the logo instantly (per-pixel alpha) while the UI engine starts.
#pragma once
#include "common.h"

namespace sw {

class Splash {
public:
    // centerX/centerY in screen pixels, logoPx = logo edge length in pixels.
    bool Show(int centerX, int centerY, int logoPx);
    void FadeOut();  // starts a short fade, then destroys itself
    void Destroy();
    bool Active() const { return hwnd_ != nullptr; }

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    void Tick();
    void Paint(BYTE alpha);
    HWND hwnd_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int size_ = 0;
    POINT pos_{};
    double start_ = 0;
    double fadeStart_ = 0;
    bool fading_ = false;
};

}  // namespace sw
