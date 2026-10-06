// Window capture back ends: Windows.Graphics.Capture (default), DXGI Desktop Duplication and GDI (fallback).
#pragma once
#include "d3d.h"

namespace sw {

struct CaptureResult {
    bool newFrame = false;   // a new image was copied into the destination
    bool lost = false;       // the capture must be recreated
    double time = 0;         // QPC seconds of the frame
    int frames = 0;          // frames the game delivered since the last poll (also the dropped ones)
};

class Capture {
public:
    virtual ~Capture() = default;
    virtual bool Init(Gpu& gpu, HWND target, HMONITOR monitor) = 0;
    // Copies the newest frame's area 'clientScreen' into 'dst' (BGRA8 of the same size as the rect).
    virtual CaptureResult Poll(const RECT& clientScreen, ID3D11Texture2D* dst) = 0;
    virtual const char* Name() const = 0;
    const std::string& Error() const { return error_; }
    // false: frames are still counted and released but not copied (we are skipping this one).
    void SetCopy(bool copy) { copy_ = copy; }
    // false when Windows draws its yellow capture border around the window (Windows 10).
    virtual bool BorderFree() const { return true; }
    // Stops capturing (and Windows' border with it) until Resume(). Poll() returns nothing meanwhile.
    virtual void Suspend() {}
    virtual bool Resume() { return true; }

protected:
    bool copy_ = true;
    std::string error_;
};

Capture* CreateWgcCapture();
Capture* CreateDdaCapture();
Capture* CreateGdiCapture();
bool WgcSupported();

}  // namespace sw
