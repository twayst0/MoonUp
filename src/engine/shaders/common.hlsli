// MoonUp shared shader helpers. Prepended to every shader source at compile time.
SamplerState sPoint  : register(s0);
SamplerState sLinear : register(s1);

static const float PI = 3.14159265359;

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float Sinc(float x) {
    x *= PI;
    return abs(x) < 1e-4 ? 1.0 : sin(x) / x;
}

// Lanczos window with support 'a' (2 or 3).
float Lanczos(float x, float a) {
    x = abs(x);
    return x < a ? Sinc(x) * Sinc(x / a) : 0.0;
}

// Catmull-Rom cubic weight.
float CatmullRom(float x) {
    x = abs(x);
    float x2 = x * x, x3 = x2 * x;
    if (x < 1.0) return 1.5 * x3 - 2.5 * x2 + 1.0;
    if (x < 2.0) return -0.5 * x3 + 2.5 * x2 - 4.0 * x + 2.0;
    return 0.0;
}
