// Exercises the actual feeder submission and colour-format code on D3D12 WARP.
// Including the translation unit keeps fault injection out of the shipped add-on.
#include "../src/dlss5-feed.cpp"
#include <cstdlib>

static void Require(bool ok, const char *message)
{
    if (!ok) { std::printf("FAIL: %s\n", message); std::exit(1); }
}

int main()
{
    InitializeCriticalSection(&g_log_cs);
    strcpy_s(g_log_path, "build\\compat-tests\\submission.log");
    g_self = GetModuleHandleW(nullptr);
    Require(SameTexelLayout(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), "BGRA sRGB is a raw-copy layout");
    Require(!SameTexelLayout(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), "channel swizzles must not be raw-copied");
    Require(!SameTexelLayout(DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN), "unknown formats must not be raw-copied");
    Require(OutputFormatFor(DXGI_FORMAT_B8G8R8A8_UNORM) == DXGI_FORMAT_B8G8R8A8_UNORM, "preserve BGRA output");
    Require(OutputFormatFor(DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_FLOAT, "preserve FP16 output");

    const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    using FactoryFn = HRESULT (WINAPI *)(REFIID, void **);
    const auto makeFactory = reinterpret_cast<FactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory4 *factory = nullptr;
    Require(makeFactory && SUCCEEDED(makeFactory(IID_PPV_ARGS(&factory))), "DXGI factory");
    IDXGIAdapter *adapter = nullptr;
    Require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
    const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    const auto makeDevice = reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    Require(makeDevice && SUCCEEDED(makeDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev12))), "WARP D3D12 device");
    D3D12_COMMAND_QUEUE_DESC qd = {};
    Require(SUCCEEDED(g.dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue))), "queue");
    for (int i = 0; i < Feed::kFrames; ++i)
        Require(SUCCEEDED(g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc[i]))), "allocator");
    Require(SUCCEEDED(g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr, IID_PPV_ARGS(&g.list))), "command list");
    Require(SUCCEEDED(g.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence12))), "fence");
    g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(g.fence_event != nullptr, "fence event");
    // Closing a closed list forces Close() failure without recording invalid GPU work.
    Require(SUCCEEDED(g.list->Close()), "initial close");
    g.frame_ready = true;
    g_mv_probe_fence = 1;
    Require(EndCommands() == 0, "failed Close is not submitted");
    Require(g.fence_value == 0 && g.alloc_fence[0] == 0 && g.frame_slot == 0, "failed close does not advance retirement");
    Require(!g.frame_ready && g_mv_probe_fence == 0 && g.need_reset, "failed work invalidates output and cancels the readback");
    Require(g.consecutive_fails == 1, "failed close enters failure handling");
    Require(BeginCommands(), "replacement command list opens");
    Require(EndCommands() == 1, "valid work submits after failure");
    Require(g.frame_slot == 1 && g.alloc_fence[0] == 1, "valid submission retires the allocator");
    DrainGpu();
    Require(SUCCEEDED(g.dev12->GetDeviceRemovedReason()), "device remains usable");

    // Editing mode in the config must rebuild, including transport-only -> NGX.
    g_cfg.mode = 1; g_cfg.preset = 13; g_cfg.vk_present_sync = 0;
    CfgSave(); g_cfg.mode = 2;
    Require(CfgReload() && g_cfg.mode == 1, "file mode change requests a feature rebuild");
    g_cfg.preset = 0; g_cfg.vk_present_sync = 1;
    Require(CfgReload() && g_cfg.preset == 13 && g_cfg.vk_present_sync == 0, "Model M and diagnostic switch round-trip through config");
    g_ngx_dying = true;
    ShutdownSession();
    adapter->Release(); factory->Release();
    DeleteCriticalSection(&g_log_cs);
    std::puts("Feeder WARP submission, colour layout and config regression tests passed.");
}