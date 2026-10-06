// MoonUp Neural Render v3: one-step, deterministic neural rendering stage.
//
// Modelled on what NVIDIA has published about DLSS 5 (one frame in, one frame out, conditioned on
// the rendered frame, separately controllable "tone" and "structure"), built for what a capture
// tool can see: the final colour frame only.
//
// Tone (low frequency lighting and colour): a convolutional network looks at a 256x144 copy of
// the frame and predicts a bilateral grid (16 x 9 cells x 8 brightness bins) of 3x4 colour
// transforms; a global path (scene lighting context) conditions every cell. Every pixel looks up
// its transform with a learned guide, so the result follows edges at full resolution while the
// network cost does not depend on the resolution.
//
// Structure (high frequency shading and detail): a 12-channel dilated context network (receptive field 71
// half-resolution pixels) on half-resolution luma predicts a log shading term (contact shadows,
// ambient occlusion, depth) and a detail gain, applied at full resolution.
//
// Temporal state: the grid is blended per cell and the structure maps per pixel with the previous
// frame according to how much that part of the image changed (stable look, no ghosting).
// Trained by tools/train_render.py; weights are bound as one float buffer.

Texture2D<float4> Src : register(t0);           // full resolution frame
Texture2DArray<float4> FeatIn : register(t1);   // feature maps (4 channels per slice)
Texture2D<float4> LowCur : register(t2);        // 256x144 input, current
Texture2D<float4> LowPrev : register(t3);       // 256x144 input, previous frame
Texture3D<float4> GridPrev0 : register(t4);
Texture3D<float4> GridPrev1 : register(t5);
Buffer<float> Wts : register(t6);
Texture3D<float4> GridPrev2 : register(t7);
Texture2DArray<float4> LocalIn : register(t8);  // c6 output (local path)
Buffer<float> GlobalIn : register(t9);          // 64 global features
Texture2DArray<float4> StructPrev : register(t10);  // structure maps, previous frame (a, k)
Texture2DArray<float4> HalfCur : register(t11);     // half resolution luma, current
Texture2DArray<float4> HalfPrev : register(t12);    // half resolution luma, previous
Texture2DArray<float4> StructIn : register(t13);    // structure maps used by CSApply

RWTexture2D<float4> LowOut : register(u0);
RWTexture2DArray<float4> FeatOut : register(u1);
RWBuffer<float> GlobalOut : register(u2);
RWTexture3D<float4> Grid0 : register(u3);
RWTexture3D<float4> Grid1 : register(u4);
RWTexture3D<float4> Grid2 : register(u5);
RWTexture2D<unorm float4> Dst : register(u6);

cbuffer RenderParams : register(b0) {
    int2 inSize;
    int2 outSize;
    int cin;
    int cout;
    int stride;
    int relu;
    int wOff;
    int bOff;
    int hasPrev;      // temporal state available
    float temporal;   // 0..1, how strongly the look is carried over
    // guide / global / output offsets
    int offCcm, offCcmb, offSlopes, offMix;
    int offMixb, offF1w, offF1b, offF2w;
    int offF2b, offOw, offOb, dilation;
    float tone, color, structure, pad1;
    float2 srcSize, invSrcSize;
};

// The guide weights (ccm, ccmb, slopes, mix, mixb: 64 floats) live in a constant buffer.
cbuffer NetConsts : register(b1) {
    float4 NC[16];
};
float NCf(int i) { return NC[i >> 2][i & 3]; }

static const int GW = 16, GH = 9, GD = 8, GF = 64;
static const int LOWW = 256, LOWH = 144;

// ------------------------------------------------------------------ 1. low resolution inputs
[numthreads(8, 8, 1)]
void CSDownIn(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)outSize)) return;
    // Area average of the footprint with a 4x4 grid of bilinear taps.
    float2 fp = srcSize / float2(outSize);
    float2 base = float2(id.xy) * fp;
    float3 s = 0;
    [unroll] for (int j = 0; j < 4; j++)
        [unroll] for (int i = 0; i < 4; i++) {
            float2 p = base + (float2(i, j) + 0.5) * fp * 0.25;
            s += Src.SampleLevel(sLinear, p * invSrcSize, 0).rgb;
        }
    LowOut[id.xy] = float4(saturate(s / 16.0), 0);
}

// Half resolution luma (2x2 box) for the structure network.
[numthreads(8, 8, 1)]
void CSDownHalf(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)outSize)) return;
    float2 uv = (float2(id.xy) * 2.0 + 1.0) * invSrcSize;
    float3 c = Src.SampleLevel(sLinear, uv, 0).rgb;
    FeatOut[uint3(id.xy, 0)] = float4(Luma(c), 0, 0, 0);
}

