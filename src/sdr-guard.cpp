// sdr-guard - ReShade add-on
//
// Gets HDR out of a host application that only ever presents 8-bit SDR, by
// widening its swapchain, adopting an HDR colour space on it, and stopping the
// app from undoing either. The actual SDR->HDR tone mapping is done by a normal
// ReShade effect (Pumbo's AdvancedAutoHDR.fx, or Lilium's lilium__map_SDR_into_HDR),
// which now has an HDR back buffer to write into.
//
// Written for AVerMedia Streaming Center, which presents B8G8R8A8_UNORM and
// re-asserts SDR on every swapchain it creates, within a millisecond --
//
//     CreateSwapChainForHwnd(...)                Format = B8G8R8A8_UNORM
//     IDXGISwapChain3::SetColorSpace1(ColorSpace = 0)      <- SDR, explicit
//     IDXGISwapChain::ResizeBuffers(NewFormat = 87)        <- 8-bit BGRA, explicit
//
// (ColorSpace 0 is DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, format 87 is
// DXGI_FORMAT_B8G8R8A8_UNORM.)
//
// Order is the thing that matters. An 8-bit swapchain rejects every HDR colour
// space, so CheckColorSpaceSupport returns 0 and SetColorSpace1 cannot succeed
// until the buffer has been widened. AutoHDR-ReShade sets the colour space first,
// watches its own check fail, resizes anyway, marks itself enabled and never
// retries -- which is why it leaves a 10-bit buffer still tagged SDR. Here the
// format is set at creation (create_swapchain), and the colour space only after
// the buffer exists at that format (init_swapchain).
//
// ReShade exposes no event for SetColorSpace1, so the app's attempts to undo the
// result are dropped by detouring two IDXGISwapChain3 vtable slots directly
// (SetColorSpace1 = 38, ResizeBuffers = 13).
//
// This is a fix for one badly behaved host, not a general-purpose tool: in a real
// game, suppressing SetColorSpace1(SDR) would break legitimate SDR paths. So it is
// gated on the executable name (sdr-guard.cfg, "process="), on surface size -- the
// host builds preview swapchains that are composited into the main window, and
// tone mapping those too would apply the curve twice -- and on an explicit mode.
//
// Modes, in sdr-guard.cfg. Walk up them; do not start at 3.
//
//   0  observe        log everything, change nothing
//   1  hold           drop the app's SDR re-stamp, but never upgrade anything
//   2  upgrade+probe  widen the back buffer and report what CheckColorSpaceSupport
//                     says about it, without adopting HDR
//   3  drive          widen, adopt the HDR colour space, and hold it
//
// Mode 2 is the one that answers whether this can work at all: if the buffer
// widens and the probe still says "not presentable", stop -- the display or the
// present path will not take it. Only then is mode 3 worth trying.
//
// sdr-guard.cfg is re-read on every swapchain creation. sdr-guard.log records
// every decision.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <reshade.hpp>

#define SDR_GUARD_VERSION "0.1.0"

extern "C" __declspec(dllexport) const char *NAME = "SDR Guard " SDR_GUARD_VERSION;
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Widens a host application's swapchain and adopts an HDR colour space on it, so a "
    "ReShade tone-mapping effect can output HDR from an SDR-only app -- and stops the app "
    "undoing it. Gated on executable name and surface size; observe-only until mode is "
    "raised in sdr-guard.cfg.";

// IDXGISwapChain3 vtable layout. IUnknown 0-2, IDXGIObject 3-6,
// IDXGIDeviceSubObject 7, IDXGISwapChain 8-17, IDXGISwapChain1 18-28,
// IDXGISwapChain2 29-35, IDXGISwapChain3 36-39.
static const int kSlotResizeBuffers = 13;
static const int kSlotSetColorSpace1 = 38;

typedef HRESULT(STDMETHODCALLTYPE *PFN_ResizeBuffers)(
    IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_SetColorSpace1)(
    IDXGISwapChain3 *, DXGI_COLOR_SPACE_TYPE);

