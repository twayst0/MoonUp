#include "d3d.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>

#include "shaders_gen.h"
#include "shaders_bin.h"

namespace sw {

const char* VendorName(UINT v) {
    switch (v) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: case 0x1022: return "AMD";
        case 0x8086: case 0x8087: return "Intel";
        case 0x5143: return "Qualcomm";
        case 0x1414: return "Microsoft";
        default: return "Unknown";
    }
}

static AdapterInfo DescribeAdapter(IDXGIAdapter1* a, int index) {
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    AdapterInfo info;
    info.index = index;
    info.name = Utf8(d.Description);
    info.vendorId = d.VendorId;
    info.deviceId = d.DeviceId;
    info.dedicatedVideoMemory = d.DedicatedVideoMemory;
    info.sharedSystemMemory = d.SharedSystemMemory;
    info.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    info.luid = d.AdapterLuid;
    // Heuristic: small dedicated memory -> integrated / APU.
    bool intelArc = d.VendorId == 0x8086 && info.name.find("Arc") != std::string::npos;
    info.integrated = !info.software && !intelArc &&
                      (d.DedicatedVideoMemory < (512ull << 20) || (d.VendorId == 0x8086 && !intelArc));
    return info;
}

std::vector<AdapterInfo> EnumerateAdapters() {
    std::vector<AdapterInfo> out;
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return out;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        AdapterInfo info = DescribeAdapter(a.Get(), (int)i);
        if (!info.software) out.push_back(info);
        a.Reset();
    }
    return out;
}

bool Gpu::Create(int adapterIndex, const LUID* preferLuid) {
    Release();
    ComPtr<IDXGIFactory2> f2;
    if (!Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&f2)), "CreateDXGIFactory2")) return false;
    factory = f2;

    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(factory.As(&f5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            tearingSupported = allow != FALSE;
    }

    ComPtr<IDXGIAdapter1> chosen;
    if (preferLuid && (preferLuid->LowPart || preferLuid->HighPart)) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            if (d.AdapterLuid.LowPart == preferLuid->LowPart && d.AdapterLuid.HighPart == preferLuid->HighPart) {
                chosen = a;
                info = DescribeAdapter(a.Get(), (int)i);
                break;
            }
            a.Reset();
        }
    }
    if (!chosen && adapterIndex >= 0) {
        if (SUCCEEDED(factory->EnumAdapters1((UINT)adapterIndex, &chosen))) info = DescribeAdapter(chosen.Get(), adapterIndex);
    }
    if (!chosen) {
        ComPtr<IDXGIFactory6> f6;
        if (SUCCEEDED(factory.As(&f6))) {
            ComPtr<IDXGIAdapter1> a;
            if (SUCCEEDED(f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)))) {
                chosen = a;
                // find its index for display purposes
                ComPtr<IDXGIAdapter1> b;
                DXGI_ADAPTER_DESC1 da;
                chosen->GetDesc1(&da);
                for (UINT i = 0; factory->EnumAdapters1(i, &b) != DXGI_ERROR_NOT_FOUND; i++) {
                    DXGI_ADAPTER_DESC1 db;
                    b->GetDesc1(&db);
                    if (db.AdapterLuid.LowPart == da.AdapterLuid.LowPart && db.AdapterLuid.HighPart == da.AdapterLuid.HighPart) {
                        info = DescribeAdapter(chosen.Get(), (int)i);
                        break;
                    }
                    b.Reset();
                }
            }
        }
    }
    if (!chosen) {
        if (FAILED(factory->EnumAdapters1(0, &chosen))) return false;
        info = DescribeAdapter(chosen.Get(), 0);
    }
    adapter = chosen;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2,
                                   D3D11_SDK_VERSION, &device, &featureLevel, &ctx);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels + 1, 1,
                               D3D11_SDK_VERSION, &device, &featureLevel, &ctx);
    }
    if (!Check(hr, "D3D11CreateDevice")) return false;

    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice1> dxgiDev;
    if (SUCCEEDED(device.As(&dxgiDev))) {
        dxgiDev->SetMaximumFrameLatency(1);
        // The game comes first: our GPU work yields to it when the GPU is contended.
        dxgiDev->SetGPUThreadPriority(-3);
    }
    {
        typedef LONG(WINAPI * SetPrioFn)(HANDLE, int);
        HMODULE g = GetModuleHandleW(L"gdi32.dll");
        auto fn = g ? (SetPrioFn)GetProcAddress(g, "D3DKMTSetProcessSchedulingPriorityClass") : nullptr;
        if (fn) {
            LONG st = fn(GetCurrentProcess(), 1);  // D3DKMT_SCHEDULINGPRIORITYCLASS_BELOW_NORMAL
            SW_LOG("GPU scheduling priority: below normal (0x%lx)", (unsigned long)st);
        }
    }

    SW_LOG("GPU: %s (vendor %s, %llu MB dedicated, FL %x, tearing %d)", info.name.c_str(), VendorName(info.vendorId),
           (unsigned long long)(info.dedicatedVideoMemory >> 20), (unsigned)featureLevel, (int)tearingSupported);
    return true;
}

