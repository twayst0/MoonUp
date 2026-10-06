// MoonUp Motion: frame interpolation.
//
// Three passes per generated frame:
//   CSStatic   (once per frame pair, output resolution) counts for how many consecutive pairs
//              every pixel has stayed exactly the same. HUDs, text, crosshairs and static UI
//              build up a count and are then never moved, even over moving backgrounds.
//   CSSelect   (half output resolution) picks the motion of every 2x2 block at time t from a
//              set of candidates (all as A->B vectors):
//                * the forward field traced back to where the pixel left frame A
//                * the backward field traced to where the pixel left frame B
//                * the vectors of the four nearest flow texels of both fields (sharp motion
//                  boundaries instead of a blend of foreground and background motion)
//                * zero motion
//              Each candidate fetches a 3x3 neighbourhood from both frames along its path; the
//              symmetric difference is its error. The two best distinct candidates are kept
//              with a blend weight (soft minimum, so near ties do not flicker).
//   CSCompose  (output resolution) fetches both frames along the selected paths.

Texture2D<float4> FrameA : register(t0);
Texture2D<float4> FrameB : register(t1);
Texture2D<float2> FlowAB : register(t2);
Texture2D<float2> FlowBA : register(t3);
Texture2D<float> Scene : register(t4);
Texture2D<float4> Sel : register(t5);      // v1.xy, v2.xy
Texture2D<float> SelW : register(t6);      // weight of v2
Texture2D<float> StaticIn : register(t7);
RWTexture2D<unorm float4> Dst : register(u0);
RWTexture2D<float4> SelOut : register(u1);
RWTexture2D<float> SelWOut : register(u2);
RWTexture2D<float> StaticOut : register(u3);

cbuffer InterpParams : register(b0) {
    float2 outSize;
    float2 invOutSize;
    float2 flowScale;   // flow-level pixels -> output pixels
    float t;
    float sigma;        // soft-min temperature
    float zeroBias;
    float sceneThreshold;
    int sceneCut;
    int hudProtect;
    float2 flowSize;
    float2 selSize;
    float staticEps;    // max per-channel change of a static pixel
    float staticFrames; // pairs a pixel must stay unchanged before it is locked
    float refineStep;   // output pixels, 0 = off
    float pad0;
};

float3 SA(float2 uv) { return FrameA.SampleLevel(sLinear, uv, 0).rgb; }
float3 SB(float2 uv) { return FrameB.SampleLevel(sLinear, uv, 0).rgb; }

float2 FAB(float2 pos) { return FlowAB.SampleLevel(sLinear, pos * invOutSize, 0) * flowScale; }
float2 FBA(float2 pos) { return FlowBA.SampleLevel(sLinear, pos * invOutSize, 0) * flowScale; }
float2 FABn(int2 texel) { return FlowAB.Load(int3(clamp(texel, int2(0, 0), int2(flowSize) - 1), 0)) * flowScale; }
float2 FBAn(int2 texel) { return FlowBA.Load(int3(clamp(texel, int2(0, 0), int2(flowSize) - 1), 0)) * flowScale; }

bool SceneChanged() { return sceneCut != 0 && Scene.Load(int3(0, 0, 0)) > sceneThreshold; }

// ------------------------------------------------------------------ static (HUD) detector
[numthreads(8, 8, 1)]
void CSStatic(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)outSize)) return;
    float3 d = abs(FrameA.Load(int3(id.xy, 0)).rgb - FrameB.Load(int3(id.xy, 0)).rgb);
    bool same = max(d.r, max(d.g, d.b)) <= staticEps;
    float n = StaticIn.Load(int3(id.xy, 0));
    StaticOut[id.xy] = same ? min(n + 1.0, 16.0) : 0.0;
}

// ------------------------------------------------------------------ vector selection
// Symmetric patch error of motion v (A->B, output px) at pixel centre c.
float PathError(float2 c, float2 v) {
    float2 pa = (c - t * v) * invOutSize;
    float2 pb = (c + (1.0 - t) * v) * invOutSize;
    float e = 0;
    [unroll] for (int j = -1; j <= 1; j++)
        [unroll] for (int i = -1; i <= 1; i++) {
            float2 o = float2(i, j) * invOutSize;
            float3 d = SA(pa + o) - SB(pb + o);
            e += abs(d.r) + abs(d.g) + abs(d.b);
        }
    return e * (1.0 / 27.0);
}