// ------------------------------------------------------------------ 2. convolution layers
// 3x3, edge clamped, optional stride and dilation. Each thread computes 4 output channels;
// inputs are read 4 channels (one slice) at a time.
float4 LoadSlice(int2 p, int s) {
    p = clamp(p, int2(0, 0), inSize - 1);
    return FeatIn.Load(int4(p, s, 0));
}

[numthreads(8, 8, 1)]
void CSConv(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)outSize)) return;
    int2 c0 = int2(id.xy) * stride;
    int co = (int)id.z * 4;
    int n = min(cout - co, 4);
    float4 acc;
    acc.x = Wts[bOff + co];
    acc.y = n > 1 ? Wts[bOff + co + 1] : 0;
    acc.z = n > 2 ? Wts[bOff + co + 2] : 0;
    acc.w = n > 3 ? Wts[bOff + co + 3] : 0;
    int perOut = cin * 9;
    int slices = (cin + 3) / 4;
    for (int s = 0; s < slices; s++) {
        int cs = min(cin - s * 4, 4);
        [unroll] for (int ky = 0; ky < 3; ky++)
            [unroll] for (int kx = 0; kx < 3; kx++) {
                float4 v = LoadSlice(c0 + int2(kx - 1, ky - 1) * dilation, s);
                int tap = ky * 3 + kx;
                [unroll] for (int k = 0; k < 4; k++) {
                    if (k < cs) {
                        float x = k == 0 ? v.x : k == 1 ? v.y : k == 2 ? v.z : v.w;
                        int w = wOff + (s * 4 + k) * 9 + tap;
                        acc.x += x * Wts[w + co * perOut];
                        if (n > 1) acc.y += x * Wts[w + (co + 1) * perOut];
                        if (n > 2) acc.z += x * Wts[w + (co + 2) * perOut];
                        if (n > 3) acc.w += x * Wts[w + (co + 3) * perOut];
                    }
                }
            }
    }
    if (relu != 0) acc = max(acc, 0);
    FeatOut[uint3(id.xy, id.z)] = acc;
}

// Structure network layers (12 channels). Each layer's weights ([cout][cin][ky][kx] then biases)
// are in their own constant buffer at b2, so every weight is a constant-register operand.
cbuffer StructWeights : register(b2) {
    float4 SW[328];
};
#define SWf(i) (SW[(i) >> 2][(i) & 3])

void LoadS12(int2 p, out float f[12]) {
    p = clamp(p, int2(0, 0), inSize - 1);
    float4 a = FeatIn.Load(int4(p, 0, 0)), b = FeatIn.Load(int4(p, 1, 0)), c = FeatIn.Load(int4(p, 2, 0));
    f[0] = a.x; f[1] = a.y; f[2] = a.z; f[3] = a.w;
    f[4] = b.x; f[5] = b.y; f[6] = b.z; f[7] = b.w;
    f[8] = c.x; f[9] = c.y; f[10] = c.z; f[11] = c.w;
}

void StoreS12(uint2 p, float o[12]) {
    FeatOut[uint3(p, 0)] = float4(o[0], o[1], o[2], o[3]);
    FeatOut[uint3(p, 1)] = float4(o[4], o[5], o[6], o[7]);
    FeatOut[uint3(p, 2)] = float4(o[8], o[9], o[10], o[11]);
}

// s1: half resolution luma -> 12, ReLU
[numthreads(8, 8, 1)]
void CSStructIn(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)outSize)) return;
    float x[9];
    [unroll] for (int k = 0; k < 9; k++) {
        int2 p = clamp(int2(id.xy) + int2(k % 3 - 1, k / 3 - 1) * dilation, int2(0, 0), inSize - 1);
        x[k] = FeatIn.Load(int4(p, 0, 0)).x;
    }
    float o[12];
    [unroll] for (int c = 0; c < 12; c++) {
        float s = SWf(108 + c);
        [unroll] for (int k2 = 0; k2 < 9; k2++) s += SWf(c * 9 + k2) * x[k2];
        o[c] = max(s, 0.0);
    }
    StoreS12(id.xy, o);
}

// s2..s5: 12 -> 12, ReLU, dilated
[numthreads(8, 8, 1)]
void CSStructMid(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)outSize)) return;
    float acc[12];
    [unroll] for (int c = 0; c < 12; c++) acc[c] = SWf(1296 + c);
    [unroll] for (int k = 0; k < 9; k++) {
        float f[12];
        LoadS12(int2(id.xy) + int2(k % 3 - 1, k / 3 - 1) * dilation, f);
        [unroll] for (int o = 0; o < 12; o++) {
            [unroll] for (int i = 0; i < 12; i++) acc[o] += SWf((o * 12 + i) * 9 + k) * f[i];
        }
    }
    [unroll] for (int r = 0; r < 12; r++) acc[r] = max(acc[r], 0.0);
    StoreS12(id.xy, acc);
}

