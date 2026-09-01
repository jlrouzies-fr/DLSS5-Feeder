// dlss5-feed-host64 - the 64-bit half of DLSS5-Feeder for 32-bit games.
//
// A 32-bit game cannot load NGX or the DLSS 5 add-on (both x64-only). This little
// process can: it puts ReShade x64 (dxgi.dll) and renodx-dlss5.addon64 next to
// itself, opens a hidden 1x1 window with a minimal D3D12 swapchain -- so from the
// DLSS 5 add-on's point of view it IS a D3D12 game -- and runs the NGX DLAA
// evaluate on frames the game delivers through cross-process shared textures and
// shared fences. Which side creates the textures depends on the game's API and is
// the driver's call, not ours: D3D11 games create them (the phase-0-proven
// direction), OpenGL and Vulkan games cannot, so this host creates them instead and
// duplicates the handles in. See src/feed_ipc.h.
//
//   dlss5-feed-host64.exe --test   stand-alone: synthetic pattern, no game needed
//                                  (phase-1 proof: "feature 18 created" in ReShade.log)
//   dlss5-feed-host64.exe <pid>    serve the game with that PID over the pipe
//
// Logs to dlss5-feed-host.log next to the exe; the DLSS 5 add-on's own state
// appears in the host's ReShade.log.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include "../src/feed_ipc.h"
#include "../src/feed_fmt.h"

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static char g_log_path[MAX_PATH];
static bool g_show_window = false;   // visible host window = the user's door to the DLSS 5 panel
static bool g_renodx_lazy = false;   // DLSS 5 add-on is v45+ (per-present rescan, lazy adoption)
static bool g_renodx_v46  = false;   // DLSS 5 add-on is v4.6+ (global hotkeys, upscaling latch)
static bool g_renodx_v47  = false;   // DLSS 5 add-on is v4.7+ (reversible colour bridge, workset pool)

static void Log(const char *fmt, ...);

// Write a RenoDX.DLSS5 key into the host's ReShade.ini, only when the user has not
// set it themselves (the add-on persists any overlay change, and a saved value wins).
static void HostRenodxDefault(const char *ini, const char *key, const char *value, const char *why)
{
    char v[16] = {};
    GetPrivateProfileStringA("RenoDX.DLSS5", key, "", v, sizeof(v), ini);
    if (v[0] == '\0')
    {
        WritePrivateProfileStringA("RenoDX.DLSS5", key, value, ini);
        Log("[host] %s was unset; wrote %s=%s into the host's ReShade.ini (%s)", key, key, value, why);
    }
    else
        Log("[host] %s=%s (user-set; leaving it alone)", key, v);
}

// Detect the DLSS 5 add-on generation next to this exe: v45+ ('EnableHooks' marker in
// the binary) rescans every present and adopts missed features lazily, so the warm-up
// re-create is unnecessary -- and its EnableHooks key should be '2' (NGX-only) for this
// feeder, written into OUR ReShade.ini before ReShade loads and the add-on reads it.
// v4.6+ ('NRToggleKey' marker) keeps that engine and adds two GetAsyncKeyState-polled
// global hotkeys, which this background helper must unbind (see below). v4.7+
// ('NRGlobalTone' marker) keeps both and reworks its own colour path -- a reversible
// SDR/linear-HDR/PQ bridge chosen from the contract we publish -- which needs nothing
// new from this host. Each marker is a NUL-terminated config key, so the match includes
// the terminator and cannot hit a longer string that merely starts with it.
// Mirrors DetectRenodxAddon() in src/dlss5-feed.cpp; keep the two in step.
static bool RenodxHasLiteral(const char *buf, DWORD size, const char *needle)
{
    const DWORD n = static_cast<DWORD>(strlen(needle)) + 1;   // include the NUL
    if (buf == nullptr || size < n) return false;
    for (DWORD i = 0; i + n <= size; ++i)
        if (buf[i] == needle[0] && memcmp(buf + i, needle, n) == 0) return true;
    return false;
}

// The add-on's own generation banner ("v4.6", "v4.7"), which only v4.6+ builds carry.
// The version resource cannot tell those two apart -- both report 0.2026.0828.0517.
static void RenodxFindBanner(const char *buf, DWORD size, char *out, size_t out_size)
{
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (DWORD i = 1; i + 5 <= size; ++i)
    {
        if (buf[i - 1] != '\0' || buf[i] != 'v' || !digit(buf[i + 1]) || buf[i + 2] != '.' || !digit(buf[i + 3]))
            continue;
        DWORD end = i + 4;
        while (end < size && digit(buf[end])) ++end;
        if (end < size && buf[end] == '\0' && end - i < out_size)
        {
            memcpy(out, buf + i, end - i);
            out[end - i] = '\0';
            return;
        }
    }
}

static void DetectRenodxAddon()
{
    char dir[MAX_PATH], path[MAX_PATH], ini[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char *s = strrchr(dir, '\\')) *(s + 1) = '\0';
    sprintf_s(path, "%srenodx-dlss5.addon64", dir);
    sprintf_s(ini, "%sReShade.ini", dir);

    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log("[host] renodx-dlss5.addon64 not found next to the host"); return; }
    const DWORD size = GetFileSize(f, nullptr);
    DWORD got = 0;
    char *buf = (size > 0 && size < 8u * 1024 * 1024) ? static_cast<char *>(malloc(size)) : nullptr;
    char gen[16] = "";
    if (buf != nullptr && ReadFile(f, buf, size, &got, nullptr) && got == size)
    {
        g_renodx_lazy = RenodxHasLiteral(buf, size, "EnableHooks");
        g_renodx_v46  = RenodxHasLiteral(buf, size, "NRToggleKey");
        g_renodx_v47  = RenodxHasLiteral(buf, size, "NRGlobalTone");
        RenodxFindBanner(buf, size, gen, sizeof(gen));
    }
    free(buf);
    CloseHandle(f);
    if (g_renodx_v47) g_renodx_v46 = true;    // each generation keeps the previous engine
    if (g_renodx_v46) g_renodx_lazy = true;   // v4.6+ is a per-present-rescan engine too

    char ver[48] = "?";
    DWORD dummy = 0;
    const DWORD vsize = GetFileVersionInfoSizeA(path, &dummy);
    if (vsize > 0)
    {
        void *vdata = malloc(vsize);
        VS_FIXEDFILEINFO *ffi = nullptr;
        UINT flen = 0;
        if (vdata != nullptr && GetFileVersionInfoA(path, 0, vsize, vdata) &&
            VerQueryValueA(vdata, "\\", reinterpret_cast<void **>(&ffi), &flen) && ffi != nullptr)
            sprintf_s(ver, "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                      HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
        free(vdata);
    }
    Log("[host] DLSS 5 add-on: %s%s (file version %s) -- %s engine",
        gen[0] != '\0' ? "" : "v", gen[0] != '\0' ? gen : ver, ver,
        g_renodx_v47  ? "v4.7+ (lazy adoption, colour bridge, workset pool)"
      : g_renodx_v46  ? "v4.6 (lazy adoption, global hotkeys, upscaling latch)"
      : g_renodx_lazy ? "v45+ (lazy adoption; warm-up skipped)" : "classic (warm-up stays on)");

    if (g_renodx_lazy)
        HostRenodxDefault(ini, "EnableHooks", "2", "NGX-only -- this host calls NGX directly, no Streamline");

    // Every known add-on generation reads these two keys; make a fresh install
    // deterministic (NeuralUplift on is the whole point; upscaling can never engage
    // against the 1:1 DLAA contract this host publishes, and v4.6 pairs its WIP
    // upscaling path with a rejection latch that parks NR on the native path).
    HostRenodxDefault(ini, "NeuralUplift", "1", "neural rendering on");
    HostRenodxDefault(ini, "NREnableUpscaling", "0", "upscaling off; this host publishes a complete 1:1 DLAA contract");

    if (g_renodx_v46)
    {
        // v4.6+ polls its NR-toggle and screenshot hotkeys with GetAsyncKeyState, which
        // sees keys pressed in the game's window too. This helper usually runs headless,
        // so a gameplay keypress (F5 is a common quicksave key) would silently toggle NR
        // off, or fire GPU-readback screenshots, in a process with no visible feedback.
        // Unbind both unless the user bound them deliberately.
        HostRenodxDefault(ini, "NRToggleKey", "0", "unbound; gameplay keys must not reach this background helper");
        HostRenodxDefault(ini, "NRScreenshotKey", "0", "unbound; same reason");

        // Same warning the in-game add-on gives (src/dlss5-feed.cpp): NRStyle=2 crashed the
        // reference machine 1-2 s into every boot with a null read on the present path. It
        // matters more here -- this helper is usually headless, and on the 32-bit path the
        // setting is edited from a panel whose values land in THIS ini, where nothing else
        // would ever show it. Warn, never rewrite: it is the user's explicit choice.
        char style[16] = {};
        GetPrivateProfileStringA("RenoDX.DLSS5", "NRStyle", "", style, sizeof(style), ini);
        if (atoi(style) == 2)
            Log("[host] WARNING: RenoDX.DLSS5 NRStyle=2 is set in this host's ReShade.ini -- this crashed at "
                "startup on the reference machine (null read on the present path, blamed on whichever module "
                "presents next). If the game or this host dies on launch, set NRStyle=0 in host64\\ReShade.ini.");
    }
}

