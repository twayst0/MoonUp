// AMD FidelityFX Super Resolution 1.0 (EASU upscaling + RCAS sharpening), 32-bit float path.
// Ported to plain HLSL from ffx_fsr1.h / ffx_a.h of https://github.com/GPUOpen-Effects/FidelityFX-FSR
// keeping the reference maths (12-tap kernel, gather layout, approximations) unchanged.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions: The above copyright notice and this
// permission notice shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
// OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Texture2D<float4> Src : register(t0);
RWTexture2D<unorm float4> Dst : register(u0);

cbuffer FsrParams : register(b0) {  // same layout as the other upscalers
    float2 srcSize;
    float2 invSrcSize;
    float2 dstSize;
    float2 invDstSize;
    float4 params;  // x: RCAS sharpness as a linear value (exp2(-stops))
};

// ffx_a.h approximations (bit tricks), identical constants.
float APrxLoRcpF1(float a) { return asfloat(0x7ef07ebbu - asuint(a)); }
float APrxMedRcpF1(float a) { float b = asfloat(0x7ef19fffu - asuint(a)); return b * (-b * a + 2.0); }
float APrxLoRsqF1(float a) { return asfloat(0x5f347d74u - (asuint(a) >> 1u)); }
float3 AMin3F3(float3 x, float3 y, float3 z) { return min(x, min(y, z)); }
float3 AMax3F3(float3 x, float3 y, float3 z) { return max(x, max(y, z)); }
float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }
float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }

float4 FsrEasuRF(float2 p) { return Src.GatherRed(sPoint, p); }
float4 FsrEasuGF(float2 p) { return Src.GatherGreen(sPoint, p); }
float4 FsrEasuBF(float2 p) { return Src.GatherBlue(sPoint, p); }

void FsrEasuTapF(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {
    float2 v;
    v.x = (off.x * (dir.x)) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void FsrEasuSetF(inout float2 dir, inout float len, float2 pp, bool biS, bool biT, bool biU, bool biV,
                 float lA, float lB, float lC, float lD, float lE) {
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = APrxLoRcpF1(lenX);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = APrxLoRcpF1(lenY);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

float3 FsrEasuF(uint2 ip) {
    // FsrEasuCon() constants, computed here from the upscaler parameters.
    float4 con0 = float4(srcSize * invDstSize, 0.5 * srcSize * invDstSize - 0.5);
    float4 con1 = float4(invSrcSize, float2(1.0, -1.0) * invSrcSize);
    float4 con2 = float4(float2(-1.0, 2.0) * invSrcSize, float2(1.0, 2.0) * invSrcSize);
    float2 con3 = float2(0.0, 4.0) * invSrcSize;

    float2 pp = float2(ip) * con0.xy + con0.zw;
    float2 fp = floor(pp);
    pp -= fp;
    float2 p0 = fp * con1.xy + con1.zw;
    float2 p1 = p0 + con2.xy;
    float2 p2 = p0 + con2.zw;
    float2 p3 = p0 + con3;
    float4 bczzR = FsrEasuRF(p0), bczzG = FsrEasuGF(p0), bczzB = FsrEasuBF(p0);
    float4 ijfeR = FsrEasuRF(p1), ijfeG = FsrEasuGF(p1), ijfeB = FsrEasuBF(p1);
    float4 klhgR = FsrEasuRF(p2), klhgG = FsrEasuGF(p2), klhgB = FsrEasuBF(p2);
    float4 zzonR = FsrEasuRF(p3), zzonG = FsrEasuGF(p3), zzonB = FsrEasuBF(p3);
    float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    float bL = bczzL.x, cL = bczzL.y;
    float iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
    float kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w;
    float oL = zzonL.z, nL = zzonL.w;
    float2 dir = 0.0;
    float len = 0.0;
    FsrEasuSetF(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
    FsrEasuSetF(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
    FsrEasuSetF(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
    FsrEasuSetF(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = APrxLoRsqF1(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * APrxLoRcpF1(max(abs(dir.x), abs(dir.y)));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = APrxLoRcpF1(lob);
    float3 fC = float3(ijfeR.z, ijfeG.z, ijfeB.z), gC = float3(klhgR.w, klhgG.w, klhgB.w);
    float3 jC = float3(ijfeR.y, ijfeG.y, ijfeB.y), kC = float3(klhgR.x, klhgG.x, klhgB.x);
    float3 min4 = min(AMin3F3(fC, gC, jC), kC);
    float3 max4 = max(AMax3F3(fC, gC, jC), kC);
    float3 aC = 0.0;
    float aW = 0.0;
    FsrEasuTapF(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x));  // b
    FsrEasuTapF(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y));  // c
    FsrEasuTapF(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x));  // i
    FsrEasuTapF(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);                                  // j
    FsrEasuTapF(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);                                  // f
    FsrEasuTapF(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w));  // e
    FsrEasuTapF(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);                                  // k
    FsrEasuTapF(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y));  // l
    FsrEasuTapF(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z));  // h
    FsrEasuTapF(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);                                  // g
    FsrEasuTapF(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z));  // o
    FsrEasuTapF(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w));  // n
    return min(max4, max(min4, aC * rcp(aW)));
}

[numthreads(8, 8, 1)]
void CSEasu(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dstSize)) return;
    Dst[id.xy] = float4(saturate(FsrEasuF(id.xy)), 1);
}

// ------------------------------------------------------------------ RCAS (output resolution)
#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

float3 RcasLoad(int2 p) {
    p = clamp(p, int2(0, 0), int2(dstSize) - 1);
    return Src.Load(int3(p, 0)).rgb;
}

[numthreads(8, 8, 1)]
void CSRcas(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)dstSize)) return;
    int2 sp = int2(id.xy);
    float3 b = RcasLoad(sp + int2(0, -1));
    float3 d = RcasLoad(sp + int2(-1, 0));
    float3 e = RcasLoad(sp);
    float3 f = RcasLoad(sp + int2(1, 0));
    float3 h = RcasLoad(sp + int2(0, 1));
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = h.b * 0.5 + (h.r * 0.5 + h.g);
    // Noise detection (FSR_RCAS_DENOISE path: grain is not amplified).
    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    nz = saturate(abs(nz) * APrxMedRcpF1(AMax3F1(AMax3F1(bL, dL, eL), fL, hL) - AMin3F1(AMin3F1(bL, dL, eL), fL, hL)));
    nz = -0.5 * nz + 1.0;
    float3 mn4 = min(AMin3F3(b, d, f), h);
    float3 mx4 = max(AMax3F3(b, d, f), h);
    float2 peakC = float2(1.0, -1.0 * 4.0);
    float3 hitMin = min(mn4, e) * rcp(4.0 * mx4);
    float3 hitMax = (peakC.x - max(mx4, e)) * rcp(4.0 * mn4 + peakC.y);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-FSR_RCAS_LIMIT, min(AMax3F1(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0)) * params.x;
    lobe *= nz;
    float rcpL = APrxMedRcpF1(4.0 * lobe + 1.0);
    float3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
    Dst[id.xy] = float4(saturate(pix), 1);
}
