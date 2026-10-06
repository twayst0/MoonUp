// MoonUp upscalers. All entry points read 'Src' and write 'Dst' at dstSize.
Texture2D<float4> Src : register(t0);
RWTexture2D<unorm float4> Dst : register(u0);

cbuffer UpscaleParams : register(b0) {
    float2 srcSize;
    float2 invSrcSize;
    float2 dstSize;
    float2 invDstSize;
    float4 params;  // x: edge sensitivity, y: anti-ringing strength
};

float2 SrcPos(uint2 id) { return (float2(id) + 0.5) * srcSize * invDstSize - 0.5; }

float3 LoadClamped(int2 p) {
    p = clamp(p, int2(0, 0), int2(srcSize) - 1);
    return Src.Load(int3(p, 0)).rgb;
}

[numthreads(8, 8, 1)]
void CSCopy(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dstSize)) return;
    Dst[id.xy] = float4(Src.Load(int3(id.xy, 0)).rgb, 1);
}

[numthreads(8, 8, 1)]
void CSBilinear(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dstSize)) return;
    float2 uv = (float2(id.xy) + 0.5) * invDstSize;
    Dst[id.xy] = float4(Src.SampleLevel(sLinear, uv, 0).rgb, 1);
}

[numthreads(8, 8, 1)]
void CSNearest(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dstSize)) return;
    int2 p = int2((float2(id.xy) + 0.5) * srcSize * invDstSize);
    Dst[id.xy] = float4(LoadClamped(p), 1);
}

// Sharp bilinear: nearest-looking pixels with antialiased pixel borders (pixel art).
[numthreads(8, 8, 1)]
void CSPixel(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dstSize)) return;
    float2 scale = dstSize * invSrcSize;
    float2 p = (float2(id.xy) + 0.5) * srcSize * invDstSize;
    float2 cell = floor(p);
    float2 f = p - cell;
    float2 region = 0.5 - 0.5 / max(scale, 1.0);
    float2 t = saturate((f - region) / max(1.0 - 2.0 * region, 1e-4));
    float2 uv = (cell + t) * invSrcSize;
    Dst[id.xy] = float4(Src.SampleLevel(sLinear, uv, 0).rgb, 1);
}

[numthreads(8, 8, 1)]
void CSBicubic(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dstSize)) return;
    float2 pos = SrcPos(id.xy);
    float2 ip = floor(pos);
    float2 f = pos - ip;
    float3 acc = 0;
    float wsum = 0;
    [unroll] for (int j = -1; j <= 2; j++) {
        float wy = CatmullRom(j - f.y);
        [unroll] for (int i = -1; i <= 2; i++) {
            float w = CatmullRom(i - f.x) * wy;
            acc += w * LoadClamped(int2(ip) + int2(i, j));
            wsum += w;
        }
    }
    Dst[id.xy] = float4(saturate(acc / wsum), 1);
}

[numthreads(8, 8, 1)]
void CSLanczos(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dstSize)) return;
    float2 pos = SrcPos(id.xy);
    float2 ip = floor(pos);
    float2 f = pos - ip;
    float3 acc = 0;
    float wsum = 0;
    float3 mn = 1, mx = 0;
    [unroll] for (int j = -2; j <= 3; j++) {
        float wy = Lanczos(j - f.y, 3.0);
        [unroll] for (int i = -2; i <= 3; i++) {
            float3 c = LoadClamped(int2(ip) + int2(i, j));
            float w = Lanczos(i - f.x, 3.0) * wy;
            acc += w * c;
            wsum += w;
            if (i >= 0 && i <= 1 && j >= 0 && j <= 1) { mn = min(mn, c); mx = max(mx, c); }
        }
    }
    float3 r = acc / wsum;
    // Anti-ringing: pull overshoot back towards the local 2x2 range.
    r = lerp(r, clamp(r, mn, mx), params.y);
    Dst[id.xy] = float4(saturate(r), 1);
}

// ---------------------------------------------------------------------------------------------
// MoonUp Edge: edge-adaptive anisotropic reconstruction.
// A structure tensor over the 2x2 footprint gives the local edge orientation and anisotropy.
// A Lanczos-2 kernel is evaluated in a rotated frame: stretched along the edge (less staircase),
// compressed across it (crisper transition). In flat or noisy regions negative lobes are faded
// out so grain is not amplified, and the result is clamped to the local range (no halos).
// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSEdge(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dstSize)) return;
    float2 pos = SrcPos(id.xy);
    float2 ip = floor(pos);
    float2 f = pos - ip;
    int2 base = int2(ip) - 1;

    float3 c[16];
    float l[16];
    [unroll] for (int j = 0; j < 4; j++) {
        [unroll] for (int i = 0; i < 4; i++) {
            float3 v = LoadClamped(base + int2(i, j));
            c[j * 4 + i] = v;
            l[j * 4 + i] = Luma(v);
        }
    }

    // Structure tensor, bilinearly weighted over the 4 center texels.
    float wq[4] = { (1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y };
    int2 cq[4] = { int2(1, 1), int2(2, 1), int2(1, 2), int2(2, 2) };
    float jxx = 0, jxy = 0, jyy = 0;
    [unroll] for (int k = 0; k < 4; k++) {
        int x = cq[k].x, y = cq[k].y;
        float gx = l[y * 4 + x + 1] - l[y * 4 + x - 1];
        float gy = l[(y + 1) * 4 + x] - l[(y - 1) * 4 + x];
        jxx += wq[k] * gx * gx;
        jxy += wq[k] * gx * gy;
        jyy += wq[k] * gy * gy;
    }
    float tr = jxx + jyy;
    float det = jxx * jyy - jxy * jxy;
    float disc = sqrt(max(tr * tr * 0.25 - det, 0.0));
    float l1 = tr * 0.5 + disc;
    float l2 = max(tr * 0.5 - disc, 0.0);

    float2 g = abs(jxy) > 1e-7 ? normalize(float2(jxy, l1 - jxx)) : (jxx >= jyy ? float2(1, 0) : float2(0, 1));
    float2 e = float2(-g.y, g.x);
    float anis = (l1 - l2) / (l1 + l2 + 1e-6);
    float strength = sqrt(l1);
    // Only clearly directional edges are stretched (pow 3): stretching on texture/noise smears it.
    float edgeAmt = saturate(strength * 6.0 * params.x) * anis * anis * anis;

    float along = 1.0 / (1.0 + 0.2 * edgeAmt);
    float across = 1.0 + 0.1 * edgeAmt;
    float lobe = saturate(strength * 10.0);  // keep negative lobes only where there is structure

    float3 acc = 0;
    float wsum = 0;
    [unroll] for (int j2 = 0; j2 < 4; j2++) {
        [unroll] for (int i2 = 0; i2 < 4; i2++) {
            float2 d = float2(i2 - 1, j2 - 1) - f;
            float u = dot(d, e) * along;
            float v = dot(d, g) * across;
            float w = Lanczos(sqrt(u * u + v * v), 2.0);
            w = lerp(max(w, 0.0), w, lobe);
            acc += w * c[j2 * 4 + i2];
            wsum += w;
        }
    }
    float3 r = acc / max(wsum, 1e-4);
    float3 mn = min(min(c[5], c[6]), min(c[9], c[10]));
    float3 mx = max(max(c[5], c[6]), max(c[9], c[10]));
    r = lerp(r, clamp(r, mn, mx), params.y);
    Dst[id.xy] = float4(saturate(r), 1);
}