// Detect Alex's Toolkit next to this exe: a third-party NGX interposer that wraps every
// feature-18 (DLSS-NR) create the DLSS 5 add-on makes and runs it two or three times per
// frame as a cascade (a private 1:1 native pass feeds its output to the real pass as
// colour). It does not mishandle the guides -- every stage sees the same input
// dimensions we publish, so the motion vectors and depth stay valid -- but each stage
// keeps its OWN temporal history, so the cascade multiplies the effective history
// length. With screen-space estimated motion vectors that means visible smearing behind
// fast motion and a much slower settle after a hard camera cut. Report only.
// A d3dcompiler_47.dll sitting next to this exe is loaded in preference to System32's
// (it is not a KnownDLL). A Windows 8.1-SDK-era copy knows nothing past Shader Model 5.0
// and rejects the DLSS 5 add-on's cs_5_1 neural pass with "error X3506: unrecognized
// compiler target", every frame, silently -- the add-on keeps reporting evaluates while
// neural rendering does nothing. The verdict is a live compile, since a copy outside
// System32 may equally be a newer one. (Reported on Space Engineers; the 64-bit add-on
// runs the same probe in-process, and for the split path renodx lives HERE, not in the
// game, so this is the copy that matters.)
static void DetectStaleD3DCompiler()
{
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    if (m == nullptr) { Log("[host] d3dcompiler_47.dll not loadable"); return; }

    char path[MAX_PATH] = "?";
    GetModuleFileNameA(m, path, MAX_PATH);

    wchar_t sysdir[MAX_PATH] = {}, wpath[MAX_PATH] = {};
    GetSystemDirectoryW(sysdir, MAX_PATH);
    GetModuleFileNameW(m, wpath, MAX_PATH);
    const bool from_system = _wcsnicmp(wpath, sysdir, wcslen(sysdir)) == 0;

    using PFN_D3DCompile_ = HRESULT (WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR,
                                               UINT, UINT, ID3DBlob **, ID3DBlob **);
    auto compile = reinterpret_cast<PFN_D3DCompile_>(GetProcAddress(m, "D3DCompile"));
    if (compile == nullptr) return;

    static const char kProbe[] =
        "RWTexture2D<float4> o : register(u0);\n"
        "[numthreads(8,8,1)] void cs(uint3 t : SV_DispatchThreadID) { o[t.xy] = 0; }\n";
    ID3DBlob *code = nullptr, *err = nullptr;
    const HRESULT hr = compile(kProbe, sizeof(kProbe) - 1, "sm51probe", nullptr, nullptr, "cs", "cs_5_1", 0, 0, &code, &err);
    const bool ok = SUCCEEDED(hr) && code != nullptr;
    if (code != nullptr) code->Release();
    if (err != nullptr) err->Release();

    if (ok)
    {
        if (!from_system) Log("[host] d3dcompiler_47.dll: %s (not System32, but it accepts cs_5_1 -- fine)", path);
        return;
    }
    Log("[host] ###############################################################");
    Log("[host] %s is TOO OLD for Shader Model 5.1 (cs_5_1 rejected, hr=0x%08X).", path, hr);
    Log("[host] The DLSS 5 add-on compiles its neural pass as cs_5_1, so NEURAL RENDERING WILL DO NOTHING");
    Log("[host] while everything else looks healthy. host64\\ReShade.log will show:");
    Log("[host]     error X3506: unrecognized compiler target 'cs_5_1'");
    Log("[host] Fix: %s", from_system ? "unexpectedly this is the System32 copy -- update Windows."
                                      : "delete or rename that file; System32's current copy is then used.");
    Log("[host] ###############################################################");
}

static void DetectToolkitAddon()
{
    char dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char *s = strrchr(dir, '\\')) *(s + 1) = '\0';
    sprintf_s(path, "%salexs-toolkit.addon64", dir);

    // It advertises itself in its exported NAME string, the same way this feeder does.
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        Log("[host] Alex's Toolkit: not present -- DLSS 5 runs a single neural pass");
        return;
    }
    char ver[64] = "?";
    const DWORD size = GetFileSize(f, nullptr);
    DWORD got = 0;
    char *buf = (size > 0 && size < 16u * 1024 * 1024) ? static_cast<char *>(malloc(size)) : nullptr;
    if (buf != nullptr && ReadFile(f, buf, size, &got, nullptr) && got == size)
    {
        static const char kMark[] = "Alex's Toolkit ";
        const DWORD mlen = sizeof(kMark) - 1;
        for (DWORD i = 0; i + mlen < size; ++i)
            if (memcmp(buf + i, kMark, mlen) == 0)
            {
                const char *v = buf + i + mlen;
                size_t n = 0;
                while (n + 1 < sizeof(ver) && i + mlen + n < size && v[n] >= 32 && v[n] < 127 && v[n] != '%')
                    ++n;
                if (n > 0) { memcpy(ver, v, n); ver[n] = '\0'; }
                break;
            }
    }
    free(buf);
    CloseHandle(f);

    // Its live settings. It re-reads this file itself while the game runs, so this is
    // only what it will start with.
    sprintf_s(path, "%salexs-toolkit.cfg", dir);
    int enabled = 1, two_pass = 0, three_pass = 0;
    FILE *cf = nullptr;
    if (fopen_s(&cf, path, "rb") == 0 && cf != nullptr)
    {
        char text[2048];
        const size_t tn = fread(text, 1, sizeof(text) - 1, cf);
        text[tn] = '\0';
        fclose(cf);
        // Plain "key=value" lines with no [section] header: GetPrivateProfileInt cannot read it.
        for (const char *p = text; *p != '\0'; ++p)
        {
            if (p != text && p[-1] != '\n' && p[-1] != '\r') continue;
            if (_strnicmp(p, "enabled=", 8) == 0)          enabled    = atoi(p + 8);
            else if (_strnicmp(p, "two_pass=", 9) == 0)    two_pass   = atoi(p + 9);
            else if (_strnicmp(p, "three_pass=", 11) == 0) three_pass = atoi(p + 11);
        }
    }
    const int passes = (enabled && two_pass) ? (three_pass ? 3 : 2) : 0;

    if (passes >= 2)
        Log("[host] Alex's Toolkit %s: %d-pass DLSS 5 cascade active -- roughly %dx the temporal history "
            "(expect smearing behind fast motion and a slow settle after a camera cut); its own log is "
            "alexs-toolkit.log next to this exe", ver, passes, passes);
    else
        Log("[host] Alex's Toolkit %s: present but the cascade is off (%s) -- DLSS 5 runs a single pass",
            ver, enabled ? "two_pass=0" : "enabled=0");
    Log("[host] Alex's Toolkit config: enabled=%d two_pass=%d three_pass=%d (it re-reads that file live)",
        enabled, two_pass, three_pass);
}

