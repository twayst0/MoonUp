// Minimal WebView2 host: creates the browser inside a window and bridges JSON messages.
#pragma once
#include <functional>
#include <string>

#include "common.h"
#include "third_party/webview2_min.h"

namespace sw {

class WebView {
public:
    using MessageFn = std::function<void(const std::wstring& message)>;
    using ReadyFn = std::function<void(bool ok, HRESULT hr)>;

    // Asynchronous: 'ready' is called on the UI thread once the page started loading (or failed).
    void Create(HWND parent, const std::wstring& uiFolder, const std::wstring& dataFolder, MessageFn onMessage,
                ReadyFn ready, bool devTools);
    void Resize(const RECT& r);
    void Post(const std::string& json);
    void Focus();
    void Close();
    bool Ready() const { return view_ != nullptr; }

private:
    void OnController(ICoreWebView2Controller* controller);

    HWND parent_ = nullptr;
    std::wstring uiFolder_;
    MessageFn onMessage_;
    ReadyFn ready_;
    bool devTools_ = false;
    ComPtr<ICoreWebView2Environment> env_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> view_;
};

}  // namespace sw