void Gpu::Release() {
    if (ctx) {
        ctx->ClearState();
        ctx->Flush();
    }
    ctx.Reset();
    device.Reset();
    adapter.Reset();
    factory.Reset();
}

// ------------------------------------------------------------------------------------ Texture
bool Texture::Create(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, UINT misc) {
    Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = std::max(1u, w);
    d.Height = std::max(1u, h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    d.MiscFlags = misc;
    if (!Check(dev->CreateTexture2D(&d, nullptr, &tex), "CreateTexture2D")) return false;
    if (bind & D3D11_BIND_SHADER_RESOURCE) dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (bind & D3D11_BIND_UNORDERED_ACCESS) dev->CreateUnorderedAccessView(tex.Get(), nullptr, &uav);
    if (bind & D3D11_BIND_RENDER_TARGET) dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
    width = d.Width;
    height = d.Height;
    format = fmt;
    return true;
}

// ------------------------------------------------------------------------------------ CB
bool ConstantBuffer::Create(ID3D11Device* dev, UINT bytes) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = (bytes + 15) & ~15u;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    size_ = d.ByteWidth;
    return Check(dev->CreateBuffer(&d, nullptr, &buf_), "CreateBuffer(cb)");
}

void ConstantBuffer::Update(ID3D11DeviceContext* ctx, const void* data, size_t bytes) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(buf_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memset(m.pData, 0, size_);
        memcpy(m.pData, data, std::min<size_t>(bytes, size_));
        ctx->Unmap(buf_.Get(), 0);
    }
}

// ------------------------------------------------------------------------------------ shaders
typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                        LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

bool ShaderLibrary::Init(ID3D11Device* dev) {
    dev_ = dev;
    cs_.clear();
    vs_.clear();
    ps_.clear();
    if (!compiler_) {
        compiler_ = LoadLibraryW(L"d3dcompiler_47.dll");
        if (compiler_) compileFn_ = (void*)GetProcAddress(compiler_, "D3DCompile");
    }
    if (!compileFn_) SW_LOG("d3dcompiler_47.dll not available, relying on the shader cache only");
    CreateDirectoryW((CacheDir() + L"\\shaders").c_str(), nullptr);
    return true;
}

static uint64_t Fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Development: MOONUP_SHADER_DIR points at src/engine/shaders so edits apply without a rebuild.
static const char* DevShaderSource(const char* file) {
    static std::map<std::string, std::string> cache;
    wchar_t dir[512];
    DWORD n = GetEnvironmentVariableW(L"MOONUP_SHADER_DIR", dir, 512);
    if (!n || n >= 512) return nullptr;
    auto it = cache.find(file);
    if (it == cache.end()) {
        std::wstring path = std::wstring(dir) + L"\\" + Wide(file);
        std::string text;
        if (FILE* f = _wfopen(path.c_str(), L"rb")) {
            char buf[4096];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
            fclose(f);
        }
        it = cache.emplace(file, text).first;
    }
    return it->second.empty() ? nullptr : it->second.c_str();
}

std::string ShaderLibrary::Source(const char* file) {
    const char* common = DevShaderSource("common.hlsli");
    const char* body = DevShaderSource(file);
    if (!common) common = shaders::Find("common.hlsli");
    if (!body) body = shaders::Find(file);
    if (!common || !body) return std::string();
    return std::string(common) + "\n#line 1 \"" + file + "\"\n" + body;
}