static PFN_ResizeBuffers   g_real_resize = nullptr;
static PFN_SetColorSpace1  g_real_setcs  = nullptr;

static CRITICAL_SECTION g_cs;
static HMODULE          g_self;
static char             g_log_path[MAX_PATH];
static char             g_cfg_path[MAX_PATH];

// cfg
//   mode 0 = observe only          -- log everything, change nothing
//   mode 1 = hold                  -- drop the app's SDR re-stamp, but never upgrade
//   mode 2 = upgrade + probe       -- create the swapchain 10-bit/FP16 and report what
//                                     CheckColorSpaceSupport says, without adopting HDR
//   mode 3 = drive                 -- upgrade, adopt the HDR colour space, and hold it
static int  g_enabled   = 1;
static int  g_mode      = 0;
static int  g_log_calls = 1;
static int  g_hdr_kind  = 0;    // 0 = HDR10 (R10G10B10A2 + PQ), 1 = scRGB (FP16 + linear)
static int  g_min_w     = 1280; // ignore the small preview surfaces
static int  g_min_h     = 700;
static char g_process[128] = "StreamingCenter.exe";

static bool g_host_matches = false;

// Driver-side HDR (RTX HDR, and NvTrueHDR which only flips the driver profile
// that turns it on) is injected by the display driver, and can arrive well after
// the swapchain exists. Rescanning periodically is the only way to catch it.
static std::map<std::string, bool> g_seen_nv_modules;

// Per-swapchain state. The app recreates its swapchains constantly, so this is
// keyed on the object and cleaned up in destroy_swapchain.
struct SwapState
{
    DXGI_COLOR_SPACE_TYPE colour_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    DXGI_FORMAT           hdr_format   = DXGI_FORMAT_UNKNOWN;  // format seen when HDR was adopted
    bool                  hdr_seen     = false;
    DXGI_FORMAT           last_format  = DXGI_FORMAT_UNKNOWN;  // for change detection in Present
    unsigned              frames       = 0;
};
static std::map<IDXGISwapChain3 *, SwapState> g_state;

// Vtables are shared between swapchains from the same factory, so patch each
// distinct one once.
static std::map<void **, bool> g_patched;

// ---------------------------------------------------------------------------

static void Log(const char *fmt, ...)
{
    EnterCriticalSection(&g_cs);
    FILE *f = nullptr;
    if (fopen_s(&f, g_log_path, "a") == 0 && f)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_cs);
}

static bool IsSdrColourSpace(DXGI_COLOR_SPACE_TYPE cs)
{
    return cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

static bool IsHdrColourSpace(DXGI_COLOR_SPACE_TYPE cs)
{
    // scRGB (linear, P709) and HDR10 (PQ, P2020) are the two an injector uses.
    return cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ||
           cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
           cs == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
}

static bool Is8BitFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_B8G8R8A8_UNORM      || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_R8G8B8A8_UNORM      || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

static const char *ColourSpaceName(DXGI_COLOR_SPACE_TYPE cs)
{
    switch (cs)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:      return "RGB_FULL_G22_NONE_P709 (SDR)";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:      return "RGB_FULL_G10_NONE_P709 (scRGB)";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:   return "RGB_FULL_G2084_NONE_P2020 (HDR10)";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020: return "RGB_STUDIO_G2084_NONE_P2020 (HDR10 studio)";
    default:                                           return "other";
    }
}

static DXGI_FORMAT TargetDxgiFormat()
{
    return g_hdr_kind == 1 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
}

static reshade::api::format TargetReshadeFormat()
{
    return g_hdr_kind == 1 ? reshade::api::format::r16g16b16a16_float
                           : reshade::api::format::r10g10b10a2_unorm;
}

