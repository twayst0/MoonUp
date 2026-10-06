// Draws the mouse cursor into the scaled output and maps its position.
#pragma once
#include "d3d.h"

namespace sw {

class CursorRenderer {
public:
    ~CursorRenderer() { Shutdown(); }
    bool Init(Gpu& gpu);
    void Shutdown();

    // srcClient: window client rect (screen). dst: output rect in overlay coordinates.
    // overlayOrigin: screen position of the overlay. Returns true when a cursor must be drawn.
    bool Update(const RECT& srcClient, const RECT& dst, POINT overlayOrigin, bool scaled);

    ID3D11ShaderResourceView* Srv() const { return tex_.srv.Get(); }
    float X() const { return x_; }
    float Y() const { return y_; }
    float W() const { return w_; }
    float H() const { return h_; }

    void HideSystemCursor(bool hide);
    void ClipTo(const RECT* r);

private:
    bool Rebuild(HCURSOR c);
    Gpu* gpu_ = nullptr;
    Texture tex_;
    HCURSOR current_ = nullptr;
    int hotX_ = 0, hotY_ = 0, cw_ = 0, ch_ = 0;
    float x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    bool systemHidden_ = false;
    bool clipped_ = false;
    HMODULE mag_ = nullptr;
    BOOL(WINAPI* magShow_)(BOOL) = nullptr;
    BOOL(WINAPI* magInit_)() = nullptr;
    BOOL(WINAPI* magUninit_)() = nullptr;
    bool magReady_ = false;
};

}  // namespace sw
