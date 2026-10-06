// MoonUp engine configuration (one profile worth of processing settings).
#pragma once
#include <string>

namespace sw {

enum class CaptureApi { WGC = 0, DDA = 1, GDI = 2 };

enum class ScaleMode {
    Auto = 0,        // fit the monitor, keep aspect ratio
    Fullscreen = 1,  // stretch to the whole monitor
    Custom = 2,      // fixed factor
    Integer = 3,     // largest integer factor that fits
    Off = 4          // no scaling, overlay sits exactly on the window (frame generation only)
};

enum class Upscaler {
    Neural = 0,    // MoonUp Neural SR (16-channel CNN trained on photos and game frames)
    Edge = 1,      // MoonUp Edge (edge-adaptive, anisotropic kernel)
    Lanczos = 2,
    Bicubic = 3,
    Bilinear = 4,
    Nearest = 5,
    PixelArt = 6,  // sharp bilinear for pixel art
    Fsr = 7,       // AMD FidelityFX Super Resolution 1 (EASU + RCAS)
    Nis = 8,       // NVIDIA Image Scaling (1x-2x, falls back to FSR above 2x)
    Anime = 9      // ArtCNN C4F16 neural network (anime / 2D art), same passes as Neural
};

enum class FrameGenMode { Off = 0, X2 = 2, X3 = 3, X4 = 4, Adaptive = 100 };
enum class FlowQuality { Performance = 0, Balanced = 1, Quality = 2 };
enum class HudPosition { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 };

struct VisionConfig {
    bool enabled = false;
    float clarity = 0.35f;    // local contrast
    float detail = 0.25f;     // micro detail
    float vibrance = 0.20f;
    float contrast = 0.10f;
    float warmth = 0.0f;      // -1 .. 1
    float brightness = 0.0f;  // -1 .. 1
};

// MoonUp Neural Render v2 (one-step neural rendering stage, see shaders/render.hlsl).
// Like DLSS 5's controls: tone = low frequency lighting, structure = high frequency shading/detail.
struct NeuralRenderConfig {
    bool enabled = false;
    float tone = 1.0f;       // 0..2 lighting depth, local contrast, highlights (luminance)
    float color = 0.8f;      // 0..2 materials and colour (chroma)
    float structure = 1.0f;  // 0..2 contact shadows, ambient occlusion, micro detail
    float temporal = 0.7f;   // 0..1 how much the look is carried between frames
    bool Active() const { return enabled && tone + color + structure > 0.001f; }
};

struct EngineConfig {
    CaptureApi capture = CaptureApi::WGC;
    int adapterIndex = -1;  // -1 = high performance automatic

    ScaleMode scaleMode = ScaleMode::Auto;
    float customFactor = 1.5f;
    Upscaler upscaler = Upscaler::Edge;
    float sharpness = 0.35f;  // 0..1

    FrameGenMode frameGen = FrameGenMode::Off;
    int targetFps = 0;  // adaptive target, 0 = monitor refresh
    FlowQuality flowQuality = FlowQuality::Balanced;
    bool hudProtect = true;
    bool sceneCut = true;

    VisionConfig vision;
    NeuralRenderConfig render;

    bool vsync = true;
    bool allowTearing = false;
    int maxFrameLatency = 1;

    bool showFps = true;
    bool showGraph = true;
    HudPosition hudPosition = HudPosition::TopLeft;

    bool drawCursor = true;
    bool clipCursor = true;
    bool pauseWhenUnfocused = true;

    // Performance protection: keep MoonUp' GPU use below a budget so the game keeps its frame rate
    // (throttles the processing rate and lowers effects step by step when the GPU is saturated).
    bool autoPerf = true;
};

}  // namespace sw