// scRGB is linear P709; HDR10 is PQ P2020. Pick the one that matches the format --
// a mismatch here is what makes an "HDR" swapchain render washed out or blown.
static DXGI_COLOR_SPACE_TYPE TargetColourSpace()
{
    return g_hdr_kind == 1 ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                           : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

// The app builds several swapchains: two 308x168 and two 1280x960 preview
// surfaces alongside the main window. The small ones are composited into the main
// window, so tone mapping them as well would apply the curve twice.
static bool SurfaceIsBigEnough(UINT w, UINT h)
{
    return w >= static_cast<UINT>(g_min_w) && h >= static_cast<UINT>(g_min_h);
}

// Which module a function pointer belongs to. This is the probe that matters:
// if SetColorSpace1 resolves into an NVIDIA module rather than dxgi.dll, an
// injector has already wrapped the swapchain and sits in front of this add-on.
static void LogSlotOwner(const char *what, void *fn)
{
    HMODULE mod = nullptr;
    char    path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(fn), &mod) && mod)
        GetModuleFileNameA(mod, path, MAX_PATH);

    const char *base = strrchr(path, '\\');
    Log("  %-16s %p  owner=%s", what, fn, base ? base + 1 : path);
}

// Log any NVIDIA module that has appeared since the last scan. The driver injects
// its HDR path around or after swapchain creation, so "none at attach" proves
// nothing -- only a rescan does.
static void ScanNvModules(const char *when)
{
    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return;

    const size_t count = needed / sizeof(HMODULE);
    for (size_t i = 0; i < count && i < 1024; ++i)
    {
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(mods[i], path, MAX_PATH)) continue;

        const char *slash = strrchr(path, '\\');
        const char *base  = slash ? slash + 1 : path;

        // Match the driver's injected pieces: NvPresent*, nvngx*, _nvngx, nvapi*,
        // NvCamera (Freestyle/Ansel), and anything else NVIDIA drops in.
        if (_strnicmp(base, "nv", 2) != 0 && _strnicmp(base, "_nv", 3) != 0)
            continue;

        std::string key(base);
        EnterCriticalSection(&g_cs);
        const bool known = g_seen_nv_modules.find(key) != g_seen_nv_modules.end();
        if (!known) g_seen_nv_modules[key] = true;
        LeaveCriticalSection(&g_cs);

        if (!known)
            Log("  NVIDIA module appeared (%s): %s", when, path);
    }
}

// ---------------------------------------------------------------------------
// cfg

static void CfgWriteDefault()
{
    if (GetFileAttributesA(g_cfg_path) != INVALID_FILE_ATTRIBUTES) return;

    FILE *f = nullptr;
    if (fopen_s(&f, g_cfg_path, "w") != 0 || !f) return;
    fprintf(f,
        "enabled=1\n"
        "; mode 0 = observe only   -- log everything, change nothing\n"
        "; mode 1 = hold           -- drop the app's SDR re-stamp, never upgrade\n"
        "; mode 2 = upgrade+probe  -- create the swapchain HDR-capable and report what\n"
        ";                            CheckColorSpaceSupport says, without adopting HDR\n"
        "; mode 3 = drive          -- upgrade, adopt the HDR colour space, and hold it\n"
        "mode=0\n"
        "; only act in this executable; empty = any process (not recommended)\n"
        "process=StreamingCenter.exe\n"
        "; 0 = HDR10 (R10G10B10A2 + PQ), 1 = scRGB (R16G16B16A16_FLOAT + linear)\n"
        "hdr_kind=0\n"
        "; ignore swapchains smaller than this -- the preview surfaces\n"
        "min_width=1280\n"
        "min_height=700\n"
        "log_calls=1\n");
    fclose(f);
}

static int CfgInt(const char *text, const char *key, int fallback)
{
    char pat[64];
    sprintf_s(pat, "%s=", key);
    const char *p = strstr(text, pat);
    if (!p) return fallback;
    if (p != text && p[-1] != '\n' && p[-1] != '\r') return fallback;
    return atoi(p + strlen(pat));
}

