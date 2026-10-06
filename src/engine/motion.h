// MoonUp Motion: optical flow + frame interpolation (frame generation).
#pragma once
#include "config.h"
#include "d3d.h"

namespace sw {

class Motion {
public:
    bool Init(Gpu& gpu, ShaderLibrary& lib, CommonStates& states);
    void Reset();

    // Registers a new source frame (pre-upscale image). Computes the flow fields for the pair
    // (seq-1, seq) when the previous frame is known.
    bool AddFrame(ID3D11ShaderResourceView* source, UINT w, UINT h, FlowQuality q, uint64_t seq);
    bool HasPair(uint64_t seqB) const;

    // Writes the in-between frame at time t (0 = A, 1 = B) for the pair ending at seqB.
    bool Interpolate(uint64_t seqB, ID3D11ShaderResourceView* frameA, ID3D11ShaderResourceView* frameB, float t,
                     Texture& out, const EngineConfig& cfg);

    const std::string& Error() const { return error_; }

    // Tuning (exposed for the bench).
    struct Params {
        float lambda = 0.003f;
        float zeroBias = 0.0f;
        float meanRemoval = 0.0f;
        int propagate = 2;
        float sigma = 0.01f;          // soft-min temperature of the candidate selection
        float interpZeroBias = 0.004f; // static candidate bonus (HUD protection)
        float staticEps = 1.5f / 255.0f;
        float staticFrames = 2.0f;
        float refineStep = 0.5f;
    };
    Params& Tuning() { return params_; }
    // Debug access to a pair's fields (bench only).
    Texture* DebugFlow(uint64_t seqB, bool ab) {
        Pair& p = pairs_[seqB & 1];
        return p.seqB == seqB ? (ab ? &p.flowAB : &p.flowBA) : nullptr;
    }

private:
    Params params_;
    static constexpr int kMaxLevels = 6;
    struct Pyramid {
        Texture level[kMaxLevels];
        int levels = 0;
        uint64_t seq = UINT64_MAX;
    };
    struct Pair {
        Texture flowAB, flowBA, scene;
        uint64_t seqB = UINT64_MAX;
        UINT flowW = 0, flowH = 0;
    };
    bool BuildPyramid(Pyramid& p, ID3D11ShaderResourceView* src, UINT w, UINT h);
    bool Estimate(const Pyramid& a, const Pyramid& b, int finest, Texture& result, Texture* scene, const Texture* prev);
    void Run(ID3D11ComputeShader* cs, UINT w, UINT h, const void* cb, size_t cbSize);

    Gpu* gpu_ = nullptr;
    ShaderLibrary* lib_ = nullptr;
    CommonStates* states_ = nullptr;
    ConstantBuffer cb_;
    Pyramid pyr_[2];
    Pair pairs_[2];
    Texture raw_[kMaxLevels], tmp_[kMaxLevels], filtered_[kMaxLevels], cost_;
    Texture sel_, selW_, static_[2];
    int staticCur_ = 0;
    uint64_t staticSeq_ = UINT64_MAX;
    std::string error_;
};

}  // namespace sw
