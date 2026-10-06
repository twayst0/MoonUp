// MoonUp Neural SR v2: 2x luma super-resolution with a 16-channel convolutional network, run
// with plain compute shaders on any DirectX 11 GPU. Two models share these passes:
//   * MoonUp Neural (trained by tools/train_neural.py on photographs and game frames):
//     predicts the luma residual over a Catmull-Rom 2x base ("residual" mode)
//   * ArtCNN C4F16 (MIT, https://github.com/Artoriuz/ArtCNN), made for anime / 2D art:
//     predicts luma directly ("direct" mode)
// Layout (same as ArtCNN C4F16): c0 1->16 | c1..c4 16->16 ReLU | c5 16->16 + c0 | c6 16->4 -> 2x.
// Each layer's weights ([cout][cin][ky][kx] then biases) live in their own constant buffer, so
// every weight read is a constant-register operand. Features are 4 RGBA16F textures (16 ch).

Texture2D<float4> Src : register(t0);
Texture2D<float4> In0 : register(t1);
Texture2D<float4> In1 : register(t2);
Texture2D<float4> In2 : register(t3);
Texture2D<float4> In3 : register(t4);
Texture2D<float4> Sk0 : register(t5);
Texture2D<float4> Sk1 : register(t6);
Texture2D<float4> Sk2 : register(t7);
Texture2D<float4> Sk3 : register(t8);

RWTexture2D<float4> O0 : register(u0);
RWTexture2D<float4> O1 : register(u1);
RWTexture2D<float4> O2 : register(u2);
RWTexture2D<float4> O3 : register(u3);
RWTexture2D<unorm float4> Out2x : register(u4);

cbuffer NeuralParams : register(b0) {
    int2 size;     // source size
    int relu;      // ReLU after this layer
    int addSkip;   // add the c0 features (Sk*) before the activation
    int direct;    // output layer: 1 = network predicts luma, 0 = residual over Catmull-Rom
    int3 pad0;
};

cbuffer LayerWeights : register(b1) {
    float4 LW[580];  // up to 16*16*9 weights + 16 biases
};

#define W(i) (LW[(i) >> 2][(i) & 3])

float SrcY(int2 p) {
    p = clamp(p, int2(0, 0), size - 1);
    return Luma(Src.Load(int3(p, 0)).rgb);
}

void LoadIn(int2 p, out float f[16]) {
    p = clamp(p, int2(0, 0), size - 1);
    float4 a = In0.Load(int3(p, 0)), b = In1.Load(int3(p, 0)), c = In2.Load(int3(p, 0)), d = In3.Load(int3(p, 0));
    f[0] = a.x; f[1] = a.y; f[2] = a.z; f[3] = a.w;
    f[4] = b.x; f[5] = b.y; f[6] = b.z; f[7] = b.w;
    f[8] = c.x; f[9] = c.y; f[10] = c.z; f[11] = c.w;
    f[12] = d.x; f[13] = d.y; f[14] = d.z; f[15] = d.w;
}

void Store(uint2 p, float o[16]) {
    O0[p] = float4(o[0], o[1], o[2], o[3]);
    O1[p] = float4(o[4], o[5], o[6], o[7]);
    O2[p] = float4(o[8], o[9], o[10], o[11]);
    O3[p] = float4(o[12], o[13], o[14], o[15]);
}

// c0: luma -> 16 features (no activation; also used as the skip connection)
[numthreads(8, 8, 1)]
void CSConvIn(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    int2 p = int2(id.xy);
    float x[9];
    [unroll] for (int k = 0; k < 9; k++) x[k] = SrcY(p + int2(k % 3 - 1, k / 3 - 1));
    float o[16];
    [unroll] for (int c = 0; c < 16; c++) {
        float s = W(144 + c);
        [unroll] for (int k2 = 0; k2 < 9; k2++) s += W(c * 9 + k2) * x[k2];
        o[c] = s;
    }
    Store(id.xy, o);
}

// c1..c5: 16 -> 16
[numthreads(8, 8, 1)]
void CSConvMid(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    int2 p = int2(id.xy);
    float acc[16];
    [unroll] for (int c = 0; c < 16; c++) acc[c] = W(2304 + c);
    [unroll] for (int k = 0; k < 9; k++) {
        float f[16];
        LoadIn(p + int2(k % 3 - 1, k / 3 - 1), f);
        [unroll] for (int o = 0; o < 16; o++) {
            [unroll] for (int i = 0; i < 16; i++) acc[o] += W((o * 16 + i) * 9 + k) * f[i];
        }
    }
    if (addSkip != 0) {
        float4 a = Sk0.Load(int3(p, 0)), b = Sk1.Load(int3(p, 0)), c = Sk2.Load(int3(p, 0)), d = Sk3.Load(int3(p, 0));
        acc[0] += a.x; acc[1] += a.y; acc[2] += a.z; acc[3] += a.w;
        acc[4] += b.x; acc[5] += b.y; acc[6] += b.z; acc[7] += b.w;
        acc[8] += c.x; acc[9] += c.y; acc[10] += c.z; acc[11] += c.w;
        acc[12] += d.x; acc[13] += d.y; acc[14] += d.z; acc[15] += d.w;
    }
    [unroll] for (int r = 0; r < 16; r++) acc[r] = relu != 0 ? max(acc[r], 0.0) : acc[r];
    Store(id.xy, acc);
}

float3 SrcRgb(int2 p) {
    p = clamp(p, int2(0, 0), size - 1);
    return Src.Load(int3(p, 0)).rgb;
}

// Catmull-Rom sample of the source at fractional source position 'pos' (pixel-center = integer).
float3 BicubicRgb(float2 pos) {
    float2 ip = floor(pos);
    float2 f = pos - ip;
    float3 acc = 0;
    [unroll] for (int j = -1; j <= 2; j++) {
        float wy = CatmullRom(j - f.y);
        [unroll] for (int i = -1; i <= 2; i++) acc += CatmullRom(i - f.x) * wy * SrcRgb(int2(ip) + int2(i, j));
    }
    return acc;
}

// c6: 16 -> 4, pixel shuffle, colour reconstruction at 2x.
[numthreads(8, 8, 1)]
void CSConvOut(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    int2 p = int2(id.xy);
    float r[4];
    [unroll] for (int c = 0; c < 4; c++) r[c] = W(576 + c);
    [unroll] for (int k = 0; k < 9; k++) {
        float f[16];
        LoadIn(p + int2(k % 3 - 1, k / 3 - 1), f);
        [unroll] for (int o = 0; o < 4; o++) {
            [unroll] for (int i = 0; i < 16; i++) r[o] += W((o * 16 + i) * 9 + k) * f[i];
        }
    }
    // Pixel shuffle: channel s = sy*2+sx -> high resolution pixel (2x+sx, 2y+sy).
    [unroll] for (int s = 0; s < 4; s++) {
        int sx = s & 1, sy = s >> 1;
        float2 hr = float2(p * 2 + int2(sx, sy));
        float2 pos = (hr + 0.5) * 0.5 - 0.5;
        float3 base = BicubicRgb(pos);
        float yb = Luma(base);
        float y = direct != 0 ? saturate(r[s]) : yb + r[s];
        // Adding the same delta to R, G and B changes luma by exactly (y - yb) and keeps chroma.
        Out2x[uint2(hr)] = float4(saturate(base + (y - yb).xxx), 1);
    }
}