bool ShaderLibrary::Compile(const std::string& src, const char* file, const char* entry, const char* profile, std::string& out) {
    if (!compileFn_) {
        lastError_ = "d3dcompiler_47.dll missing";
        return false;
    }
    ComPtr<ID3DBlob> code, errors;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
    HRESULT hr = ((PFN_D3DCompile)compileFn_)(src.data(), src.size(), file, nullptr, nullptr, entry, profile, flags, 0,
                                              &code, &errors);
    if (FAILED(hr)) {
        lastError_ = std::string(file) + ":" + entry + " -> " +
                     (errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : HrToString(hr));
        SW_LOG("Shader compile error: %s", lastError_.c_str());
        return false;
    }
    out.assign((const char*)code->GetBufferPointer(), code->GetBufferSize());
    return true;
}

bool ShaderLibrary::Precompile(const char* file, const char* entry, const char* profile, std::string& out, uint64_t& hash) {
    std::string src = Source(file);
    if (src.empty()) {
        lastError_ = std::string("missing shader source ") + file;
        return false;
    }
    hash = Fnv1a(src);
    return Compile(src, file, entry, profile, out);
}

bool ShaderLibrary::Blob(const char* file, const char* entry, const char* profile, std::string& out) {
    std::string src = Source(file);
    if (src.empty()) {
        lastError_ = std::string("missing shader source ") + file;
        return false;
    }
    // 1. Shaders compiled at build time and embedded in the executable (no compiler work on the
    //    user's machine). The source hash guarantees the blob matches this build's source.
    if (const auto* b = shaders_bin::Find(file, entry, profile, Fnv1a(src))) {
        out.assign((const char*)b->data, b->size);
        return true;
    }
    uint64_t key = Fnv1a(src + "|" + entry + "|" + profile + "|v3");
    wchar_t name[64];
    swprintf(name, 64, L"\\shaders\\%016llx.cso", (unsigned long long)key);
    std::wstring path = CacheDir() + name;

    if (FILE* in = _wfopen(path.c_str(), L"rb")) {
        fseek(in, 0, SEEK_END);
        long n = ftell(in);
        fseek(in, 0, SEEK_SET);
        out.resize(n > 0 ? (size_t)n : 0);
        size_t got = n > 0 ? fread(out.data(), 1, (size_t)n, in) : 0;
        fclose(in);
        if (got == out.size() && !out.empty()) return true;
        out.clear();
    }
    if (!Compile(src, file, entry, profile, out)) return false;
    if (FILE* o = _wfopen(path.c_str(), L"wb")) {
        fwrite(out.data(), 1, out.size(), o);
        fclose(o);
    }
    return true;
}

const std::vector<ShaderEntry>& AllComputeShaders() {
    static const std::vector<ShaderEntry> kAll = {
        {"upscale.hlsl", "CSCopy"},     {"upscale.hlsl", "CSEdge"},      {"upscale.hlsl", "CSBilinear"},
        {"upscale.hlsl", "CSLanczos"},  {"upscale.hlsl", "CSBicubic"},   {"upscale.hlsl", "CSNearest"},
        {"upscale.hlsl", "CSPixel"},    {"post.hlsl", "CSSharpen"},      {"post.hlsl", "CSVision"},
        {"post.hlsl", "CSDown4"},       {"post.hlsl", "CSBlur"},         {"flow.hlsl", "CSLuma"},
        {"flow.hlsl", "CSDown2"},       {"flow.hlsl", "CSFlow"},         {"flow.hlsl", "CSPropagate"},
        {"flow.hlsl", "CSRefine"},      {"flow.hlsl", "CSMedian"},       {"flow.hlsl", "CSScene"},
        {"interp.hlsl", "CSStatic"},    {"interp.hlsl", "CSSelect"},     {"interp.hlsl", "CSCompose"},
        {"neural.hlsl", "CSConvIn"},    {"neural.hlsl", "CSConvMid"},    {"neural.hlsl", "CSConvOut"},
        {"render.hlsl", "CSDownIn"},    {"render.hlsl", "CSConv"},       {"render.hlsl", "CSGlobal"},
        {"render.hlsl", "CSFuse"},      {"render.hlsl", "CSApply"},      {"render.hlsl", "CSDownHalf"},
        {"render.hlsl", "CSStructBlend"}, {"render.hlsl", "CSStructIn"}, {"render.hlsl", "CSStructMid"},
        {"render.hlsl", "CSStructOut"}, {"fsr.hlsl", "CSEasu"},        {"fsr.hlsl", "CSRcas"},
        {"nis.hlsl", "CSNis"},
    };
    return kAll;
}