// s6: 12 -> 2 (a = log shading, k = detail gain)
[numthreads(8, 8, 1)]
void CSStructOut(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)outSize)) return;
    float acc[2];
    acc[0] = SWf(216);
    acc[1] = SWf(217);
    [unroll] for (int k = 0; k < 9; k++) {
        float f[12];
        LoadS12(int2(id.xy) + int2(k % 3 - 1, k / 3 - 1) * dilation, f);
        [unroll] for (int o = 0; o < 2; o++) {
            [unroll] for (int i = 0; i < 12; i++) acc[o] += SWf((o * 12 + i) * 9 + k) * f[i];
        }
    }
    FeatOut[uint3(id.xy, 0)] = float4(acc[0], acc[1], 0, 0);
}

// ------------------------------------------------------------------ 3. global path
groupshared float gsMean[64];
groupshared float gsHidden[64];

float Chan(float4 v, int k) { return k == 0 ? v.x : k == 1 ? v.y : k == 2 ? v.z : v.w; }

[numthreads(64, 1, 1)]
void CSGlobal(uint3 gtid : SV_GroupThreadID) {
    int c = (int)gtid.x;
    if (c < cin) {
        float s = 0;
        for (int y = 0; y < inSize.y; y++)
            for (int x = 0; x < inSize.x; x++) s += Chan(FeatIn.Load(int4(x, y, c >> 2, 0)), c & 3);
        gsMean[c] = s / float(inSize.x * inSize.y);
    }
    GroupMemoryBarrierWithGroupSync();
    if (c < cin) {
        float h = Wts[offF1b + c];
        for (int i = 0; i < cin; i++) h += Wts[offF1w + c * cin + i] * gsMean[i];
        gsHidden[c] = max(h, 0);
    }
    GroupMemoryBarrierWithGroupSync();
    if (c < cin) {
        float o = Wts[offF2b + c];
        for (int k = 0; k < cin; k++) o += Wts[offF2w + c * cin + k] * gsHidden[k];
        GlobalOut[c] = o;
    }
}

// ------------------------------------------------------------------ 4. fuse -> grid (+ temporal)
[numthreads(4, 4, 1)]
void CSFuse(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)GW || id.y >= (uint)GH) return;
    float f[GF];
    [unroll] for (int s = 0; s < GF / 4; s++) {
        float4 v = LocalIn.Load(int4(id.xy, s, 0));
        f[s * 4 + 0] = max(v.x + GlobalIn[s * 4 + 0], 0);
        f[s * 4 + 1] = max(v.y + GlobalIn[s * 4 + 1], 0);
        f[s * 4 + 2] = max(v.z + GlobalIn[s * 4 + 2], 0);
        f[s * 4 + 3] = max(v.w + GlobalIn[s * 4 + 3], 0);
    }
    // How much this cell's content changed since the previous frame -> how much of the old
    // grid is kept. Static areas get a calm, stable look; moving areas update immediately.
    float alpha = 1.0;
    if (hasPrev != 0) {
        int2 lo = int2(id.xy) * 16;
        float d = 0;
        int n = 0;
        for (int y = lo.y; y < min(lo.y + 16, LOWH); y += 2)
            for (int x = lo.x; x < min(lo.x + 16, LOWW); x += 2) {
                float3 a = LowCur.Load(int3(x, y, 0)).rgb, b = LowPrev.Load(int3(x, y, 0)).rgb;
                d += abs(a.r - b.r) + abs(a.g - b.g) + abs(a.b - b.b);
                n++;
            }
        d /= max(n * 3, 1);
        alpha = lerp(1.0 - temporal * 0.75, 1.0, saturate((d - 0.015) / 0.06));
    }
    [loop] for (int bin = 0; bin < GD; bin++) {
        float o[12];
        [unroll] for (int j = 0; j < 12; j++) {
            int k = bin * 12 + j;
            float a = Wts[offOb + k];
            [unroll] for (int i = 0; i < GF; i++) a += Wts[offOw + k * GF + i] * f[i];
            o[j] = a;
        }
        uint3 cell = uint3(id.xy, bin);
        float4 g0 = float4(o[0], o[1], o[2], o[3]);
        float4 g1 = float4(o[4], o[5], o[6], o[7]);
        float4 g2 = float4(o[8], o[9], o[10], o[11]);
        if (hasPrev != 0) {
            g0 = lerp(GridPrev0.Load(int4(cell, 0)), g0, alpha);
            g1 = lerp(GridPrev1.Load(int4(cell, 0)), g1, alpha);
            g2 = lerp(GridPrev2.Load(int4(cell, 0)), g2, alpha);
        }
        Grid0[cell] = g0;
        Grid1[cell] = g1;
        Grid2[cell] = g2;
    }
}

