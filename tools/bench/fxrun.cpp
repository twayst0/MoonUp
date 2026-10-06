// Minimal ReShade FX runtime for testing MoonUp Upgraph without a game: parses the effect with
// ReShade's own compiler (tools/third_party/reshadefx), compiles every entry point with
// d3dcompiler and executes the technique on a test image the way ReShade's runtime does
// (implicit back buffer copies, render targets, compute dispatches, uniforms, image sources).
//
//   bench upgraph <effect.fx> <texture dir> <in.png> <out.png> [frames] [name=value ...]
//   bench upgraphcmp <effect.fx> <texture dir> <in.png>   (Neural part vs. the desktop engine)
#include <d3dcompiler.h>
#include <wincodec.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "../../src/common.h"
#include "../../src/engine/d3d.h"
#include "../../src/engine/render.h"
#include "../third_party/reshadefx/effect_codegen.hpp"
#include "../third_party/reshadefx/effect_parser.hpp"
#include "../third_party/reshadefx/effect_preprocessor.hpp"

using namespace sw;

bool BenchLoadPng(const std::wstring& path, std::vector<uint8_t>& bgra, UINT& w, UINT& h);
bool BenchSavePng(const std::wstring& path, const uint8_t* bgra, UINT w, UINT h);

namespace {

struct FxTex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT w = 0, h = 0;
};

struct FxPass {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
};

std::string AnnotationString(const std::vector<reshadefx::annotation>& a, const char* name) {
    for (auto& x : a)
        if (x.name == name) return x.value.string_data;
    return {};
}

class FxRunner {
public:
    std::string error;

    bool Load(Gpu& gpu, const std::string& path, const std::wstring& texDir, UINT w, UINT h,
              const std::map<std::string, float>& overrides) {
        gpu_ = &gpu;
        w_ = w;
        h_ = h;
        reshadefx::preprocessor pp;
        pp.add_macro_definition("__RESHADE__", "60501");
        pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
        pp.add_macro_definition("__RENDERER__", "0xb000");
        pp.add_macro_definition("__APPLICATION__", "0");
        pp.add_macro_definition("__VENDOR__", "0x1002");
        pp.add_macro_definition("__DEVICE__", "0");
        pp.add_macro_definition("BUFFER_WIDTH", std::to_string(w));
        pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(h));
        pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
        pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
        pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");
        pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");
        if (!pp.append_file(path)) {
            error = pp.errors();
            return false;
        }
        backend_.reset(reshadefx::create_codegen_hlsl(50, false, false));
        reshadefx::parser parser;
        if (!parser.parse(pp.output(), backend_.get())) {
            error = pp.errors() + parser.errors();
            return false;
        }
        mod_ = &backend_->module();
        auto* dev = gpu.device.Get();
        if (getenv("FX_VERBOSE")) {
            for (auto& t : mod_->textures) printf("tex %s / %s %ux%u fmt %d rt %d st %d\n", t.name.c_str(), t.unique_name.c_str(), t.width, t.height, (int)t.format, t.render_target, t.storage_access);
            for (auto& s : mod_->samplers) printf("sampler %s -> %s\n", s.unique_name.c_str(), s.texture_name.c_str());
            for (auto& p : mod_->techniques[0].passes) printf("pass %s vs %s ps %s cs %s rt0 %s disp %u %u %u\n", p.name.c_str(), p.vs_entry_point.c_str(), p.ps_entry_point.c_str(), p.cs_entry_point.c_str(), p.render_target_names[0].c_str(), p.viewport_width, p.viewport_height, p.viewport_dispatch_z);
        }

