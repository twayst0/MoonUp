#include "webview.h"

namespace sw {

namespace {

// Generic COM callback object for WebView2 completion/event handlers.
template <typename I, typename... Args>
class Callback final : public I {
public:
    Callback(const GUID& iid, std::function<HRESULT(Args...)> fn) : iid_(iid), fn_(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, iid_)) {
            *ppv = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return fn_(args...); }

private:
    GUID iid_;
    std::function<HRESULT(Args...)> fn_;
    std::atomic<ULONG> ref_{1};
};

using EnvHandler = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, void*>;
using CtrlHandler = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, void*>;
using MsgHandler = Callback<ICoreWebView2WebMessageReceivedEventHandler, void*, void*>;

}  // namespace

void WebView::Create(HWND parent, const std::wstring& uiFolder, const std::wstring& dataFolder, MessageFn onMessage,
                     ReadyFn ready, bool devTools) {
    parent_ = parent;
    uiFolder_ = uiFolder;
    onMessage_ = std::move(onMessage);
    ready_ = std::move(ready);
    devTools_ = devTools;

    std::wstring loaderPath = ExeDir() + L"\\WebView2Loader.dll";
    HMODULE loader = LoadLibraryW(loaderPath.c_str());
    if (!loader) loader = LoadLibraryW(L"WebView2Loader.dll");
    auto create = loader ? (PFN_CreateCoreWebView2EnvironmentWithOptions)GetProcAddress(
                               loader, "CreateCoreWebView2EnvironmentWithOptions")
                         : nullptr;
    if (!create) {
        if (ready_) ready_(false, HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
        return;
    }
    auto* handler = new EnvHandler(IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                                   [this](HRESULT hr, void* envPtr) -> HRESULT {
                                       auto* env = (ICoreWebView2Environment*)envPtr;
                                       if (FAILED(hr) || !env) {
                                           if (ready_) ready_(false, hr);
                                           return S_OK;
                                       }
                                       env_ = env;
                                       auto* ch = new CtrlHandler(
                                           IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                                           [this](HRESULT hr2, void* ctrl) -> HRESULT {
                                               if (FAILED(hr2) || !ctrl) {
                                                   if (ready_) ready_(false, hr2);
                                                   return S_OK;
                                               }
                                               OnController((ICoreWebView2Controller*)ctrl);
                                               return S_OK;
                                           });
                                       HRESULT r = env_->CreateCoreWebView2Controller(parent_, ch);
                                       ch->Release();
                                       if (FAILED(r) && ready_) ready_(false, r);
                                       return S_OK;
                                   });
    HRESULT hr = create(nullptr, dataFolder.c_str(), nullptr, handler);
    handler->Release();
    if (FAILED(hr) && ready_) ready_(false, hr);
}

void WebView::OnController(ICoreWebView2Controller* controller) {
    controller_ = controller;
    ComPtr<ICoreWebView2> view;
    controller_->CoreWebView2((void**)view.GetAddressOf());
    if (!view) {
        if (ready_) ready_(false, E_FAIL);
        return;
    }
    view_ = view;

    // Dark background so there is never a white flash before the page paints.
    ICoreWebView2Controller2* c2 = nullptr;
    if (SUCCEEDED(controller_->QueryInterface(IID_ICoreWebView2Controller2, (void**)&c2)) && c2) {
        COREWEBVIEW2_COLOR bg{255, 5, 7, 12};
        c2->SetDefaultBackgroundColor(bg);
        c2->Release();
    }

    ComPtr<ICoreWebView2Settings> settings;
    view_->Settings((void**)settings.GetAddressOf());
    if (settings) {
        settings->SetAreDevToolsEnabled(devTools_ ? TRUE : FALSE);
        settings->SetAreDefaultContextMenusEnabled(devTools_ ? TRUE : FALSE);
        settings->SetIsStatusBarEnabled(FALSE);
        settings->SetIsZoomControlEnabled(FALSE);
        settings->SetAreDefaultScriptDialogsEnabled(TRUE);
        ICoreWebView2Settings3* s3 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(IID_ICoreWebView2Settings3, (void**)&s3)) && s3) {
            s3->SetAreBrowserAcceleratorKeysEnabled(devTools_ ? TRUE : FALSE);
            s3->Release();
        }
    }

    auto* mh = new MsgHandler(IID_ICoreWebView2WebMessageReceivedEventHandler, [this](void*, void* argsPtr) -> HRESULT {
        auto* args = (ICoreWebView2WebMessageReceivedEventArgs*)argsPtr;
        LPWSTR msg = nullptr;
        if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
            std::wstring m(msg);
            CoTaskMemFree(msg);
            if (onMessage_) onMessage_(m);
        }
        return S_OK;
    });
    INT64 token = 0;
    view_->add_WebMessageReceived(mh, &token);
    mh->Release();

    RECT rc;
    GetClientRect(parent_, &rc);
    controller_->SetBounds(rc);
    controller_->SetIsVisible(TRUE);

    ICoreWebView2_3* v3 = nullptr;
    bool mapped = false;
    if (SUCCEEDED(view_->QueryInterface(IID_ICoreWebView2_3, (void**)&v3)) && v3) {
        // COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW = 1
        mapped = SUCCEEDED(v3->SetVirtualHostNameToFolderMapping(L"moonup.local", uiFolder_.c_str(), 1));
        v3->Release();
    }
    std::wstring url = mapped ? L"https://moonup.local/index.html" : (L"file:///" + uiFolder_ + L"/index.html");
    for (auto& ch : url)
        if (ch == L'\\') ch = L'/';
    HRESULT hr = view_->Navigate(url.c_str());
    if (ready_) ready_(SUCCEEDED(hr), hr);
}

void WebView::Resize(const RECT& r) {
    if (controller_) controller_->SetBounds(r);
}

void WebView::Post(const std::string& json) {
    if (!view_) return;
    std::wstring w = Wide(json);
    view_->PostWebMessageAsJson(w.c_str());
}

void WebView::Focus() {
    if (controller_) controller_->MoveFocus(0);  // COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC
}

void WebView::Close() {
    if (controller_) controller_->Close();
    view_.Reset();
    controller_.Reset();
    env_.Reset();
}

}  // namespace sw