static void Log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    FILE *f = nullptr;
    if (fopen_s(&f, g_log_path, "a") == 0 && f != nullptr)
    {
        fprintf(f, "%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
        fclose(f);
    }
}

static const char *NgxResultName(NVSDK_NGX_Result r)
{
    switch (static_cast<unsigned>(r))
    {
    case 0x1:        return "Success";
    case 0xBAD00005: return "InvalidParameter";
    case 0xBAD00007: return "NotInitialized";
    case 0xBAD00008: return "UnsupportedInputFormat";
    case 0xBAD0000A: return "MissingInput";
    case 0xBAD0000B: return "UnableToInitializeFeature";
    case 0xBAD0000D: return "OutOfGPUMemory";
    case 0xBAD0000E: return "UnsupportedFormat";
    default:         return "?";
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Host
{
    HWND                       hwnd;
    IDXGISwapChain1           *swap;
    ID3D12Device              *dev;
    ID3D12CommandQueue        *queue;      // NGX work
    ID3D12CommandQueue        *pump_queue; // owns the dummy swapchain
    ID3D12GraphicsCommandList *list;
    static const int           kFrames = 3;
    ID3D12CommandAllocator    *alloc[kFrames];
    UINT64                     alloc_fence[kFrames];
    int                        frame_slot;
    ID3D12Fence               *fence;      // internal (allocator ring)
    HANDLE                     fence_event;
    UINT64                     fence_value;

    // cross-process
    ID3D12Fence *fence_in;   // game signals, host waits
    ID3D12Fence *fence_out;  // host signals, game waits

    bool                 ngx_inited;
    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle    *feature;

    ID3D12Resource *tex[FEED_SLOTS];
    UINT            width, height;
    DXGI_FORMAT     color_fmt, output_fmt;
};

static Host h;

// ---------------------------------------------------------------------------
// Command submission (allocator ring), same shape as the add-on
// ---------------------------------------------------------------------------

static bool BeginCommands()
{
    const int slot = h.frame_slot;
    const UINT64 retire = h.alloc_fence[slot];
    if (retire != 0 && h.fence->GetCompletedValue() < retire)
    {
        // fence_event is auto-reset and shared with WaitFenceValue, and a timed-out wait
        // leaves its registration armed -- so it can be signalled later by a completion
        // nobody is waiting for. Clear it first, then confirm the fence really passed
        // 'retire': resetting an allocator the GPU still reads from is far worse than
        // dropping a frame.
        ResetEvent(h.fence_event);
        h.fence->SetEventOnCompletion(retire, h.fence_event);
        if (WaitForSingleObject(h.fence_event, 2000) != WAIT_OBJECT_0 ||
            h.fence->GetCompletedValue() < retire)
        { Log("[host] GPU did not retire allocator slot %d", slot); return false; }
    }
    if (FAILED(h.alloc[slot]->Reset())) return false;
    return SUCCEEDED(h.list->Reset(h.alloc[slot], nullptr));
}

static UINT64 EndCommands()
{
    h.list->Close();
    ID3D12CommandList *lists[] = { h.list };
    h.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++h.fence_value;
    h.queue->Signal(h.fence, v);
    h.alloc_fence[h.frame_slot] = v;
    h.frame_slot = (h.frame_slot + 1) % Host::kFrames;
    return v;
}

static bool WaitFenceValue(ID3D12Fence *f, UINT64 v, DWORD ms)
{
    if (f->GetCompletedValue() >= v) return true;
    ResetEvent(h.fence_event);   // drop any signal left armed by an earlier timed-out wait
    f->SetEventOnCompletion(v, h.fence_event);
    return WaitForSingleObject(h.fence_event, ms) == WAIT_OBJECT_0 && f->GetCompletedValue() >= v;
}

static void CloseListGuarded()
{
    __try { h.list->Close(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void AbortCommands()   // never execute a list NGX crashed in
{
    if (h.list == nullptr) return;
    CloseListGuarded();
    h.list->Release();
    h.list = nullptr;
    if (SUCCEEDED(h.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h.alloc[h.frame_slot], nullptr,
                                           __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&h.list))))
        h.list->Close();
}

static NVSDK_NGX_Result SafeCreateDLSS(NVSDK_NGX_DLSS_Create_Params *cp, DWORD *code)
{
    *code = 0;
    __try { return NGX_D3D12_CREATE_DLSS_EXT(h.list, 1, 1, &h.feature, h.params, cp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return static_cast<NVSDK_NGX_Result>(0x7FFFFFFF); }
}

static NVSDK_NGX_Result SafeEvaluateDLSS(NVSDK_NGX_D3D12_DLSS_Eval_Params *ep, DWORD *code)
{
    *code = 0;
    __try { return NGX_D3D12_EVALUATE_DLSS_EXT(h.list, h.feature, h.params, ep); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return static_cast<NVSDK_NGX_Result>(0x7FFFFFFF); }
}

static void SafeReleaseFeature(NVSDK_NGX_Handle *f)
{
    if (f == nullptr) return;
    __try { NVSDK_NGX_D3D12_ReleaseFeature(f); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("[host] ReleaseFeature raised 0x%08X (ignored)", GetExceptionCode()); }
}

// ---------------------------------------------------------------------------
// The disguise: hidden window + minimal D3D12 swapchain so ReShade x64 loads
// and the DLSS 5 add-on arms itself, exactly as in a real D3D12 game.
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_CLOSE) { ShowWindow(w, SW_HIDE); return 0; }   // closing only hides; the feed lives on
    return DefWindowProcW(w, m, wp, lp);
}

// --- banner: "32-bit DLSS 5 Feeder" rendered once with GDI, copied into every frame ---

static ID3D12Resource             *g_banner;
static IDXGISwapChain3            *g_swap3;
static ID3D12CommandAllocator     *g_pump_alloc;
static ID3D12GraphicsCommandList  *g_pump_list;
static ID3D12Fence                *g_pump_fence;
static UINT64                      g_pump_val;

static bool BeginCommands();
static UINT64 EndCommands();
static bool WaitFenceValue(ID3D12Fence *f, UINT64 v, DWORD ms);

static void InitBanner()
{
    const int W = 960, H = 540;

    // 1. Render the text with GDI into a 32-bit DIB.
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = -H;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dc == nullptr || bmp == nullptr || bits == nullptr) return;
    HGDIOBJ old_bmp = SelectObject(dc, bmp);

    RECT full = { 0, 0, W, H };
    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 22));
    FillRect(dc, &full, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    HFONT fnt_big   = CreateFontW(64, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT fnt_small = CreateFontW(26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, fnt_big);
    SetTextColor(dc, RGB(118, 185, 0));
    RECT r1 = { 0, 150, W, 240 };
    DrawTextW(dc, L"32-bit DLSS 5 Feeder", -1, &r1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, fnt_small);
    SetTextColor(dc, RGB(200, 200, 205));
    RECT r2 = { 0, 260, W, 300 };
    DrawTextW(dc, L"DLSS 5 neural rendering runs here for your 32-bit game.", -1, &r2,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT r3 = { 0, 305, W, 345 };
    DrawTextW(dc, L"Press  Home  in this window to tune it  \x2022  closing only hides the window", -1, &r3,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, old_font);
    DeleteObject(fnt_big);
    DeleteObject(fnt_small);
    GdiFlush();

    // 2. Upload it (BGRA -> RGBA) and keep it as a copy source.
    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    const UINT pitch = (W * 4 + 255) & ~255u;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = static_cast<UINT64>(pitch) * H;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *staging = nullptr;
    D3D12_HEAP_PROPERTIES def = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = W;
    td.Height           = H;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(h.dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&staging))) ||
        FAILED(h.dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_banner))))
    { SelectObject(dc, old_bmp); DeleteObject(bmp); DeleteDC(dc); return; }

    BYTE *dst = nullptr;
    staging->Map(0, nullptr, reinterpret_cast<void **>(&dst));
    const BYTE *srcp = static_cast<const BYTE *>(bits);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const BYTE *p = srcp + (static_cast<size_t>(y) * W + x) * 4;   // GDI: BGRA
            BYTE *q = dst + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4;
            q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = 0xFF;
        }
    staging->Unmap(0, nullptr);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);

    if (BeginCommands())
    {
        D3D12_TEXTURE_COPY_LOCATION src = {}, dcl = {};
        src.pResource = staging;
        src.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width    = W;
        src.PlacedFootprint.Footprint.Height   = H;
        src.PlacedFootprint.Footprint.Depth    = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        dcl.pResource = g_banner;
        dcl.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        h.list->CopyTextureRegion(&dcl, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = g_banner;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        h.list->ResourceBarrier(1, &b);
        const UINT64 v = EndCommands();
        WaitFenceValue(h.fence, v, 2000);
    }
    staging->Release();

    // 3. A tiny allocator/list/fence pair on the pump queue for the per-frame copy.
    h.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                  reinterpret_cast<void **>(&g_pump_alloc));
    if (g_pump_alloc != nullptr)
        h.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_pump_alloc, nullptr,
                                 __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g_pump_list));
    if (g_pump_list != nullptr) g_pump_list->Close();
    h.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g_pump_fence));
    h.swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&g_swap3));
    Log("[host] banner ready");
}

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice_)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1_)(REFIID, void **);

