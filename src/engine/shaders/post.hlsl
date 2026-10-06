// MoonUp post processing: adaptive sharpening and the "Vision" image enhancement.
Texture2D<float4> Src : register(t0);
Texture2D<float4> Base : register(t1);   // blurred quarter-resolution image (Vision)
RWTexture2D<unorm float4> Dst : register(u0);

cbuffer PostParams : register(b0) {
    float2 size;
    float2 invSize;
    float4 p0;   // sharpen: x = amount | vision: x clarity, y detail, z vibrance, w contrast
    float4 p1;   // vision: x warmth, y brightness
    float2 srcSize;  // for down/blur passes: source size
    float2 invSrcSize;
};

float3 Ld(int2 p) {
    p = clamp(p, int2(0, 0), int2(size) - 1);
    return Src.Load(int3(p, 0)).rgb;
}

// Contrast adaptive sharpening. The idea of scaling the negative lobe by the local headroom
// (so already-contrasty pixels are not over-sharpened) follows AMD FidelityFX CAS (MIT).
// A noise guard keeps flat areas untouched so compression noise is not amplified.
[numthreads(8, 8, 1)]
void CSSharpen(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    int2 p = int2(id.xy);
    float3 a = Ld(p + int2(0, -1));
    float3 b = Ld(p + int2(-1, 0));
    float3 c = Ld(p);
    float3 d = Ld(p + int2(1, 0));
    float3 e = Ld(p + int2(0, 1));
    float3 mn = min(min(min(a, b), min(d, e)), c);
    float3 mx = max(max(max(a, b), max(d, e)), c);
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    float peak = -1.0 / lerp(8.0, 4.0, saturate(p0.x));
    float range = Luma(mx) - Luma(mn);
    float guard = smoothstep(0.012, 0.05, range);
    float3 w = amp * peak * guard * saturate(p0.x * 4.0);
    float3 r = (c + (a + b + d + e) * w) / (1.0 + 4.0 * w);
    Dst[id.xy] = float4(saturate(r), 1);
}

// 4x4 box downsample (4 bilinear taps) into a quarter resolution target of 'size'.
[numthreads(8, 8, 1)]
void CSDown4(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    float2 center = (float2(id.xy) * 4.0 + 2.0) * invSrcSize;
    float2 o = invSrcSize;
    float3 s = Src.SampleLevel(sLinear, center + float2(-o.x, -o.y), 0).rgb +
               Src.SampleLevel(sLinear, center + float2( o.x, -o.y), 0).rgb +
               Src.SampleLevel(sLinear, center + float2(-o.x,  o.y), 0).rgb +
               Src.SampleLevel(sLinear, center + float2( o.x,  o.y), 0).rgb;
    Dst[id.xy] = float4(s * 0.25, 1);
}

// 9-tap gaussian using 5 linear taps. p0.xy = direction in texels.
[numthreads(8, 8, 1)]
void CSBlur(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    float2 uv = (float2(id.xy) + 0.5) * invSize;
    float2 dir = p0.xy * invSize;
    float3 s = Src.SampleLevel(sLinear, uv, 0).rgb * 0.2270270270;
    s += Src.SampleLevel(sLinear, uv + dir * 1.3846153846, 0).rgb * 0.3162162162;
    s += Src.SampleLevel(sLinear, uv - dir * 1.3846153846, 0).rgb * 0.3162162162;
    s += Src.SampleLevel(sLinear, uv + dir * 3.2307692308, 0).rgb * 0.0702702703;
    s += Src.SampleLevel(sLinear, uv - dir * 3.2307692308, 0).rgb * 0.0702702703;
    Dst[id.xy] = float4(s, 1);
}

float SoftLimit(float x, float k) { return x / (1.0 + abs(x) * k); }

// MoonUp Vision: local contrast, micro detail, vibrance, tone and white balance.
[numthreads(8, 8, 1)]
void CSVision(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)size)) return;
    int2 p = int2(id.xy);
    float2 uv = (float2(id.xy) + 0.5) * invSize;
    float3 c = Ld(p);
    float3 base = Base.SampleLevel(sLinear, uv, 0).rgb;

    // Local contrast ("clarity"): emphasise mid-frequency detail mostly in mid tones.
    float L = Luma(c);
    float mid = 1.0 - pow(abs(2.0 * L - 1.0), 2.0);
    float dl = L - Luma(base);
    float boost = SoftLimit(dl, 6.0) * p0.x * 1.4 * mid;

    // Micro detail: small radius unsharp mask on luma.
    float3 n = (Ld(p + int2(1, 0)) + Ld(p + int2(-1, 0)) + Ld(p + int2(0, 1)) + Ld(p + int2(0, -1))) * 0.25;
    float micro = SoftLimit(L - Luma(n), 10.0) * p0.y * 1.2;

    c = c + (boost + micro);

    // Global S-curve contrast.
    float3 s = c * c * (3.0 - 2.0 * c);
    c = lerp(c, s, p0.w * 0.6);

    // Brightness (exposure-like, keeps highlights).
    c = c * (1.0 + p1.y * 0.25);

    // Vibrance: saturate the less saturated colours more.
    float L2 = Luma(c);
    float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    float vib = p0.z * (1.0 - sat) * 1.2;
    c = lerp(L2.xxx, c, 1.0 + vib);

    // White balance.
    c *= float3(1.0 + p1.x * 0.06, 1.0, 1.0 - p1.x * 0.06);

    Dst[id.xy] = float4(saturate(c), 1);
}