        // textures
        for (auto& t : mod_->textures) {
            FxTex ft;
            if (t.semantic == "COLOR" || t.semantic == "DEPTH") {
                textures_[t.unique_name] = ft;
                semantic_[t.unique_name] = t.semantic;
                continue;
            }
            D3D11_TEXTURE2D_DESC d{};
            d.Width = t.width;
            d.Height = t.height;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = (DXGI_FORMAT)t.format;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (t.render_target ? D3D11_BIND_RENDER_TARGET : 0) |
                          (t.storage_access ? D3D11_BIND_UNORDERED_ACCESS : 0);
            std::vector<uint8_t> init;
            std::string src = AnnotationString(t.annotations, "source");
            D3D11_SUBRESOURCE_DATA sd{};
            if (!src.empty()) {
                std::vector<uint8_t> bgra;
                UINT iw, ih;
                if (!BenchLoadPng(texDir + L"\\" + Wide(src), bgra, iw, ih) || iw != t.width || ih != t.height) {
                    error = "image source " + src + " missing or wrong size";
                    return false;
                }
                init.resize(bgra.size());
                for (size_t i = 0; i < bgra.size(); i += 4) {  // BGRA -> RGBA
                    init[i] = bgra[i + 2];
                    init[i + 1] = bgra[i + 1];
                    init[i + 2] = bgra[i];
                    init[i + 3] = bgra[i + 3];
                }
                sd.pSysMem = init.data();
                sd.SysMemPitch = t.width * 4;
            }
            if (FAILED(dev->CreateTexture2D(&d, init.empty() ? nullptr : &sd, &ft.tex))) {
                error = "texture " + t.unique_name;
                return false;
            }
            dev->CreateShaderResourceView(ft.tex.Get(), nullptr, &ft.srv);
            if (t.render_target) dev->CreateRenderTargetView(ft.tex.Get(), nullptr, &ft.rtv);
            if (t.storage_access) dev->CreateUnorderedAccessView(ft.tex.Get(), nullptr, &ft.uav);
            ft.w = t.width;
            ft.h = t.height;
            textures_[t.unique_name] = ft;
        }
        // samplers
        for (auto& s : mod_->samplers) {
            D3D11_SAMPLER_DESC d{};
            d.Filter = (D3D11_FILTER)s.filter;
            d.AddressU = (D3D11_TEXTURE_ADDRESS_MODE)s.address_u;
            d.AddressV = (D3D11_TEXTURE_ADDRESS_MODE)s.address_v;
            d.AddressW = (D3D11_TEXTURE_ADDRESS_MODE)s.address_w;
            d.MinLOD = s.min_lod;
            d.MaxLOD = s.max_lod;
            d.MipLODBias = s.lod_bias;
            d.MaxAnisotropy = 1;
            ComPtr<ID3D11SamplerState> st;
            dev->CreateSamplerState(&d, &st);
            samplers_.push_back(st);
        }
        // uniforms
        cbData_.assign((mod_->total_uniform_size + 15) / 16 * 16 + 16, 0);
        for (auto& u : mod_->uniforms) {
            std::string source = AnnotationString(u.annotations, "source");
            uint32_t v = 0;
            bool set = false;
            if (source == "bufready_depth") {
                v = 1;
                set = true;
            }
            if (set) {
                memcpy(&cbData_[u.offset], &v, 4);
            } else if (u.has_initializer_value) {
                memcpy(&cbData_[u.offset], u.initializer_value.as_uint, std::min<size_t>(u.size, 64));
            }
            auto it = overrides.find(u.name);
            if (it != overrides.end()) {
                if (u.type.is_floating_point()) {
                    float f = it->second;
                    memcpy(&cbData_[u.offset], &f, 4);
                } else {
                    int32_t i = (int32_t)it->second;
                    memcpy(&cbData_[u.offset], &i, 4);
                }
            }
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)cbData_.size();
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cbi{cbData_.data(), 0, 0};
        dev->CreateBuffer(&bd, &cbi, &cb_);

