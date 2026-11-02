#include "platform/webview.h"
#include "platform/platform.h"

#include <windows.h>
#include <objbase.h>
#include <string>

#include <WebView2.h>

#define IDR_WEBVIEW2_LOADER 100

static HMODULE g_loader;
static ICoreWebView2Environment *g_env;
static ICoreWebView2Controller *g_controller;
static ICoreWebView2 *g_webview;
static HWND g_parent;
static std::wstring g_url;

class ControllerHandler;

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateEnv)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetVersion)(PCWSTR, LPWSTR *);

static int write_loader(const wchar_t *dir, const void *data, DWORD size,
                        wchar_t *out, size_t cap)
{
    HANDLE f;
    DWORD written = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad;

    CreateDirectoryW(dir, NULL);
    _snwprintf(out, cap, L"%s\\WebView2Loader-%lu.dll", dir, (unsigned long)size);
    out[cap - 1] = L'\0';

    if (GetFileAttributesExW(out, GetFileExInfoStandard, &fad) &&
        fad.nFileSizeLow == size)
        return 1;

    f = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (!WriteFile(f, data, size, &written, NULL) || written != size) {
        CloseHandle(f);
        return 0;
    }
    CloseHandle(f);
    return 1;
}

static int extract_loader(wchar_t *out, size_t cap)
{
    HRSRC res;
    HGLOBAL mem;
    DWORD size;
    void *data;
    wchar_t dir[1024];
    DWORD n;

    res = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_WEBVIEW2_LOADER),
                        MAKEINTRESOURCEW(10));
    if (!res)
        return 0;
    size = SizeofResource(NULL, res);
    mem = LoadResource(NULL, res);
    data = mem ? LockResource(mem) : NULL;
    if (!data || !size)
        return 0;

    {
        char cfg[600];
        const char *cd = nd_config_dir();
        if (cd && cd[0]) {
            snprintf(cfg, sizeof(cfg), "%swebview2", cd);
            if (MultiByteToWideChar(CP_UTF8, 0, cfg, -1, dir,
                                    (int)(sizeof(dir) / sizeof(dir[0]))) > 0 &&
                write_loader(dir, data, size, out, cap))
                return 1;
        }
    }

    n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, (DWORD)(sizeof(dir) / sizeof(dir[0])));
    if (n && n < sizeof(dir) / sizeof(dir[0])) {
        wcscat(dir, L"\\NetDebug");
        if (write_loader(dir, data, size, out, cap))
            return 1;
    }

    n = GetTempPathW((DWORD)(sizeof(dir) / sizeof(dir[0])), dir);
    if (n) {
        wcscat(dir, L"NetDebug");
        if (write_loader(dir, data, size, out, cap))
            return 1;
    }
    return 0;
}

static HMODULE load_loader(void)
{
    wchar_t path[MAX_PATH];
    wchar_t embedded[1024];
    DWORD n;
    wchar_t *slash;

    if (g_loader)
        return g_loader;

    if (extract_loader(embedded, 1024)) {
        g_loader = LoadLibraryW(embedded);
        if (g_loader)
            return g_loader;
    }

    n = GetModuleFileNameW(NULL, path, (DWORD)(sizeof(path) / sizeof(path[0])));
    if (n && n < sizeof(path) / sizeof(path[0])) {
        path[n] = L'\0';
        slash = wcsrchr(path, L'\\');
        if (slash) {
            slash[1] = L'\0';
            wcscat(path, L"WebView2Loader.dll");
            g_loader = LoadLibraryW(path);
        }
    }
    if (!g_loader)
        g_loader = LoadLibraryW(L"WebView2Loader.dll");
    return g_loader;
}

int nd_webview_available(void)
{
    HMODULE loader = load_loader();
    PFN_GetVersion getver;
    LPWSTR version = NULL;
    HRESULT hr;

    if (!loader)
        return 0;
    getver = (PFN_GetVersion)GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString");
    if (!getver)
        return 0;
    hr = getver(NULL, &version);
    if (version)
        CoTaskMemFree(version);
    return SUCCEEDED(hr) && version != NULL;
}

class EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    EnvHandler(HWND parent, const std::wstring &url) : refs_(1), parent_(parent), url_(url) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown ||
            riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0)
            delete this;
        return (ULONG)r;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                     ICoreWebView2Environment *env) override;

private:
    LONG refs_;
    HWND parent_;
    std::wstring url_;
};

class ControllerHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    ControllerHandler(HWND parent, const std::wstring &url)
        : refs_(1), parent_(parent), url_(url) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown ||
            riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0)
            delete this;
        return (ULONG)r;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                     ICoreWebView2Controller *controller) override
    {
        if (!IsWindow(parent_)) {
            Release();
            return S_OK;
        }
        if (SUCCEEDED(result) && controller) {
            g_controller = controller;
            g_controller->AddRef();
            g_controller->get_CoreWebView2(&g_webview);
            if (g_webview) {
                RECT rc;
                GetClientRect(parent_, &rc);
                g_controller->put_Bounds(rc);
                g_webview->Navigate(g_url.c_str());
            }
        }
        Release();
        return S_OK;
    }

private:
    LONG refs_;
    HWND parent_;
    std::wstring url_;
};

HRESULT EnvHandler::Invoke(HRESULT result, ICoreWebView2Environment *env)
{
    if (!IsWindow(parent_)) {
        Release();
        return S_OK;
    }
    if (SUCCEEDED(result) && env) {
        g_env = env;
        g_env->AddRef();
        ControllerHandler *h = new ControllerHandler(parent_, url_);
        HRESULT hr = env->CreateCoreWebView2Controller(parent_, h);
        if (FAILED(hr)) {
            g_env->Release();
            g_env = NULL;
        }
    }
    Release();
    return S_OK;
}

int nd_webview_attach(HWND parent, const wchar_t *url, const wchar_t *user_data_dir)
{
    HMODULE loader = load_loader();
    PFN_CreateEnv create_env;

    if (!loader)
        return -1;
    create_env = (PFN_CreateEnv)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!create_env)
        return -1;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    nd_webview_close();
    g_parent = parent;
    g_url = url ? url : L"";

    EnvHandler *h = new EnvHandler(parent, g_url);
    HRESULT hr = create_env(NULL, user_data_dir, NULL, h);
    return SUCCEEDED(hr) ? 0 : -1;
}

void nd_webview_resize(void)
{
    RECT rc;
    if (g_controller && g_parent && GetClientRect(g_parent, &rc))
        g_controller->put_Bounds(rc);
}

void nd_webview_reload(void)
{
    if (g_webview)
        g_webview->Reload();
}

void nd_webview_close(void)
{
    if (g_controller) {
        g_controller->Close();
        g_controller->Release();
        g_controller = NULL;
    }
    if (g_webview) {
        g_webview->Release();
        g_webview = NULL;
    }
    if (g_env) {
        g_env->Release();
        g_env = NULL;
    }
    g_parent = NULL;
    g_url.clear();
}
