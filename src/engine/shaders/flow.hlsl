// MoonUp Motion: optical flow estimation for frame generation.
//
// Dense, per-pixel, coarse-to-fine block matching on a luma pyramid. At every level:
//   1. CSFlow       candidates: the upsampled coarser field and its neighbours, zero motion and
//                   the previous frame pair's motion (temporal prediction, keeps fast and
//                   consistent motion locked); the coarsest level also searches exhaustively.
//   2. CSPropagate  passes where every pixel tries the vectors of its neighbours (PatchMatch
//                   style), so good vectors spread across objects.
//   3. CSRefine     integer, half and quarter pixel refinement around the winner.
//   4. CSMedian     3x3 vector median.
// The cost is the mean absolute luma difference over a 5x5 patch with (part of) the patch mean
// difference removed (robust to fades and flashes), plus a smoothness penalty towards the
// predicted vector and a small bias towards zero motion (keeps HUDs and text still).

Texture2D<float4> Rgb : register(t3);
Texture2D<float> LumA : register(t0);
Texture2D<float> LumB : register(t1);
Texture2D<float2> Coarse : register(t2);
Texture2D<float2> FlowIn : register(t4);
Texture2D<float> CostIn : register(t5);
Texture2D<float2> Prev : register(t6);

RWTexture2D<float> LumOut : register(u0);
RWTexture2D<float2> FlowOut : register(u1);
RWTexture2D<float> CostOut : register(u2);
RWTexture2D<float> SceneOut : register(u3);

cbuffer FlowParams : register(b0) {
    int2 size;          // size of the level being written
    float2 invSize;
    int2 srcSize;       // input size (luma / downsample passes) or coarse size (flow pass)
    float2 invSrcSize;
    int hasCoarse;
    int radius;         // exhaustive search radius (coarsest level) or propagation step
    float lambda;
    float zeroBias;
    int hasPrev;
    float prevScale;    // previous field units -> this level's pixels
    float meanRemoval;  // 0..1: how much of the patch mean difference is ignored
    float pad0;
};

// ------------------------------------------------------------------ luma pyramid
[numthreads(8, 8, 1)]
void CSLuma(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    // Box filter the footprint of this texel with 4 bilinear taps.
    float2 ratio = float2(srcSize) * invSize;
    float2 center = (float2(id.xy) + 0.5) * ratio * invSrcSize;
    float2 o = 0.25 * ratio * invSrcSize;
    float3 s = Rgb.SampleLevel(sLinear, center + float2(-o.x, -o.y), 0).rgb +
               Rgb.SampleLevel(sLinear, center + float2( o.x, -o.y), 0).rgb +
               Rgb.SampleLevel(sLinear, center + float2(-o.x,  o.y), 0).rgb +
               Rgb.SampleLevel(sLinear, center + float2( o.x,  o.y), 0).rgb;
    LumOut[id.xy] = Luma(s * 0.25);
}

[numthreads(8, 8, 1)]
void CSDown2(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    float2 uv = (float2(id.xy) * 2.0 + 1.0) * invSrcSize;
    LumOut[id.xy] = LumA.SampleLevel(sLinear, uv, 0);
}

// ------------------------------------------------------------------ matching cost
struct Patch {
    float v[25];
    float mean;
};

Patch LoadPatch(int2 c) {
    Patch p;
    float s = 0;
    [unroll] for (int j = -2; j <= 2; j++) {
        [unroll] for (int i = -2; i <= 2; i++) {
            int2 q = clamp(c + int2(i, j), int2(0, 0), size - 1);
            float l = LumA.Load(int3(q, 0));
            p.v[(j + 2) * 5 + (i + 2)] = l;
            s += l;
        }
    }
    p.mean = s * (1.0 / 25.0);
    return p;
}

float PatchCost(Patch a, float2 p, float2 v) {
    float b[25];
    float mb = 0;
    [unroll] for (int j = -2; j <= 2; j++) {
        [unroll] for (int i = -2; i <= 2; i++) {
            float2 uv = (p + v + float2(i, j) + 0.5) * invSize;
            float l = LumB.SampleLevel(sLinear, uv, 0);
            b[(j + 2) * 5 + (i + 2)] = l;
            mb += l;
        }
    }
    mb *= (1.0 / 25.0);
    float off = (mb - a.mean) * meanRemoval;
    float s = 0;
    [unroll] for (int k = 0; k < 25; k++) s += abs(a.v[k] - b[k] + off);
    // Vectors that leave the frame have no evidence; make them slightly worse than equal ones.
    float2 q = p + v;
    float outside = (any(q < -0.5) || any(q > float2(size) - 0.5)) ? 0.01 : 0.0;
    return s * (1.0 / 25.0) + outside;
}

void Consider(Patch a, float2 p, float2 v, float2 pred, inout float bestCost, inout float2 best) {
    float c = PatchCost(a, p, v) + lambda * length(v - pred);
    if (all(v == 0)) c -= zeroBias;
    if (c < bestCost) { bestCost = c; best = v; }
}

float2 Predict(float2 p) {
    if (hasCoarse == 0) return 0;
    return Coarse.SampleLevel(sLinear, (p + 0.5) * invSize, 0) * 2.0;
}

