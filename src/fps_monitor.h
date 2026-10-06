// Stand-alone FPS counter: shows the frame rate of the foreground window in a small click-through
// window, also when MoonUp is not scaling (or its own overlay is hidden: passthrough, bypass).
// The rate is measured with Windows Graphics Capture: every frame the window presents is counted
// and released at once (no copy, no processing).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "common.h"

namespace sw {

class FpsMonitor {
public:
    ~FpsMonitor() { Stop(); }
    void Start();
    void Stop();
    // UI thread, a few times per second. target nullptr hides the counter.
    // corner: 0 top left, 1 top right, 2 bottom left, 3 bottom right.
    void SetTarget(HWND target, int corner);
    // false on Windows 10: capturing shows a yellow border, so only the window MoonUp already
    // captures (the one being scaled) gets the counter.
    bool AnyWindow() const { return anyWindow_; }
    // Localised word for "display" (shown when the game renders faster than the screen refreshes).
    void SetLabel(const std::wstring& display) {
        std::lock_guard<std::mutex> lock(m_);
        display_ = display;
    }

private:
    void Thread();
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex m_;
    HWND want_ = nullptr;
    int corner_ = 0;
    std::atomic<bool> anyWindow_{true};
    std::wstring display_ = L"display";
};

}  // namespace sw