void ShaderLibrary::WarmCache() {
    ShaderLibrary lib;
    lib.Init(nullptr);
    std::string blob;
    double t0 = NowSeconds();
    int n = 0;
    for (auto& e : AllComputeShaders()) n += lib.Blob(e.file, e.entry, "cs_5_0", blob) ? 1 : 0;
    n += lib.Blob("present.hlsl", "VSFull", "vs_5_0", blob) ? 1 : 0;
    n += lib.Blob("present.hlsl", "PSFrame", "ps_5_0", blob) ? 1 : 0;
    n += lib.Blob("present.hlsl", "VSQuad", "vs_5_0", blob) ? 1 : 0;
    n += lib.Blob("present.hlsl", "PSQuad", "ps_5_0", blob) ? 1 : 0;
    SW_LOG("Shader cache ready (%d shaders, %.2f s)", n, NowSeconds() - t0);
}

ID3D11ComputeShader* ShaderLibrary::CS(const char* file, const char* entry) {
    std::string key = std::string(file) + ":" + entry;
    auto it = cs_.find(key);
    if (it != cs_.end()) return it->second.Get();
    auto bad = csFailed_.find(key);
    if (bad != csFailed_.end()) {
        lastError_ = bad->second;  // do not retry a failed compile every frame
        return nullptr;
    }
    std::string blob;
    ComPtr<ID3D11ComputeShader> s;
    if (!Blob(file, entry, "cs_5_0", blob) || !Check(dev_->CreateComputeShader(blob.data(), blob.size(), nullptr, &s), entry)) {
        csFailed_[key] = lastError_;
        return nullptr;
    }
    cs_[key] = s;
    return s.Get();
}

ID3D11VertexShader* ShaderLibrary::VS(const char* file, const char* entry) {
    std::string key = std::string(file) + ":" + entry;
    auto it = vs_.find(key);
    if (it != vs_.end()) return it->second.Get();
    std::string blob;
    if (!Blob(file, entry, "vs_5_0", blob)) return nullptr;
    ComPtr<ID3D11VertexShader> s;
    if (!Check(dev_->CreateVertexShader(blob.data(), blob.size(), nullptr, &s), entry)) return nullptr;
    vs_[key] = s;
    return s.Get();
}

ID3D11PixelShader* ShaderLibrary::PS(const char* file, const char* entry) {
    std::string key = std::string(file) + ":" + entry;
    auto it = ps_.find(key);
    if (it != ps_.end()) return it->second.Get();
    std::string blob;
    if (!Blob(file, entry, "ps_5_0", blob)) return nullptr;
    ComPtr<ID3D11PixelShader> s;
    if (!Check(dev_->CreatePixelShader(blob.data(), blob.size(), nullptr, &s), entry)) return nullptr;
    ps_[key] = s;
    return s.Get();
}

// ------------------------------------------------------------------------------------ states
bool CommonStates::Create(ID3D11Device* dev) {
    D3D11_SAMPLER_DESC s{};
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(dev->CreateSamplerState(&s, &point))) return false;
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(dev->CreateSamplerState(&s, &linear))) return false;

    D3D11_BLEND_DESC b{};
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&b, &premulAlpha))) return false;

    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    return SUCCEEDED(dev->CreateRasterizerState(&r, &noCull));
}

void CommonStates::BindSamplers(ID3D11DeviceContext* ctx) {
    ID3D11SamplerState* ss[2] = {point.Get(), linear.Get()};
    ctx->CSSetSamplers(0, 2, ss);
    ctx->PSSetSamplers(0, 2, ss);
}

void ClearCS(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* nullSrv[8] = {};
    ID3D11UnorderedAccessView* nullUav[4] = {};
    ctx->CSSetShaderResources(0, 8, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 4, nullUav, nullptr);
}

}  // namespace sw