        // shaders
        if (mod_->techniques.empty()) {
            error = "no technique";
            return false;
        }
        for (auto& p : mod_->techniques[0].passes) {
            FxPass fp;
            auto compile = [&](const std::string& entry, const char* target, ComPtr<ID3DBlob>& blob) {
                std::string code, asmb, errs;
                if (!backend_->assemble_code_for_entry_point(entry, code, asmb, errs)) {
                    error = "assemble " + entry + ": " + errs;
                    return false;
                }
                ComPtr<ID3DBlob> err;
                double t0 = NowSeconds();
                HRESULT hr = D3DCompile(code.data(), code.size(), entry.c_str(), nullptr, nullptr, entry.c_str(), target,
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
                double dt = NowSeconds() - t0;
                if (dt > 2.0) printf("  compile %s: %.1f s\n", entry.c_str(), dt);
                if (FAILED(hr)) {
                    error = "compile " + entry + ": " + (err ? std::string((char*)err->GetBufferPointer()) : "?");
                    FILE* f = fopen("fx_failed.hlsl", "wb");
                    if (f) {
                        fwrite(code.data(), 1, code.size(), f);
                        fclose(f);
                    }
                    return false;
                }
                return true;
            };
            ComPtr<ID3DBlob> b;
            if (!p.cs_entry_point.empty()) {
                if (!compile(p.cs_entry_point, "cs_5_0", b)) return false;
                dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &fp.cs);
            } else {
                if (!compile(p.vs_entry_point, "vs_5_0", b)) return false;
                dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &fp.vs);
                if (!compile(p.ps_entry_point, "ps_5_0", b)) return false;
                dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &fp.ps);
            }
            passes_.push_back(fp);
        }
        // back buffer, its copy (COLOR) and the depth buffer
        auto make = [&](FxTex& t, DXGI_FORMAT f, UINT bind) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w;
            d.Height = h;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = f;
            d.SampleDesc.Count = 1;
            d.BindFlags = bind;
            dev->CreateTexture2D(&d, nullptr, &t.tex);
            if (bind & D3D11_BIND_SHADER_RESOURCE) dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
            if (bind & D3D11_BIND_RENDER_TARGET) dev->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv);
            t.w = w;
            t.h = h;
        };
        make(backBuffer_, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        make(color_, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        make(depth_, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        return true;
    }

    // One frame: back buffer = image, depth = synthetic, run the technique.
    void Frame(const std::vector<uint8_t>& rgba, const std::vector<float>& depth) {
        auto* ctx = gpu_->ctx.Get();
        ctx->UpdateSubresource(backBuffer_.tex.Get(), 0, nullptr, rgba.data(), w_ * 4, 0);
        ctx->UpdateSubresource(depth_.tex.Get(), 0, nullptr, depth.data(), w_ * 4, 0);
        bool needCopy = true;
        auto& tech = mod_->techniques[0];
        for (size_t i = 0; i < tech.passes.size(); i++) {
            auto& p = tech.passes[i];
            if (needCopy) ctx->CopyResource(color_.tex.Get(), backBuffer_.tex.Get());
            ID3D11Buffer* cbs[1] = {cb_.Get()};
            std::vector<ID3D11ShaderResourceView*> srvs(32, nullptr);
            std::vector<ID3D11SamplerState*> sams(16, nullptr);
            UINT nsrv = 0, nsam = 0;
            for (auto& b : p.texture_bindings) {
                srvs[b.entry_point_binding] = SrvFor(mod_->samplers[b.index].texture_name);
                nsrv = std::max(nsrv, b.entry_point_binding + 1);
            }
            for (auto& b : p.sampler_bindings) {
                sams[b.entry_point_binding] = samplers_[b.index].Get();
                nsam = std::max(nsam, b.entry_point_binding + 1);
            }
            if (!p.cs_entry_point.empty()) {
                needCopy = false;
                std::vector<ID3D11UnorderedAccessView*> uavs(8, nullptr);
                UINT nuav = 0;
                for (auto& b : p.storage_bindings) {
                    uavs[b.entry_point_binding] = textures_[mod_->storages[b.index].texture_name].uav.Get();
                    nuav = std::max(nuav, b.entry_point_binding + 1);
                }
                ctx->CSSetShader(passes_[i].cs.Get(), nullptr, 0);
                ctx->CSSetConstantBuffers(0, 1, cbs);
                if (nsrv) ctx->CSSetShaderResources(0, nsrv, srvs.data());
                if (nsam) ctx->CSSetSamplers(0, nsam, sams.data());
                if (nuav) ctx->CSSetUnorderedAccessViews(0, nuav, uavs.data(), nullptr);
                ctx->Dispatch(p.viewport_width, p.viewport_height, p.viewport_dispatch_z);
                std::vector<ID3D11ShaderResourceView*> ns(32, nullptr);
                std::vector<ID3D11UnorderedAccessView*> nu(8, nullptr);
                ctx->CSSetShaderResources(0, 32, ns.data());
                ctx->CSSetUnorderedAccessViews(0, 8, nu.data(), nullptr);
            } else {
                ID3D11RenderTargetView* rtvs[8] = {};
                UINT nrt = 0, vw = w_, vh = h_;
                if (p.render_target_names[0].empty()) {
                    needCopy = true;
                    rtvs[0] = backBuffer_.rtv.Get();
                    nrt = 1;
                } else {
                    needCopy = false;
                    for (int k = 0; k < 8 && !p.render_target_names[k].empty(); k++) {
                        FxTex& t = textures_[p.render_target_names[k]];
                        rtvs[k] = t.rtv.Get();
                        vw = t.w;
                        vh = t.h;
                        nrt++;
                    }
                }
                ctx->OMSetRenderTargets(nrt, rtvs, nullptr);
                D3D11_VIEWPORT vp{0, 0, (float)vw, (float)vh, 0, 1};
                ctx->RSSetViewports(1, &vp);
                if (!rs_) {
                    D3D11_RASTERIZER_DESC rd{};
                    rd.FillMode = D3D11_FILL_SOLID;
                    rd.CullMode = D3D11_CULL_NONE;
                    rd.DepthClipEnable = TRUE;
                    gpu_->device->CreateRasterizerState(&rd, &rs_);
                }
                ctx->RSSetState(rs_.Get());
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx->IASetInputLayout(nullptr);
                ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
                ctx->VSSetShader(passes_[i].vs.Get(), nullptr, 0);
                ctx->PSSetShader(passes_[i].ps.Get(), nullptr, 0);
                ctx->VSSetConstantBuffers(0, 1, cbs);
                ctx->PSSetConstantBuffers(0, 1, cbs);
                if (nsrv) ctx->PSSetShaderResources(0, nsrv, srvs.data());
                if (nsam) ctx->PSSetSamplers(0, nsam, sams.data());
                ctx->Draw(p.num_vertices, 0);
                std::vector<ID3D11ShaderResourceView*> ns(32, nullptr);
                ctx->PSSetShaderResources(0, 32, ns.data());
                ctx->OMSetRenderTargets(0, nullptr, nullptr);
            }
        }
    }

    void ReadBack(std::vector<uint8_t>& rgba) { Read(backBuffer_.tex.Get(), rgba, 4); }

    bool ReadTexture(const std::string& name, std::vector<uint8_t>& raw, UINT bpp) {
        auto it = textures_.find(name);
        if (it == textures_.end())
            for (auto& t : mod_->textures)
                if (t.name == name) it = textures_.find(t.unique_name);
        if (it == textures_.end() || !it->second.tex) return false;
        Read(it->second.tex.Get(), raw, bpp);
        return true;
    }

private:
    ID3D11ShaderResourceView* SrvFor(const std::string& texName) {
        auto s = semantic_.find(texName);
        if (s != semantic_.end()) return s->second == "COLOR" ? color_.srv.Get() : depth_.srv.Get();
        return textures_[texName].srv.Get();
    }
    void Read(ID3D11Texture2D* src, std::vector<uint8_t>& out, UINT bpp) {
        D3D11_TEXTURE2D_DESC d;
        src->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        gpu_->device->CreateTexture2D(&d, nullptr, &st);
        gpu_->ctx->CopyResource(st.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m;
        gpu_->ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m);
        out.resize((size_t)d.Width * d.Height * bpp);
        for (UINT y = 0; y < d.Height; y++) memcpy(&out[(size_t)y * d.Width * bpp], (uint8_t*)m.pData + y * m.RowPitch, d.Width * bpp);
        gpu_->ctx->Unmap(st.Get(), 0);
    }

    Gpu* gpu_ = nullptr;
    UINT w_ = 0, h_ = 0;
    std::unique_ptr<reshadefx::codegen> backend_;
    reshadefx::effect_module* mod_ = nullptr;
    std::map<std::string, FxTex> textures_;
    std::map<std::string, std::string> semantic_;
    std::vector<ComPtr<ID3D11SamplerState>> samplers_;
    std::vector<FxPass> passes_;
    std::vector<uint8_t> cbData_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11RasterizerState> rs_;
    FxTex backBuffer_, color_, depth_;
};

// Synthetic reversed-Z depth: a floor plane, a wall and two spheres (enough for contact shadows).
std::vector<float> SyntheticDepth(UINT w, UINT h) {
    std::vector<float> d((size_t)w * h);
    for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++) {
            float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            float z = 0.9f;  // wall
            if (v > 0.55f) z = std::min(z, 0.05f + 0.85f * (1.0f - (v - 0.55f) / 0.45f) * 0.9f);  // floor
            for (int s = 0; s < 2; s++) {
                float cx = s ? 0.7f : 0.35f, cy = 0.62f, r = s ? 0.12f : 0.18f;
                float dx = (u - cx) * w / h, dy = v - cy, q = dx * dx + dy * dy;
                if (q < r * r) z = std::min(z, 0.3f + s * 0.1f - std::sqrt(r * r - q));
            }
            // view distance (far plane 1000, near 1) -> conventional depth -> reversed depth
            const float F = 1000.0f, N = 1.0f;
            float dist = 2.0f + std::clamp(z, 0.0f, 1.0f) * 60.0f;
            float raw = F / (F - N) * (1.0f - N / dist);
            d[(size_t)y * w + x] = 1.0f - std::clamp(raw, 0.0f, 1.0f);  // reversed
        }
    return d;
}

}  // namespace

