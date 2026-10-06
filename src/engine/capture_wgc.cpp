// Windows.Graphics.Capture back end (window capture, works across GPUs and with occlusion).
#include <dwmapi.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>

#include "../third_party/wgc_min.h"
#include "capture.h"
#include "overlay.h"

extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice* dxgiDevice, IInspectable** graphicsDevice);

// COM interfaces implemented by Windows. They must have external linkage: declared inside an
// anonymous namespace, GCC concludes nothing can implement them and turns every call through
// them into a call to a pure virtual function (which crashed the engine at "WGC: device").
struct IGraphicsCaptureItemInterop : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid, void** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID riid, void** result) = 0;
};
const GUID IID_IGraphicsCaptureItemInterop = {0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};

struct IDirect3DDxgiInterfaceAccess : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** p) = 0;
};
const GUID IID_IDirect3DDxgiInterfaceAccess = {0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};

namespace sw {

namespace {


// Windows.Graphics.DirectX.Direct3D11.IDirect3DDevice. WinRT methods expect this exact interface
// pointer, not the IInspectable returned by CreateDirect3D11DeviceFromDXGIDevice.
const GUID IID_IDirect3DDevice_WinRT = {0xa37624ab, 0x8d5f, 0x4650, {0x9d, 0x3e, 0x9e, 0xae, 0x3d, 0x9b, 0xc6, 0x70}};

constexpr INT32 kPixelFormatB8G8R8A8 = 87;  // DirectXPixelFormat::B8G8R8A8UIntNormalized

template <typename T>
HRESULT GetFactory(const wchar_t* cls, const GUID& iid, T** out) {
    HSTRING_HEADER header;
    HSTRING str = nullptr;
    HRESULT hr = WindowsCreateStringReference(cls, (UINT32)wcslen(cls), &header, &str);
    if (FAILED(hr)) return hr;
    return RoGetActivationFactory(str, iid, (void**)out);
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void CloseObject(IUnknown* obj) {
    if (!obj) return;
    IClosable* c = nullptr;
    if (SUCCEEDED(obj->QueryInterface(IID_IClosable, (void**)&c)) && c) {
        c->Close();
        c->Release();
    }
}

void RequestBorderless() {
    static bool done = false;
    if (done) return;
    done = true;
    IGraphicsCaptureAccessStatics* st = nullptr;
    if (FAILED(GetFactory(L"Windows.Graphics.Capture.GraphicsCaptureAccess", IID_IGraphicsCaptureAccessStatics, &st)) || !st) return;
    IInspectable* op = nullptr;
    if (SUCCEEDED(st->RequestAccessAsync(0, (void**)&op)) && op) {
        IAsyncInfoMin* info = nullptr;
        if (SUCCEEDED(op->QueryInterface(IID_IAsyncInfoMin, (void**)&info)) && info) {
            INT32 status = 0;
            for (int i = 0; i < 100 && SUCCEEDED(info->get_Status(&status)) && status == 0; i++) Sleep(10);
            SW_LOG("WGC: borderless access request status %d", status);
            info->Release();
        }
        op->Release();
    }
    st->Release();
}

class WgcCapture final : public Capture {
public:
    ~WgcCapture() override { Shutdown(); }

    bool Init(Gpu& gpu, HWND target, HMONITOR) override {
        gpu_ = &gpu;
        target_ = target;
        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(gpu.device.As(&dxgi))) return Fail("IDXGIDevice unavailable");
        SW_LOG("WGC: device");
        IInspectable* inspectable = nullptr;
        HRESULT hrd = CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), &inspectable);
        if (FAILED(hrd) || !inspectable) return Fail("CreateDirect3D11DeviceFromDXGIDevice: " + HrToString(hrd));
        hrd = inspectable->QueryInterface(IID_IDirect3DDevice_WinRT, (void**)&winrtDevice_);
        inspectable->Release();
        if (FAILED(hrd) || !winrtDevice_) return Fail("IDirect3DDevice: " + HrToString(hrd));

