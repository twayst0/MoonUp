// MoonUp presentation: letterboxed frame, cursor and HUD quads.
Texture2D<float4> Tex : register(t0);

cbuffer PresentParams : register(b0) {
    float4 rect;      // x, y, w, h in back buffer pixels
    float2 bbSize;
    float useLinear;
    float opacity;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Full screen triangle.
VSOut VSFull(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float4 PSFrame(VSOut i) : SV_Target {
    float2 px = i.pos.xy;
    float2 local = (px - rect.xy) / rect.zw;
    if (any(local < 0.0) || any(local > 1.0)) return float4(0, 0, 0, 1);
    float4 c;
    if (useLinear > 0.5)
        c = Tex.SampleLevel(sLinear, local, 0);
    else
        c = Tex.SampleLevel(sPoint, local, 0);
    return float4(c.rgb, 1);
}

// Screen space quad for cursor / HUD (premultiplied alpha).
VSOut VSQuad(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2(id & 1, id >> 1);
    float2 px = rect.xy + uv * rect.zw;
    o.pos = float4(px / bbSize * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float4 PSQuad(VSOut i) : SV_Target {
    float4 c;
    if (useLinear > 0.5)
        c = Tex.SampleLevel(sLinear, i.uv, 0);
    else
        c = Tex.SampleLevel(sPoint, i.uv, 0);
    return c * opacity;
}