static void CfgStr(const char *text, const char *key, char *out, size_t cap)
{
    char pat[64];
    sprintf_s(pat, "%s=", key);
    const char *p = strstr(text, pat);
    if (!p) return;
    if (p != text && p[-1] != '\n' && p[-1] != '\r') return;
    p += strlen(pat);
    size_t n = 0;
    while (n + 1 < cap && p[n] && p[n] != '\r' && p[n] != '\n') { out[n] = p[n]; ++n; }
    out[n] = '\0';
}

static void CfgReload()
{
    FILE *f = nullptr;
    if (fopen_s(&f, g_cfg_path, "rb") != 0 || !f) return;
    char text[2048] = {};
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = '\0';
    fclose(f);

    g_enabled   = CfgInt(text, "enabled", g_enabled);
    g_mode      = CfgInt(text, "mode", g_mode);
    g_log_calls = CfgInt(text, "log_calls", g_log_calls);
    g_hdr_kind  = CfgInt(text, "hdr_kind", g_hdr_kind);
    g_min_w     = CfgInt(text, "min_width", g_min_w);
    g_min_h     = CfgInt(text, "min_height", g_min_h);
    CfgStr(text, "process", g_process, sizeof(g_process));

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char *base = strrchr(exe, '\\');
    base = base ? base + 1 : exe;

    g_host_matches = (g_process[0] == '\0') || (_stricmp(base, g_process) == 0);
}

// ---------------------------------------------------------------------------
// detours

static bool GuardActive()
{
    return g_enabled != 0 && g_host_matches;
}

static HRESULT STDMETHODCALLTYPE Hook_SetColorSpace1(
    IDXGISwapChain3 *self, DXGI_COLOR_SPACE_TYPE cs)
{
    SwapState *st = nullptr;
    EnterCriticalSection(&g_cs);
    auto it = g_state.find(self);
    if (it != g_state.end()) st = &it->second;
    LeaveCriticalSection(&g_cs);

    if (st && IsHdrColourSpace(cs))
    {
        // Somebody -- the injector -- just put this swapchain into HDR. Remember
        // it, and remember the format it is wearing, so a later 8-bit
        // ResizeBuffers can be answered with something that keeps HDR alive.
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        if (SUCCEEDED(self->GetDesc1(&desc))) st->hdr_format = desc.Format;
        st->colour_space = cs;
        st->hdr_seen     = true;
        if (g_log_calls)
            Log("SetColorSpace1(%s) -> HDR adopted on %p (format %d)",
                ColourSpaceName(cs), self, static_cast<int>(st->hdr_format));
        return g_real_setcs(self, cs);
    }

    if (st && st->hdr_seen && IsSdrColourSpace(cs) && GuardActive())
    {
        if (g_mode == 0)
        {
            if (g_log_calls)
                Log("SetColorSpace1(SDR) on %p -- WOULD DROP (observe only, mode=0)", self);
            return g_real_setcs(self, cs);
        }

        // Enforcing. Report success without touching the swapchain, so the app's
        // own logic carries on believing it got what it asked for.
        Log("SetColorSpace1(SDR) on %p -- DROPPED (HDR held)", self);
        return S_OK;
    }

    if (st) st->colour_space = cs;
    if (g_log_calls && st)
        Log("SetColorSpace1(%s) on %p -- passed through", ColourSpaceName(cs), self);
    return g_real_setcs(self, cs);
}

static HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(
    IDXGISwapChain *self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    IDXGISwapChain3 *sc3 = nullptr;
    SwapState       *st  = nullptr;
    if (SUCCEEDED(self->QueryInterface(__uuidof(IDXGISwapChain3),
                                       reinterpret_cast<void **>(&sc3))) && sc3)
    {
        EnterCriticalSection(&g_cs);
        auto it = g_state.find(sc3);
        if (it != g_state.end()) st = &it->second;
        LeaveCriticalSection(&g_cs);
        sc3->Release();
    }

    // DXGI_FORMAT_UNKNOWN means "keep the current format" -- always harmless.
    if (st && st->hdr_seen && fmt != DXGI_FORMAT_UNKNOWN && Is8BitFormat(fmt) &&
        st->hdr_format != DXGI_FORMAT_UNKNOWN && GuardActive())
    {
        if (g_mode == 0)
        {
            if (g_log_calls)
                Log("ResizeBuffers(NewFormat = %d) on %p -- WOULD SUBSTITUTE %d "
                    "(observe only, mode=0)",
                    static_cast<int>(fmt), self, static_cast<int>(st->hdr_format));
            return g_real_resize(self, count, w, h, fmt, flags);
        }

        Log("ResizeBuffers(NewFormat = %d) on %p -- SUBSTITUTED %d (HDR held)",
            static_cast<int>(fmt), self, static_cast<int>(st->hdr_format));
        return g_real_resize(self, count, w, h, st->hdr_format, flags);
    }

    if (g_log_calls && st)
        Log("ResizeBuffers(NewFormat = %d) on %p -- passed through",
            static_cast<int>(fmt), self);
    return g_real_resize(self, count, w, h, fmt, flags);
}

// Patch the two slots on this swapchain's vtable. Each distinct vtable is
// patched once; swapchains from the same factory share one.
static void PatchVTable(IDXGISwapChain3 *sc)
{
    void **vt = *reinterpret_cast<void ***>(sc);

    EnterCriticalSection(&g_cs);
    const bool done = g_patched.find(vt) != g_patched.end();
    if (!done) g_patched[vt] = true;
    LeaveCriticalSection(&g_cs);
    if (done) return;

    Log("patching swapchain vtable %p", static_cast<void *>(vt));
    LogSlotOwner("ResizeBuffers", vt[kSlotResizeBuffers]);
    LogSlotOwner("SetColorSpace1", vt[kSlotSetColorSpace1]);

    // First vtable wins for the trampolines: every swapchain in the process
    // routes through the same dxgi.dll implementations.
    if (!g_real_resize)
        g_real_resize = reinterpret_cast<PFN_ResizeBuffers>(vt[kSlotResizeBuffers]);
    if (!g_real_setcs)
        g_real_setcs = reinterpret_cast<PFN_SetColorSpace1>(vt[kSlotSetColorSpace1]);

    DWORD old = 0;
    if (!VirtualProtect(&vt[kSlotResizeBuffers], sizeof(void *) * 1,
                        PAGE_READWRITE, &old))
    {
        Log("  VirtualProtect failed on ResizeBuffers slot (win32 %lu) -- not patched",
            GetLastError());
    }
    else
    {
        vt[kSlotResizeBuffers] = reinterpret_cast<void *>(&Hook_ResizeBuffers);
        VirtualProtect(&vt[kSlotResizeBuffers], sizeof(void *) * 1, old, &old);
    }

    if (!VirtualProtect(&vt[kSlotSetColorSpace1], sizeof(void *) * 1,
                        PAGE_READWRITE, &old))
    {
        Log("  VirtualProtect failed on SetColorSpace1 slot (win32 %lu) -- not patched",
            GetLastError());
    }
    else
    {
        vt[kSlotSetColorSpace1] = reinterpret_cast<void *>(&Hook_SetColorSpace1);
        VirtualProtect(&vt[kSlotSetColorSpace1], sizeof(void *) * 1, old, &old);
    }

    Log("  patched (mode=%d, %s)", g_mode,
        GuardActive() ? "active in this process" : "inactive in this process");
}

// ---------------------------------------------------------------------------
// ReShade events

