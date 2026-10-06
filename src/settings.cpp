#include "settings.h"

#include "common.h"

namespace sw {

json DefaultProfile(const std::string& name) {
    return json{{"name", name},
                {"match", ""},
                {"capture", "wgc"},
                {"adapter", -1},
                {"scaleMode", "auto"},
                {"factor", 1.5},
                {"upscaler", "edge"},
                {"sharpness", 0.35},
                {"frameGen", "off"},
                {"targetFps", 0},
                {"flowQuality", "balanced"},
                {"hudProtect", true},
                {"sceneCut", true},
                {"vision",
                 {{"enabled", false},
                  {"preset", "natural"},
                  {"clarity", 0.35},
                  {"detail", 0.25},
                  {"vibrance", 0.2},
                  {"contrast", 0.1},
                  {"warmth", 0.0},
                  {"brightness", 0.0}}},
                {"render", {{"enabled", false}, {"tone", 1.0}, {"color", 0.8}, {"structure", 1.0}, {"temporal", 0.7}}},
                {"vsync", true},
                {"allowTearing", false},
                {"maxLatency", 1},
                {"showFps", true},
                {"showGraph", true},
                {"hudPosition", "tl"},
                {"drawCursor", true},
                {"clipCursor", true},
                {"pauseWhenUnfocused", true},
                {"autoPerf", true},
                {"renderScale", 100}};
}

json DefaultSettings() {
    json profiles = json::object();
    profiles["default"] = DefaultProfile("Default");
    return json{{"version", 1},
                {"language", ""},
                {"intro", true},
                {"reducedMotion", false},
                {"accent", "moon"},
                {"minimizeToTray", true},
                {"closeToTray", false},
                {"startWithWindows", false},
                {"startMinimized", false},
                {"hotkey", {{"ctrl", true}, {"alt", true}, {"shift", false}, {"key", "S"}}},
                {"scaleDelay", 5},
                {"activeProfile", "default"},
                {"profiles", profiles},
                {"lastSession", json::object()},
                {"upgraph", {{"folders", json::array()},
                             {"preset", {{"tone", 1.0}, {"color", 0.8}, {"structure", 1.0}, {"temporal", 0.7},
                                         {"light", 0.6}, {"lightRadius", 1.0}, {"sharpness", 0.35}}}}},
                {"firstRun", true}};
}

json MergeDefaults(const json& def, const json& val) {
    if (!val.is_object() || !def.is_object()) return val.is_null() ? def : val;
    json out = val;
    for (auto it = def.begin(); it != def.end(); ++it) {
        if (!out.contains(it.key()))
            out[it.key()] = it.value();
        else if (it.value().is_object() && out[it.key()].is_object() && it.key() != "profiles")
            out[it.key()] = MergeDefaults(it.value(), out[it.key()]);
    }
    return out;
}

static std::wstring SettingsPath() { return DataDir() + L"\\settings.json"; }

json LoadSettings() {
    json def = DefaultSettings();
    FILE* f = _wfopen(SettingsPath().c_str(), L"rb");
    if (!f) return def;
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    json s = json::parse(text, nullptr, false);
    if (s.is_discarded() || !s.is_object()) {
        SW_LOG("settings.json is invalid, using defaults");
        return def;
    }
    s = MergeDefaults(def, s);
    if (!s["profiles"].is_object() || s["profiles"].empty()) s["profiles"] = def["profiles"];
    for (auto it = s["profiles"].begin(); it != s["profiles"].end(); ++it)
        it.value() = MergeDefaults(DefaultProfile(it.value().value("name", "Profile")), it.value());
    if (!s["profiles"].contains(s.value("activeProfile", "default"))) s["activeProfile"] = s["profiles"].begin().key();
    return s;
}

bool SaveSettings(const json& s) {
    std::wstring tmp = SettingsPath() + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    std::string text = s.dump(2);
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return MoveFileExW(tmp.c_str(), SettingsPath().c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

static float F(const json& j, const char* k, float d) {
    auto it = j.find(k);
    return (it != j.end() && it->is_number()) ? it->get<float>() : d;
}
static bool B(const json& j, const char* k, bool d) {
    auto it = j.find(k);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : d;
}
static std::string S(const json& j, const char* k, const char* d) {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : d;
}

EngineConfig ProfileToConfig(const json& p) {
    EngineConfig c;
    {
        std::string cap = S(p, "capture", "wgc");
        c.capture = cap == "dda" ? CaptureApi::DDA : cap == "gdi" ? CaptureApi::GDI : CaptureApi::WGC;
    }
    c.adapterIndex = (int)F(p, "adapter", -1);

    std::string m = S(p, "scaleMode", "auto");
    c.scaleMode = m == "fullscreen" ? ScaleMode::Fullscreen
                : m == "custom"     ? ScaleMode::Custom
                : m == "integer"    ? ScaleMode::Integer
                : m == "off"        ? ScaleMode::Off
                                    : ScaleMode::Auto;
    c.customFactor = std::clamp(F(p, "factor", 1.5f), 0.25f, 8.0f);

    std::string u = S(p, "upscaler", "edge");
    c.upscaler = u == "neural"   ? Upscaler::Neural
               : u == "lanczos"  ? Upscaler::Lanczos
               : u == "bicubic"  ? Upscaler::Bicubic
               : u == "bilinear" ? Upscaler::Bilinear
               : u == "nearest"  ? Upscaler::Nearest
               : u == "pixel"    ? Upscaler::PixelArt
               : u == "fsr"      ? Upscaler::Fsr
               : u == "nis"      ? Upscaler::Nis
               : u == "anime"    ? Upscaler::Anime
                                 : Upscaler::Edge;
    c.sharpness = std::clamp(F(p, "sharpness", 0.35f), 0.0f, 1.0f);

    std::string fg = S(p, "frameGen", "off");
    c.frameGen = fg == "x2"         ? FrameGenMode::X2
               : fg == "x3"         ? FrameGenMode::X3
               : fg == "x4"         ? FrameGenMode::X4
               : fg == "adaptive"   ? FrameGenMode::Adaptive
                                    : FrameGenMode::Off;
    c.targetFps = (int)F(p, "targetFps", 0);
    std::string q = S(p, "flowQuality", "balanced");
    c.flowQuality = q == "performance" ? FlowQuality::Performance : q == "quality" ? FlowQuality::Quality : FlowQuality::Balanced;
    c.hudProtect = B(p, "hudProtect", true);
    c.sceneCut = B(p, "sceneCut", true);

    if (p.contains("vision") && p["vision"].is_object()) {
        const json& v = p["vision"];
        c.vision.enabled = B(v, "enabled", false);
        c.vision.clarity = std::clamp(F(v, "clarity", 0.35f), 0.0f, 1.0f);
        c.vision.detail = std::clamp(F(v, "detail", 0.25f), 0.0f, 1.0f);
        c.vision.vibrance = std::clamp(F(v, "vibrance", 0.2f), -1.0f, 1.0f);
        c.vision.contrast = std::clamp(F(v, "contrast", 0.1f), -1.0f, 1.0f);
        c.vision.warmth = std::clamp(F(v, "warmth", 0.0f), -1.0f, 1.0f);
        c.vision.brightness = std::clamp(F(v, "brightness", 0.0f), -1.0f, 1.0f);
    }
    if (p.contains("render") && p["render"].is_object()) {
        const json& r = p["render"];
        c.render.enabled = B(r, "enabled", false);
        c.render.tone = std::clamp(F(r, "tone", 1.0f), 0.0f, 2.0f);
        c.render.color = std::clamp(F(r, "color", 0.8f), 0.0f, 2.0f);
        c.render.structure = std::clamp(F(r, "structure", 1.0f), 0.0f, 2.0f);
        c.render.temporal = std::clamp(F(r, "temporal", 0.7f), 0.0f, 1.0f);
    }
    c.vsync = B(p, "vsync", true);
    c.allowTearing = B(p, "allowTearing", false);
    c.maxFrameLatency = std::clamp((int)F(p, "maxLatency", 1), 1, 3);
    c.showFps = B(p, "showFps", true);
    c.showGraph = B(p, "showGraph", true);
    std::string hp = S(p, "hudPosition", "tl");
    c.hudPosition = hp == "tr" ? HudPosition::TopRight : hp == "bl" ? HudPosition::BottomLeft : hp == "br" ? HudPosition::BottomRight : HudPosition::TopLeft;
    c.drawCursor = B(p, "drawCursor", true);
    c.clipCursor = B(p, "clipCursor", true);
    c.pauseWhenUnfocused = B(p, "pauseWhenUnfocused", true);
    c.autoPerf = B(p, "autoPerf", true);
    return c;
}

static std::string Lower(std::string s) {
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

json ProfileForExe(const json& settings, const std::string& exe, std::string* id) {
    const json& profiles = settings["profiles"];
    std::string lexe = Lower(exe);
    if (!lexe.empty()) {
        for (auto it = profiles.begin(); it != profiles.end(); ++it) {
            std::string m = Lower(it.value().value("match", ""));
            if (!m.empty() && m == lexe) {
                if (id) *id = it.key();
                return it.value();
            }
        }
    }
    std::string active = settings.value("activeProfile", "default");
    if (profiles.contains(active)) {
        if (id) *id = active;
        return profiles[active];
    }
    if (id) *id = profiles.begin().key();
    return profiles.begin().value();
}

}  // namespace sw