        IGraphicsCaptureItemInterop* interop = nullptr;
        HRESULT hr = GetFactory(L"Windows.Graphics.Capture.GraphicsCaptureItem", IID_IGraphicsCaptureItemInterop, &interop);
        if (FAILED(hr)) return Fail("GraphicsCaptureItem factory: " + HrToString(hr));
        hr = interop->CreateForWindow(target, IID_IGraphicsCaptureItem, (void**)&item_);
        interop->Release();
        if (FAILED(hr) || !item_) return Fail("CreateForWindow: " + HrToString(hr));

        SW_LOG("WGC: item created");
        if (FAILED(item_->Size(&size_))) return Fail("item size");
        SW_LOG("WGC: item %dx%d", size_.Width, size_.Height);

        IDirect3D11CaptureFramePoolStatics2* statics = nullptr;
        hr = GetFactory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", IID_IDirect3D11CaptureFramePoolStatics2, &statics);
        if (FAILED(hr)) return Fail("FramePool statics2: " + HrToString(hr));
        hr = statics->CreateFreeThreaded(winrtDevice_, kPixelFormatB8G8R8A8, 2, size_, (void**)&pool_);
        statics->Release();
        if (FAILED(hr) || !pool_) return Fail("CreateFreeThreaded: " + HrToString(hr));

        SW_LOG("WGC: pool created");
        hr = pool_->CreateCaptureSession(item_, (void**)&session_);
        if (FAILED(hr) || !session_) return Fail("CreateCaptureSession: " + HrToString(hr));

