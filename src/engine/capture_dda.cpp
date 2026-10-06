// DXGI Desktop Duplication back end (monitor capture cropped to the window's client area).
#include "capture.h"

namespace sw {
namespace {

class DdaCapture final : public Capture {
public:
    bool Init(Gpu& gpu, HWND, HMONITOR monitor) override {
        gpu_ = &gpu;
        monitor_ = monitor;
        return Recreate();
    }

    CaptureResult Poll(const RECT& client, ID3D11Texture2D* dst) override {
        CaptureResult r;
        if (!dupl_ && !Recreate()) {
            r.lost = true;
            return r;
        }
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dupl_->AcquireNextFrame(0, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return r;
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            dupl_.Reset();
            Recreate();
            return r;
        }
        if (FAILED(hr)) {
            r.lost = true;
            return r;
        }
        // LastPresentTime 0: only the mouse pointer moved. AccumulatedFrames: images shown since the
        // last call (also the ones we did not pick up).
        if (info.LastPresentTime.QuadPart != 0) r.frames = (int)std::max<UINT>(info.AccumulatedFrames, 1);
        if (copy_ && dst && info.LastPresentTime.QuadPart != 0 && info.AccumulatedFrames > 0) {
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(res.As(&tex))) {
                D3D11_TEXTURE2D_DESC td;
                tex->GetDesc(&td);
                LONG ox = client.left - desktop_.left;
                LONG oy = client.top - desktop_.top;
                LONG w = client.right - client.left;
                LONG h = client.bottom - client.top;
                D3D11_BOX box;
                box.left = (UINT)std::clamp<LONG>(ox, 0, (LONG)td.Width);
                box.top = (UINT)std::clamp<LONG>(oy, 0, (LONG)td.Height);
                box.right = (UINT)std::clamp<LONG>(ox + w, 0, (LONG)td.Width);
                box.bottom = (UINT)std::clamp<LONG>(oy + h, 0, (LONG)td.Height);
                box.front = 0;
                box.back = 1;
                if (box.right > box.left && box.bottom > box.top) {
                    gpu_->ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, tex.Get(), 0, &box);
                    r.newFrame = true;
                    double t = QpcToSeconds(info.LastPresentTime.QuadPart);
                    double now = NowSeconds();
                    r.time = std::abs(now - t) < 1.0 ? t : now;
                }
            }
        }
        dupl_->ReleaseFrame();
        return r;
    }

    const char* Name() const override { return "DXGI Desktop Duplication"; }

private:
    bool Recreate() {
        dupl_.Reset();
        ComPtr<IDXGIOutput> out;
        for (UINT i = 0; gpu_->adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_OUTPUT_DESC d;
            out->GetDesc(&d);
            if (d.Monitor == monitor_) {
                desktop_ = d.DesktopCoordinates;
                ComPtr<IDXGIOutput1> o1;
                if (SUCCEEDED(out.As(&o1))) {
                    HRESULT hr = o1->DuplicateOutput(gpu_->device.Get(), &dupl_);
                    if (FAILED(hr)) {
                        error_ = "DuplicateOutput failed " + HrToString(hr);
                        SW_LOG("DDA: %s", error_.c_str());
                        return false;
                    }
                    SW_LOG("DDA capture started");
                    return true;
                }
            }
            out.Reset();
        }
        error_ = "The selected GPU does not drive this monitor (Desktop Duplication needs the display GPU)";
        SW_LOG("DDA: %s", error_.c_str());
        return false;
    }

    Gpu* gpu_ = nullptr;
    HMONITOR monitor_ = nullptr;
    RECT desktop_{};
    ComPtr<IDXGIOutputDuplication> dupl_;
};

}  // namespace

Capture* CreateDdaCapture() { return new DdaCapture(); }

}  // namespace sw