[numthreads(8, 8, 1)]
void CSSelect(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)selSize)) return;
    float2 c = float2(id.xy) * 2.0 + 1.0;  // centre of the 2x2 output block
    c = min(c, outSize - 0.5);

    float2 vf = FAB(c);
    vf = FAB(c - t * vf);
    float2 vb = -FBA(c);
    vb = -FBA(c + (1.0 - t) * vb);

    float2 cand[11];
    cand[0] = 0;
    cand[1] = vf;
    cand[2] = vb;
    float2 fa = (c - t * vf) / flowScale - 0.5;
    float2 fb = (c + (1.0 - t) * vb) / flowScale - 0.5;
    int2 ia = int2(floor(fa)), ib = int2(floor(fb));
    cand[3] = FABn(ia);
    cand[4] = FABn(ia + int2(1, 0));
    cand[5] = FABn(ia + int2(0, 1));
    cand[6] = FABn(ia + int2(1, 1));
    cand[7] = -FBAn(ib);
    cand[8] = -FBAn(ib + int2(1, 0));
    cand[9] = -FBAn(ib + int2(0, 1));
    cand[10] = -FBAn(ib + int2(1, 1));

    float e1 = 1e9, e2 = 1e9;
    float2 v1 = 0, v2 = 0;
    [unroll] for (int k = 0; k < 11; k++) {
        float e = PathError(c, cand[k]);
        if (k == 0 && hudProtect != 0) e = max(e - zeroBias, 0.0);
        if (e < e1) {
            if (length(cand[k] - v1) > 0.5) { e2 = e1; v2 = v1; }
            e1 = e; v1 = cand[k];
        } else if (e < e2 && length(cand[k] - v1) > 0.5) {
            e2 = e; v2 = cand[k];
        }
    }
    // Sub-pixel refinement of the winner along the symmetric path (flow precision is limited
    // by the flow resolution; this works in output pixels).
    if (refineStep > 0) {
        float2 base = v1;
        [unroll] for (int r = 0; r < 4; r++) {
            float2 o = r == 0 ? float2(1, 0) : r == 1 ? float2(-1, 0) : r == 2 ? float2(0, 1) : float2(0, -1);
            float2 v = base + o * refineStep;
            float e = PathError(c, v);
            if (e < e1) { e1 = e; v1 = v; }
        }
    }
    float w2 = e2 < 1e8 ? exp(-(e2 - e1) / sigma) : 0.0;
    SelOut[id.xy] = float4(v1, v2);
    SelWOut[id.xy] = w2 / (1.0 + w2);
}

// ------------------------------------------------------------------ composition
[numthreads(8, 8, 1)]
void CSCompose(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)outSize)) return;
    float2 c = float2(id.xy) + 0.5;
    float2 uv = c * invOutSize;
    if (SceneChanged()) {
        Dst[id.xy] = float4(t < 0.5 ? SA(uv) : SB(uv), 1);
        return;
    }
    if (hudProtect != 0 && StaticIn.Load(int3(id.xy, 0)) >= staticFrames) {
        Dst[id.xy] = float4(lerp(SA(uv), SB(uv), t), 1);
        return;
    }
    float2 suv = c * 0.5 / selSize;
    int2 si = clamp(int2(id.xy) / 2, int2(0, 0), int2(selSize) - 1);
    float4 s = Sel.Load(int3(si, 0));
    float w = SelW.Load(int3(si, 0));
    float2 v1 = s.xy, v2 = s.zw;
    float3 c1 = lerp(SA(uv - t * v1 * invOutSize), SB(uv + (1.0 - t) * v1 * invOutSize), t);
    float3 c2 = lerp(SA(uv - t * v2 * invOutSize), SB(uv + (1.0 - t) * v2 * invOutSize), t);
    Dst[id.xy] = float4(saturate(lerp(c1, c2, w)), 1);
}