        // We draw our own cursor and do not want the yellow capture border (Windows 11).
        IGraphicsCaptureSession2* s2 = nullptr;
        if (SUCCEEDED(session_->QueryInterface(IID_IGraphicsCaptureSession2, (void**)&s2)) && s2) {
            s2->SetIsCursorCaptureEnabled(0);
            s2->Release();
        }
        // No yellow border: Windows 11 wants the borderless access requested first (granted at once
        // for desktop apps). Windows 10 cannot remove it.
        RequestBorderless();
        borderFree_ = false;
        IGraphicsCaptureSession3* s3 = nullptr;
        if (SUCCEEDED(session_->QueryInterface(IID_IGraphicsCaptureSession3, (void**)&s3)) && s3) {
            HRESULT hb = s3->SetIsBorderRequired(0);
            BYTE req = 1;
            s3->IsBorderRequired(&req);
            borderFree_ = SUCCEEDED(hb) && !req;
            s3->Release();
        }
        SW_LOG("WGC: capture border %s", borderFree_ ? "off" : "shown (cannot be removed on this system)");
        // Windows 11 24H2: the default MinUpdateInterval throttles capture to ~50 fps. One display
        // refresh lets the capture follow the game up to what the screen can show.
        IGraphicsCaptureSession5* s5 = nullptr;
        if (SUCCEEDED(session_->QueryInterface(IID_IGraphicsCaptureSession5, (void**)&s5)) && s5) {
            double hz = std::max(30.0, MonitorRefreshRate(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST)));
            INT64 ivl = std::max<INT64>(10000, (INT64)(1e7 / (hz * 1.1)));
            HRESULT hm = s5->put_MinUpdateInterval(ivl);
            SW_LOG("WGC: MinUpdateInterval %.2f ms (%s)", ivl / 1e4, HrToString(hm).c_str());
            s5->Release();
        }
        IGraphicsCaptureSession4* s4 = nullptr;
        if (SUCCEEDED(session_->QueryInterface(IID_IGraphicsCaptureSession4, (void**)&s4)) && s4) {
            dirtyRegions_ = SUCCEEDED(s4->put_DirtyRegionMode(0));
            s4->Release();
        }
        SW_LOG("WGC: dirty regions %s", dirtyRegions_ ? "on (repeated frames are ignored)" : "not available");
        hr = session_->StartCapture();
        if (FAILED(hr)) return Fail("StartCapture: " + HrToString(hr));
        SW_LOG("WGC capture started %dx%d", size_.Width, size_.Height);
        return true;
    }

    CaptureResult Poll(const RECT& client, ID3D11Texture2D* dst) override {
        CaptureResult r;
        if (suspended_) return r;
        if (!pool_) {
            r.lost = true;
            return r;
        }
        IDirect3D11CaptureFrame* frame = nullptr;
        for (;;) {
            IDirect3D11CaptureFrame* next = nullptr;
            HRESULT hr = pool_->TryGetNextFrame((void**)&next);
            if (FAILED(hr)) {
                r.lost = true;
                break;
            }
            if (!next) break;
            // Windows can deliver the same image many times (seen: 500 capture frames per second
            // for a game rendering 47). Only frames whose pixels changed are new game frames.
            bool raw = ContentChanged(next);
            bool changed = raw || !trustDirty_;
            // Safety: a whole second of frames without any reported change means the dirty regions
            // cannot be trusted on this system (or the image is static): count every frame then.
            if (dirtyRegions_) {
                double nowT = NowSeconds();
                if (checkT0_ == 0) checkT0_ = nowT;
                checkAll_++;
                checkChanged_ += raw ? 1 : 0;
                if (nowT - checkT0_ >= 1.0) {
                    bool trust = !(checkAll_ >= 30 && checkChanged_ == 0);
                    if (trust != trustDirty_) SW_LOG("WGC: dirty regions %s", trust ? "trusted again" : "report nothing, counting every frame");
                    trustDirty_ = trust;
                    checkT0_ = nowT;
                    checkAll_ = checkChanged_ = 0;
                }
            }
            if (changed) {
                r.frames++;
                contentId_++;
                INT64 rel = 0;
                next->SystemRelativeTime(&rel);
                double t = (double)rel / 1e7, now = NowSeconds();
                contentTime_ = (t > 0 && std::abs(now - t) < 1.0) ? t : now;
            }
            if (frame) {
                CloseObject(frame);
                frame->Release();
            }
            frame = next;
        }
        if (!frame) return r;
        if (dst != lastDst_) {
            lastDst_ = dst;  // new destination (resized): it needs the current image again
            copiedId_ = UINT64_MAX;
        }
        if (!copy_ || !dst || contentId_ == copiedId_) {
            // Skipped frame, or the same image we already have: release it.
            CloseObject(frame);
            frame->Release();
            return r;
        }

        WgcSizeInt32 content{};
        frame->ContentSize(&content);
        r.time = contentTime_ > 0 ? contentTime_ : NowSeconds();

        IInspectable* surface = nullptr;
        if (SUCCEEDED(frame->Surface((void**)&surface)) && surface) {
            IDirect3DDxgiInterfaceAccess* access = nullptr;
            if (SUCCEEDED(surface->QueryInterface(IID_IDirect3DDxgiInterfaceAccess, (void**)&access)) && access) {
                ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D), (void**)tex.GetAddressOf()))) {
                    D3D11_TEXTURE2D_DESC td;
                    tex->GetDesc(&td);
                    // WGC frames cover the DWM extended frame bounds of the window.
                    RECT frameBounds{};
                    if (FAILED(DwmGetWindowAttribute(target_, DWMWA_EXTENDED_FRAME_BOUNDS, &frameBounds, sizeof(frameBounds))))
                        GetWindowRect(target_, &frameBounds);
                    LONG ox = client.left - frameBounds.left;
                    LONG oy = client.top - frameBounds.top;
                    LONG w = client.right - client.left;
                    LONG h = client.bottom - client.top;
                    LONG maxW = std::min<LONG>((LONG)td.Width, content.Width);
                    LONG maxH = std::min<LONG>((LONG)td.Height, content.Height);
                    D3D11_BOX box;
                    box.left = (UINT)std::clamp<LONG>(ox, 0, maxW);
                    box.top = (UINT)std::clamp<LONG>(oy, 0, maxH);
                    box.right = (UINT)std::clamp<LONG>(ox + w, 0, maxW);
                    box.bottom = (UINT)std::clamp<LONG>(oy + h, 0, maxH);
                    box.front = 0;
                    box.back = 1;
                    if (box.right > box.left && box.bottom > box.top) {
                        gpu_->ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, tex.Get(), 0, &box);
                        r.newFrame = true;
                        copiedId_ = contentId_;
                    }
                }
                access->Release();
            }
            surface->Release();
        }
        CloseObject(frame);
        frame->Release();

        if (content.Width != size_.Width || content.Height != size_.Height) {
            if (content.Width > 0 && content.Height > 0) {
                size_ = content;
                pool_->Recreate(winrtDevice_, kPixelFormatB8G8R8A8, 2, size_);
            }
        }
        return r;
    }

    const char* Name() const override { return "Windows Graphics Capture"; }
    bool BorderFree() const override { return borderFree_; }
    void Suspend() override {
        if (suspended_) return;
        Shutdown();
        suspended_ = true;
        SW_LOG("WGC: capture suspended");
    }
    bool Resume() override {
        if (!suspended_) return true;
        suspended_ = false;
        copiedId_ = UINT64_MAX;
        SW_LOG("WGC: capture resumed");
        return Init(*gpu_, target_, nullptr);
    }

