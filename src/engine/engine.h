// MoonUp engine: owns the GPU, capture, processing, frame generation and the output overlay.
// Runs on its own thread; communicates with the app through JSON events.
#pragma once
#include <functional>
#include <thread>

#include "config.h"
#include "../common.h"

namespace sw {

class Engine {
public:
    using EventFn = std::function<void(const std::string& json)>;

    Engine();
    ~Engine();

    bool Start(HWND target, const EngineConfig& cfg, EventFn events);
    void Stop();
    bool Running() const { return running_.load(); }
    HWND Target() const { return target_; }

    // Applies settings that can change while running (upscaler, sharpness, frame generation, HUD...).
    void UpdateConfig(const EngineConfig& cfg);
    // True when 'next' differs from the running config in a way that needs a restart.
    bool NeedsRestart(const EngineConfig& next) const;

private:
    void ThreadMain();
    void Run();
    void Emit(const std::string& json);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    HWND target_ = nullptr;
    EngineConfig startCfg_;
    std::mutex cfgMutex_;
    EngineConfig pendingCfg_;
    std::atomic<bool> cfgDirty_{false};
    EventFn events_;
    std::atomic<bool> stoppedSent_{false};
};

}  // namespace sw