// Make the back buffer HDR-capable at birth. Doing it here rather than with a
// later ResizeBuffers is the whole point: an 8-bit swapchain rejects every HDR
// colour space, so anything that sets the colour space before widening the buffer
// (AutoHDR-ReShade does exactly that) fails its own support check and gives up.
//
// The size gate cannot always run here -- this host passes Width = 0, Height = 0
// and lets DXGI take them from the window. Widening a preview surface to 10 bits
// is harmless on its own, so when the size is unknown the format is upgraded
// anyway and the decision that actually matters -- whether to adopt an HDR colour
// space -- is deferred to OnInitSwapchain, where the real size is known.
static bool OnCreateSwapchain(reshade::api::device_api api,
                              reshade::api::swapchain_desc &desc, void *hwnd)
{
    CfgReload();

    if (!GuardActive() || g_mode < 2) return false;
    if (api != reshade::api::device_api::d3d11 && api != reshade::api::device_api::d3d12)
        return false;

    const uint32_t w = desc.back_buffer.texture.width;
    const uint32_t h = desc.back_buffer.texture.height;

    if (w != 0 && h != 0 && !SurfaceIsBigEnough(w, h))
    {
        if (g_log_calls)
            Log("create_swapchain %ux%u hwnd=%p -- below %dx%d, left alone",
                w, h, hwnd, g_min_w, g_min_h);
        return false;
    }

    const reshade::api::format from = desc.back_buffer.texture.format;
    const reshade::api::format to   = TargetReshadeFormat();
    if (from == to) return false;

    desc.back_buffer.texture.format = to;

    Log("create_swapchain %ux%u hwnd=%p -- format %d -> %d (%s)%s",
        w, h, hwnd, static_cast<int>(from), static_cast<int>(to),
        g_hdr_kind == 1 ? "scRGB FP16" : "HDR10 R10G10B10A2",
        (w == 0 || h == 0) ? " [size unknown here, colour space decided at init]" : "");
    return true;
}

