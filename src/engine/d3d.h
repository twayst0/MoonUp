// Direct3D 11 helpers: device creation, textures, constant buffers and the shader library.
#pragma once
#include "../common.h"

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <map>
#include <vector>
#include <string>

namespace sw {

struct AdapterInfo {
    int index = 0;
    std::string name;
    UINT vendorId = 0;
    UINT deviceId = 0;
    uint64_t dedicatedVideoMemory = 0;
    uint64_t sharedSystemMemory = 0;
    bool software = false;
    bool integrated = false;
    LUID luid{};
};

std::vector<AdapterInfo> EnumerateAdapters();
const char* VendorName(UINT vendorId);

struct Gpu {
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    AdapterInfo info;
    bool tearingSupported = false;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;

    // adapterIndex < 0: high performance GPU. 'preferLuid' (non zero) forces a specific adapter.
    bool Create(int adapterIndex, const LUID* preferLuid = nullptr);
    void Release();
};

struct Texture {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    // bind: D3D11_BIND_* flags. Creates views that match the bind flags.
    bool Create(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, UINT misc = 0);
    bool Ensure(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, UINT misc = 0) {
        if (tex && width == w && height == h && format == fmt) return true;
        return Create(dev, w, h, fmt, bind, misc);
    }
    void Reset() { *this = Texture(); }
    explicit operator bool() const { return tex != nullptr; }
};

class ConstantBuffer {
public:
    bool Create(ID3D11Device* dev, UINT bytes = 256);
    void Update(ID3D11DeviceContext* ctx, const void* data, size_t bytes);
    ID3D11Buffer* Get() const { return buf_.Get(); }
    ID3D11Buffer* const* Addr() const { return buf_.GetAddressOf(); }

private:
    ComPtr<ID3D11Buffer> buf_;
    UINT size_ = 0;
};

// Compiles the embedded HLSL at runtime with d3dcompiler_47.dll and caches the bytecode on disk.
class ShaderLibrary {
public:
    bool Init(ID3D11Device* dev);
    ID3D11ComputeShader* CS(const char* file, const char* entry);
    ID3D11VertexShader* VS(const char* file, const char* entry);
    ID3D11PixelShader* PS(const char* file, const char* entry);
    const std::string& LastError() const { return lastError_; }
    // Compiles every shader into the disk cache without a device (run once at startup).
    static void WarmCache();
    // Build tool: compiles one shader from the embedded source (no cache), returns its source hash.
    bool Precompile(const char* file, const char* entry, const char* profile, std::string& out, uint64_t& hash);

private:
    bool Blob(const char* file, const char* entry, const char* profile, std::string& out);
    std::string Source(const char* file);
    bool Compile(const std::string& src, const char* file, const char* entry, const char* profile, std::string& out);
    ID3D11Device* dev_ = nullptr;
    HMODULE compiler_ = nullptr;
    void* compileFn_ = nullptr;
    std::map<std::string, ComPtr<ID3D11ComputeShader>> cs_;
    std::map<std::string, std::string> csFailed_;
    std::map<std::string, ComPtr<ID3D11VertexShader>> vs_;
    std::map<std::string, ComPtr<ID3D11PixelShader>> ps_;
    std::string lastError_;
};

// Common sampler / blend states created once per device.
struct CommonStates {
    ComPtr<ID3D11SamplerState> point, linear;
    ComPtr<ID3D11BlendState> premulAlpha;
    ComPtr<ID3D11RasterizerState> noCull;
    bool Create(ID3D11Device* dev);
    void BindSamplers(ID3D11DeviceContext* ctx);
};

inline UINT DivUp(UINT a, UINT b) { return (a + b - 1) / b; }

// Every compute shader entry point the engine uses (file, entry). Precompiled at startup.
struct ShaderEntry {
    const char* file;
    const char* entry;
};
const std::vector<ShaderEntry>& AllComputeShaders();

// Unbinds compute shader resources (avoids read/write hazards between passes).
void ClearCS(ID3D11DeviceContext* ctx);

}  // namespace sw