int UpgraphTest(int argc, wchar_t** argv, bool compare) {
    if (argc < 5) {
        printf("usage: bench upgraph <fx> <texdir> <in.png> <out.png> [frames] [name=value ...]\n");
        return 1;
    }
    std::string fx = Utf8(argv[2]);
    std::wstring texDir = argv[3];
    std::vector<uint8_t> bgra;
    UINT w, h;
    if (!BenchLoadPng(argv[4], bgra, w, h)) {
        printf("cannot load input\n");
        return 1;
    }
    std::vector<uint8_t> rgba(bgra.size());
    for (size_t i = 0; i < bgra.size(); i += 4) {
        rgba[i] = bgra[i + 2];
        rgba[i + 1] = bgra[i + 1];
        rgba[i + 2] = bgra[i];
        rgba[i + 3] = 255;
    }
    int frames = (!compare && argc > 6) ? _wtoi(argv[6]) : 3;
    std::map<std::string, float> ov;
    for (int i = compare ? 5 : 7; i < argc; i++) {
        std::string a = Utf8(argv[i]);
        size_t e = a.find('=');
        if (e != std::string::npos) ov[a.substr(0, e)] = (float)atof(a.c_str() + e + 1);
    }
    if (compare) {
        ov["LightStrength"] = 0;
        ov["Sharpness"] = 0;
    }
    Gpu gpu;
    if (!gpu.Create(-1)) return 2;
    FxRunner run;
    double t0 = NowSeconds();
    if (!run.Load(gpu, fx, texDir, w, h, ov)) {
        printf("FX error: %s\n", run.error.c_str());
        return 3;
    }
    printf("effect loaded and compiled in %.1f s\n", NowSeconds() - t0);
    std::vector<float> depth = SyntheticDepth(w, h);
    for (int f = 0; f < frames; f++) run.Frame(rgba, depth);
    std::vector<uint8_t> out;
    run.ReadBack(out);
    // sanity: NaN-free (unorm output cannot be NaN), mean change
    double diff = 0;
    for (size_t i = 0; i < out.size(); i += 4)
        for (int c = 0; c < 3; c++) diff += std::abs((int)out[i + c] - (int)rgba[i + c]);
    printf("mean |out - in| = %.2f / 255\n", diff / (w * h * 3.0));
    std::vector<uint8_t> info;
    if (run.ReadTexture("UG_DepthInfoTex", info, 8)) {
        auto half = [](uint16_t hv) {
            uint32_t s = (hv >> 15) & 1, e = (hv >> 10) & 31, m = hv & 1023;
            float f = e == 0 ? m / 1024.0f * std::pow(2.0f, -14.0f) : (1 + m / 1024.0f) * std::pow(2.0f, (float)e - 15);
            return s ? -f : f;
        };
        uint16_t* hv = (uint16_t*)info.data();
        printf("depth info: reversed %.0f usable %.0f avg %.3f\n", half(hv[0]), half(hv[1]), half(hv[2]));
    }

    {
        // intermediate textures (RGBA16F / RG16F / R16F) for debugging
        const char* names[] = {"UG_LowTex", "UG_F1Tex", "UG_F4Tex", "UG_F6Tex", "UG_GridTex", "UG_HalfTex", "UG_SATex", "UG_SRawTex", "UG_SMapTex", "UG_AOTex", "UG_StateTex", "UG_GlobalTex"};
        UINT bpps[] = {8, 8, 8, 8, 8, 2, 8, 4, 4, 4, 8, 16};
        for (int k = 0; k < 12; k++) {
            std::vector<uint8_t> raw;
            if (!run.ReadTexture(names[k], raw, bpps[k])) { printf("  %s: missing\n", names[k]); continue; }
            double sum = 0; size_t n = 0; int nans = 0;
            if (bpps[k] == 16) {
                float* f = (float*)raw.data();
                for (size_t i = 0; i < raw.size() / 4; i++) { if (std::isnan(f[i])) nans++; else sum += std::abs(f[i]); n++; }
            } else {
                uint16_t* hv = (uint16_t*)raw.data();
                for (size_t i = 0; i < raw.size() / 2; i++) {
                    uint32_t e = (hv[i] >> 10) & 31, m = hv[i] & 1023;
                    if (e == 31) { nans++; continue; }
                    float f = e == 0 ? m / 1024.0f * std::pow(2.0f, -14.0f) : (1 + m / 1024.0f) * std::pow(2.0f, (float)e - 15);
                    sum += f; n++;
                }
            }
            printf("  %-14s mean|x| %.4f  nan/inf %d\n", names[k], n ? sum / n : 0.0, nans);
        }
    }
    if (!compare) {
        std::vector<uint8_t> ob(out.size());
        for (size_t i = 0; i < out.size(); i += 4) {
            ob[i] = out[i + 2];
            ob[i + 1] = out[i + 1];
            ob[i + 2] = out[i];
            ob[i + 3] = 255;
        }
        BenchSavePng(argv[5], ob.data(), w, h);
        return 0;
    }

    // Desktop engine Neural Render on the same frames.
    ShaderLibrary lib;
    lib.Init(gpu.device.Get());
    CommonStates states;
    states.Create(gpu.device.Get());
    NeuralRender nr;
    if (!nr.Init(gpu, lib)) {
        printf("engine NR init failed\n");
        return 4;
    }
    Texture src, dst;
    src.Create(gpu.device.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    dst.Create(gpu.device.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    gpu.ctx->UpdateSubresource(src.tex.Get(), 0, nullptr, rgba.data(), w * 4, 0);
    NeuralRenderConfig rc;
    rc.enabled = true;
    for (int f = 0; f < frames; f++) {
        states.BindSamplers(gpu.ctx.Get());
        nr.Run(src.srv.Get(), w, h, dst, rc);
    }
    D3D11_TEXTURE2D_DESC d;
    dst.tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    gpu.device->CreateTexture2D(&d, nullptr, &st);
    gpu.ctx->CopyResource(st.Get(), dst.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    gpu.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m);
    double sum = 0, mx = 0, base = 0;
    for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                int a = ((uint8_t*)m.pData)[y * m.RowPitch + x * 4 + c];
                int b = out[((size_t)y * w + x) * 4 + c];
                double e = std::abs(a - b);
                sum += e;
                mx = std::max(mx, e);
                base += std::abs(a - (int)rgba[((size_t)y * w + x) * 4 + c]);
            }
    gpu.ctx->Unmap(st.Get(), 0);
    double n = w * h * 3.0;
    printf("engine vs Upgraph: mean %.3f, max %.0f (engine effect strength: mean %.2f) /255\n", sum / n, mx, base / n);
    return 0;
}