private:
    // Did this frame change anything? Windows 11 24H2+ reports the changed areas of every frame
    // (dirty regions): a frame without any is the same image delivered again (seen at 500 frames
    // per second for a game rendering 47). Free, no GPU work. Older systems: every frame counts.
    bool ContentChanged(IDirect3D11CaptureFrame* f) {
        if (!dirtyRegions_) return true;
        IDirect3D11CaptureFrame2* f2 = nullptr;
        if (FAILED(f->QueryInterface(IID_IDirect3D11CaptureFrame2, (void**)&f2)) || !f2) return true;
        bool changed = true;
        IVectorViewSizeOnly* v = nullptr;
        if (SUCCEEDED(f2->get_DirtyRegions((void**)&v)) && v) {
            UINT32 n = 0;
            if (SUCCEEDED(v->get_Size(&n))) changed = n > 0;
            v->Release();
        }
        f2->Release();
        return changed;
    }

    bool dirtyRegions_ = false;
    bool suspended_ = false;
    double checkT0_ = 0;
    bool trustDirty_ = true;
    int checkAll_ = 0, checkChanged_ = 0;
    bool borderFree_ = true;
    uint64_t contentId_ = 0, copiedId_ = UINT64_MAX;
    double contentTime_ = 0;
    ID3D11Texture2D* lastDst_ = nullptr;

    bool Fail(const std::string& msg) {
        error_ = msg;
        SW_LOG("WGC: %s", msg.c_str());
        Shutdown();
        return false;
    }

    void Shutdown() {
        if (session_) CloseObject(session_);
        if (pool_) CloseObject(pool_);
        SafeRelease(session_);
        SafeRelease(pool_);
        SafeRelease(item_);
        SafeRelease(winrtDevice_);
    }

    Gpu* gpu_ = nullptr;
    HWND target_ = nullptr;
    IInspectable* winrtDevice_ = nullptr;
    IGraphicsCaptureItem* item_ = nullptr;
    IDirect3D11CaptureFramePool* pool_ = nullptr;
    IGraphicsCaptureSession* session_ = nullptr;
    WgcSizeInt32 size_{};
};

}  // namespace

bool WgcSupported() {
    IGraphicsCaptureSessionStatics* statics = nullptr;
    if (FAILED(GetFactory(L"Windows.Graphics.Capture.GraphicsCaptureSession", IID_IGraphicsCaptureSessionStatics, &statics)) || !statics)
        return false;
    BYTE supported = 0;
    statics->IsSupported(&supported);
    statics->Release();
    return supported != 0;
}

Capture* CreateWgcCapture() { return new WgcCapture(); }

}  // namespace sw