static void OnInitSwapchain(reshade::api::swapchain *swapchain, bool)
{
    CfgReload();   // the host recreates swapchains constantly, so this is live enough

    reshade::api::device *dev = swapchain->get_device();
    if (dev == nullptr) return;

    const reshade::api::device_api api = dev->get_api();
    if (api != reshade::api::device_api::d3d11 && api != reshade::api::device_api::d3d12)
        return;

    IDXGISwapChain *native = reinterpret_cast<IDXGISwapChain *>(swapchain->get_native());
    if (native == nullptr) return;

    IDXGISwapChain3 *sc3 = nullptr;
    if (FAILED(native->QueryInterface(__uuidof(IDXGISwapChain3),
                                      reinterpret_cast<void **>(&sc3))) || !sc3)
    {
        Log("swapchain %p has no IDXGISwapChain3 -- skipped", native);
        return;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    sc3->GetDesc1(&desc);

    EnterCriticalSection(&g_cs);
    SwapState &st = g_state[sc3];
    st.colour_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    st.hdr_format   = DXGI_FORMAT_UNKNOWN;
    st.hdr_seen     = false;
    LeaveCriticalSection(&g_cs);

    Log("swapchain %p init: %ux%u format=%d buffers=%u flags=0x%x",
        sc3, desc.Width, desc.Height, static_cast<int>(desc.Format),
        desc.BufferCount, desc.Flags);

    PatchVTable(sc3);
    ScanNvModules("swapchain init");

    // Now that the buffer is whatever it is going to be, ask DXGI whether the HDR
    // colour space is actually presentable on it. On an 8-bit buffer this returns
    // 0; on a widened one it should carry the PRESENT flag.
    if (GuardActive() && g_mode >= 2)
    {
        if (!SurfaceIsBigEnough(desc.Width, desc.Height))
        {
            Log("  %ux%u is below %dx%d -- preview surface, no colour space change",
                desc.Width, desc.Height, g_min_w, g_min_h);
        }
        else
        {
            const DXGI_COLOR_SPACE_TYPE target = TargetColourSpace();
            UINT support = 0;
            const HRESULT hr = sc3->CheckColorSpaceSupport(target, &support);

            if (FAILED(hr))
            {
                Log("  CheckColorSpaceSupport(%s) failed 0x%08x",
                    ColourSpaceName(target), hr);
            }
            else
            {
                const bool presentable =
                    (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;

                Log("  CheckColorSpaceSupport(%s) = 0x%x (%s) on format %d",
                    ColourSpaceName(target), support,
                    presentable ? "PRESENTABLE" : "not presentable",
                    static_cast<int>(desc.Format));

                if (!presentable)
                {
                    Log("  -- buffer did not widen, or the display will not take this "
                        "colour space. Not adopting.");
                }
                else if (g_mode < 3)
                {
                    Log("  -- WOULD ADOPT (mode=%d is probe only; set mode=3 to drive)",
                        g_mode);
                }
                else
                {
                    // Goes through Hook_SetColorSpace1, which records the adoption and
                    // arms the guard against the app's SDR re-stamp.
                    const HRESULT sr = sc3->SetColorSpace1(target);
                    if (SUCCEEDED(sr))
                        Log("  -- ADOPTED %s", ColourSpaceName(target));
                    else
                        Log("  -- SetColorSpace1 failed 0x%08x", sr);
                }
            }
        }
    }

    sc3->Release();   // the state map key is the pointer only; ReShade owns the object
}

// An injector can upgrade the buffer format without ever calling SetColorSpace1,
// so watch the live format too. Also rescans for late driver injection.
static void OnPresent(reshade::api::command_queue *, reshade::api::swapchain *swapchain,
                      const reshade::api::rect *, const reshade::api::rect *,
                      uint32_t, const reshade::api::rect *)
{
    IDXGISwapChain *native = reinterpret_cast<IDXGISwapChain *>(swapchain->get_native());
    if (native == nullptr) return;

    IDXGISwapChain3 *sc3 = nullptr;
    if (FAILED(native->QueryInterface(__uuidof(IDXGISwapChain3),
                                      reinterpret_cast<void **>(&sc3))) || !sc3)
        return;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    if (FAILED(sc3->GetDesc1(&desc))) { sc3->Release(); return; }

    EnterCriticalSection(&g_cs);
    auto it = g_state.find(sc3);
    if (it == g_state.end()) { LeaveCriticalSection(&g_cs); sc3->Release(); return; }
    SwapState &st = it->second;

    const DXGI_FORMAT           prev_format = st.last_format;
    const DXGI_COLOR_SPACE_TYPE cs          = st.colour_space;
    const unsigned              n           = ++st.frames;
    st.last_format = desc.Format;
    LeaveCriticalSection(&g_cs);

    if (prev_format != DXGI_FORMAT_UNKNOWN && prev_format != desc.Format)
        Log("swapchain %p FORMAT CHANGED %d -> %d (colour space %s) -- somebody upgraded it",
            sc3, static_cast<int>(prev_format), static_cast<int>(desc.Format),
            ColourSpaceName(cs));

    if (n == 1 || (n % 600) == 0)
    {
        Log("swapchain %p frame %u: format=%d colour space=%s hdr_seen=%d",
            sc3, n, static_cast<int>(desc.Format), ColourSpaceName(cs),
            st.hdr_seen ? 1 : 0);
        ScanNvModules("present");
    }

    sc3->Release();
}

// Adopting the colour space on the DXGI swapchain is only half of it: ReShade
// keeps its own idea of the presentation colour space and hands it to effects as
// BUFFER_COLOR_SPACE. Without this, the tone-mapping effect still believes it is
// writing SDR and emits the wrong transfer function into a PQ buffer.
static void OnInitEffectRuntime(reshade::api::effect_runtime *runtime)
{
    if (!GuardActive() || g_mode < 3) return;

    IDXGISwapChain *native = reinterpret_cast<IDXGISwapChain *>(runtime->get_native());
    if (native == nullptr) return;

    IDXGISwapChain3 *sc3 = nullptr;
    if (FAILED(native->QueryInterface(__uuidof(IDXGISwapChain3),
                                      reinterpret_cast<void **>(&sc3))) || !sc3)
        return;

    bool adopted = false;
    EnterCriticalSection(&g_cs);
    auto it = g_state.find(sc3);
    if (it != g_state.end()) adopted = it->second.hdr_seen;
    LeaveCriticalSection(&g_cs);

    if (adopted)
    {
        const reshade::api::color_space cs = (g_hdr_kind == 1)
            ? reshade::api::color_space::extended_srgb_linear
            : reshade::api::color_space::hdr10_st2084;
        runtime->set_color_space(cs);
        Log("effect runtime on %p: ReShade colour space set to %s",
            sc3, g_hdr_kind == 1 ? "scrgb" : "hdr10_pq");
    }

    sc3->Release();
}

static void OnDestroySwapchain(reshade::api::swapchain *swapchain, bool)
{
    IDXGISwapChain *native = reinterpret_cast<IDXGISwapChain *>(swapchain->get_native());
    if (native == nullptr) return;

    IDXGISwapChain3 *sc3 = nullptr;
    if (FAILED(native->QueryInterface(__uuidof(IDXGISwapChain3),
                                      reinterpret_cast<void **>(&sc3))) || !sc3)
        return;

    EnterCriticalSection(&g_cs);
    g_state.erase(sc3);
    LeaveCriticalSection(&g_cs);
    sc3->Release();
}

// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_cs);

        GetModuleFileNameA(module, g_log_path, MAX_PATH);
        if (char *s = strrchr(g_log_path, '\\'))
            strcpy_s(s + 1, MAX_PATH - (s + 1 - g_log_path), "sdr-guard.log");
        { FILE *f = nullptr; if (fopen_s(&f, g_log_path, "w") == 0 && f) fclose(f); }

        GetModuleFileNameA(module, g_cfg_path, MAX_PATH);
        if (char *s = strrchr(g_cfg_path, '\\'))
            strcpy_s(s + 1, MAX_PATH - (s + 1 - g_cfg_path), "sdr-guard.cfg");

        if (!reshade::register_addon(module)) return FALSE;

        CfgWriteDefault();
        CfgReload();

        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        Log("sdr-guard %s (built %s %s) attached.", SDR_GUARD_VERSION, __DATE__, __TIME__);
        Log("  host: %s", exe);
        Log("  enabled=%d mode=%d process=%s -> %s",
            g_enabled, g_mode, g_process,
            GuardActive() ? "ACTIVE" : "inactive (host does not match)");
        Log("  hdr_kind=%d (%s) min_surface=%dx%d",
            g_hdr_kind, g_hdr_kind == 1 ? "scRGB FP16" : "HDR10 R10G10B10A2",
            g_min_w, g_min_h);
        switch (g_mode)
        {
        case 0:  Log("  observe only: nothing will be changed."); break;
        case 1:  Log("  hold: the app's SDR re-stamp is dropped once something adopts HDR."); break;
        case 2:  Log("  upgrade+probe: the back buffer is widened and reported on, but no "
                     "colour space is adopted. Set mode=3 to drive."); break;
        default: Log("  drive: widening the back buffer and adopting HDR."); break;
        }

        ScanNvModules("attach");

        reshade::register_event<reshade::addon_event::create_swapchain>(OnCreateSwapchain);
        reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        reshade::unregister_event<reshade::addon_event::create_swapchain>(OnCreateSwapchain);
        reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
        reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        reshade::unregister_event<reshade::addon_event::present>(OnPresent);
        reshade::unregister_addon(module);
        // The patched vtable slots are deliberately left in place: another add-on
        // may have patched over them since, and restoring blindly would install a
        // stale pointer. The process is exiting; the OS reclaims it.
    }
    return TRUE;
}
