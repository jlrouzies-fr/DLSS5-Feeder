// DLSS5 CK3 app-local DXGI bootstrap.
// Loaded explicitly by VK_LAYER_feed_vk before Vulkan device creation.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>

#include <cstdarg>
#include <cstdio>

static HMODULE g_self;
static HMODULE g_system_dxgi;
static HMODULE g_reshade;

static void BridgeLog(const char *format, ...)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    if (wchar_t *slash = wcsrchr(path, L'\\'))
        wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"dlss5-dxgi.log");
    char message[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    FILE *file = nullptr;
    if (_wfopen_s(&file, path, L"a") == 0 && file != nullptr)
    {
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        fprintf(file, "%02u:%02u:%02u.%03u  %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
        fclose(file);
    }
}

static HMODULE LoadSystemDxgi()
{
    if (g_system_dxgi != nullptr) return g_system_dxgi;
    wchar_t path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 10) return nullptr;
    wcscat_s(path, L"\\dxgi.dll");
    g_system_dxgi = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return g_system_dxgi;
}

static HMODULE LoadPrivateReShade()
{
    if (g_reshade != nullptr) return g_reshade;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return nullptr;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"dlss5-vulkan\\ReShade64.dll");
    g_reshade = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_reshade == nullptr)
        BridgeLog("[bridge] failed to load private ReShade64.dll (Win32 %lu)", GetLastError());
    else
        BridgeLog("[bridge] private ReShade64.dll loaded from %ls", path);
    return g_reshade;
}

template <typename T>
static T Resolve(const char *name)
{
    if (HMODULE reshade = LoadPrivateReShade())
        if (FARPROC proc = GetProcAddress(reshade, name)) return reinterpret_cast<T>(proc);
    if (HMODULE system = LoadSystemDxgi())
        if (FARPROC proc = GetProcAddress(system, name)) return reinterpret_cast<T>(proc);
    return nullptr;
}

extern "C" HRESULT WINAPI BridgeCreateDXGIFactory(REFIID riid, void **factory)
{
    using Fn = HRESULT (WINAPI *)(REFIID, void **);
    const Fn fn = Resolve<Fn>("CreateDXGIFactory");
    return fn ? fn(riid, factory) : DXGI_ERROR_NOT_FOUND;
}

extern "C" HRESULT WINAPI BridgeCreateDXGIFactory1(REFIID riid, void **factory)
{
    using Fn = HRESULT (WINAPI *)(REFIID, void **);
    const Fn fn = Resolve<Fn>("CreateDXGIFactory1");
    return fn ? fn(riid, factory) : DXGI_ERROR_NOT_FOUND;
}

extern "C" HRESULT WINAPI BridgeCreateDXGIFactory2(UINT flags, REFIID riid, void **factory)
{
    using Fn = HRESULT (WINAPI *)(UINT, REFIID, void **);
    const Fn fn = Resolve<Fn>("CreateDXGIFactory2");
    return fn ? fn(flags, riid, factory) : DXGI_ERROR_NOT_FOUND;
}

extern "C" HRESULT WINAPI BridgeDXGIGetDebugInterface1(UINT flags, REFIID riid, void **object)
{
    using Fn = HRESULT (WINAPI *)(UINT, REFIID, void **);
    const Fn fn = Resolve<Fn>("DXGIGetDebugInterface1");
    return fn ? fn(flags, riid, object) : DXGI_ERROR_NOT_FOUND;
}

static ID3D12Device *g_boot_device;
static ID3D12CommandQueue *g_boot_queue;
static IDXGISwapChain3 *g_boot_swapchain;
static HWND g_boot_window;

static LRESULT CALLBACK BridgeWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

static HRESULT CreateHiddenD3D12Runtime()
{
    HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (d3d12 == nullptr) return HRESULT_FROM_WIN32(GetLastError());
    using CreateDeviceFn = HRESULT (WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    const auto create_device = reinterpret_cast<CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (create_device == nullptr) return HRESULT_FROM_WIN32(GetLastError());

    HRESULT result = create_device(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                   reinterpret_cast<void **>(&g_boot_device));
    if (FAILED(result)) return result;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = g_boot_device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue),
                                                reinterpret_cast<void **>(&g_boot_queue));
    if (FAILED(result)) return result;

    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = BridgeWindowProc;
    window_class.hInstance = g_self;
    window_class.lpszClassName = L"CK3DLSS5HiddenDXGI";
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32(GetLastError());
    g_boot_window = CreateWindowExW(0, window_class.lpszClassName, L"CK3 DLSS5 DXGI bootstrap",
                                    WS_OVERLAPPEDWINDOW, 0, 0, 960, 540,
                                    nullptr, nullptr, g_self, nullptr);
    if (g_boot_window == nullptr) return HRESULT_FROM_WIN32(GetLastError());

    IDXGIFactory2 *factory = nullptr;
    result = BridgeCreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory));
    if (FAILED(result)) return result;

    DXGI_SWAP_CHAIN_DESC1 swap_desc = {};
    swap_desc.Width = 960;
    swap_desc.Height = 540;
    swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 2;
    swap_desc.Scaling = DXGI_SCALING_STRETCH;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    IDXGISwapChain1 *swapchain = nullptr;
    result = factory->CreateSwapChainForHwnd(g_boot_queue, g_boot_window, &swap_desc,
                                              nullptr, nullptr, &swapchain);
    factory->Release();
    if (FAILED(result)) return result;
    result = swapchain->QueryInterface(__uuidof(IDXGISwapChain3),
                                       reinterpret_cast<void **>(&g_boot_swapchain));
    swapchain->Release();
    if (FAILED(result)) return result;

    result = g_boot_swapchain->Present(0, 0);
    BridgeLog("[bridge] hidden D3D12 runtime present -> 0x%08lX", static_cast<unsigned long>(result));
    return result;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DLSS5Bootstrap()
{
    static LONG attempted = 0;
    if (InterlockedCompareExchange(&attempted, 1, 0) != 0)
        return g_boot_swapchain != nullptr ? S_OK : E_FAIL;
    if (LoadPrivateReShade() == nullptr) return HRESULT_FROM_WIN32(GetLastError());
    const HRESULT result = CreateHiddenD3D12Runtime();
    BridgeLog("[bridge] DXGI/ReShade/D3D12 bootstrap -> 0x%08lX", static_cast<unsigned long>(result));
    return result;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