// The message drain always runs -- it is what keeps the window responsive. When idle
// (no frames arriving), the banner copy and the Present behind it are throttled to
// 30 Hz; what made the old per-frame call expensive was the CPU wait for our own
// banner copy, and that is gone either way (the retire check below). The per-EVALUATE
// call stays force=true: the DLSS 5 add-on rides this swapchain, and its engine keys
// per-frame state to Present (the v4.7 banner says "workset pool") -- letting several
// evaluates land between presents made consecutive evaluates share state and the game
// flicker. One cheap Present per evaluate keeps its world consistent.
static void PumpPresent(bool force = false)
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (h.swap == nullptr) return;

    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (!force && now - last < 33) return;
    last = now;

    // Paint the banner into the backbuffer (ReShade's overlay composites on top at Present).
    if (g_banner != nullptr && g_pump_list != nullptr && g_swap3 != nullptr)
    {
        ID3D12Resource *bb = nullptr;
        if (SUCCEEDED(g_swap3->GetBuffer(g_swap3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource),
                                         reinterpret_cast<void **>(&bb))) && bb != nullptr)
        {
            // Resetting an allocator the GPU is still reading is the hazard the old
            // blocking wait guarded against; skip this tick rather than wait for it.
            if (g_pump_fence->GetCompletedValue() >= g_pump_val &&
                SUCCEEDED(g_pump_alloc->Reset()) && SUCCEEDED(g_pump_list->Reset(g_pump_alloc, nullptr)))
            {
                D3D12_RESOURCE_BARRIER b = {};
                b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = bb;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                g_pump_list->ResourceBarrier(1, &b);
                g_pump_list->CopyResource(bb, g_banner);
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
                g_pump_list->ResourceBarrier(1, &b);
                g_pump_list->Close();
                ID3D12CommandList *lists[] = { g_pump_list };
                h.pump_queue->ExecuteCommandLists(1, lists);
                h.pump_queue->Signal(g_pump_fence, ++g_pump_val);
            }
            bb->Release();
        }
    }
    h.swap->Present(0, 0);
}

static bool InitDisguise()
{
    // ReShade first: the app-directory dxgi.dll IS ReShade x64. Loading it before
    // d3d12.dll means every later D3D12/DXGI entry point goes through its hooks --
    // the same order a real game gets, and what lets the DLSS 5 add-on see us.
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create_device  = d3d12 ? reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    auto create_factory = dxgi ? reinterpret_cast<PFN_CreateDXGIFactory1_>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (create_device == nullptr || create_factory == nullptr) { Log("[host] dxgi/d3d12 exports missing"); return false; }

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(dxgi, exe, MAX_PATH);
    Log("[host] dxgi.dll: %ls", exe);

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"dlss5feedhost";
    RegisterClassW(&wc);
    h.hwnd = CreateWindowExW(0, wc.lpszClassName,
                             L"DLSS 5 Feed host - press Home HERE to tune DLSS 5 neural rendering",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 540,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (h.hwnd == nullptr) { Log("[host] window creation failed"); return false; }
    if (g_show_window) ShowWindow(h.hwnd, SW_SHOWNOACTIVATE);   // never steal the game's focus

    HRESULT hr = create_device(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                               reinterpret_cast<void **>(&h.dev));
    if (FAILED(hr)) { Log("[host] D3D12CreateDevice failed 0x%08X", hr); return false; }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    h.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&h.pump_queue));
    h.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&h.queue));
    if (h.pump_queue == nullptr || h.queue == nullptr) { Log("[host] queue creation failed"); return false; }

    IDXGIFactory2 *factory = nullptr;
    hr = create_factory(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory));
    if (FAILED(hr)) { Log("[host] CreateDXGIFactory1 failed 0x%08X", hr); return false; }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width            = 960;
    sd.Height           = 540;
    sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(h.pump_queue, h.hwnd, &sd, nullptr, nullptr, &h.swap);
    // By default DXGI watches this window and may act on window/foreground changes -- which,
    // for a helper spawned behind a fullscreen game, can pull the game out of focus. We only
    // ever use this swapchain to keep ReShade pumping, so tell DXGI to keep its hands off.
    if (SUCCEEDED(hr))
        factory->MakeWindowAssociation(h.hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    if (FAILED(hr)) { Log("[host] CreateSwapChainForHwnd failed 0x%08X", hr); return false; }
    // Park it at the bottom of the Z-order without activating, so a respawn never surfaces
    // over the game. The user can still raise it from the taskbar to reach the add-on panel.
    if (g_show_window)
        SetWindowPos(h.hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Log("[host] disguise up: hidden window + D3D12 swapchain (ReShade should be attached now)");

    for (int i = 0; i < 60; ++i) PumpPresent(true);   // let ReShade + the DLSS 5 add-on settle

    // Ring + internal fence for our own submissions.
    for (int i = 0; i < Host::kFrames; ++i)
        h.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                      reinterpret_cast<void **>(&h.alloc[i]));
    if (h.alloc[0] != nullptr)
        h.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h.alloc[0], nullptr,
                                 __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&h.list));
    if (h.list != nullptr) h.list->Close();
    h.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&h.fence));
    h.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (h.list == nullptr || h.fence == nullptr) { Log("[host] list/fence creation failed"); return false; }

    if (g_show_window) InitBanner();
    return true;
}