int2 Ring(int k, int s) {
    return k == 0 ? int2(s, 0) : k == 1 ? int2(-s, 0) : k == 2 ? int2(0, s) : k == 3 ? int2(0, -s)
         : k == 4 ? int2(s, s) : k == 5 ? int2(-s, -s) : k == 6 ? int2(s, -s) : int2(-s, s);
}

// ------------------------------------------------------------------ 1. initial candidates
[numthreads(8, 8, 1)]
void CSFlow(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    float2 p = float2(id.xy);
    Patch a = LoadPatch(int2(id.xy));

    float2 best = 0;
    float bestCost = 1e9;
    float2 pred = Predict(p);

    Consider(a, p, 0, pred, bestCost, best);
    if (hasPrev != 0) {
        // Same place, and advected: where this pixel was one frame earlier, moving at its
        // previous velocity (small fast objects have moved away from their old position).
        float2 tv = Prev.SampleLevel(sLinear, (p + 0.5) * invSize, 0) * prevScale;
        Consider(a, p, tv, pred, bestCost, best);
        float2 tv2 = Prev.SampleLevel(sLinear, (p - tv + 0.5) * invSize, 0) * prevScale;
        Consider(a, p, tv2, pred, bestCost, best);
        float2 tv3 = Prev.SampleLevel(sLinear, (p - tv2 + 0.5) * invSize, 0) * prevScale;
        Consider(a, p, tv3, pred, bestCost, best);
    }
    if (hasCoarse != 0) {
        Consider(a, p, pred, pred, bestCost, best);
        int2 cp = clamp(int2(id.xy) / 2, int2(0, 0), srcSize - 1);
        [unroll] for (int k = 0; k < 4; k++) {
            Consider(a, p, Coarse.Load(int3(clamp(cp + Ring(k, 1), int2(0, 0), srcSize - 1), 0)) * 2.0, pred, bestCost, best);
            Consider(a, p, Coarse.Load(int3(clamp(cp + Ring(k, 2), int2(0, 0), srcSize - 1), 0)) * 2.0, pred, bestCost, best);
        }
    } else {
        for (int y = -radius; y <= radius; y++)
            for (int x = -radius; x <= radius; x++)
                Consider(a, p, float2(x, y), pred, bestCost, best);
    }
    FlowOut[id.xy] = best;
}

// ------------------------------------------------------------------ 2. propagation
[numthreads(8, 8, 1)]
void CSPropagate(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    float2 p = float2(id.xy);
    Patch a = LoadPatch(int2(id.xy));
    float2 pred = Predict(p);
    float2 own = FlowIn.Load(int3(id.xy, 0));
    float2 best = own;
    float bestCost = 1e9;
    Consider(a, p, own, pred, bestCost, best);
    [unroll] for (int k = 0; k < 8; k++) {
        float2 v = FlowIn.Load(int3(clamp(int2(id.xy) + Ring(k, radius), int2(0, 0), size - 1), 0));
        Consider(a, p, v, pred, bestCost, best);
    }
    FlowOut[id.xy] = best;
}

// ------------------------------------------------------------------ 3. refinement
[numthreads(8, 8, 1)]
void CSRefine(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    float2 p = float2(id.xy);
    Patch a = LoadPatch(int2(id.xy));
    float2 pred = Predict(p);
    float2 best = FlowIn.Load(int3(id.xy, 0));
    float bestCost = 1e9;
    Consider(a, p, best, pred, bestCost, best);
    float step = 1.0;
    [unroll] for (int r = 0; r < 3; r++) {
        float2 c = best;
        [unroll] for (int k = 0; k < 8; k++) Consider(a, p, c + float2(Ring(k, 1)) * step, pred, bestCost, best);
        step *= 0.5;
    }
    FlowOut[id.xy] = best;
    CostOut[id.xy] = PatchCost(a, p, best);
}

// 3x3 vector median: removes isolated outliers while keeping motion boundaries.
[numthreads(8, 8, 1)]
void CSMedian(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    float2 v[9];
    [unroll] for (int j = -1; j <= 1; j++)
        [unroll] for (int i = -1; i <= 1; i++)
            v[(j + 1) * 3 + (i + 1)] = FlowIn.Load(int3(clamp(int2(id.xy) + int2(i, j), int2(0, 0), size - 1), 0));
    float bestD = 1e9;
    float2 best = v[4];
    [unroll] for (int k = 0; k < 9; k++) {
        float d = 0;
        [unroll] for (int m = 0; m < 9; m++) d += abs(v[k].x - v[m].x) + abs(v[k].y - v[m].y);
        if (d < bestD) { bestD = d; best = v[k]; }
    }
    FlowOut[id.xy] = best;
}

// Mean motion compensated error of a level -> scene change metric (1x1 texture).
groupshared float gsSum[256];
[numthreads(16, 16, 1)]
void CSScene(uint3 gtid : SV_GroupThreadID) {
    uint idx = gtid.y * 16 + gtid.x;
    float s = 0;
    for (int y = gtid.y; y < size.y; y += 16)
        for (int x = gtid.x; x < size.x; x += 16)
            s += CostIn.Load(int3(x, y, 0));
    gsSum[idx] = s;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (idx < stride) gsSum[idx] += gsSum[idx + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (idx == 0) SceneOut[uint2(0, 0)] = gsSum[0] / max(float(size.x * size.y), 1.0);
}
