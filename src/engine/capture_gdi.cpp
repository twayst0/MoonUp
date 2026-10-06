// GDI back end: BitBlt from the window's client area. Last-resort fallback when neither
// Windows Graphics Capture nor Desktop Duplication can be used (very old systems, remote
// sessions, some virtual machines). Slower, and flip-model games may capture as black, but it
// keeps MoonUp usable everywhere. New frames are detected with a sparse pixel checksum.
#include "capture.h"

namespace sw {
namespace {

class GdiCapture final : public Capture {
public:
    ~GdiCapture() override { Release(); }

    bool Init(Gpu& gpu, HWND target, HMONITOR) override {
        gpu_ = &gpu;
        target_ = target;
        HDC test = GetDC(target);
        if (!test) {
            error_ = "GetDC failed";
            return false;
        }
        ReleaseDC(target, test);
        return true;
    }

    CaptureResult Poll(const RECT& client, ID3D11Texture2D* dst) override {
        CaptureResult r;
        if (!IsWindow(target_)) {
            r.lost = true;
            return r;
        }
        int w = client.right - client.left, h = client.bottom - client.top;
        if (w <= 0 || h <= 0) return r;
        if (w != w_ || h != h_) {
            Release();
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            mem_ = CreateCompatibleDC(nullptr);
            bmp_ = CreateDIBSection(mem_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
            if (!mem_ || !bmp_) {
                Release();
                r.lost = true;
                return r;
            }
            old_ = SelectObject(mem_, bmp_);
            w_ = w;
            h_ = h;
            lastSum_ = 0;
        }
        HDC src = GetDC(target_);
        if (!src) return r;
        BOOL ok = BitBlt(mem_, 0, 0, w, h, src, 0, 0, SRCCOPY);
        ReleaseDC(target_, src);
        if (!ok) return r;
        GdiFlush();

        // Sparse checksum: ~4096 samples spread over the image.
        const uint32_t* px = (const uint32_t*)bits_;
        size_t n = (size_t)w * h;
        size_t step = std::max<size_t>(1, n / 4096) | 1;
        uint64_t sum = 1469598103934665603ull;
        for (size_t i = 0; i < n; i += step) sum = (sum ^ (px[i] & 0x00ffffffu)) * 1099511628211ull;
        if (sum == lastSum_) return r;
        lastSum_ = sum;

        // Alpha is undefined after BitBlt; the pipeline ignores it.
        D3D11_TEXTURE2D_DESC dd;
        dst->GetDesc(&dd);
        D3D11_BOX box{0, 0, 0, std::min<UINT>((UINT)w, dd.Width), std::min<UINT>((UINT)h, dd.Height), 1};
        gpu_->ctx->UpdateSubresource(dst, 0, &box, bits_, (UINT)w * 4, 0);
        r.newFrame = true;
        r.time = NowSeconds();
        return r;
    }

    const char* Name() const override { return "GDI"; }

private:
    void Release() {
        if (mem_ && old_) SelectObject(mem_, old_);
        if (bmp_) DeleteObject(bmp_);
        if (mem_) DeleteDC(mem_);
        mem_ = nullptr;
        bmp_ = nullptr;
        old_ = nullptr;
        bits_ = nullptr;
        w_ = h_ = 0;
    }
    Gpu* gpu_ = nullptr;
    HWND target_ = nullptr;
    HDC mem_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    uint64_t lastSum_ = 0;
};

}  // namespace

Capture* CreateGdiCapture() { return new GdiCapture(); }

}  // namespace sw