static bool InitNgx()
{
    wchar_t data_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, data_path, MAX_PATH);
    if (wchar_t *s = wcsrchr(data_path, L'\\')) *(s + 1) = L'\0';

    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, data_path, h.dev, nullptr, NVSDK_NGX_Version_API);
    Log("[host] NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", r, NgxResultName(r));
    if (NVSDK_NGX_FAILED(r))
    {
        r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                "1.0", data_path, h.dev, nullptr, NVSDK_NGX_Version_API);
        Log("[host] Init_with_ProjectID -> 0x%08X (%s)", r, NgxResultName(r));
    }
    if (NVSDK_NGX_FAILED(r)) return false;
    h.ngx_inited = true;

    NVSDK_NGX_Parameter *caps = nullptr;
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_SUCCEED(r) && caps != nullptr)
    {
        int avail = 0;
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
        Log("[host] SuperSampling.Available=%d", avail);
        if (!avail) return false;
    }
    r = NVSDK_NGX_D3D12_AllocateParameters(&h.params);
    if (NVSDK_NGX_FAILED(r) || h.params == nullptr) { Log("[host] AllocateParameters failed 0x%08X", r); return false; }
    return true;
}

static bool CreateFeature(UINT w, UINT h_, int flags, NVSDK_NGX_Result *out_r)
{
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth            = w;
    cp.Feature.InHeight           = h_;
    cp.Feature.InTargetWidth      = w;
    cp.Feature.InTargetHeight     = h_;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags       = flags;
    cp.InEnableOutputSubrects     = false;

    if (!BeginCommands()) return false;
    DWORD ccode = 0;
    NVSDK_NGX_Result rf = SafeCreateDLSS(&cp, &ccode);
    if (out_r != nullptr) *out_r = rf;
    if (ccode != 0)
    {
        AbortCommands();
        // NGX may have partially written *OutHandle before the fault; never trust it.
        h.feature = nullptr;
        Log("[host] CreateFeature raised 0x%08X (caught; nothing submitted)", ccode);
        return false;
    }
    const UINT64 v = EndCommands();
    if (!WaitFenceValue(h.fence, v, 4000)) { Log("[host] feature create did not complete"); return false; }
    if (NVSDK_NGX_FAILED(rf) || h.feature == nullptr)
    { Log("[host] CreateFeature failed 0x%08X (%s)", rf, NgxResultName(rf)); h.feature = nullptr; return false; }
    Log("[host] feature ready: %ux%u DLAA flags=%d", w, h_, flags);
    return true;
}

// A crashed CreateFeature can leave NGX's own internal state broken (seen in BioShock
// Remastered: the add-on faulted once during a resolution/HDR change, and every following
// create failed too, with the SEH catching a different exception each time -- NGX was
// never going to recover on its own). Reset NGX itself as a last resort so the feed can
// come back without the user having to restart the game.
static bool ReinitNgx()
{
    Log("[host] NGX looks corrupted after repeated failures; reinitializing");
    if (h.params != nullptr) { NVSDK_NGX_D3D12_DestroyParameters(h.params); h.params = nullptr; }
    if (h.ngx_inited) { NVSDK_NGX_D3D12_Shutdown1(h.dev); h.ngx_inited = false; }
    h.feature = nullptr;
    return InitNgx();
}

static bool Evaluate(ID3D12Resource *color, ID3D12Resource *output, ID3D12Resource *depth, ID3D12Resource *mv,
                     UINT w, UINT h_, int reset, float mvsx, float mvsy)
{
    if (!BeginCommands()) return false;

    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor  = color;
    ep.Feature.pInOutput = output;
    ep.pInDepth          = depth;
    ep.pInMotionVectors  = mv;
    ep.InRenderSubrectDimensions.Width  = w;
    ep.InRenderSubrectDimensions.Height = h_;
    ep.InReset           = reset;
    ep.InMVScaleX        = mvsx;
    ep.InMVScaleY        = mvsy;
    ep.InPreExposure     = 1.0f;
    ep.InExposureScale   = 1.0f;

    DWORD ecode = 0;
    NVSDK_NGX_Result re = SafeEvaluateDLSS(&ep, &ecode);
    if (ecode != 0) { AbortCommands(); Log("[host] evaluate raised 0x%08X (caught; nothing submitted)", ecode); return false; }
    EndCommands();
    if (NVSDK_NGX_FAILED(re)) { Log("[host] evaluate failed 0x%08X (%s)", re, NgxResultName(re)); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// --test: prove the whole stack with no game attached
// ---------------------------------------------------------------------------

static ID3D12Resource *MakeTex(UINT w, UINT h_, DXGI_FORMAT fmt, bool uav)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h_;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *t = nullptr;
    h.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                   __uuidof(ID3D12Resource), reinterpret_cast<void **>(&t));
    return t;
}