// ------------------------------------------------------------------ 5. structure maps (+ temporal)
[numthreads(8, 8, 1)]
void CSStructBlend(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)outSize)) return;
    float4 cur = FeatIn.Load(int4(id.xy, 0, 0));
    if (hasPrev != 0) {
        // Largest luma change in the 3x3 neighbourhood: edges that move update at once.
        float d = 0;
        [unroll] for (int j = -1; j <= 1; j++)
            [unroll] for (int i = -1; i <= 1; i++) {
                int2 p = clamp(int2(id.xy) + int2(i, j), int2(0, 0), outSize - 1);
                d = max(d, abs(HalfCur.Load(int4(p, 0, 0)).x - HalfPrev.Load(int4(p, 0, 0)).x));
            }
        float alpha = lerp(1.0 - temporal * 0.6, 1.0, saturate((d - 0.008) / 0.04));
        cur = lerp(StructPrev.Load(int4(id.xy, 0, 0)), cur, alpha);
    }
    FeatOut[uint3(id.xy, 0)] = cur;
}

// ------------------------------------------------------------------ 6. apply at full resolution
float Guide(float3 rgb) {
    // NC layout: ccm 0..8, ccmb 9..11, slopes 12..59, mix 60..62, mixb 63
    float3 y;
    y.x = NCf(0) * rgb.r + NCf(1) * rgb.g + NCf(2) * rgb.b + NCf(9);
    y.y = NCf(3) * rgb.r + NCf(4) * rgb.g + NCf(5) * rgb.b + NCf(10);
    y.z = NCf(6) * rgb.r + NCf(7) * rgb.g + NCf(8) * rgb.b + NCf(11);
    float g = NCf(63);
    [unroll] for (int c = 0; c < 3; c++) {
        float v = c == 0 ? y.x : c == 1 ? y.y : y.z;
        float cv = 0;
        [unroll] for (int k = 0; k < 16; k++) cv += max(v - k / 16.0, 0) * NCf(12 + c * 16 + k);
        g += cv * NCf(60 + c);
    }
    return saturate(g);
}

float LumaAt(int2 p) {
    p = clamp(p, int2(0, 0), outSize - 1);
    return Luma(Src.Load(int3(p, 0)).rgb);
}

[numthreads(8, 8, 1)]
void CSApply(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)outSize)) return;
    int2 p = int2(id.xy);
    float3 rgb = Src.Load(int3(p, 0)).rgb;
    float2 uv = (float2(id.xy) + 0.5) / float2(outSize);

    // Tone: bilateral grid lookup, lighting (luminance) and colour (chroma) mixed separately.
    float g = Guide(rgb);
    float3 uvw = float3(uv, g);
    float4 a0 = GridPrev0.SampleLevel(sLinear, uvw, 0);
    float4 a1 = GridPrev1.SampleLevel(sLinear, uvw, 0);
    float4 a2 = GridPrev2.SampleLevel(sLinear, uvw, 0);
    float3 o;
    o.r = dot(a0.xyz, rgb) + a0.w;
    o.g = dot(a1.xyz, rgb) + a1.w;
    o.b = dot(a2.xyz, rgb) + a2.w;
    o = saturate(o);
    float3 delta = o - rgb;
    float dl = Luma(delta);
    float3 dc = delta - dl;
    float3 toned = rgb + tone * dl + color * dc;

    // Structure: shading (multiplicative, log domain) and detail (luma high-pass gain).
    float2 m = StructIn.SampleLevel(sLinear, float3(uv, 0), 0).xy;
    float a = clamp(m.x, -1.5, 1.0);
    float k = clamp(m.y, -1.0, 3.0);
    float hc = 0;
    [unroll] for (int j = -1; j <= 1; j++)
        [unroll] for (int i = -1; i <= 1; i++) hc += LumaAt(p + int2(i, j)) * ((i == 0 ? 2.0 : 1.0) * (j == 0 ? 2.0 : 1.0));
    float hp = Luma(rgb) - hc / 16.0;
    float3 result = toned * exp(structure * a) + structure * k * hp;
    Dst[id.xy] = float4(saturate(result), 1);
}