// NGX writes the Output through a UAV, and typed UAV *stores* to B8G8R8A8_UNORM are
// an optional D3D12 feature -- only a device can answer whether this GPU has it, and
// for a host-creating client (OpenGL, Vulkan) this host owns the only one. Where the
// support is missing, fall back to RGBA and let the game's copy home convert: wrong
// channel order beats a feature that cannot be created at all.
static DXGI_FORMAT ResolveOutputFormatHost(DXGI_FORMAT want)
{
    if (want != DXGI_FORMAT_B8G8R8A8_UNORM || h.dev == nullptr) return want;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
    fs.Format = want;
    if (SUCCEEDED(h.dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
        (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0)
        return want;
    Log("[host] B8G8R8A8_UNORM has no typed UAV store on this device; the output stays R8G8B8A8_UNORM "
        "(the game's copy home will convert, so expect the washed-out image of issue #11)");
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

// The shared half of MakeTex, for clients whose API cannot export importable memory
// (OpenGL: memory objects are import-only; Vulkan: D3D12 cannot open what it exports).
// This host creates the texture and hands out an NT handle plus the allocation size
// the import needs. ALLOW_SIMULTANEOUS_ACCESS is what pairs with GL_LAYOUT_GENERAL_EXT
// / VK_IMAGE_LAYOUT_GENERAL on the other side; SHARED puts it in a heap the other
// process can open. This is also a better resource than the D3D11 route's: it is born
// with exactly the flags NGX wants, with none of MakeSharedPair's UAV-flag uncertainty.
static ID3D12Resource *MakeSharedTexHost(UINT w, UINT h_, DXGI_FORMAT fmt, bool uav,
                                         HANDLE *out_handle, uint64_t *out_size)
{
    *out_handle = nullptr;
    *out_size   = 0;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h_;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                          (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource *t = nullptr;
    HRESULT hr = h.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&t));
    if (SUCCEEDED(hr)) hr = h.dev->CreateSharedHandle(t, nullptr, GENERIC_ALL, nullptr, out_handle);
    if (FAILED(hr))
    {
        Log("[host] shared texture %ux%u fmt=%u failed 0x%08X", w, h_, fmt, hr);
        if (t != nullptr) t->Release();
        return nullptr;
    }
    *out_size = h.dev->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes;
    return t;
}

static int RunTest()
{
    const UINT W = 640, H = 360;
    Log("[host] --test: %ux%u synthetic DLAA", W, H);

    ID3D12Resource *color  = MakeTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    ID3D12Resource *output = MakeTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    ID3D12Resource *depth  = MakeTex(W, H, DXGI_FORMAT_R32_FLOAT, false);
    ID3D12Resource *mv     = MakeTex(W, H, DXGI_FORMAT_R16G16_FLOAT, false);
    if (!color || !output || !depth || !mv) { Log("[host] test texture creation failed"); return 1; }

    // Give the DLSS 5 add-on its hook-arming time, with the swapchain pumping.
    for (int i = 0; i < 120; ++i) { PumpPresent(true); Sleep(8); }

    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    NVSDK_NGX_Result rf = NVSDK_NGX_Result_Fail;
    if (!CreateFeature(W, H, flags, &rf)) return 1;

    int good = 0;
    for (int i = 0; i < 300; ++i)
    {
        PumpPresent(true);
        if (Evaluate(color, output, depth, mv, W, H, i == 0 ? 1 : 0, 1.0f, 1.0f)) ++good;
        else break;
        if (i == 180)   // the warm-up re-create, same medicine as in-game
        {
            Log("[host] warm-up: re-creating the feature once");
            NVSDK_NGX_Handle *old = h.feature;
            h.feature = nullptr;
            if (!CreateFeature(W, H, flags, &rf)) { h.feature = old; Log("[host] keeping the previous feature"); }
            else SafeReleaseFeature(old);
        }
    }
    Log("[host] --test finished: %d/300 evaluates succeeded", good);
    Log("[host] check the host's ReShade.log for 'feature 18 created' / 'evaluation succeeded'");
    return good >= 250 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Serve mode: the real pipe server for a 32-bit game
// ---------------------------------------------------------------------------

// The pipe is opened FILE_FLAG_OVERLAPPED (the serve loop needs a pended read it can
// wait on alongside the window's message queue), so every synchronous transfer has to
// carry an OVERLAPPED and block on it here. Byte-mode pipes may satisfy a read short,
// hence the loop; the game's end stays an ordinary blocking handle and is unaffected.
static bool TransferFull(HANDLE pipe, void *buf, DWORD len, bool write)
{
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ev == nullptr) return false;
    BYTE *p = static_cast<BYTE *>(buf);
    bool ok = true;
    for (DWORD left = len; left > 0; )
    {
        OVERLAPPED ov = {};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD moved = 0;
        const BOOL started = write ? WriteFile(pipe, p, left, nullptr, &ov)
                                   : ReadFile(pipe, p, left, nullptr, &ov);
        if (!started && GetLastError() != ERROR_IO_PENDING) { ok = false; break; }
        if (!GetOverlappedResult(pipe, &ov, &moved, TRUE) || moved == 0) { ok = false; break; }
        p    += moved;
        left -= moved;
    }
    CloseHandle(ev);
    return ok;
}

static bool ReadFull(HANDLE pipe, void *buf, DWORD len) { return TransferFull(pipe, buf, len, false); }
static bool WriteFull(HANDLE pipe, const void *buf, DWORD len)
{ return TransferFull(pipe, const_cast<void *>(buf), len, true); }

static int Serve(DWORD game_pid)
{
    char name[128];
    sprintf_s(name, FEED_PIPE_FMT, static_cast<unsigned long>(game_pid));
    HANDLE pipe = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1, 1024, 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { Log("[host] CreateNamedPipe failed %lu", GetLastError()); return 1; }
    Log("[host] serving on %s", name);
    {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OVERLAPPED ov = {};
        ov.hEvent = ev;
        DWORD ignored = 0;
        const BOOL connected = ConnectNamedPipe(pipe, &ov);
        const DWORD err = GetLastError();
        if (!connected && err == ERROR_IO_PENDING) GetOverlappedResult(pipe, &ov, &ignored, TRUE);
        else if (!connected && err != ERROR_PIPE_CONNECTED)
        { Log("[host] ConnectNamedPipe failed %lu", err); CloseHandle(ev); return 1; }
        CloseHandle(ev);
    }

    // A v1 client's hello is three uint32s; v2 appends client_kind. Read the common
    // prefix, then only what this client's version actually sent -- reading the full
    // struct from a v1 client would block on bytes it never writes.
    FeedHello hello = {};
    if (!ReadFull(pipe, &hello, FEED_HELLO_V1_SIZE) || hello.magic != FEED_IPC_MAGIC)
    { Log("[host] bad hello"); return 1; }
    if (hello.version >= 2 && !ReadFull(pipe, &hello.client_kind, sizeof(hello.client_kind)))
    { Log("[host] truncated hello"); return 1; }
    if (hello.version >= 4 && !ReadFull(pipe, &hello.self_process, sizeof(hello.self_process)))
    { Log("[host] truncated hello (v4 self_process)"); return 1; }

    // Answer first, THEN bail on a mismatch: the ack carries our version, so the
    // add-on can name the problem in the game's log instead of just timing out.
    // Nothing else may be read -- FeedBuild and FeedBuildAck changed size between
    // versions, so a mismatched pair would desync the pipe on the very next message.
    FeedHelloAck ack = { FEED_IPC_MAGIC, FEED_IPC_VERSION };
    WriteFull(pipe, &ack, sizeof(ack));
    if (hello.version != FEED_IPC_VERSION)
    {
        Log("[host] the game add-on speaks protocol v%u, this host v%u -- the two halves are from "
            "different releases; reinstall dlss5-feed.addon32 and host64\\ together",
            hello.version, FEED_IPC_VERSION);
        return 1;
    }

    const bool host_creates = FeedHostCreatesTextures(hello.client_kind);
    const char *client_name = hello.client_kind == FEED_CLIENT_GL     ? "OpenGL"
                            : hello.client_kind == FEED_CLIENT_VULKAN ? "Vulkan"
                            : "D3D11";
    Log("[host] game pid %u connected (protocol v%u, %s client -- %s creates the shared textures)",
        hello.pid, hello.version, client_name, host_creates ? "this host" : "the game");

    // Prefer the handle the game duplicated in (v4+): OpenProcess is denied with error 5
    // by DACL-protected game processes (anti-cheat/DRM -- the vanilla-WoW report), and
    // the handed-over handle needs no access check at all.
    HANDLE hgame = nullptr;
    if (hello.version >= 4 && hello.self_process != 0)
    {
        HANDLE h2 = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(hello.self_process));
        if (GetProcessId(h2) == hello.pid)
            hgame = h2;
        else
            Log("[host] the handed-over process handle does not name pid %u; falling back to OpenProcess", hello.pid);
    }
    if (hgame == nullptr)
    {
        hgame = OpenProcess(PROCESS_DUP_HANDLE, FALSE, hello.pid);
        if (hgame == nullptr)
        {
            Log("[host] OpenProcess failed %lu -- the game's process denies handle access "
                "(a protective DACL from anti-cheat/DRM, or an elevation mismatch), and the "
                "add-on did not hand a handle over", GetLastError());
            return 1;
        }
    }

    // Shared fences live for the whole session.
    HANDLE hin = nullptr, hout = nullptr;
    h.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&h.fence_in));
    h.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&h.fence_out));
    if (h.fence_in == nullptr || h.fence_out == nullptr ||
        FAILED(h.dev->CreateSharedHandle(h.fence_in, nullptr, GENERIC_ALL, nullptr, &hin)) ||
        FAILED(h.dev->CreateSharedHandle(h.fence_out, nullptr, GENERIC_ALL, nullptr, &hout)))
    { Log("[host] shared fence creation failed"); return 1; }

    HANDLE game_in = nullptr, game_out = nullptr;
    DuplicateHandle(GetCurrentProcess(), hin, hgame, &game_in, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(GetCurrentProcess(), hout, hgame, &game_out, 0, FALSE, DUPLICATE_SAME_ACCESS);

    int flags_active = 0;
    bool transport_only = false;
    float mvsx = 1.0f, mvsy = 1.0f;
    // The DLSS 5 add-on arms its NGX hooks ~150 ms after NGX init; the first create must
    // not race that (a 15 ms miss latched STANDBY in Blacklist), so hold it briefly.
    UINT64 hold_until = GetTickCount64() + 800;
    UINT64 evaluated  = 0;
    bool   warm_done  = g_renodx_lazy;   // v45+ adopts missed creates on its own
    int    build_fails = 0;

    // The tag read stays pended across pump ticks: a plain blocking ReadFile starves
    // the message pump (and Present) whenever the game stops feeding frames -- paused,
    // loading, a menu -- and Windows shows the host window as "Not Responding". Polling
    // it with a Sleep instead cost the game a whole timer quantum of latency on EVERY
    // frame (issue #15). Waiting on the read's own event together with the message queue
    // gives both: an instant wake on the next tag byte, and a live window in between.
    HANDLE     ev_tag  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED ov_tag  = {};
    bool       pending = false;
    BYTE       tag     = 0;
    if (ev_tag == nullptr) { Log("[host] tag event creation failed %lu", GetLastError()); return 1; }

    for (;;)
    {
        // Build (big) and FrameMsg (small) share no prefix, so the client precedes
        // every message with a 1-byte tag and we dispatch on that.
        bool tag_read = false;
        for (;;)
        {
            if (!pending)
            {
                ov_tag = {};
                ov_tag.hEvent = ev_tag;
                ResetEvent(ev_tag);
                if (!ReadFile(pipe, &tag, 1, nullptr, &ov_tag) && GetLastError() != ERROR_IO_PENDING)
                    break;                                   // pipe broken or closed
                pending = true;
            }
            const DWORD r = MsgWaitForMultipleObjects(1, &ev_tag, FALSE, 100, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0 + 1 || r == WAIT_TIMEOUT) { PumpPresent(); continue; }
            if (r != WAIT_OBJECT_0) break;
            DWORD got = 0;
            if (!GetOverlappedResult(pipe, &ov_tag, &got, FALSE) || got != 1) { pending = false; break; }
            pending  = false;
            tag_read = true;
            break;
        }
        if (!tag_read) { Log("[host] pipe closed by the game"); break; }

        if (tag == 'B')
        {
            FeedBuild b = {};
            if (!ReadFull(pipe, &b, sizeof(b))) break;
            Log("[host] build: %ux%u color=%u output=%u hdr=%d inverted=%d", b.width, b.height,
                b.color_fmt, b.output_fmt, b.hdr, b.depth_inverted);

            // Tear down the old set.
            SafeReleaseFeature(h.feature);
            h.feature = nullptr;
            for (int i = 0; i < FEED_SLOTS; ++i)
                if (h.tex[i] != nullptr) { h.tex[i]->Release(); h.tex[i] = nullptr; }

            bool ok = true;
            uint64_t game_tex[FEED_SLOTS] = {}, tex_size[FEED_SLOTS] = {};
            // The Output format is the host's call whenever the host creates the
            // textures -- only this process has the D3D12 device that can be asked
            // about typed UAV stores. In transport mode there is no UAV at all and
            // the copy is Color -> Output on this side, so the two must stay equal.
            DXGI_FORMAT out_fmt = static_cast<DXGI_FORMAT>(b.output_fmt);
            if (host_creates && b.transport == 0) out_fmt = ResolveOutputFormatHost(out_fmt);
            if (host_creates)
            {
                // OpenGL / Vulkan client: WE create, and duplicate the handles into the
                // game, which imports them (PLAN-OPENGL §5 design A; PLAN-VULKAN32 §2).
                const DXGI_FORMAT fmt[FEED_SLOTS] = {
                    static_cast<DXGI_FORMAT>(b.color_fmt), out_fmt,
                    DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT };
                for (int i = 0; i < FEED_SLOTS && ok; ++i)
                {
                    HANDLE local = nullptr;
                    h.tex[i] = MakeSharedTexHost(b.width, b.height, fmt[i], i == FEED_OUTPUT, &local, &tex_size[i]);
                    if (h.tex[i] == nullptr) { ok = false; break; }
                    HANDLE remote = nullptr;
                    if (!DuplicateHandle(GetCurrentProcess(), local, hgame, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
                    { Log("[host] DuplicateHandle(tex %d into the game) failed %lu", i, GetLastError()); ok = false; }
                    CloseHandle(local);   // the duplicate stands on its own; the resource keeps the memory
                    game_tex[i] = reinterpret_cast<uint64_t>(remote);
                }
                if (ok)
                    Log("[host] created and handed over %d shared textures (%ux%u, color %s, output %s)",
                        FEED_SLOTS, b.width, b.height, FeedFmtName(static_cast<DXGI_FORMAT>(b.color_fmt)),
                        FeedFmtName(out_fmt));
            }
            else
            {
                // D3D11 client: open the game's textures (duplicate the handles out).
                for (int i = 0; i < FEED_SLOTS && ok; ++i)
                {
                    HANDLE local = nullptr;
                    if (!DuplicateHandle(hgame, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(b.tex[i])),
                                         GetCurrentProcess(), &local, 0, FALSE, DUPLICATE_SAME_ACCESS))
                    { Log("[host] DuplicateHandle(tex %d) failed %lu", i, GetLastError()); ok = false; break; }
                    HRESULT hr = h.dev->OpenSharedHandle(local, __uuidof(ID3D12Resource),
                                                         reinterpret_cast<void **>(&h.tex[i]));
                    CloseHandle(local);
                    if (FAILED(hr)) { Log("[host] OpenSharedHandle(tex %d) failed 0x%08X", i, hr); ok = false; }
                }
            }

            NVSDK_NGX_Result rf = NVSDK_NGX_Result_Fail;
            if (ok)
            {
                h.width = b.width; h.height = b.height;
                h.color_fmt  = static_cast<DXGI_FORMAT>(b.color_fmt);
                h.output_fmt = out_fmt;
                mvsx = b.mv_scale_x; mvsy = b.mv_scale_y;
                transport_only = b.transport != 0;
                flags_active = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                if (b.depth_inverted) flags_active |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
                if (b.hdr)            flags_active |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
                if (b.flags_override >= 0) flags_active = b.flags_override;

                if (transport_only)
                {
                    rf = static_cast<NVSDK_NGX_Result>(1);   // no NGX in the loop at all
                    Log("[host] transport-only mode: Color will be copied to Output, no evaluate");
                }
                else
                {
                    const UINT64 now = GetTickCount64();
                    if (now < hold_until) Sleep(static_cast<DWORD>(hold_until - now));  // hook-arming grace
                    ok = CreateFeature(b.width, b.height, flags_active, &rf);
                    hold_until = GetTickCount64() + 1000;   // next create not before +1 s

                    if (ok) build_fails = 0;
                    else if (++build_fails >= 2 && ReinitNgx())
                    {
                        Log("[host] retrying the create after an NGX reinit");
                        ok = CreateFeature(b.width, b.height, flags_active, &rf);
                        if (ok) build_fails = 0;
                    }
                }
            }

            evaluated = 0;
            warm_done = transport_only || g_renodx_lazy;   // no warm-up without NGX / with v45+

            FeedBuildAck back = {};
            back.ok         = ok ? 1 : 0;
            back.ngx_result = static_cast<uint32_t>(rf);
            back.fence_in   = reinterpret_cast<uint64_t>(game_in);
            back.fence_out  = reinterpret_cast<uint64_t>(game_out);
            back.output_fmt = static_cast<uint32_t>(out_fmt);
            for (int i = 0; i < FEED_SLOTS; ++i) { back.tex[i] = game_tex[i]; back.tex_size[i] = tex_size[i]; }
            WriteFull(pipe, &back, sizeof(back));
        }
        else if (tag == 'F')
        {
            FeedFrameMsg fm = {};
            if (!ReadFull(pipe, &fm, sizeof(fm))) break;
            if (h.feature == nullptr && !transport_only) { h.fence_out->Signal(fm.n); continue; }

            // Order the evaluate behind the game's input copies on the GPU timeline and
            // move on. This used to block the CPU on the same value first, which
            // serialized the two processes frame by frame for no ordering benefit
            // (issue #15). A game whose GPU never reaches fm.n now surfaces three frames
            // later as BeginCommands' allocator-retire timeout, which fails the evaluate
            // and CPU-signals fence_out below -- the never-hang guarantee is unchanged.
            h.queue->Wait(h.fence_in, fm.n);

            bool done = false;
            if (transport_only)
            {
                if (BeginCommands())
                {
                    // Deliberately copy only the LEFT half: a split screen in the game is
                    // unambiguous visual proof that the host's output reaches the screen.
                    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
                    src.pResource = h.tex[FEED_COLOR];
                    src.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.pResource = h.tex[FEED_OUTPUT];
                    dst.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    D3D12_BOX box = { 0, 0, 0, h.width / 2, h.height, 1 };
                    h.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                    EndCommands();
                    done = true;
                }
            }
            else
                done = Evaluate(h.tex[FEED_COLOR], h.tex[FEED_OUTPUT], h.tex[FEED_DEPTH], h.tex[FEED_MV],
                                h.width, h.height, fm.reset ? 1 : 0, mvsx, mvsy);

            if (done)
            {
                h.queue->Signal(h.fence_out, fm.n);
                // One warm-up re-create per build: the DLSS 5 add-on misses the very first
                // create (STANDBY latch) when its hooks armed a moment too late.
                if (!warm_done && ++evaluated >= 180)
                {
                    warm_done = true;
                    Log("[host] warm-up: re-creating the feature once");
                    WaitFenceValue(h.fence, h.fence_value, 2000);
                    NVSDK_NGX_Handle *old = h.feature;
                    h.feature = nullptr;
                    NVSDK_NGX_Result rr = NVSDK_NGX_Result_Fail;
                    if (CreateFeature(h.width, h.height, flags_active, &rr)) SafeReleaseFeature(old);
                    else { h.feature = old; Log("[host] keeping the previous feature"); }
                }
            }
            else
                h.fence_out->Signal(fm.n);     // CPU-signal so the game never hangs on us

            if (fm.n <= 3 || (fm.n % 1800) == 0)
                Log("[host] frame %llu evaluated", (unsigned long long)fm.n);
            PumpPresent(true);   // per evaluate, deliberately -- see PumpPresent
        }
        else
        {
            Log("[host] unknown tag 0x%02X", tag);
            break;
        }
    }

    // The game closed the pipe (a settings apply, a shutdown, or a crash). It queues a
    // GPU-side wait on fence_out for every frame message it writes, and a successful
    // pipe write does not mean we ever read it: exiting now could leave a wait that
    // nothing will ever satisfy, wedging the game's whole GPU queue -- Present
    // included -- until the driver TDRs (seen once as a system-wide freeze). Drain our
    // own queue, then release every wait the game could possibly hold.
    if (pending) { CancelIo(pipe); DWORD got = 0; GetOverlappedResult(pipe, &ov_tag, &got, TRUE); }
    CloseHandle(ev_tag);
    WaitFenceValue(h.fence, h.fence_value, 2000);
    if (h.fence_out != nullptr) h.fence_out->Signal(UINT64_MAX);
    Log("[host] pending game fence waits released; exiting");
    return 0;
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    GetModuleFileNameA(nullptr, g_log_path, MAX_PATH);
    if (char *s = strrchr(g_log_path, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - g_log_path), "dlss5-feed-host.log");
    { FILE *f = nullptr; if (fopen_s(&f, g_log_path, "w") == 0 && f) fclose(f); }

    Log("dlss5-feed-host64 (built %s %s)", __DATE__, __TIME__);

    // Every Sleep in this process is a frame-pacing decision: at the default 15.6 ms
    // timer tick a Sleep(1) lands at 15.6 ms, and the serve loop's poll alone used to
    // cap the game near 35 fps (issue #15). No timeEndPeriod -- this is a dedicated
    // helper, and process exit restores the resolution anyway.
    timeBeginPeriod(1);

    bool  test = false, hide = false;
    DWORD pid = 0;
    for (int i = 1; i < argc; ++i)
    {
        if      (strcmp(argv[i], "--test") == 0) test = true;
        else if (strcmp(argv[i], "--hide") == 0) hide = true;
        else pid = static_cast<DWORD>(strtoul(argv[i], nullptr, 10));
    }
    if (!test && pid == 0)
    {
        Log("usage: dlss5-feed-host64 --test | dlss5-feed-host64 <game pid> [--hide]");
        return 1;
    }
    g_show_window = !test && !hide;   // the visible window carries the DLSS 5 add-on's tuning panel

    DetectRenodxAddon();   // must run BEFORE ReShade loads, so an EnableHooks write is read
    DetectToolkitAddon();
    DetectStaleD3DCompiler();

    if (!InitDisguise()) return 1;
    if (!InitNgx()) { Log("[host] NGX unavailable"); return 1; }

    return test ? RunTest() : Serve(pid);
}
