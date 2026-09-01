// dlss5-feed - ReShade add-on
//
// Makes DLSS 5 neural rendering work in a D3D11, D3D12, Vulkan or OpenGL game that has
// no DLSS of its own.
//
// The DLSS 5 add-on (renodx-dlss5) only detours NVSDK_NGX_D3D12_CreateFeature /
// EvaluateFeature and reads the DLSS "contract" it finds there (Color, Depth,
// MotionVectors, Output, sizes, jitter, reset...). Nothing in a DLSS-less game ever
// issues those calls, so this add-on issues them itself: it takes the frame ReShade is
// processing (the backbuffer), the raw depth and the motion vectors prepared by the
// companion effect "DLSS5_Feed.fx" (fed by any texMotionVectors provider),
// copies the three into textures shared with a private D3D12 device, runs a genuine
// DLSS DLAA evaluate on that device -- where the DLSS 5 add-on inserts its pass --
// and copies the result back over the backbuffer, still inside ReShade's effect chain.
//
// The D3D11 <-> D3D12 transport (shared textures, shared fence, allocator ring) is
// adapted from NIGos' dlss5-dx11-bridge (MIT), see external/bridge-1.0.19/LICENSE.
// In a D3D12 game there is no transport at all: NGX runs on the game's own device
// and queue (the DLSS 5 add-on's native scenario), with the motion vectors and
// depth consumed zero-copy straight from the effect textures.
// Vulkan and OpenGL games reuse the private-D3D12 shape, with the shared textures
// and fences created on D3D12 and imported into the game's API -- raw, because
// ReShade's own import uses the wrong external handle type. See feed_vk.h (Vulkan,
// where the imports are handed back to ReShade so queue operations stay in its
// locks) and feed_gl.h (OpenGL, where they cannot be and the whole per-frame path
// is raw -- which is safe, since GL has no queue object to race).
// The NGX side uses NVIDIA's NGX SDK static library, which locates and loads the
// driver's _nvngx.dll by itself.
//
// Behaviour is driven by dlss5-feed.cfg (re-read while the game runs). dlss5-feed.log
// records what was found, what was built and the result of every NGX call.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

#define ImTextureID ImU64   // required by reshade_overlay.hpp before including imgui.h
#include <imgui.h>
#include <reshade.hpp>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include "feed_vk.h"   // raw-Vulkan interop for the Vulkan transport (see PLAN-VULKAN)
#include "feed_vk_hook.h"   // in-process vkCreateDevice hook: appends the interop extensions the transport needs
#include "feed_gl.h"   // raw-OpenGL interop for the OpenGL transport (see PLAN-OPENGL)

#define FEED_VERSION "0.8.0-beta.1"

extern "C" __declspec(dllexport) const char *NAME = "DLSS 5 Feed " FEED_VERSION;
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Feeds DLSS 5 neural rendering with ReShade's depth and estimated motion vectors in D3D11, "
    "D3D12, Vulkan and OpenGL games without DLSS: runs a real DLSS DLAA pass where the DLSS 5 add-on "
    "hooks in (a private D3D12 device for D3D11, Vulkan and OpenGL games, the game's own device for "
    "D3D12) and writes the result back into the frame.Needs DLSS5_Feed.fx and a motion-vector provider (DRME, qUINT, Launchpad, VORT or LumeniteFX; pick it with the DLSS5_MV_PROVIDER definition). "
    "Settings in dlss5-feed.cfg.";

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static HMODULE          g_self;
static char             g_log_path[MAX_PATH];
static CRITICAL_SECTION g_log_cs;

static void Log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    static long written = 0;
    static bool capped  = false;

    EnterCriticalSection(&g_log_cs);
    if (!capped)
    {
        FILE *f = nullptr;
        if (fopen_s(&f, g_log_path, "a") == 0 && f != nullptr)
        {
            written += fprintf(f, "%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond,
                               st.wMilliseconds, line);
            if (written > 8 * 1024 * 1024)
            {
                fprintf(f, "\n--- log capped at 8 MB ---\n");
                capped = true;
            }
            fclose(f);
        }
    }
    LeaveCriticalSection(&g_log_cs);
}

// Also raised in ReShade's log/overlay: reserved for things that stop the add-on working.
static void Warn(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    Log("%s", line);
    char tagged[1100];
    _snprintf_s(tagged, sizeof(tagged), _TRUNCATE, "[DLSS 5 Feed] %s", line);
    reshade::log::message(reshade::log::level::warning, tagged);
}

static const char *volatile g_where = "starting up";
static void Breadcrumb(const char *what) { g_where = what; }

static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    const void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    wchar_t owner[MAX_PATH] = L"unknown";
    HMODULE mod = nullptr;
    if (addr != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &mod) && mod != nullptr)
        GetModuleFileNameW(mod, owner, MAX_PATH);
    Log("### CRASH RECORDED ###  exception 0x%08X at %p in %ls; this add-on was last doing: %s%s", code, addr,
        owner, g_where, mod == g_self ? " (inside this add-on)" : "");
    return g_prev_filter != nullptr ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Which DLSS 5 add-on build is installed? Its engine generation changes how we
// should behave, so detect it instead of assuming:
//  - classic builds hook the NGX vtable once and can miss the first create (the
//    STANDBY latch our warm-up re-create medicates),
//  - v45+ builds rescan every present and adopt missed features lazily from the
//    evaluate, making the warm-up re-create pure waste (and a small crash surface),
//    and add the EnableHooks policy key ('2' = NGX-only, correct for this feeder,
//    since '1' patches Streamline modules at a self-described contested site).
//  - v4.6 builds keep the v45+ engine (rescan every present, adopt lazily) and add
//    global hotkeys, a rejected-upscaling latch and richer decline diagnostics;
//    nothing they gate on is missing from the contract this feeder publishes.
// The 'EnableHooks' string in the binary is the v45+ marker; 'NRToggleKey' is the
// v4.6 one (its only new config keys are EnableHooks and the two hotkey binds).
// ---------------------------------------------------------------------------

static char g_renodx_ver[48] = "not found";
static bool g_renodx_lazy    = false;
static bool g_renodx_v46     = false;

// Set when the game's device (or the process) is being destroyed: from that moment,
// never call back into NGX. The DLSS 5 add-on tears its hooks down during device
// destruction, and releasing a feature created through those hooks afterwards throws
// on a foreign thread and wedges the quitting game (seen in DOOM: 0xE06D7363 in
// KERNELBASE 18 ms after the add-on's vtable::Unhook). The OS reclaims it all anyway.
static bool g_ngx_dying = false;

// Write a RenoDX.DLSS5 config default, only when the user has not set the key
// themselves (the add-on persists any overlay change, and a saved value wins here).
static void RenodxDefault(const char *key, const char *value, const char *why)
{
    char v[16];
    size_t n = sizeof(v);
    if (!reshade::get_config_value(nullptr, "RenoDX.DLSS5", key, v, &n))
    {
        reshade::set_config_value(nullptr, "RenoDX.DLSS5", key, value);
        Log("[feed] %s was unset; wrote %s=%s (%s)", key, key, value, why);
    }
    else
        Log("[feed] %s=%s (user-set; leaving it alone)", key, v);
}

static void DetectRenodxAddon()
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    if (char *s = strrchr(path, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - path), "renodx-dlss5.addon64");

    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        Log("[feed] DLSS 5 add-on: renodx-dlss5.addon64 not found next to this add-on");
        return;
    }
    const DWORD size = GetFileSize(f, nullptr);
    DWORD got = 0;
    char *buf = (size > 0 && size < 8u * 1024 * 1024) ? static_cast<char *>(malloc(size)) : nullptr;
    if (buf != nullptr && ReadFile(f, buf, size, &got, nullptr) && got == size)
        for (DWORD i = 0; i + 11 < size; ++i)   // both markers happen to be 11 bytes
        {
            if (!g_renodx_lazy && memcmp(buf + i, "EnableHooks", 11) == 0) g_renodx_lazy = true;
            if (!g_renodx_v46  && memcmp(buf + i, "NRToggleKey", 11) == 0) g_renodx_v46  = true;
            if (g_renodx_lazy && g_renodx_v46) break;
        }
    free(buf);
    CloseHandle(f);
    if (g_renodx_v46) g_renodx_lazy = true;   // v4.6 is a per-present-rescan engine too

    DWORD dummy = 0;
    const DWORD vsize = GetFileVersionInfoSizeA(path, &dummy);
    if (vsize > 0)
    {
        void *vdata = malloc(vsize);
        VS_FIXEDFILEINFO *ffi = nullptr;
        UINT flen = 0;
        if (vdata != nullptr && GetFileVersionInfoA(path, 0, vsize, vdata) &&
            VerQueryValueA(vdata, "\\", reinterpret_cast<void **>(&ffi), &flen) && ffi != nullptr)
            sprintf_s(g_renodx_ver, "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                      HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
        free(vdata);
    }

    Log("[feed] DLSS 5 add-on: renodx-dlss5.addon64 v%s -- %s engine", g_renodx_ver,
        g_renodx_v46  ? "v4.6+ (per-present rescan, lazy adoption, global hotkeys, upscaling latch)"
      : g_renodx_lazy ? "v45+ (per-present rescan, lazy feature adoption; warm-up re-create skipped)"
                      : "classic (single hook pass; warm-up re-create stays on)");

    if (g_renodx_lazy)
        RenodxDefault("EnableHooks", "2", "NGX-only -- this feeder calls NGX directly, no Streamline");

    // Every known add-on generation reads these two keys; make a fresh install
    // deterministic. NeuralUplift on is the whole point of installing this feeder.
    // NREnableUpscaling off matches the contract: this feeder always publishes 1:1
    // DLAA (even below 100% work resolution -- DLSS runs at the reduced size and the
    // feeder scales the result back itself), so upscaling could never engage, and
    // v4.6 pairs its WIP upscaling path with a rejection latch that parks NR on the
    // native path for the rest of the run. A build too old to know a key never reads
    // it, so both writes are inert on older generations.
    RenodxDefault("NeuralUplift", "1", "neural rendering on");
    RenodxDefault("NREnableUpscaling", "0", "upscaling off; this feeder publishes a complete 1:1 DLAA contract");
}
// ---------------------------------------------------------------------------
// Alex's Toolkit (alexs-toolkit.addon64) -- a third-party NGX interposer that sits
// between the DLSS 5 add-on and nvngx_dlssnr.dll. It hooks GetProcAddress in the
// Generic NGX module, wraps every feature-18 (DLSS-NR) create, and for each real
// feature the DLSS 5 add-on makes it creates one or two private copies at a 1:1
// native contract. Per frame it then runs them as a cascade -- private pass ->
// intermediate -> the real pass, which takes the intermediate as its colour.
//
// It does NOT mishandle our inputs: every stage has the same input dimensions as
// the contract we publish, so the motion vectors and depth stay dimensionally
// valid throughout (a 16k-frame capture shows zero fallbacks and no rejection).
// What it costs is temporal: each stage keeps its OWN history, so a two-pass
// cascade roughly doubles the effective history length and a three-pass one
// triples it. With screen-space estimated motion vectors -- which are inherently
// one frame late -- that reads as smearing and lag behind fast motion, and it
// multiplies how long the image takes to settle after a hard camera cut.
//
// We only detect and report it. It arms itself at the final swapchain and must be
// attached before the DLSS 5 add-on first resolves nvngx_dlssnr.dll, or it logs
// "Generic already cached ... before toolkit attach" and stays pass-through for
// the whole run. That first resolve is triggered by OUR first CreateFeature, so
// the create_delay grace below is what keeps the ordering safe -- which is why
// that grace is re-armed for every feature (re)build, not just the first.
// ---------------------------------------------------------------------------

static char g_toolkit_ver[64]     = "not found";
static char g_toolkit_status[192] = "not present";
static int  g_toolkit_passes      = 0;   // 0 = absent or disabled, 2 = two-pass, 3 = three-pass

// Reads "key=<int>" from a small ini-style file. Returns 'fallback' if absent.
static int ToolkitCfgInt(const char *text, const char *key, int fallback)
{
    const size_t klen = strlen(key);
    for (const char *p = text; *p != '\0'; ++p)
    {
        if ((p != text && p[-1] != '\n' && p[-1] != '\r') || _strnicmp(p, key, klen) != 0 || p[klen] != '=')
            continue;
        return atoi(p + klen + 1);
    }
    return fallback;
}

static void DetectToolkitAddon()
{
    char dir[MAX_PATH];
    GetModuleFileNameA(g_self, dir, MAX_PATH);
    char *slash = strrchr(dir, '\\');
    if (slash == nullptr) return;
    slash[1] = '\0';

    char path[MAX_PATH];
    sprintf_s(path, "%salexs-toolkit.addon64", dir);

    // The add-on advertises itself in its exported NAME string ("Alex's Toolkit <ver>"),
    // the same way this one does. Scan the file rather than the loaded module: ReShade
    // may not have loaded it yet when this runs.
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        Log("[feed] Alex's Toolkit: not present -- DLSS 5 runs a single neural pass");
        return;
    }
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
                while (n + 1 < sizeof(g_toolkit_ver) && i + mlen + n < size &&
                       v[n] >= 32 && v[n] < 127 && v[n] != '%')
                    ++n;
                if (n > 0) { memcpy(g_toolkit_ver, v, n); g_toolkit_ver[n] = '\0'; }
                break;
            }
    }
    free(buf);
    CloseHandle(f);

    // Its live settings (it re-reads this file itself while the game runs, so this is
    // only what it will start with).
    int enabled = 1, two_pass = 0, three_pass = 0;
    bool have_cfg = false;
    sprintf_s(path, "%salexs-toolkit.cfg", dir);
    if (FILE *cf = nullptr; fopen_s(&cf, path, "rb") == 0 && cf != nullptr)
    {
        char text[2048];
        const size_t n = fread(text, 1, sizeof(text) - 1, cf);
        text[n] = '\0';
        fclose(cf);
        have_cfg   = true;
        enabled    = ToolkitCfgInt(text, "enabled", 1);
        two_pass   = ToolkitCfgInt(text, "two_pass", 0);
        three_pass = ToolkitCfgInt(text, "three_pass", 0);
    }

    g_toolkit_passes = (enabled && two_pass) ? (three_pass ? 3 : 2) : 0;

    if (g_toolkit_passes >= 2)
        _snprintf_s(g_toolkit_status, sizeof(g_toolkit_status), _TRUNCATE,
                    "Alex's Toolkit %s: %d-pass DLSS 5 cascade active downstream -- expect roughly %dx the "
                    "temporal history (more smearing behind fast motion, slower settle after a camera cut)",
                    g_toolkit_ver, g_toolkit_passes, g_toolkit_passes);
    else
        _snprintf_s(g_toolkit_status, sizeof(g_toolkit_status), _TRUNCATE,
                    "Alex's Toolkit %s: present but the cascade is off (%s) -- DLSS 5 runs a single pass",
                    g_toolkit_ver, enabled ? "two_pass=0" : "enabled=0");

    Log("[feed] %s", g_toolkit_status);
    Log("[feed] Alex's Toolkit config: %s (enabled=%d two_pass=%d three_pass=%d); it re-reads that file live, "
        "so the cascade can change without restarting", have_cfg ? "alexs-toolkit.cfg" : "no cfg file, using its defaults",
        enabled, two_pass, three_pass);
}


// ---------------------------------------------------------------------------
// Configuration (dlss5-feed.cfg next to the add-on, re-read every 60 frames)
// ---------------------------------------------------------------------------

struct Cfg
{
    int   enabled;         // 0 = do nothing at all
    int   mode;            // 0 inert, 1 transport only (copies the input back, no NGX), 2 full DLSS path
    int   hdr;             // -1 auto (FP16/R11G11B10 backbuffer = HDR), 0 force SDR, 1 force HDR
    int   depth_inverted;  // -1 auto (RESHADE_DEPTH_INPUT_IS_REVERSED), 0 no, 1 yes
    int   flags;           // -1 auto, else raw DLSS.Feature.Create.Flags
    int   reset_every;     // 1 = NGX Reset flag every frame (diagnostic: no temporal history)
    int   warmup_rebuild;  // frames after the first successful evaluate at which the feature is re-created once (0 = never)
    int   rebuild;         // any change of this number re-creates the feature once (manual trigger)
    int   log_frames;      // how many first frames get a full parameter dump in the log
    int   create_delay;    // frames to hold the FIRST feature create (the DLSS 5 add-on arms its NGX hooks asynchronously)
    int   preset;          // DLSS render preset hint: 0 default, 5=E, 6=F (legacy CNN), 10=J, 11=K (transformer)
    int   work_resolution; // 64-bit D3D11 only: 50..100 percent of each backbuffer axis
    float mv_scale_x;      // multiplier applied to the motion vectors (the FX already outputs pixels)
    float mv_scale_y;
};

static Cfg g_cfg = { 1, 2, -1, -1, -1, 0, 180, 0, 3, 60, 0, 100, 1.0f, 1.0f };
static int       g_work_resolution_ui = 100;
static int       g_pending_work_resolution = 0;
static ULONGLONG g_work_resolution_apply_after = 0;

static void CfgPath(char *out)
{
    GetModuleFileNameA(g_self, out, MAX_PATH);
    if (char *s = strrchr(out, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - out), "dlss5-feed.cfg");
}

static void CfgWriteDefault()
{
    char path[MAX_PATH];
    CfgPath(path);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    FILE *f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || f == nullptr) return;
    fprintf(f,
            "enabled=%d\n"
            "mode=%d\n"
            "hdr=%d\n"
            "depth_inverted=%d\n"
            "flags=%d\n"
            "reset_every=%d\n"
            "warmup_rebuild=%d\n"
            "rebuild=%d\n"
            "log_frames=%d\n"
            "create_delay=%d\n"
            "preset=%d\n"
            "work_resolution=%d\n"
            "mv_scale_x=%.3f\n"
            "mv_scale_y=%.3f\n",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
            g_cfg.warmup_rebuild, g_cfg.rebuild, g_cfg.log_frames, g_cfg.create_delay, g_cfg.preset,
            g_cfg.work_resolution, g_cfg.mv_scale_x, g_cfg.mv_scale_y);
    fclose(f);
    Log("[feed] wrote default config to %s", path);
}

// Returns true when a creation-time value changed (the feature has to be rebuilt).
static bool CfgReload()
{
    char path[MAX_PATH];
    CfgPath(path);
    FILE *f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || f == nullptr) return false;

    Cfg next = g_cfg;
    char line[160];
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        char  key[64];
        float val = 0.0f;
        if (sscanf_s(line, "%63[^=]=%f", key, static_cast<unsigned>(sizeof(key)), &val) != 2) continue;
        const int iv = static_cast<int>(val);
        if      (_stricmp(key, "enabled")        == 0) next.enabled        = iv;
        else if (_stricmp(key, "mode")           == 0) next.mode           = iv;
        else if (_stricmp(key, "hdr")            == 0) next.hdr            = iv;
        else if (_stricmp(key, "depth_inverted") == 0) next.depth_inverted = iv;
        else if (_stricmp(key, "flags")          == 0) next.flags          = iv;
        else if (_stricmp(key, "reset_every")    == 0) next.reset_every    = iv;
        else if (_stricmp(key, "warmup_rebuild") == 0) next.warmup_rebuild = iv;
        else if (_stricmp(key, "rebuild")        == 0) next.rebuild        = iv;
        else if (_stricmp(key, "log_frames")     == 0) next.log_frames     = iv;
        else if (_stricmp(key, "create_delay")   == 0) next.create_delay   = iv;
        else if (_stricmp(key, "preset")         == 0) next.preset         = iv;
        else if (_stricmp(key, "work_resolution")== 0) next.work_resolution = iv;
        else if (_stricmp(key, "mv_scale_x")     == 0) next.mv_scale_x     = val;
        else if (_stricmp(key, "mv_scale_y")     == 0) next.mv_scale_y     = val;
    }
    fclose(f);
    if (next.mode < 0 || next.mode > 2) next.mode = g_cfg.mode;
    if (next.work_resolution < 50 || next.work_resolution > 100) next.work_resolution = g_cfg.work_resolution;

    const bool rebuild = next.hdr != g_cfg.hdr || next.depth_inverted != g_cfg.depth_inverted ||
                         next.flags != g_cfg.flags || next.rebuild != g_cfg.rebuild ||
                         next.preset != g_cfg.preset;
    const bool changed = rebuild || memcmp(&next, &g_cfg, sizeof(Cfg)) != 0;
    if (!changed) return false;
    g_cfg = next;
    Log("[feed] config: enabled=%d mode=%d hdr=%d depth_inverted=%d flags=%d reset_every=%d warmup_rebuild=%d "
        "rebuild=%d log_frames=%d create_delay=%d work_resolution=%d%% mv_scale=%.3f,%.3f",
        g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
        g_cfg.warmup_rebuild, g_cfg.rebuild, g_cfg.log_frames, g_cfg.create_delay,
        g_cfg.work_resolution, g_cfg.mv_scale_x, g_cfg.mv_scale_y);
    return rebuild;
}

// Writes every current value to dlss5-feed.cfg, overwriting it -- used by the ReShade
// overlay page so a change made there survives the next CfgReload() (which otherwise
// would read the old value straight back off disk 60 frames later).
static void CfgSave()
{
    char path[MAX_PATH];
    CfgPath(path);
    FILE *f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || f == nullptr) return;
    fprintf(f,
        "enabled=%d\nmode=%d\nhdr=%d\ndepth_inverted=%d\nflags=%d\nreset_every=%d\nwarmup_rebuild=%d\n"
            "rebuild=%d\nlog_frames=%d\ncreate_delay=%d\npreset=%d\nwork_resolution=%d\nmv_scale_x=%.3f\nmv_scale_y=%.3f\n",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
            g_cfg.warmup_rebuild, g_cfg.rebuild, g_cfg.log_frames, g_cfg.create_delay, g_cfg.preset,
            g_cfg.work_resolution, g_cfg.mv_scale_x, g_cfg.mv_scale_y);
    fclose(f);
}

// Slider input is deliberately debounced: dragging should cause one texture/feature
// rebuild after the user pauses, not one expensive rebuild per intermediate value.
static bool ApplyPendingWorkResolution()
{
    if (g_pending_work_resolution == 0 || GetTickCount64() < g_work_resolution_apply_after) return false;
    const int next = g_pending_work_resolution;
    g_pending_work_resolution = 0;
    g_work_resolution_apply_after = 0;
    if (next == g_cfg.work_resolution) return false;
    g_cfg.work_resolution = next;
    CfgSave();
    Log("[feed] settled D3D11 work resolution=%d%%; rebuilding private resources", g_cfg.work_resolution);
    return true;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// SLOT_MASK: the shader's DLSS5_Mask (R8, 1 = the motion vector there failed validation), handed
// to DLSS as its "bias current colour" mask so it favours the current frame instead of warping
// history in. Optional: an older DLSS5_Feed.fx without it simply means no mask is passed.
enum { SLOT_COLOR = 0, SLOT_OUTPUT, SLOT_DEPTH, SLOT_MV, SLOT_MASK, SLOT_COUNT };
static const char *kSlotName[SLOT_COUNT] = { "Color", "Output", "Depth", "MV", "Mask" };

static const char *kEffectFile     = "DLSS5_Feed.fx";
static const char *kTechnique      = "DLSS5_Feed";
// Known motion-vector providers, keyed by the DLSS5_MV_PROVIDER value DLSS5_Feed.fx
// was compiled with (0 texMotionVectors, 1 Launchpad, 2 VORT, 3 LumeniteFX Kernel,
// 4 LumeniteFX QuantMotion). Name checks only, for the status line and a mismatch
// warning: the shader itself binds the selected provider's output texture, and any
// effect that writes that texture works, listed here or not.
static const struct { int mode; const char *file, *tech; } kMvProviders[] = {
    { 0, "MotionEstimation.fx",     "DRME" },
    { 0, "qUINT_motionvectors.fx",  "MotionVectors" },
    { 0, "dh_uber_motion.fx",       "DH_UBER_MOTION_020" },
    { 1, "MartysMods_LAUNCHPAD.fx", "MartysMods_Launchpad" },
    { 2, "vort_Motion.fx",          "vort_MotionEffects" },
    { 3, "lumenite_Kernel.fx",      "Lumenite_Kernel" },
    { 4, "lumenite_QuantMotion.fx", "Lumenite_QuantMotion" },
};
static const char *kMvModeName[] = { "texMotionVectors", "Launchpad", "VORT", "LumeniteFX Kernel", "LumeniteFX QuantMotion" };
static const int   kMvModeCount  = static_cast<int>(sizeof(kMvModeName) / sizeof(kMvModeName[0]));

// What the overlay shows under "Motion vectors": the resolved provider line, and the problem
// with it if there is one (empty when everything lines up).
static char g_mv_status[192]  = "not checked yet";
static char g_mv_problem[640] = "";

// ReShade keeps a technique of an effect that FAILED to compile in its list, and it can even
// be "enabled" -- it just never runs. ReshadeMotionEstimation on ReShade 6.8 is the textbook
// case ("cannot sample from texture that is also used as render target"): the feed then gets
// all-zero vectors and DLSS quietly degrades to a still-image contract. There is no add-on
// API for compile status, but ReShade writes every compiler error to its own log next to the
// game as "<path>\<file>(line, col): error ...", and a success as "Successfully compiled
// '<path>\<file>'". Whichever of the two came LAST for that file is the current state.
static bool ProviderCompileError(const char *file, char *out, size_t out_size)
{
    out[0] = '\0';
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    if (char *s = strrchr(path, '\\')) strcpy_s(s + 1, MAX_PATH - (s + 1 - path), "ReShade.log");
    FILE *f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || f == nullptr) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    const long take = size < 512 * 1024 ? size : 512 * 1024;   // the tail is where the last reload is
    fseek(f, size - take, SEEK_SET);
    std::string buf(static_cast<size_t>(take), '\0');
    const size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    buf.resize(got);

    char needle_err[MAX_PATH], needle_ok[MAX_PATH];
    _snprintf_s(needle_err, sizeof(needle_err), _TRUNCATE, "\\%s(", file);
    _snprintf_s(needle_ok,  sizeof(needle_ok),  _TRUNCATE, "\\%s'",  file);
    bool failed = false;
    size_t pos = 0;
    while (pos < buf.size())
    {
        size_t eol = buf.find('\n', pos);
        if (eol == std::string::npos) eol = buf.size();
        const std::string line = buf.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.find(needle_ok) != std::string::npos && line.find("Successfully compiled") != std::string::npos)
            failed = false;
        else if (const size_t at = line.find(needle_err); at != std::string::npos && line.find("error") != std::string::npos)
        {
            failed = true;
            std::string msg = line.substr(at + 1);
            while (!msg.empty() && (msg.back() == '\r' || msg.back() == ' ')) msg.pop_back();
            strncpy_s(out, out_size, msg.c_str(), _TRUNCATE);
        }
    }
    return failed;
}

// The DLSS5_MV_PROVIDER value DLSS5_Feed.fx is compiled with: the effect's own
// definition first, then the global list, else the shader's default of 0.
static int ReadMvProviderMode(reshade::api::effect_runtime *rt)
{
    char v[16] = {};
    int mode = 0;
    if (rt->get_preprocessor_definition_for_effect(kEffectFile, "DLSS5_MV_PROVIDER", v) ||
        rt->get_preprocessor_definition("DLSS5_MV_PROVIDER", v))
        mode = atoi(v);
    return (mode < 0 || mode >= kMvModeCount) ? 0 : mode;
}

struct Feed
{
    // ReShade side
    reshade::api::effect_runtime          *runtime;
    reshade::api::effect_technique         technique;
    reshade::api::effect_technique         launchpad;
    reshade::api::effect_texture_variable  color_var;     // DLSS5_ColorInput : COLOR (D3D11 scaling source)
    reshade::api::effect_texture_variable  mv_var;
    reshade::api::effect_texture_variable  depth_var;
    reshade::api::effect_texture_variable  mask_var;      // DLSS5_Mask; handle 0 with an older shader
    bool                                   mask_ok;       // this frame: DLSS5_Mask present, right size/format, copied
    bool                                   depth_reversed;
    bool                                   handles_ok;
    bool                                   missing_reported;

    bool disabled;
    bool session_ready;
    bool frame_ready;
    bool need_reset;
    bool warmup_done;
    int  consecutive_fails;
    int  cfg_rebuild_seen;
    int  create_grace;     // frames counted while holding the first feature create

    // D3D12 side
    ID3D12Device              *dev12;
    ID3D12CommandQueue        *queue;
    ID3D12GraphicsCommandList *list;
    static const int           kFrames = 3;
    ID3D12CommandAllocator    *alloc[kFrames];
    UINT64                     alloc_fence[kFrames];
    int                        frame_slot;
    HANDLE                     fence_event;
    ID3D12Fence               *fence12;
    ID3D11Fence               *fence11;
    ID3D11DeviceContext4      *ctx4;
    UINT64                     fence_value;
    ID3D11Device              *dev11;      // not owned
    bool                       dev12_owned; // true on the D3D11/Vulkan paths (we created the private device)
    reshade::api::command_queue *rs_queue;  // D3D12/Vulkan paths: ReShade's wrapper of the game's queue (not owned)

    // Vulkan transport: the game-side halves of the shared resources, imported THROUGH
    // ReShade's API (create_resource/create_fence with an existing NT handle), so ReShade
    // performs the Vulkan external-memory import, tracks the images for barrier()/copy,
    // and keeps every queue operation inside its own locks. No raw Vk* in this add-on.
    reshade::api::device  *rs_dev;               // not owned
    reshade::api::fence    rs_fence_in, rs_fence_out;  // wrap vk_sem_* for ReShade queue signal/wait
    ID3D12Fence           *fence12_in, *fence12_out;  // the same fences, D3D12 side
    HANDLE                 fence_in_handle, fence_out_handle;
    HANDLE                 tex_shared_ext[SLOT_COUNT];   // shared NT handles (Vulkan and OpenGL transports)
    // raw-Vulkan imports of the D3D12 shared objects (ReShade's create_* import them
    // as the wrong external type). Wrapped back into rs_fence_* for ReShade queue ops.
    FeedVk                 vk;
    VkImage                vk_img[SLOT_COUNT];
    VkDeviceMemory         vk_mem[SLOT_COUNT];
    VkSemaphore            vk_sem_in, vk_sem_out;
    bool                   vk_layout_init;   // our images transitioned UNDEFINED->GENERAL once
    UINT64                 vk_frame;

    // OpenGL transport: raw-GL imports of the very same D3D12 shared objects. Nothing
    // is handed back to ReShade here -- an api::fence on GL is an opaque value, not a
    // GL semaphore name, so the whole per-frame GL side is raw (see feed_gl.h).
    FeedGl                 gl;
    HGLRC                  gl_ctx;           // the context the imports live in (share-group check)
    GLuint                 gl_tex[SLOT_COUNT], gl_memobj[SLOT_COUNT];
    GLuint                 gl_sem_in, gl_sem_out;
    GLuint                 gl_fbo_read, gl_fbo_draw;
    UINT64                 gl_frame;

    // NGX
    bool                 ngx_inited;
    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle    *feature;

    // shared textures
    ID3D12Resource  *tex12[SLOT_COUNT];
    ID3D11Texture2D *tex11[SLOT_COUNT];
    HANDLE           shared[SLOT_COUNT];
    ID3D11ShaderResourceView *output_srv;   // on tex11[SLOT_OUTPUT], for the copy-back blit
    ID3D11Texture2D          *color_stage;     // native-size copy of the frame, the only SRV-able source we get
    ID3D11ShaderResourceView *color_stage_srv; // its SRV, sampled by the work-resolution downsample
    ID3D11RenderTargetView   *input_rtv[SLOT_COUNT]; // D3D11 work-resolution resample targets
    UINT        width, height;
    UINT        backbuffer_width, backbuffer_height;
    DXGI_FORMAT color_fmt, output_fmt;      // shared texture formats
    DXGI_FORMAT bb_fmt;                     // the backbuffer's format, to notice swaps
    bool        hdr;
    int         create_flags;

    // copy-back blit
    ID3D11VertexShader *blit_vs;
    ID3D11PixelShader  *blit_ps;
    ID3D11PixelShader  *resample_ps;
    ID3D11SamplerState *blit_sampler;
    ID3D11SamplerState *point_sampler;
    ID3D11Buffer       *resample_cb;

    UINT64 frames_done;

    LONGLONG qpf, cpu_ticks, span_start;
    UINT64   timed_frames;
};

static Feed g;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template <typename T> static void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

static UINT ScaledExtent(UINT native_extent, int percent)
{
    if (percent >= 100) return native_extent;
    UINT extent = (native_extent * static_cast<UINT>(percent)) / 100u;
    extent &= ~1u; // NGX work textures use even dimensions
    return extent >= 2u ? extent : 2u;
}

static const char *FormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R11G11B10_FLOAT:       return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_R16G16_FLOAT:          return "R16G16_FLOAT";
    case DXGI_FORMAT_R32_FLOAT:             return "R32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS:          return "R32_TYPELESS";
    case DXGI_FORMAT_R24G8_TYPELESS:        return "R24G8_TYPELESS";
    default:                                return "?";
    }
}

// The shared Color copy must be typed (the DLSS 5 add-on samples it) and in the same
// typeless family as the backbuffer so CopyResource can move the frame across.
static DXGI_FORMAT TypedColorFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// DLSS writes its Output through a UAV; BGRA/X8 variants are not reliably UAV-typed, so
// they get an RGBA8 output and the copy-back blit takes care of the channel order.
// The output must keep the backbuffer's channel order. When it does not, the copy
// home has to convert, and on Vulkan that conversion is vkCmdBlitImage -- which is
// sRGB-aware, so writing our (linear-typed) output into a VK_FORMAT_*_SRGB swapchain
// applies a linear->sRGB encode and the whole image comes back washed out and bright.
// On D3D12 the mismatch is worse: copy_resource() across format families is invalid.
// The frames arrive already encoded, so the copy home must move bytes, not convert.
static DXGI_FORMAT OutputFormatFor(DXGI_FORMAT color_typed)
{
    switch (color_typed)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM:     return DXGI_FORMAT_B8G8R8A8_UNORM;   // X8 has no alpha to preserve
    default:                             return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

// Channel order and bit layout only, ignoring the transfer function: an _SRGB
// backbuffer and our UNORM output ARE interchangeable for a raw copy, and copying
// them raw is exactly the point -- the bytes must land unconverted.
static int TexelLayoutFamily(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return 2;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return 3;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 4;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return 5;
    default:
        return 0;   // unknown: never claim a raw copy is safe
    }
}

static bool SameTexelLayout(DXGI_FORMAT a, DXGI_FORMAT b)
{
    const int fa = TexelLayoutFamily(a);
    return fa != 0 && fa == TexelLayoutFamily(b);
}

// NGX writes the output through a UAV, and typed UAV *stores* to B8G8R8A8_UNORM are an
// optional D3D12 feature. Where the device lacks it, fall back to RGBA and the
// converting copy home -- wrong colours beat a feature that cannot be created at all.
static DXGI_FORMAT ResolveOutputFormat(DXGI_FORMAT color_typed, ID3D12Device *dev12)
{
    const DXGI_FORMAT want = OutputFormatFor(color_typed);
    if (want != DXGI_FORMAT_B8G8R8A8_UNORM || dev12 == nullptr) return want;

    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
    fs.Format = want;
    const bool ok = SUCCEEDED(dev12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
                    (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    if (ok) return want;
    Log("[feed] B8G8R8A8_UNORM has no typed UAV store on this device; output stays R8G8B8A8_UNORM "
        "(the copy home converts, so expect the washed-out image of issue #11)");
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

// OpenGL has no sized BGRA8 internal format, and we choose the shared textures'
// formats -- so on the GL path none is ever created. The colour moves by blit, which
// is component-wise (semantic RGBA, not byte order), so a BGRA-flavoured game surface
// lands correctly in an RGBA8 shared texture and comes home the same way.
static DXGI_FORMAT GlSafeColorFormat(DXGI_FORMAT typed)
{
    if (typed == DXGI_FORMAT_B8G8R8A8_UNORM || typed == DXGI_FORMAT_B8G8R8X8_UNORM)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    return typed;
}

static bool IsHdrFormat(DXGI_FORMAT typed)
{
    return typed == DXGI_FORMAT_R16G16B16A16_FLOAT || typed == DXGI_FORMAT_R11G11B10_FLOAT;
}

static const char *NgxResultName(NVSDK_NGX_Result r)
{
    switch (static_cast<unsigned>(r))
    {
    case 0x1:        return "Success";
    case 0xBAD00001: return "FeatureNotSupported";
    case 0xBAD00002: return "PlatformError";
    case 0xBAD00003: return "FeatureAlreadyExists";
    case 0xBAD00004: return "FeatureNotFound";
    case 0xBAD00005: return "InvalidParameter";
    case 0xBAD00006: return "ScratchBufferTooSmall";
    case 0xBAD00007: return "NotInitialized";
    case 0xBAD00008: return "UnsupportedInputFormat";
    case 0xBAD00009: return "RWFlagMissing";
    case 0xBAD0000A: return "MissingInput";
    case 0xBAD0000B: return "UnableToInitializeFeature";
    case 0xBAD0000C: return "OutOfDate";
    case 0xBAD0000D: return "OutOfGPUMemory";
    case 0xBAD0000E: return "UnsupportedFormat";
    case 0xBAD0000F: return "UnableToWriteToAppDataPath";
    case 0xBAD00010: return "UnsupportedParameter";
    case 0xBAD00011: return "Denied";
    case 0xBAD00012: return "NotImplemented";
    default:         return "?";
    }
}

static void FeedDisable(const char *why)
{
    if (g.disabled) return;
    g.disabled = true;
    Warn("stopped: %s. The game renders normally. See dlss5-feed.log for the detail.", why);
}

static void FeedFail(const char *what)
{
    Log("[feed] failure: %s", what);
    if (++g.consecutive_fails >= 3)
        FeedDisable("repeated failures");
}

// ---------------------------------------------------------------------------
// D3D12 command submission (allocator ring + shared fence), from the bridge
// ---------------------------------------------------------------------------

static bool BeginCommands()
{
    const int slot = g.frame_slot;
    const UINT64 retire = g.alloc_fence[slot];
    if (retire != 0 && g.fence12->GetCompletedValue() < retire)
    {
        g.fence12->SetEventOnCompletion(retire, g.fence_event);
        if (WaitForSingleObject(g.fence_event, 2000) != WAIT_OBJECT_0)
        {
            Log("[feed] the GPU did not retire allocator slot %d within 2 s", slot);
            FeedDisable("the GPU stopped completing work");
            return false;
        }
    }
    if (g.alloc[slot] == nullptr) return false;
    if (FAILED(g.alloc[slot]->Reset())) return false;
    return SUCCEEDED(g.list->Reset(g.alloc[slot], nullptr));
}

static UINT64 EndCommands()
{
    g.list->Close();
    ID3D12CommandList *lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++g.fence_value;
    g.queue->Signal(g.fence12, v);
    g.alloc_fence[g.frame_slot] = v;
    g.frame_slot = (g.frame_slot + 1) % Feed::kFrames;
    return v;
}

static void DrainGpu()
{
    if (g.queue == nullptr || g.fence12 == nullptr) return;
    const UINT64 v = ++g.fence_value;
    g.queue->Signal(g.fence12, v);
    if (g.fence12->GetCompletedValue() < v && g.fence_event != nullptr)
    {
        g.fence12->SetEventOnCompletion(v, g.fence_event);
        if (WaitForSingleObject(g.fence_event, 5000) != WAIT_OBJECT_0)
            Log("[feed] timed out draining the queue before teardown");
    }
    for (int i = 0; i < Feed::kFrames; ++i) g.alloc_fence[i] = 0;
}

// NGX can access-violate inside its own code or inside the DLSS 5 add-on (a leaked, closed-source
// snippet), especially across a resolution or device change. SEH keeps that from taking the game
// down -- it becomes a graceful disable instead. These wrappers hold no C++ objects, so __try is
// legal here under /EHsc (same approach as the dlss5-dx11-bridge).
static NVSDK_NGX_Result SafeCreateDLSS(NVSDK_NGX_DLSS_Create_Params *cp, DWORD *code)
{
    *code = 0;
    __try { return NGX_D3D12_CREATE_DLSS_EXT(g.list, 1, 1, &g.feature, g.params, cp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return static_cast<NVSDK_NGX_Result>(0x7FFFFFFF); }
}

static NVSDK_NGX_Result SafeEvaluateDLSS(NVSDK_NGX_D3D12_DLSS_Eval_Params *ep, DWORD *code)
{
    *code = 0;
    __try { return NGX_D3D12_EVALUATE_DLSS_EXT(g.list, g.feature, g.params, ep); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return static_cast<NVSDK_NGX_Result>(0x7FFFFFFF); }
}

static void CloseListGuarded()
{
    __try { g.list->Close(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// NGX crashed while recording into our list: it may hold half-written commands, and
// executing those is what actually takes the game down (the driver faults later on
// another thread). Close it guarded, throw it away WITHOUT executing, replace it.
static void MvProbeAbort();   // defined with the motion-vector probe below

static void AbortCommands()
{
    if (g.list == nullptr) return;
    MvProbeAbort();   // a probe copy recorded into this list will never execute
    CloseListGuarded();
    SafeRelease(g.list);
    if (g.alloc[g.frame_slot] != nullptr && g.dev12 != nullptr &&
        SUCCEEDED(g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[g.frame_slot], nullptr,
                                             __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g.list))))
        g.list->Close();
    else
        Log("[feed] could not replace the aborted command list");
}

// ---------------------------------------------------------------------------
// Motion-vector probe. Every kMvProbeEvery frames, copy a 64x64 block from the centre of
// the MV texture DLSS is about to read into a readback buffer, and analyse the PREVIOUS
// probe's block -- submitted hundreds of frames ago, so the map never stalls the GPU.
// It answers, in pixels, the question no other signal can: is the selected provider
// actually delivering vectors, or is DLSS being fed zeros?
// ---------------------------------------------------------------------------

static void Barrier(ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);   // defined below

static const UINT kMvProbeSize  = 64;
static const UINT kMvProbePitch = 256;   // 64 texels of R16G16_FLOAT = 256 bytes = the D3D12 row-pitch alignment
static const UINT kMvProbeEvery = 600;

static ID3D12Resource *g_mv_probe_buf;
static UINT64          g_mv_probe_fence;    // fence value that completes the pending copy; 0 = nothing pending
static UINT64          g_mv_probe_frames;
static char            g_mv_probe[200] = "no motion-vector probe yet (first one after 600 frames)";

static float HalfToFloat(uint16_t h)
{
    const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    uint32_t bits;
    if (e == 0)       bits = m == 0 ? s : 0;                      // zero / denormal (treated as 0)
    else if (e == 31) bits = s | 0x7F800000u | (m << 13);        // inf / nan
    else              bits = s | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &bits, 4);
    return f;
}

static void MvProbeAnalyse()
{
    void *p = nullptr;
    const D3D12_RANGE read = { 0, kMvProbePitch * kMvProbeSize };
    if (FAILED(g_mv_probe_buf->Map(0, &read, &p)) || p == nullptr) return;
    double sum = 0.0, maxlen = 0.0;
    int nonzero = 0;
    for (UINT y = 0; y < kMvProbeSize; ++y)
    {
        const uint16_t *row = reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(p) + y * kMvProbePitch);
        for (UINT x = 0; x < kMvProbeSize; ++x)
        {
            const float mx = HalfToFloat(row[x * 2]), my = HalfToFloat(row[x * 2 + 1]);
            const double len = sqrt(static_cast<double>(mx) * mx + static_cast<double>(my) * my);
            if (len != len) continue;   // NaN
            sum += len;
            if (len > maxlen) maxlen = len;
            if (len > 1e-4) ++nonzero;
        }
    }
    const D3D12_RANGE none = { 0, 0 };
    g_mv_probe_buf->Unmap(0, &none);
    const int total = static_cast<int>(kMvProbeSize * kMvProbeSize);
    _snprintf_s(g_mv_probe, sizeof(g_mv_probe), _TRUNCATE,
                "MV probe (centre 64x64, frame %llu): mean |mv| %.3f px, max %.2f px, %d%% non-zero%s",
                static_cast<unsigned long long>(g_mv_probe_frames), sum / total, maxlen, nonzero * 100 / total,
                nonzero * 100 / total < 2 ? "  <-- DLSS is getting (almost) no motion vectors" : "");
    Log("[feed] %s", g_mv_probe);
}

// Call while recording, with the MV texture in 'state' (it is returned to that state).
static void MvProbeRecord(ID3D12Resource *mv, D3D12_RESOURCE_STATES state)
{
    if (mv == nullptr || g.dev12 == nullptr || g.list == nullptr || g.fence12 == nullptr) return;
    if ((++g_mv_probe_frames % kMvProbeEvery) != 0) return;
    if (g.width < kMvProbeSize || g.height < kMvProbeSize) return;

    if (g_mv_probe_fence != 0)
    {
        if (g.fence12->GetCompletedValue() < g_mv_probe_fence) return;   // still in flight (should not happen at this cadence)
        MvProbeAnalyse();
        g_mv_probe_fence = 0;
    }
    if (g_mv_probe_buf == nullptr)
    {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC   rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = kMvProbePitch * kMvProbeSize;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_mv_probe_buf))))
            return;
    }

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = mv; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = g_mv_probe_buf; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16_FLOAT, kMvProbeSize, kMvProbeSize, 1, kMvProbePitch };
    const UINT x0 = (g.width - kMvProbeSize) / 2, y0 = (g.height - kMvProbeSize) / 2;
    const D3D12_BOX box = { x0, y0, 0, x0 + kMvProbeSize, y0 + kMvProbeSize, 1 };

    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) Barrier(mv, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) Barrier(mv, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    g_mv_probe_fence = g.fence_value + 1;   // exactly what EndCommands() signals for this list
}

static void MvProbeAbort()
{
    g_mv_probe_fence = 0;
}

static void MvProbeShutdown()
{
    SafeRelease(g_mv_probe_buf);
    g_mv_probe_fence = 0;
}

static void SafeReleaseFeature(NVSDK_NGX_Handle *f)
{
    if (f == nullptr) return;
    __try { NVSDK_NGX_D3D12_ReleaseFeature(f); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("[feed] ReleaseFeature raised exception 0x%08X (ignored)", GetExceptionCode()); }
}

static void Barrier(ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g.list->ResourceBarrier(1, &b);
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

static void ReleaseFrameResources()
{
    DrainGpu();
    // Vulkan transport: drop our raw VkImage imports (the memory is the D3D12 resource's;
    // freeing the import does not free the D3D12 resource, which SafeRelease(tex12) does).
    if (g.vk.ok)
        for (int i = 0; i < SLOT_COUNT; ++i)
        {
            if (g.vk_img[i] != VK_NULL_HANDLE) { g.vk.DestroyImage(g.vk.dev, g.vk_img[i], nullptr); g.vk_img[i] = VK_NULL_HANDLE; }
            if (g.vk_mem[i] != VK_NULL_HANDLE) { g.vk.FreeMemory(g.vk.dev, g.vk_mem[i], nullptr);   g.vk_mem[i] = VK_NULL_HANDLE; }
        }
    // OpenGL transport: same idea -- deleting the texture and its memory object drops
    // our alias, not the D3D12 resource behind it. GL objects can only be deleted from
    // the context they live in; from anywhere else they are left to the driver, which
    // reclaims them with the context.
    if (g.gl.ok)
    {
        if (g.gl.wglGetCurrentContext() == g.gl_ctx && g.gl_ctx != nullptr)
        {
            g.gl.Finish();   // the shared textures must be idle before the D3D12 side goes
            for (int i = 0; i < SLOT_COUNT; ++i)
            {
                if (g.gl_tex[i]    != 0) { g.gl.DeleteTextures(1, &g.gl_tex[i]);           g.gl_tex[i]    = 0; }
                if (g.gl_memobj[i] != 0) { g.gl.DeleteMemoryObjectsEXT(1, &g.gl_memobj[i]); g.gl_memobj[i] = 0; }
            }
        }
        else
        {
            bool any = false;
            for (int i = 0; i < SLOT_COUNT; ++i) if (g.gl_tex[i] != 0) { any = true; g.gl_tex[i] = 0; g.gl_memobj[i] = 0; }
            if (any) Log("[feed] the GL context is not current here; the imported textures are left to the driver");
        }
    }
    for (int i = 0; i < SLOT_COUNT; ++i)
        if (g.tex_shared_ext[i] != nullptr) { CloseHandle(g.tex_shared_ext[i]); g.tex_shared_ext[i] = nullptr; }
    g.vk_layout_init = false;
    SafeRelease(g.output_srv);
    SafeRelease(g.color_stage_srv);
    SafeRelease(g.color_stage);
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        SafeRelease(g.input_rtv[i]);
        SafeRelease(g.tex11[i]);
        SafeRelease(g.tex12[i]);
        if (g.shared[i] != nullptr) { CloseHandle(g.shared[i]); g.shared[i] = nullptr; }
    }
    if (g.feature != nullptr)
    {
        Breadcrumb("releasing the DLSS feature");
        if (!g_ngx_dying) SafeReleaseFeature(g.feature);
        g.feature = nullptr;
    }
    g.frame_ready = false;
}

// One texture visible to both APIs: created on D3D12 and opened on D3D11, or the other
// way round if the driver refuses (WD2's driver only accepted the second route).
static bool MakeSharedPair(ID3D11Device1 *dev1, int i, UINT w, UINT h, DXGI_FORMAT fmt, bool uav,
                           bool render_target = false)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                          (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE) |
                          (render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);

    HRESULT hr = g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                  nullptr, __uuidof(ID3D12Resource),
                                                  reinterpret_cast<void **>(&g.tex12[i]));
    if (SUCCEEDED(hr))
        hr = g.dev12->CreateSharedHandle(g.tex12[i], nullptr, GENERIC_ALL, nullptr, &g.shared[i]);
    if (SUCCEEDED(hr))
        hr = dev1->OpenSharedResource1(g.shared[i], __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&g.tex11[i]));
    if (SUCCEEDED(hr))
    {
        Log("[feed] %-6s %ux%u %s via D3D12->D3D11", kSlotName[i], w, h, FormatName(fmt));
        return true;
    }
    Log("[feed] %s: D3D12->D3D11 path failed 0x%08X, trying the other direction", kSlotName[i], hr);
    SafeRelease(g.tex11[i]);
    SafeRelease(g.tex12[i]);
    if (g.shared[i] != nullptr) { CloseHandle(g.shared[i]); g.shared[i] = nullptr; }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE |
                          (uav ? D3D11_BIND_UNORDERED_ACCESS : 0) |
                          (render_target ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    hr = dev1->CreateTexture2D(&td, nullptr, &g.tex11[i]);
    if (FAILED(hr)) { Log("[feed] %s: CreateTexture2D failed 0x%08X", kSlotName[i], hr); return false; }

    IDXGIResource1 *dxgi_res = nullptr;
    hr = g.tex11[i]->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&dxgi_res));
    if (SUCCEEDED(hr))
    {
        hr = dxgi_res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                          &g.shared[i]);
        dxgi_res->Release();
    }
    if (SUCCEEDED(hr))
        hr = g.dev12->OpenSharedHandle(g.shared[i], __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g.tex12[i]));
    if (FAILED(hr)) { Log("[feed] %s: D3D11->D3D12 path failed 0x%08X", kSlotName[i], hr); return false; }

    D3D12_RESOURCE_DESC got = g.tex12[i]->GetDesc();
    Log("[feed] %-6s %ux%u %s via D3D11->D3D12 (d3d12 flags=0x%X%s)", kSlotName[i], w, h, FormatName(fmt), got.Flags,
        (got.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? " UAV" : "");
    if (i == SLOT_OUTPUT && !(got.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        Log("[feed]   *** Output has no UAV flag on the D3D12 side: DLSS cannot write it ***");
    return true;
}

static bool MakeBlitShaders()
{
    if (g.blit_vs != nullptr && g.blit_ps != nullptr && g.resample_ps != nullptr &&
        g.blit_sampler != nullptr && g.point_sampler != nullptr && g.resample_cb != nullptr) return true;

    static const char kSrc[] =
        "Texture2D<float4> src_color : register(t0);\n"
        "Texture2D<float2> src_mv : register(t1);\n"
        "Texture2D<float> src_depth : register(t2);\n"
        "Texture2D<float> src_mask : register(t3);\n"
        "SamplerState linear_smp : register(s0);\n"
        "SamplerState point_smp : register(s1);\n"
        "cbuffer ResampleConstants : register(b0) { float2 mv_scale; float2 _pad; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(VSOut i) : SV_Target { return float4(src_color.Sample(linear_smp, i.uv).rgb, 1.0); }\n"
        "struct ResampleOut { float4 color : SV_Target0; float2 mv : SV_Target1; float depth : SV_Target2; float mask : SV_Target3; };\n"
        "ResampleOut ps_resample(VSOut i) { ResampleOut o;\n"
        "  o.color = src_color.SampleLevel(linear_smp, i.uv, 0);\n"
        "  o.mv = src_mv.SampleLevel(point_smp, i.uv, 0) * mv_scale;\n"
        "  o.depth = src_depth.SampleLevel(point_smp, i.uv, 0);\n"
        "  o.mask = src_mask.SampleLevel(point_smp, i.uv, 0); return o; }\n";

    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (compile == nullptr) { Log("[feed] d3dcompiler_47.dll unavailable"); return false; }

    ID3DBlob *vs = nullptr, *ps = nullptr, *resample = nullptr, *err = nullptr;
    HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "feedblit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { Log("[feed] blit VS compile failed 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : ""); SafeRelease(err); return false; }
    SafeRelease(err);
    hr = compile(kSrc, sizeof(kSrc) - 1, "feedblit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) { Log("[feed] blit PS compile failed 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : ""); SafeRelease(err); SafeRelease(vs); return false; }
    SafeRelease(err);
    hr = compile(kSrc, sizeof(kSrc) - 1, "feedblit", nullptr, nullptr, "ps_resample", "ps_5_0", 0, 0, &resample, &err);
    if (FAILED(hr)) { Log("[feed] resample PS compile failed 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : ""); SafeRelease(err); SafeRelease(vs); SafeRelease(ps); return false; }
    SafeRelease(err);

    hr = g.dev11->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.blit_vs);
    if (SUCCEEDED(hr)) hr = g.dev11->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.blit_ps);
    if (SUCCEEDED(hr)) hr = g.dev11->CreatePixelShader(resample->GetBufferPointer(), resample->GetBufferSize(), nullptr, &g.resample_ps);
    vs->Release();
    ps->Release();
    resample->Release();
    if (FAILED(hr)) { Log("[feed] blit shader creation failed 0x%08X", hr); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(g.dev11->CreateSamplerState(&sd, &g.blit_sampler))) { Log("[feed] blit sampler failed"); return false; }
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(g.dev11->CreateSamplerState(&sd, &g.point_sampler))) { Log("[feed] point sampler failed"); return false; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g.dev11->CreateBuffer(&cbd, nullptr, &g.resample_cb))) { Log("[feed] resample constant buffer failed"); return false; }
    Log("[feed] copy-back and work-resolution resample shaders ready");
    return true;
}

static bool CreateDlssFeature(UINT w, UINT h, bool inverted, bool *crashed);

// A same-size rebuild (warm-up, runtime recreation, cfg knob) only needs a fresh feature:
// the textures stay put, the new feature is created FIRST, and if that fails or crashes
// the old feature keeps working -- a flaky re-create can no longer take the feed down.
// (The DLSS 5 add-on has crashed twice inside a release-then-recreate; never again.)
static bool RecreateFeatureOnly(UINT w, UINT h)
{
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;
    g.hdr = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : IsHdrFormat(g.color_fmt);

    NVSDK_NGX_Handle *old = g.feature;
    g.feature = nullptr;
    bool crashed = false;
    if (CreateDlssFeature(w, h, inverted, &crashed))
    {
        DrainGpu();  // the old feature's last evaluate may still be in flight
        SafeReleaseFeature(old);
        return true;
    }
    g.feature     = old;   // keep what worked
    g.warmup_done = true;  // and stop asking
    g.frame_ready = true;
    Log("[feed] feature re-create %s; keeping the previous feature", crashed ? "crashed (caught)" : "failed");
    return true;
}

static bool BuildResources(UINT w, UINT h, UINT backbuffer_w, UINT backbuffer_h, DXGI_FORMAT bb_fmt)
{
    if (g.session_ready && g_cfg.mode >= 2 && g.feature != nullptr && g.tex12[SLOT_COLOR] != nullptr &&
        w == g.width && h == g.height && backbuffer_w == g.backbuffer_width &&
        backbuffer_h == g.backbuffer_height && bb_fmt == g.bb_fmt)
        return RecreateFeatureOnly(w, h);

    Breadcrumb("building shared textures");
    ReleaseFrameResources();

    ID3D11Device1 *dev1 = nullptr;
    if (FAILED(g.dev11->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1))) || dev1 == nullptr)
    { Log("[feed] ID3D11Device1 unavailable"); return false; }

    g.width      = w;
    g.height     = h;
    g.backbuffer_width  = backbuffer_w;
    g.backbuffer_height = backbuffer_h;
    g.bb_fmt     = bb_fmt;
    g.color_fmt  = TypedColorFormat(bb_fmt);
    g.output_fmt = ResolveOutputFormat(g.color_fmt, g.dev12);
    g.hdr        = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : IsHdrFormat(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    if (g.color_fmt == DXGI_FORMAT_UNKNOWN)
    {
        Log("[feed] backbuffer format %u (%s) is not supported", bb_fmt, FormatName(bb_fmt));
        dev1->Release();
        FeedDisable("unsupported backbuffer format");
        return false;
    }

    bool ok = MakeSharedPair(dev1, SLOT_COLOR,  w, h, g.color_fmt,             false, true)  &&
              MakeSharedPair(dev1, SLOT_OUTPUT, w, h, g.output_fmt,            true,  false) &&
              MakeSharedPair(dev1, SLOT_DEPTH,  w, h, DXGI_FORMAT_R32_FLOAT,   false, true)  &&
              MakeSharedPair(dev1, SLOT_MV,     w, h, DXGI_FORMAT_R16G16_FLOAT, false, true) &&
              MakeSharedPair(dev1, SLOT_MASK,   w, h, DXGI_FORMAT_R8_UNORM,     false, true);
    dev1->Release();
    if (!ok) { ReleaseFrameResources(); return false; }

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format              = g.output_fmt;
    sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(g.dev11->CreateShaderResourceView(g.tex11[SLOT_OUTPUT], &sv, &g.output_srv)))
    { Log("[feed] output SRV creation failed"); ReleaseFrameResources(); return false; }

    // Work resolution below 100%: a native-size, SRV-able copy of the frame to downsample from.
    if (backbuffer_w != w || backbuffer_h != h)
    {
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width      = backbuffer_w;
        sd.Height     = backbuffer_h;
        sd.MipLevels  = 1;
        sd.ArraySize  = 1;
        sd.Format     = bb_fmt;              // exact backbuffer format, so CopyResource accepts it
        sd.SampleDesc.Count = 1;
        sd.Usage      = D3D11_USAGE_DEFAULT;
        sd.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g.dev11->CreateTexture2D(&sd, nullptr, &g.color_stage)))
        { Log("[feed] work-resolution staging texture failed (%ux%u %s)", backbuffer_w, backbuffer_h, FormatName(bb_fmt)); ReleaseFrameResources(); return false; }

        D3D11_SHADER_RESOURCE_VIEW_DESC ss = {};
        ss.Format              = g.color_fmt;   // typed view, in case the backbuffer is TYPELESS
        ss.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        ss.Texture2D.MipLevels = 1;
        if (FAILED(g.dev11->CreateShaderResourceView(g.color_stage, &ss, &g.color_stage_srv)))
        { Log("[feed] work-resolution staging SRV failed"); ReleaseFrameResources(); return false; }

        Log("[feed] work-resolution source: %ux%u staging copy -> %ux%u", backbuffer_w, backbuffer_h, w, h);
    }

    const int input_slots[] = { SLOT_COLOR, SLOT_MV, SLOT_DEPTH, SLOT_MASK };
    for (const int slot : input_slots)
    {
        D3D11_RENDER_TARGET_VIEW_DESC rv = {};
        rv.Format = slot == SLOT_COLOR ? g.color_fmt :
                    (slot == SLOT_MV ? DXGI_FORMAT_R16G16_FLOAT :
                    (slot == SLOT_DEPTH ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8_UNORM));
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g.dev11->CreateRenderTargetView(g.tex11[slot], &rv, &g.input_rtv[slot])))
        { Log("[feed] %s input RTV creation failed", kSlotName[slot]); ReleaseFrameResources(); return false; }
    }

    if (!MakeBlitShaders()) { ReleaseFrameResources(); return false; }

    if (g_cfg.mode < 2) { g.frame_ready = true; g.need_reset = true; Log("[feed] transport ready (mode %d, no NGX feature)", g_cfg.mode); return true; }

    bool crashed = false;
    if (!CreateDlssFeature(w, h, inverted, &crashed))
    {
        if (crashed) FeedDisable("creating the DLSS feature crashed (the DLSS 5 add-on may be incompatible)");
        return false;
    }
    return true;
}

// The DLSS contract, shared by the D3D11 and D3D12 paths. DLAA: render size == output
// size, no jitter, MVs at render size. The DLSS 5 add-on captures this create inline.
static bool CreateDlssFeature(UINT w, UINT h, bool inverted, bool *crashed)
{
    if (crashed != nullptr) *crashed = false;
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (inverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (g.hdr)    flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (g_cfg.flags >= 0) flags = g_cfg.flags;
    g.create_flags = flags;

    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth            = w;
    cp.Feature.InHeight           = h;
    cp.Feature.InTargetWidth      = w;
    cp.Feature.InTargetHeight     = h;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags       = flags;
    cp.InEnableOutputSubrects     = false;

    // Render-preset hint: presets differ in how aggressively history is clamped, which is
    // both a diagnostic and a partial mitigation for warping around transparents (dust,
    // flames) whose optical-flow vectors drag the background along. K=11 transformer is
    // the modern default; E=5/F=6 are the legacy CNN presets with stronger clamping.
    if (g_cfg.preset > 0)
    {
        g.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, static_cast<unsigned int>(g_cfg.preset));
        Log("[feed] DLSS render preset hint: %d (%s)", g_cfg.preset,
            g_cfg.preset == 5 ? "E" : g_cfg.preset == 6 ? "F" : g_cfg.preset == 10 ? "J" : g_cfg.preset == 11 ? "K" : "?");
    }

    if (!BeginCommands()) { Log("[feed] could not start a command list"); return false; }
    Breadcrumb("creating the DLSS feature");
    DWORD ccode = 0;
    NVSDK_NGX_Result rf = SafeCreateDLSS(&cp, &ccode);
    if (ccode != 0)
    {
        AbortCommands();  // half-recorded NGX work must never reach the GPU
        Log("[feed] CreateFeature raised exception 0x%08X (caught; nothing was submitted)", ccode);
        if (crashed != nullptr) *crashed = true;
        return false;
    }
    const UINT64 v = EndCommands();
    if (g.fence12->GetCompletedValue() < v)
    {
        g.fence12->SetEventOnCompletion(v, g.fence_event);
        if (WaitForSingleObject(g.fence_event, 4000) != WAIT_OBJECT_0)
        {
            Log("[feed] feature creation did not complete within 4 s");
            FeedDisable("creating the DLSS feature hung");
            return false;
        }
    }
    if (NVSDK_NGX_FAILED(rf) || g.feature == nullptr)
    {
        Log("[feed] CreateFeature failed 0x%08X (%s)", rf, NgxResultName(rf));
        g.feature = nullptr;
        return false;
    }

    Log("[feed] feature ready: %ux%u DLAA, flags=%d (%s%s%s%s), color %s -> output %s, depth R32_FLOAT%s, mv R16G16_FLOAT",
        w, h, flags,
        (flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) ? "HDR " : "SDR ",
        (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) ? "MVLowRes " : "",
        (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) ? "DepthInverted " : "",
        (flags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) ? "AutoExposure" : "",
        FormatName(g.color_fmt), FormatName(g.output_fmt), inverted ? " (reversed)" : "");
    g.need_reset  = true;
    g.frame_ready = true;
    return true;
}

// ---------------------------------------------------------------------------
// Session: private D3D12 device + NGX
// ---------------------------------------------------------------------------

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice_)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

static bool InitSession(ID3D11Device *dev11, ID3D11DeviceContext *ctx)
{
    Breadcrumb("opening the D3D12 session");
    Log("################ feed: opening D3D12 session ################");
    g_ngx_dying = false;
    g.dev11 = dev11;

    IDXGIDevice  *dxgi_dev = nullptr;
    IDXGIAdapter *adapter  = nullptr;
    if (SUCCEEDED(dev11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_dev))) && dxgi_dev)
    {
        dxgi_dev->GetAdapter(&adapter);
        dxgi_dev->Release();
    }
    if (adapter != nullptr)
    {
        DXGI_ADAPTER_DESC ad = {};
        adapter->GetDesc(&ad);
        Log("[feed] adapter: %ls  vram=%llu MB", ad.Description, (unsigned long long)(ad.DedicatedVideoMemory >> 20));
    }

    // Loaded here, not imported: ReShade installs its D3D12 hooks when the library arrives,
    // and those hooks are what let the DLSS 5 add-on see this device.
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    if (create_device == nullptr) { Log("[feed] no D3D12CreateDevice"); goto fail; }

    {
        HRESULT hr = create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&g.dev12));
        if (FAILED(hr) || g.dev12 == nullptr) { Log("[feed] D3D12CreateDevice failed 0x%08X", hr); goto fail; }
        g.dev12_owned = true;

        wchar_t data_path[MAX_PATH] = {};
        GetModuleFileNameW(g_self, data_path, MAX_PATH);
        if (wchar_t *s = wcsrchr(data_path, L'\\')) *(s + 1) = L'\0';

        Breadcrumb("initialising NGX on D3D12");
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, data_path, g.dev12, nullptr, NVSDK_NGX_Version_API);
        Log("[feed] NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", r, NgxResultName(r));
        if (NVSDK_NGX_FAILED(r))
        {
            r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                    "1.0", data_path, g.dev12, nullptr, NVSDK_NGX_Version_API);
            Log("[feed] NVSDK_NGX_D3D12_Init_with_ProjectID -> 0x%08X (%s)", r, NgxResultName(r));
        }
        if (NVSDK_NGX_FAILED(r)) { Log("[feed] NGX would not initialise on this device/driver"); goto fail; }
        g.ngx_inited = true;

        NVSDK_NGX_Parameter *caps = nullptr;
        r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
        if (NVSDK_NGX_SUCCEED(r) && caps != nullptr)
        {
            int avail = 0, needs_driver = 0, maj = 0, min = 0;
            caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
            caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
            caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &maj);
            caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min);
            Log("[feed] NGX capabilities: SuperSampling.Available=%d NeedsUpdatedDriver=%d MinDriver=%d.%d", avail,
                needs_driver, maj, min);
            if (!avail) { Log("[feed] DLSS super sampling is not available on this GPU/driver"); goto fail; }
        }
        else
            Log("[feed] capability query failed 0x%08X (%s); continuing", r, NgxResultName(r));

        r = NVSDK_NGX_D3D12_AllocateParameters(&g.params);
        if (NVSDK_NGX_FAILED(r) || g.params == nullptr) { Log("[feed] AllocateParameters failed 0x%08X", r); goto fail; }

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g.dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&g.queue));
        for (int i = 0; i < Feed::kFrames; ++i)
            g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                            reinterpret_cast<void **>(&g.alloc[i]));
        if (g.alloc[0] != nullptr)
            g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr,
                                       __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g.list));
        if (g.list != nullptr) g.list->Close();
        g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        HANDLE fh = nullptr;
        hr = g.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g.fence12));
        if (SUCCEEDED(hr)) hr = g.dev12->CreateSharedHandle(g.fence12, nullptr, GENERIC_ALL, nullptr, &fh);
        ID3D11Device5 *dev5 = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(dev11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5))) && dev5)
        {
            hr = dev5->OpenSharedFence(fh, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&g.fence11));
            dev5->Release();
        }
        if (fh != nullptr) CloseHandle(fh);
        if (FAILED(hr) || g.fence11 == nullptr) { Log("[feed] shared fence setup failed 0x%08X", hr); goto fail; }

        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&g.ctx4))) || g.ctx4 == nullptr)
        { Log("[feed] ID3D11DeviceContext4 unavailable"); goto fail; }

        if (g.queue == nullptr || g.list == nullptr) { Log("[feed] D3D12 queue/list creation failed"); goto fail; }

        Log("[feed] session ready: queue=%p list=%p fence12=%p fence11=%p", (void *)g.queue, (void *)g.list,
            (void *)g.fence12, (void *)g.fence11);
        Log("############# feed: session open #############");
        if (adapter != nullptr) adapter->Release();
        g.session_ready = true;
        return true;
    }

fail:
    if (adapter != nullptr) adapter->Release();
    FeedDisable("the D3D12/NGX session failed to start");
    return false;
}

static void ShutdownSession()
{
    ReleaseFrameResources();
    if (g.params != nullptr) { if (!g_ngx_dying) NVSDK_NGX_D3D12_DestroyParameters(g.params); g.params = nullptr; }
    if (g.ngx_inited && g.dev12 != nullptr) { if (!g_ngx_dying) NVSDK_NGX_D3D12_Shutdown1(g.dev12); g.ngx_inited = false; }
    SafeRelease(g.blit_vs);
    SafeRelease(g.blit_ps);
    SafeRelease(g.resample_ps);
    SafeRelease(g.blit_sampler);
    SafeRelease(g.point_sampler);
    SafeRelease(g.resample_cb);
    SafeRelease(g.ctx4);
    SafeRelease(g.fence11);
    SafeRelease(g.fence12);
    if (g.fence_event != nullptr) { CloseHandle(g.fence_event); g.fence_event = nullptr; }
    SafeRelease(g.list);
    for (int i = 0; i < Feed::kFrames; ++i) SafeRelease(g.alloc[i]);
    MvProbeShutdown();
    SafeRelease(g.queue);
    SafeRelease(g.dev12);
    g.session_ready = false;
    g.dev11 = nullptr;
    g.rs_queue = nullptr;
    if (g.vk.ok)
    {
        if (g.vk_sem_in  != VK_NULL_HANDLE) { g.vk.DestroySemaphore(g.vk.dev, g.vk_sem_in,  nullptr); g.vk_sem_in  = VK_NULL_HANDLE; }
        if (g.vk_sem_out != VK_NULL_HANDLE) { g.vk.DestroySemaphore(g.vk.dev, g.vk_sem_out, nullptr); g.vk_sem_out = VK_NULL_HANDLE; }
    }
    if (g.gl.ok)
    {
        if (g.gl.wglGetCurrentContext() == g.gl_ctx && g.gl_ctx != nullptr)
        {
            if (g.gl_sem_in   != 0) { g.gl.DeleteSemaphoresEXT(1, &g.gl_sem_in);   g.gl_sem_in   = 0; }
            if (g.gl_sem_out  != 0) { g.gl.DeleteSemaphoresEXT(1, &g.gl_sem_out);  g.gl_sem_out  = 0; }
            if (g.gl_fbo_read != 0) { g.gl.DeleteFramebuffers(1, &g.gl_fbo_read);  g.gl_fbo_read = 0; }
            if (g.gl_fbo_draw != 0) { g.gl.DeleteFramebuffers(1, &g.gl_fbo_draw);  g.gl_fbo_draw = 0; }
        }
        else if (g.gl_sem_in != 0 || g.gl_fbo_read != 0)
        {
            Log("[feed] the GL context is not current here; the semaphores and FBOs are left to the driver");
            g.gl_sem_in = g.gl_sem_out = g.gl_fbo_read = g.gl_fbo_draw = 0;
        }
        g.gl = {};
    }
    g.gl_ctx = nullptr;
    g.rs_fence_in = {}; g.rs_fence_out = {};
    SafeRelease(g.fence12_in);
    SafeRelease(g.fence12_out);
    if (g.fence_in_handle  != nullptr) { CloseHandle(g.fence_in_handle);  g.fence_in_handle  = nullptr; }
    if (g.fence_out_handle != nullptr) { CloseHandle(g.fence_out_handle); g.fence_out_handle = nullptr; }
    g.rs_dev   = nullptr;
    g.vk_frame = 0;
    g.gl_frame = 0;
}

// ---------------------------------------------------------------------------
// Session, D3D12 same-device: NGX runs on the game's own device and queue -- the
// DLSS 5 add-on's native scenario (it watches every D3D12 device ReShade knows).
// No transport at all: MV and depth are consumed zero-copy from the effect
// textures; only the backbuffer is copied (swapchain buffers are not reliably
// shader-readable, and DLSS needs Output != Color anyway).
// ---------------------------------------------------------------------------

static bool InitSession12(reshade::api::effect_runtime *rt)
{
    Breadcrumb("opening the same-device D3D12 session");
    Log("################ feed: opening same-device D3D12 session ################");
    g_ngx_dying = false;

    reshade::api::device *dev_api = rt->get_device();
    auto *dev = reinterpret_cast<ID3D12Device *>(dev_api->get_native());
    g.rs_queue = rt->get_command_queue();
    auto *queue = g.rs_queue != nullptr ? reinterpret_cast<ID3D12CommandQueue *>(g.rs_queue->get_native()) : nullptr;
    if (dev == nullptr || queue == nullptr)
    {
        Log("[feed] no native D3D12 device/queue");
        FeedDisable("the game's D3D12 device/queue is not reachable");
        return false;
    }

    dev->AddRef();
    g.dev12 = dev;
    g.dev12_owned = false;
    queue->AddRef();
    g.queue = queue;

    wchar_t data_path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, data_path, MAX_PATH);
    if (wchar_t *s = wcsrchr(data_path, L'\\')) *(s + 1) = L'\0';

    Breadcrumb("initialising NGX on the game's device");
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, data_path, g.dev12, nullptr, NVSDK_NGX_Version_API);
    Log("[feed] NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", r, NgxResultName(r));
    if (NVSDK_NGX_FAILED(r))
    {
        r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                "1.0", data_path, g.dev12, nullptr, NVSDK_NGX_Version_API);
        Log("[feed] NVSDK_NGX_D3D12_Init_with_ProjectID -> 0x%08X (%s)", r, NgxResultName(r));
    }
    if (NVSDK_NGX_FAILED(r))
    {
        ShutdownSession();
        FeedDisable("NGX would not initialise on the game's device");
        return false;
    }
    g.ngx_inited = true;

    NVSDK_NGX_Parameter *caps = nullptr;
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_SUCCEED(r) && caps != nullptr)
    {
        int avail = 0;
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
        Log("[feed] NGX capabilities: SuperSampling.Available=%d", avail);
        if (!avail)
        {
            ShutdownSession();
            FeedDisable("DLSS is not available on this GPU/driver");
            return false;
        }
    }
    else
        Log("[feed] capability query failed 0x%08X (%s); continuing", r, NgxResultName(r));

    r = NVSDK_NGX_D3D12_AllocateParameters(&g.params);
    if (NVSDK_NGX_FAILED(r) || g.params == nullptr)
    {
        Log("[feed] AllocateParameters failed 0x%08X", r);
        ShutdownSession();
        FeedDisable("NGX parameter allocation failed");
        return false;
    }

    // Our own allocators + list on the game's device; submission goes to the game's queue.
    for (int i = 0; i < Feed::kFrames; ++i)
        g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                        reinterpret_cast<void **>(&g.alloc[i]));
    if (g.alloc[0] != nullptr)
        g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr,
                                   __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g.list));
    if (g.list != nullptr) g.list->Close();
    g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g.fence12));
    if (g.list == nullptr || g.fence12 == nullptr || g.fence_event == nullptr)
    {
        Log("[feed] D3D12 list/fence creation failed");
        ShutdownSession();
        FeedDisable("could not create the D3D12 objects");
        return false;
    }

    Log("[feed] session ready (same-device): dev=%p queue=%p list=%p fence=%p", (void *)g.dev12, (void *)g.queue,
        (void *)g.list, (void *)g.fence12);
    Log("############# feed: session open (same-device D3D12) #############");
    g.session_ready = true;
    return true;
}

static bool MakeTex12(int i, UINT w, UINT h, DXGI_FORMAT fmt, bool uav, D3D12_RESOURCE_STATES initial)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    const HRESULT hr = g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial, nullptr,
                                                        __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g.tex12[i]));
    if (FAILED(hr)) { Log("[feed] %s: CreateCommittedResource failed 0x%08X", kSlotName[i], hr); return false; }
    Log("[feed] %-6s %ux%u %s on the game's device%s", kSlotName[i], w, h, FormatName(fmt), uav ? " (UAV)" : "");
    return true;
}

static bool BuildResources12(UINT w, UINT h, DXGI_FORMAT bb_fmt)
{
    if (g.session_ready && g_cfg.mode >= 2 && g.feature != nullptr && g.tex12[SLOT_COLOR] != nullptr &&
        w == g.width && h == g.height && bb_fmt == g.bb_fmt)
        return RecreateFeatureOnly(w, h);

    Breadcrumb("building same-device textures");
    ReleaseFrameResources();

    g.width      = w;
    g.height     = h;
    g.bb_fmt     = bb_fmt;
    g.color_fmt  = TypedColorFormat(bb_fmt);
    g.output_fmt = g.color_fmt;   // the copy home is a plain CopyResource; no blit on this path
    g.hdr        = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : IsHdrFormat(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    if (g.color_fmt == DXGI_FORMAT_UNKNOWN)
    {
        Log("[feed] backbuffer format %u (%s) is not supported", bb_fmt, FormatName(bb_fmt));
        FeedDisable("unsupported backbuffer format");
        return false;
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { g.output_fmt };
    if (SUCCEEDED(g.dev12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
        (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
        Log("[feed] note: %s reports no typed UAV store on this GPU; the DLSS output may fail", FormatName(g.output_fmt));

    // Rest states: Color sits as a shader resource, Output as a UAV. Every transition away
    // and back goes through ReShade's own barrier API so its state tracking stays right.
    if (!MakeTex12(SLOT_COLOR, w, h, g.color_fmt, false,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
        !MakeTex12(SLOT_OUTPUT, w, h, g.output_fmt, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        ReleaseFrameResources();
        return false;
    }

    if (g_cfg.mode < 2) { g.frame_ready = true; g.need_reset = true; Log("[feed] transport ready (mode %d, no NGX feature)", g_cfg.mode); return true; }

    bool crashed = false;
    if (!CreateDlssFeature(w, h, inverted, &crashed))
    {
        if (crashed) FeedDisable("creating the DLSS feature crashed (the DLSS 5 add-on may be incompatible)");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Session, Vulkan transport: the game renders on Vulkan, but the DLSS 5 add-on
// only hooks D3D12 -- so the evaluate runs on our private D3D12 device exactly as
// on the D3D11 path, and the frame crosses the API boundary through shared NT
// handles. The game-side halves are imported THROUGH ReShade's documented
// shared-handle API; the create_fence/create_resource results below double as the
// PLAN-VULKAN phase-0 probe (they fail cleanly if the device lacks the
// external-memory/semaphore extensions, and the log says exactly which).
// ---------------------------------------------------------------------------

static bool InitSessionVk(reshade::api::effect_runtime *rt)
{
    Breadcrumb("opening the D3D12 session (Vulkan transport)");
    Log("################ feed: opening D3D12 session (Vulkan transport) ################");
    g_ngx_dying = false;

    g.rs_dev   = rt->get_device();
    g.rs_queue = rt->get_command_queue();
    if (g.rs_dev == nullptr || g.rs_queue == nullptr)
    {
        FeedDisable("the ReShade device/queue is not reachable");
        return false;
    }

    // Private D3D12 device, loaded so ReShade hooks it -- that hook is what lets the
    // DLSS 5 add-on see the device (proven on the D3D11 path since Metro; whether it
    // also holds when ReShade is loaded as a Vulkan layer is part of this probe:
    // look for the add-on's "hooks installed" line in ReShade.log).
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    if (create_device == nullptr)
    {
        Log("[feed] no D3D12CreateDevice");
        FeedDisable("d3d12.dll unavailable");
        return false;
    }
    HRESULT hr = create_device(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&g.dev12));
    if (FAILED(hr) || g.dev12 == nullptr)
    {
        Log("[feed] D3D12CreateDevice failed 0x%08X", hr);
        FeedDisable("the private D3D12 device failed");
        return false;
    }
    g.dev12_owned = true;

    wchar_t data_path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, data_path, MAX_PATH);
    if (wchar_t *s = wcsrchr(data_path, L'\\')) *(s + 1) = L'\0';

    Breadcrumb("initialising NGX (Vulkan transport)");
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, data_path, g.dev12, nullptr, NVSDK_NGX_Version_API);
    Log("[feed] NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", r, NgxResultName(r));
    if (NVSDK_NGX_FAILED(r))
    {
        r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                "1.0", data_path, g.dev12, nullptr, NVSDK_NGX_Version_API);
        Log("[feed] NVSDK_NGX_D3D12_Init_with_ProjectID -> 0x%08X (%s)", r, NgxResultName(r));
    }
    if (NVSDK_NGX_FAILED(r))
    {
        ShutdownSession();
        FeedDisable("NGX would not initialise");
        return false;
    }
    g.ngx_inited = true;

    NVSDK_NGX_Parameter *caps = nullptr;
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_SUCCEED(r) && caps != nullptr)
    {
        int avail = 0;
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
        Log("[feed] NGX capabilities: SuperSampling.Available=%d", avail);
        if (!avail)
        {
            ShutdownSession();
            FeedDisable("DLSS is not available on this GPU/driver");
            return false;
        }
    }
    r = NVSDK_NGX_D3D12_AllocateParameters(&g.params);
    if (NVSDK_NGX_FAILED(r) || g.params == nullptr)
    {
        Log("[feed] AllocateParameters failed 0x%08X", r);
        ShutdownSession();
        FeedDisable("NGX parameter allocation failed");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g.dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&g.queue));
    for (int i = 0; i < Feed::kFrames; ++i)
        g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                        reinterpret_cast<void **>(&g.alloc[i]));
    if (g.alloc[0] != nullptr)
        g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr,
                                   __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g.list));
    if (g.list != nullptr) g.list->Close();
    g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g.fence12));
    if (g.queue == nullptr || g.list == nullptr || g.fence12 == nullptr || g.fence_event == nullptr)
    {
        Log("[feed] D3D12 queue/list/fence creation failed");
        ShutdownSession();
        FeedDisable("could not create the D3D12 objects");
        return false;
    }

    // The two cross-API fences: created shared on D3D12, imported into the game's
    // device through ReShade. A D3D12 fence and a Vulkan timeline semaphore are the
    // same kernel object by design, so the frame counter crosses unchanged.
    hr = g.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g.fence12_in));
    if (SUCCEEDED(hr)) hr = g.dev12->CreateSharedHandle(g.fence12_in, nullptr, GENERIC_ALL, nullptr, &g.fence_in_handle);
    if (SUCCEEDED(hr)) hr = g.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g.fence12_out));
    if (SUCCEEDED(hr)) hr = g.dev12->CreateSharedHandle(g.fence12_out, nullptr, GENERIC_ALL, nullptr, &g.fence_out_handle);
    if (FAILED(hr))
    {
        Log("[feed] shared fence creation failed 0x%08X", hr);
        ShutdownSession();
        FeedDisable("shared fence creation failed");
        return false;
    }

    // Import both D3D12 fences into the game's Vulkan device as timeline semaphores,
    // ourselves (ReShade's create_fence imports as the wrong external type). Then wrap
    // the VkSemaphores back into api::fence handles -- in ReShade's Vulkan backend an
    // api::fence handle IS a VkSemaphore -- so queue signal/wait stay inside its locks.
    if (!FeedVkLoad(&g.vk, FeedVkDispatch<VkDevice>(g.rs_dev->get_native())))
    {
        // The KHR external-interop extensions were not enabled at vkCreateDevice. Our
        // vkCreateDevice hook (feed_vk_hook.h) normally appends them; if it never saw
        // this device -- the game resolved vkCreateDevice some way the hook does not
        // cover, or the hook could not be installed -- the out-of-process layer is the
        // fallback.
        Log("[feed] the Vulkan external-memory/semaphore entry points are missing: the KHR external-interop");
        Log("[feed] extensions were not enabled on this device at vkCreateDevice.");
        if (g_vk_create_device_target == nullptr)
            Log("[feed] The add-on's vkCreateDevice hook was NOT installed (see the hook lines above).");
        else if (g_vk_hook_devices == 0)
            Log("[feed] The add-on's vkCreateDevice hook was installed but never called: this game creates its device some way it does not intercept.");
        else
            Log("[feed] The hook did run (%d vkCreateDevice call(s)); check its per-extension lines above for what the driver refused.", g_vk_hook_devices);
        Log("[feed] FALLBACK: launch the game through layer\\run-with-feed-layer.bat (VK_LAYER_feed_vk appends them from outside).");
        ShutdownSession();
        FeedDisable("the Vulkan interop extensions are missing on this device -- see dlss5-feed.log");
        return false;
    }
    g.vk_sem_in  = FeedVkImportFence(&g.vk, g.fence_in_handle);
    g.vk_sem_out = FeedVkImportFence(&g.vk, g.fence_out_handle);
    Log("[feed] D3D12 fence -> Vulkan timeline semaphore import: in=%s out=%s",
        g.vk_sem_in ? "OK" : "FAILED", g.vk_sem_out ? "OK" : "FAILED");
    if (g.vk_sem_in == VK_NULL_HANDLE || g.vk_sem_out == VK_NULL_HANDLE)
    {
        ShutdownSession();
        FeedDisable("cross-API fence import failed (see dlss5-feed.log)");
        return false;
    }
    g.rs_fence_in  = { FeedVkValue(g.vk_sem_in) };
    g.rs_fence_out = { FeedVkValue(g.vk_sem_out) };

    Log("[feed] session ready (Vulkan transport): dev12=%p queue=%p", (void *)g.dev12, (void *)g.queue);
    Log("############# feed: session open (Vulkan transport) #############");
    g.session_ready = true;
    return true;
}

static bool MakeSharedTexVk(int slot, UINT w, UINT h, DXGI_FORMAT fmt, bool uav,
                            reshade::api::resource_usage vk_usage, reshade::api::resource_usage vk_initial)
{
    // D3D12 half: shared committed resource, same shape MakeSharedPair creates.
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                          (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    HRESULT hr = g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                  nullptr, __uuidof(ID3D12Resource),
                                                  reinterpret_cast<void **>(&g.tex12[slot]));
    if (SUCCEEDED(hr))
        hr = g.dev12->CreateSharedHandle(g.tex12[slot], nullptr, GENERIC_ALL, nullptr, &g.tex_shared_ext[slot]);
    if (FAILED(hr))
    {
        Log("[feed] %s: shared D3D12 texture failed 0x%08X", kSlotName[slot], hr);
        return false;
    }

    // Game half: import the D3D12 memory into a VkImage ourselves (raw Vulkan; ReShade
    // would import it as the wrong external type). Kept permanently in GENERAL layout.
    (void)vk_usage; (void)vk_initial;
    const VkFormat vkf = FeedVkFormat(fmt);
    if (vkf == VK_FORMAT_UNDEFINED)
    {
        Log("[feed] %s: no VkFormat mapping for %s", kSlotName[slot], FormatName(fmt));
        return false;
    }
    if (!FeedVkImportImage(&g.vk, g.tex_shared_ext[slot], w, h, vkf, uav, &g.vk_img[slot], &g.vk_mem[slot]))
    {
        Log("[feed] texture import FAILED: %s %ux%u %s (raw Vulkan external-memory import)", kSlotName[slot], w, h, FormatName(fmt));
        return false;
    }
    Log("[feed] %-6s %ux%u %s shared D3D12 -> imported as VkImage", kSlotName[slot], w, h, FormatName(fmt));
    return true;
}

static bool BuildResourcesVk(UINT w, UINT h, DXGI_FORMAT bb_fmt)
{
    if (g.session_ready && g_cfg.mode >= 2 && g.feature != nullptr && g.tex12[SLOT_COLOR] != nullptr &&
        w == g.width && h == g.height && bb_fmt == g.bb_fmt)
        return RecreateFeatureOnly(w, h);

    Breadcrumb("building the Vulkan-shared textures");
    ReleaseFrameResources();

    g.width      = w;
    g.height     = h;
    g.bb_fmt     = bb_fmt;
    g.color_fmt  = TypedColorFormat(bb_fmt);
    g.output_fmt = ResolveOutputFormat(g.color_fmt, g.dev12);
    Log("[feed] copy home: %s (output %s -> backbuffer %s)",
        SameTexelLayout(g.output_fmt, bb_fmt) ? "raw vkCmdCopyImage" : "vkCmdBlitImage (CONVERTS: expect issue #11 washout)",
        FormatName(g.output_fmt), FormatName(bb_fmt));
    g.hdr        = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : IsHdrFormat(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    if (g.color_fmt == DXGI_FORMAT_UNKNOWN)
    {
        Log("[feed] backbuffer format %u (%s) is not supported", bb_fmt, FormatName(bb_fmt));
        FeedDisable("unsupported backbuffer format");
        return false;
    }

    // Rest states keep the shared images permanently copy-ready on the game side:
    // inputs sit in copy_dest, the output in copy_source -- so the per-frame path
    // never has to barrier them there at all.
    const reshade::api::resource_usage copy_rw =
        reshade::api::resource_usage::copy_dest | reshade::api::resource_usage::copy_source;
    if (!MakeSharedTexVk(SLOT_COLOR,  w, h, g.color_fmt,             false, copy_rw, reshade::api::resource_usage::copy_dest) ||
        !MakeSharedTexVk(SLOT_OUTPUT, w, h, g.output_fmt,            true,  copy_rw, reshade::api::resource_usage::copy_source) ||
        !MakeSharedTexVk(SLOT_DEPTH,  w, h, DXGI_FORMAT_R32_FLOAT,   false, copy_rw, reshade::api::resource_usage::copy_dest) ||
        !MakeSharedTexVk(SLOT_MV,     w, h, DXGI_FORMAT_R16G16_FLOAT, false, copy_rw, reshade::api::resource_usage::copy_dest) ||
        !MakeSharedTexVk(SLOT_MASK,   w, h, DXGI_FORMAT_R8_UNORM,     false, copy_rw, reshade::api::resource_usage::copy_dest))
    {
        ReleaseFrameResources();
        return false;
    }

    if (g_cfg.mode < 2) { g.frame_ready = true; g.need_reset = true; Log("[feed] transport ready (mode %d, no NGX feature)", g_cfg.mode); return true; }

    bool crashed = false;
    if (!CreateDlssFeature(w, h, inverted, &crashed))
    {
        if (crashed) FeedDisable("creating the DLSS feature crashed (the DLSS 5 add-on may be incompatible)");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Session, OpenGL transport: the Vulkan path with the import third swapped out.
// The evaluate still runs on a private D3D12 device (the DLSS 5 add-on is
// D3D12-only); what changes is how the game's API gets at the shared textures and
// fences. Unlike Vulkan there is no device hook and no layer: OpenGL has no
// creation-time opt-in, so the interop extensions are simply either in the current
// context's extension string or not -- and if they are not, this frame is not being
// rendered on an NVIDIA GPU, where DLSS could not run anyway.
// ---------------------------------------------------------------------------

static bool InitSessionGl(reshade::api::effect_runtime *rt)
{
    Breadcrumb("opening the D3D12 session (OpenGL transport)");
    Log("################ feed: opening D3D12 session (OpenGL transport) ################");
    g_ngx_dying = false;

    g.rs_dev = rt->get_device();
    if (g.rs_dev == nullptr)
    {
        FeedDisable("the ReShade device is not reachable");
        return false;
    }
    // rs_queue is deliberately left null: the GL path issues zero ReShade API calls
    // per frame (no fence to hand back, so no queue signal/wait to route through it).

    // The GL half first: if the interop extensions are missing there is nothing to
    // set up, and saying so before spinning up NGX keeps the log readable.
    if (!FeedGlLoad(&g.gl))
    {
        Log("[feed] OpenGL interop unavailable: %s", g.gl.missing);
        Log("[feed] renderer=\"%s\" version=\"%s\" context=%p thread=%lu",
            g.gl.renderer, g.gl.version, (void *)(g.gl.wglGetCurrentContext ? g.gl.wglGetCurrentContext() : nullptr),
            GetCurrentThreadId());
        Log("[feed] extension query: %s", g.gl.diag);
        Log("[feed] GL_EXT_memory_object_win32 + GL_EXT_semaphore_win32 are NVIDIA-supported on every");
        Log("[feed] DLSS-capable driver. Their absence means this frame is not being rendered on the");
        Log("[feed] NVIDIA GPU -- on a hybrid laptop, force the game onto it (Windows graphics settings).");
        FeedDisable("the OpenGL interop extensions are missing on the rendering GPU -- see dlss5-feed.log");
        return false;
    }
    g.gl_ctx = g.gl.wglGetCurrentContext();
    Log("[feed] OpenGL: renderer=\"%s\" version=\"%s\" context=%p thread=%lu (interop extensions present)",
        g.gl.renderer, g.gl.version, (void *)g.gl_ctx, GetCurrentThreadId());
    Log("[feed] extension query: %s", g.gl.diag);

    // Private D3D12 device, loaded so ReShade hooks it -- that hook is what lets the
    // DLSS 5 add-on see the device. Proven under dxgi.dll and Vulkan-layer loading;
    // whether it also holds with ReShade loaded as opengl32.dll is read from the
    // add-on's "hooks installed" line in the game's ReShade.log.
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    if (create_device == nullptr)
    {
        Log("[feed] no D3D12CreateDevice");
        FeedDisable("d3d12.dll unavailable");
        return false;
    }
    HRESULT hr = create_device(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&g.dev12));
    if (FAILED(hr) || g.dev12 == nullptr)
    {
        Log("[feed] D3D12CreateDevice failed 0x%08X", hr);
        FeedDisable("the private D3D12 device failed");
        return false;
    }
    g.dev12_owned = true;

    wchar_t data_path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, data_path, MAX_PATH);
    if (wchar_t *s = wcsrchr(data_path, L'\\')) *(s + 1) = L'\0';

    Breadcrumb("initialising NGX (OpenGL transport)");
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, data_path, g.dev12, nullptr, NVSDK_NGX_Version_API);
    Log("[feed] NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", r, NgxResultName(r));
    if (NVSDK_NGX_FAILED(r))
    {
        r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                "1.0", data_path, g.dev12, nullptr, NVSDK_NGX_Version_API);
        Log("[feed] NVSDK_NGX_D3D12_Init_with_ProjectID -> 0x%08X (%s)", r, NgxResultName(r));
    }
    if (NVSDK_NGX_FAILED(r))
    {
        ShutdownSession();
        FeedDisable("NGX would not initialise");
        return false;
    }
    g.ngx_inited = true;

    NVSDK_NGX_Parameter *caps = nullptr;
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_SUCCEED(r) && caps != nullptr)
    {
        int avail = 0;
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
        Log("[feed] NGX capabilities: SuperSampling.Available=%d", avail);
        if (!avail)
        {
            ShutdownSession();
            FeedDisable("DLSS is not available on this GPU/driver");
            return false;
        }
    }
    r = NVSDK_NGX_D3D12_AllocateParameters(&g.params);
    if (NVSDK_NGX_FAILED(r) || g.params == nullptr)
    {
        Log("[feed] AllocateParameters failed 0x%08X", r);
        ShutdownSession();
        FeedDisable("NGX parameter allocation failed");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g.dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&g.queue));
    for (int i = 0; i < Feed::kFrames; ++i)
        g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                        reinterpret_cast<void **>(&g.alloc[i]));
    if (g.alloc[0] != nullptr)
        g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr,
                                   __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g.list));
    if (g.list != nullptr) g.list->Close();
    g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g.fence12));
    if (g.queue == nullptr || g.list == nullptr || g.fence12 == nullptr || g.fence_event == nullptr)
    {
        Log("[feed] D3D12 queue/list/fence creation failed");
        ShutdownSession();
        FeedDisable("could not create the D3D12 objects");
        return false;
    }

    // The two cross-API fences: created shared on D3D12, imported into GL as
    // semaphores whose value is set per use (GL_D3D12_FENCE_VALUE_EXT), so the frame
    // counter crosses unchanged -- a D3D12 fence and a GL "D3D12 fence" semaphore are
    // the same kernel object.
    hr = g.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g.fence12_in));
    if (SUCCEEDED(hr)) hr = g.dev12->CreateSharedHandle(g.fence12_in, nullptr, GENERIC_ALL, nullptr, &g.fence_in_handle);
    if (SUCCEEDED(hr)) hr = g.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g.fence12_out));
    if (SUCCEEDED(hr)) hr = g.dev12->CreateSharedHandle(g.fence12_out, nullptr, GENERIC_ALL, nullptr, &g.fence_out_handle);
    if (FAILED(hr))
    {
        Log("[feed] shared fence creation failed 0x%08X", hr);
        ShutdownSession();
        FeedDisable("shared fence creation failed");
        return false;
    }

    g.gl_sem_in  = FeedGlImportFence(&g.gl, g.fence_in_handle);
    g.gl_sem_out = FeedGlImportFence(&g.gl, g.fence_out_handle);
    Log("[feed] D3D12 fence -> GL semaphore import (GL_HANDLE_TYPE_D3D12_FENCE_EXT): in=%s out=%s",
        g.gl_sem_in ? "OK" : "FAILED", g.gl_sem_out ? "OK" : "FAILED");
    if (g.gl_sem_in == 0 || g.gl_sem_out == 0)
    {
        ShutdownSession();
        FeedDisable("cross-API fence import failed (see dlss5-feed.log)");
        return false;
    }

    // The two persistent FBOs the colour blits attach through.
    g.gl.GenFramebuffers(1, &g.gl_fbo_read);
    g.gl.GenFramebuffers(1, &g.gl_fbo_draw);
    if (g.gl_fbo_read == 0 || g.gl_fbo_draw == 0)
    {
        Log("[feed] glGenFramebuffers failed (GL error 0x%04X)", FeedGlDrainErrors(&g.gl));
        ShutdownSession();
        FeedDisable("could not create the GL framebuffer objects");
        return false;
    }

    Log("[feed] session ready (OpenGL transport): dev12=%p queue=%p glctx=%p", (void *)g.dev12, (void *)g.queue, (void *)g.gl_ctx);
    Log("############# feed: session open (OpenGL transport) #############");
    g.session_ready = true;
    return true;
}

static bool MakeSharedTexGl(int slot, UINT w, UINT h, DXGI_FORMAT fmt, bool uav)
{
    // D3D12 half: byte for byte what MakeSharedTexVk creates.
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                          (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    HRESULT hr = g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                  nullptr, __uuidof(ID3D12Resource),
                                                  reinterpret_cast<void **>(&g.tex12[slot]));
    if (SUCCEEDED(hr))
        hr = g.dev12->CreateSharedHandle(g.tex12[slot], nullptr, GENERIC_ALL, nullptr, &g.tex_shared_ext[slot]);
    if (FAILED(hr))
    {
        Log("[feed] %s: shared D3D12 texture failed 0x%08X", kSlotName[slot], hr);
        return false;
    }

    // Game half: import the D3D12 memory into a GL texture. The size a GL memory
    // object needs is the D3D12 ALLOCATION size, not w*h*bpp -- padding and tiling
    // make the two differ, and the import fails on the wrong one.
    const GLenum glf = FeedGlFormat(fmt);
    if (glf == 0)
    {
        Log("[feed] %s: no GL internal format for %s", kSlotName[slot], FormatName(fmt));
        return false;
    }
    const D3D12_RESOURCE_ALLOCATION_INFO ai = g.dev12->GetResourceAllocationInfo(0, 1, &rd);
    if (!FeedGlImportImage(&g.gl, g.tex_shared_ext[slot], ai.SizeInBytes,
                           static_cast<GLsizei>(w), static_cast<GLsizei>(h), glf,
                           &g.gl_tex[slot], &g.gl_memobj[slot]))
    {
        Log("[feed] texture import FAILED: %s %ux%u %s (GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, %llu bytes, GL error 0x%04X)",
            kSlotName[slot], w, h, FormatName(fmt), static_cast<unsigned long long>(ai.SizeInBytes),
            FeedGlDrainErrors(&g.gl));
        return false;
    }
    Log("[feed] %-6s %ux%u %s shared D3D12 (%llu bytes) -> imported as GL texture %u",
        kSlotName[slot], w, h, FormatName(fmt), static_cast<unsigned long long>(ai.SizeInBytes), g.gl_tex[slot]);
    return true;
}

static bool BuildResourcesGl(UINT w, UINT h, DXGI_FORMAT bb_fmt, uint64_t rtv_handle)
{
    if (g.session_ready && g_cfg.mode >= 2 && g.feature != nullptr && g.tex12[SLOT_COLOR] != nullptr &&
        w == g.width && h == g.height && bb_fmt == g.bb_fmt)
        return RecreateFeatureOnly(w, h);

    Breadcrumb("building the OpenGL-shared textures");
    ReleaseFrameResources();

    g.width      = w;
    g.height     = h;
    g.bb_fmt     = bb_fmt;
    g.color_fmt  = GlSafeColorFormat(TypedColorFormat(bb_fmt));
    g.output_fmt = GlSafeColorFormat(ResolveOutputFormat(g.color_fmt, g.dev12));
    g.hdr        = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : IsHdrFormat(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    if (g.color_fmt == DXGI_FORMAT_UNKNOWN)
    {
        Log("[feed] backbuffer format %u (%s) is not supported", bb_fmt, FormatName(bb_fmt));
        FeedDisable("unsupported backbuffer format");
        return false;
    }

    // What the technique's render target actually is decides which blit branch runs,
    // and its colour encoding decides whether the sRGB trap of issue #11 can bite.
    {
        FeedGlStateGuard guard(&g.gl);
        const GLenum ty = FeedGlHandleType(rtv_handle);
        const GLint enc = FeedGlColorEncoding(&g.gl, g.gl_fbo_read, rtv_handle);
        Log("[feed] technique target: %s (GL object type 0x%04X, name %u), colour encoding %s",
            rtv_handle == 0 ? "the DEFAULT framebuffer" :
            ty == GL_RENDERBUFFER ? "a renderbuffer" :
            ty == GL_TEXTURE_2D ? "a GL_TEXTURE_2D" : "an unexpected GL object",
            ty, FeedGlHandleName(rtv_handle),
            enc == GL_SRGB ? "GL_SRGB (blits stay with GL_FRAMEBUFFER_SRGB off, so the bytes move raw)" :
            enc == GL_LINEAR ? "GL_LINEAR" : "unknown");
    }
    Log("[feed] copy home: glBlitFramebuffer (output %s -> backbuffer %s)",
        FormatName(g.output_fmt), FormatName(bb_fmt));

    if (!MakeSharedTexGl(SLOT_COLOR,  w, h, g.color_fmt,              false) ||
        !MakeSharedTexGl(SLOT_OUTPUT, w, h, g.output_fmt,             true)  ||
        !MakeSharedTexGl(SLOT_DEPTH,  w, h, DXGI_FORMAT_R32_FLOAT,    false) ||
        !MakeSharedTexGl(SLOT_MV,     w, h, DXGI_FORMAT_R16G16_FLOAT, false) ||
        !MakeSharedTexGl(SLOT_MASK,   w, h, DXGI_FORMAT_R8_UNORM,     false))
    {
        ReleaseFrameResources();
        return false;
    }

    if (g_cfg.mode < 2) { g.frame_ready = true; g.need_reset = true; Log("[feed] transport ready (mode %d, no NGX feature)", g_cfg.mode); return true; }

    bool crashed = false;
    if (!CreateDlssFeature(w, h, inverted, &crashed))
    {
        if (crashed) FeedDisable("creating the DLSS feature crashed (the DLSS 5 add-on may be incompatible)");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// D3D11 work-resolution input preparation and copy-back
// ---------------------------------------------------------------------------

static bool CopyOrResampleInputs(ID3D11DeviceContext *ctx,
                                 ID3D11Texture2D *color, ID3D11Texture2D *mv, ID3D11Texture2D *depth,
                                 ID3D11Texture2D *mask, ID3D11ShaderResourceView *color_srv,
                                 ID3D11ShaderResourceView *mv_srv, ID3D11ShaderResourceView *depth_srv,
                                 ID3D11ShaderResourceView *mask_srv, UINT source_w, UINT source_h)
{
    // Below 100% the frame has to be sampled, and neither candidate source can be:
    // ReShade's backbuffer has no D3D11_BIND_SHADER_RESOURCE (CreateShaderResourceView
    // on it fails), and `DLSS5_ColorInput : COLOR` is a semantic texture with no resource
    // of its own, so get_texture_binding() returns a null view for it. So copy the frame
    // into a texture we own and sample that. One native-resolution copy, only below 100%.
    if (source_w != g.width || source_h != g.height)
    {
        if (g.color_stage == nullptr || g.color_stage_srv == nullptr) return false;
        ctx->CopyResource(g.color_stage, color);
        color_srv = g.color_stage_srv;
    }

    if (source_w == g.width && source_h == g.height)
    {
        ctx->CopyResource(g.tex11[SLOT_COLOR], color);
        ctx->CopyResource(g.tex11[SLOT_DEPTH], depth);
        ctx->CopyResource(g.tex11[SLOT_MV], mv);
        if (g.mask_ok) ctx->CopyResource(g.tex11[SLOT_MASK], mask);
        else
        {
            const FLOAT zero[4] = {};
            ctx->ClearRenderTargetView(g.input_rtv[SLOT_MASK], zero);
        }
        return true;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(g.resample_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    { Log("[feed] resample constant-buffer map failed"); return false; }
    const float constants[4] = {
        static_cast<float>(g.width) / static_cast<float>(source_w),
        static_cast<float>(g.height) / static_cast<float>(source_h), 0.0f, 0.0f
    };
    memcpy(mapped.pData, constants, sizeof(constants));
    ctx->Unmap(g.resample_cb, 0);

    ID3D11RenderTargetView *old_rtvs[4] = {};
    ID3D11DepthStencilView *old_dsv = nullptr;
    ID3D11VertexShader *old_vs = nullptr;
    ID3D11PixelShader *old_ps = nullptr;
    ID3D11ShaderResourceView *old_srvs[4] = {};
    ID3D11SamplerState *old_samplers[2] = {};
    ID3D11Buffer *old_cb = nullptr;
    ID3D11InputLayout *old_il = nullptr;
    ID3D11BlendState *old_bs = nullptr; FLOAT old_bf[4] = {}; UINT old_mask = 0;
    ID3D11DepthStencilState *old_ds = nullptr; UINT old_sref = 0;
    ID3D11RasterizerState *old_rs = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY old_topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    UINT nvp = 1; D3D11_VIEWPORT old_vp = {};

    ctx->OMGetRenderTargets(4, old_rtvs, &old_dsv);
    ctx->VSGetShader(&old_vs, nullptr, nullptr);
    ctx->PSGetShader(&old_ps, nullptr, nullptr);
    ctx->PSGetShaderResources(0, 4, old_srvs);
    ctx->PSGetSamplers(0, 2, old_samplers);
    ctx->PSGetConstantBuffers(0, 1, &old_cb);
    ctx->IAGetInputLayout(&old_il);
    ctx->IAGetPrimitiveTopology(&old_topo);
    ctx->OMGetBlendState(&old_bs, old_bf, &old_mask);
    ctx->OMGetDepthStencilState(&old_ds, &old_sref);
    ctx->RSGetState(&old_rs);
    ctx->RSGetViewports(&nvp, &old_vp);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(g.width);
    vp.Height = static_cast<float>(g.height);
    vp.MaxDepth = 1.0f;
    ID3D11RenderTargetView *rtvs[4] = {
        g.input_rtv[SLOT_COLOR], g.input_rtv[SLOT_MV],
        g.input_rtv[SLOT_DEPTH], g.input_rtv[SLOT_MASK]
    };
    ID3D11ShaderResourceView *srvs[4] = { color_srv, mv_srv, depth_srv, mask_srv };
    ID3D11SamplerState *samplers[2] = { g.blit_sampler, g.point_sampler };

    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(4, rtvs, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.blit_vs, nullptr, 0);
    ctx->PSSetShader(g.resample_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 4, srvs);
    ctx->PSSetSamplers(0, 2, samplers);
    ctx->PSSetConstantBuffers(0, 1, &g.resample_cb);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView *null_srvs[4] = {};
    ID3D11RenderTargetView *null_rtvs[4] = {};
    ctx->PSSetShaderResources(0, 4, null_srvs);
    ctx->OMSetRenderTargets(4, null_rtvs, nullptr);

    ctx->OMSetRenderTargets(4, old_rtvs, old_dsv);
    ctx->VSSetShader(old_vs, nullptr, 0);
    ctx->PSSetShader(old_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 4, old_srvs);
    ctx->PSSetSamplers(0, 2, old_samplers);
    ctx->PSSetConstantBuffers(0, 1, &old_cb);
    ctx->IASetInputLayout(old_il);
    ctx->IASetPrimitiveTopology(old_topo);
    ctx->OMSetBlendState(old_bs, old_bf, old_mask);
    ctx->OMSetDepthStencilState(old_ds, old_sref);
    ctx->RSSetState(old_rs);
    if (nvp != 0) ctx->RSSetViewports(1, &old_vp);

    for (auto *p : old_rtvs) SafeRelease(p);
    SafeRelease(old_dsv); SafeRelease(old_vs); SafeRelease(old_ps);
    for (auto *p : old_srvs) SafeRelease(p);
    for (auto *p : old_samplers) SafeRelease(p);
    SafeRelease(old_cb); SafeRelease(old_il); SafeRelease(old_bs); SafeRelease(old_ds); SafeRelease(old_rs);
    return true;
}

static void BlitOutputToBackbuffer(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv)
{
    // Save what we touch; ReShade rebinds its own state for every following pass anyway.
    ID3D11RenderTargetView   *old_rtv = nullptr;
    ID3D11DepthStencilView   *old_dsv = nullptr;
    ID3D11VertexShader       *old_vs  = nullptr;
    ID3D11PixelShader        *old_ps  = nullptr;
    ID3D11ShaderResourceView *old_srv = nullptr;
    ID3D11SamplerState       *old_smp = nullptr;
    ID3D11InputLayout        *old_il  = nullptr;
    ID3D11BlendState         *old_bs  = nullptr; FLOAT old_bf[4]; UINT old_mask = 0;
    ID3D11DepthStencilState  *old_ds  = nullptr; UINT old_sref = 0;
    ID3D11RasterizerState    *old_rs  = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY  old_topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    UINT nvp = 1; D3D11_VIEWPORT old_vp = {};
    ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    ctx->VSGetShader(&old_vs, nullptr, nullptr);
    ctx->PSGetShader(&old_ps, nullptr, nullptr);
    ctx->PSGetShaderResources(0, 1, &old_srv);
    ctx->PSGetSamplers(0, 1, &old_smp);
    ctx->IAGetInputLayout(&old_il);
    ctx->IAGetPrimitiveTopology(&old_topo);
    ctx->OMGetBlendState(&old_bs, old_bf, &old_mask);
    ctx->OMGetDepthStencilState(&old_ds, &old_sref);
    ctx->RSGetState(&old_rs);
    ctx->RSGetViewports(&nvp, &old_vp);

    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(g.backbuffer_width);
    vp.Height   = static_cast<float>(g.backbuffer_height);
    vp.MaxDepth = 1.0f;
    ID3D11RenderTargetView *rtvs[] = { rtv };
    ID3D11ShaderResourceView *srvs[] = { g.output_srv };
    ID3D11SamplerState *smps[] = { g.blit_sampler };
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.blit_vs, nullptr, 0);
    ctx->PSSetShader(g.blit_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, srvs);
    ctx->PSSetSamplers(0, 1, smps);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView *no_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &no_srv);
    ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
    ctx->VSSetShader(old_vs, nullptr, 0);
    ctx->PSSetShader(old_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &old_srv);
    ctx->PSSetSamplers(0, 1, &old_smp);
    ctx->IASetInputLayout(old_il);
    ctx->IASetPrimitiveTopology(old_topo);
    ctx->OMSetBlendState(old_bs, old_bf, old_mask);
    ctx->OMSetDepthStencilState(old_ds, old_sref);
    ctx->RSSetState(old_rs);
    if (nvp) ctx->RSSetViewports(1, &old_vp);
    SafeRelease(old_rtv); SafeRelease(old_dsv); SafeRelease(old_vs); SafeRelease(old_ps); SafeRelease(old_srv);
    SafeRelease(old_smp); SafeRelease(old_il); SafeRelease(old_bs); SafeRelease(old_ds); SafeRelease(old_rs);
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

static void TimingTick(LONGLONG entry, LONGLONG exit)
{
    if (g.qpf == 0)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g.qpf = f.QuadPart;
        g.span_start = entry;
    }
    g.cpu_ticks += (exit - entry);
    if (++g.timed_frames < 600) return;
    const double span_ms = 1000.0 * double(exit - g.span_start) / double(g.qpf);
    const double cpu_ms  = 1000.0 * double(g.cpu_ticks) / double(g.qpf);
    const double n       = double(g.timed_frames);
    Log("[feed] 600 frames: feed CPU %.2f ms/frame | frame interval %.2f ms (%.1f fps) | feed is %.0f%% of the frame",
        cpu_ms / n, span_ms / n, 1000.0 / (span_ms / n), 100.0 * cpu_ms / span_ms);
    g.cpu_ticks = 0;
    g.timed_frames = 0;
    g.span_start = exit;
}

static ID3D11Texture2D *AsTexture2D(ID3D11Resource *res, D3D11_TEXTURE2D_DESC *desc)
{
    if (res == nullptr) return nullptr;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) || tex == nullptr)
        return nullptr;
    tex->GetDesc(desc);
    return tex;  // caller releases
}

// ---------------------------------------------------------------------------
// Per frame, D3D12 same-device: ReShade's own command list carries every barrier
// and copy (so its state tracking stays right); our list carries only the NGX
// evaluate, executed on the game's queue right after ReShade's work is flushed.
// ---------------------------------------------------------------------------

static void FeedFrame12(reshade::api::effect_runtime *rt, reshade::api::command_list *cl, reshade::api::resource_view rtv)
{
    using namespace reshade::api;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    if ((g.frames_done % 60) == 0 && CfgReload()) g.frame_ready = false;
    if (!g_cfg.enabled || g_cfg.mode == 0) return;

    device *dev_api = rt->get_device();

    resource_view mv_srv = {}, mv_srgb = {}, d_srv = {}, d_srgb = {};
    if (g.mv_var.handle != 0)    rt->get_texture_binding(g.mv_var, &mv_srv, &mv_srgb);
    if (g.depth_var.handle != 0) rt->get_texture_binding(g.depth_var, &d_srv, &d_srgb);
    if (mv_srv.handle == 0 || d_srv.handle == 0)
    {
        if (!g.missing_reported)
        {
            g.missing_reported = true;
            Warn("DLSS5_Feed.fx textures not found (technique %s). Install DLSS5_Feed.fx + a texMotionVectors provider and enable both.",
                 g.technique.handle ? "found" : "MISSING");
        }
        return;
    }

    const resource bb_res = dev_api->get_resource_from_view(rtv);
    auto *bb    = reinterpret_cast<ID3D12Resource *>(bb_res.handle);
    auto *mv    = reinterpret_cast<ID3D12Resource *>(dev_api->get_resource_from_view(mv_srv).handle);
    auto *depth = reinterpret_cast<ID3D12Resource *>(dev_api->get_resource_from_view(d_srv).handle);
    if (bb == nullptr || mv == nullptr || depth == nullptr) return;

    // Optional validation mask (older shaders have none); zero-copy like mv/depth.
    ID3D12Resource *mask = nullptr;
    g.mask_ok = false;
    if (g.mask_var.handle != 0)
    {
        resource_view m_srv = {}, m_srgb = {};
        rt->get_texture_binding(g.mask_var, &m_srv, &m_srgb);
        if (m_srv.handle != 0) mask = reinterpret_cast<ID3D12Resource *>(dev_api->get_resource_from_view(m_srv).handle);
        if (mask != nullptr)
        {
            const D3D12_RESOURCE_DESC kd = mask->GetDesc();
            g.mask_ok = kd.Width == bb->GetDesc().Width && kd.Height == bb->GetDesc().Height && kd.Format == DXGI_FORMAT_R8_UNORM;
        }
    }

    const D3D12_RESOURCE_DESC cd = bb->GetDesc(), md = mv->GetDesc(), dd = depth->GetDesc();
    const UINT w = static_cast<UINT>(cd.Width), h = cd.Height;
    if (cd.Width != md.Width || h != md.Height || cd.Width != dd.Width || h != dd.Height ||
        cd.SampleDesc.Count != 1 || md.Format != DXGI_FORMAT_R16G16_FLOAT || dd.Format != DXGI_FORMAT_R32_FLOAT)
    {
        static bool said12 = false;
        if (!said12)
        {
            said12 = true;
            Log("[feed] input mismatch: color %ux%u %s samp=%u | mv %ux%u %s | depth %ux%u %s -- skipping",
                w, h, FormatName(cd.Format), cd.SampleDesc.Count, static_cast<UINT>(md.Width), md.Height,
                FormatName(md.Format), static_cast<UINT>(dd.Width), dd.Height, FormatName(dd.Format));
        }
        return;
    }

    auto *native_dev = reinterpret_cast<ID3D12Device *>(dev_api->get_native());
    if (g.session_ready && !g.dev12_owned && g.dev12 != nullptr && g.dev12 != native_dev)
    {
        Log("[feed] the game recreated its D3D12 device; rebuilding the session");
        ShutdownSession();
    }
    bool ok = g.session_ready || InitSession12(rt);

    // Same hook-arming grace as the D3D11 path: never call into NGX while the DLSS 5
    // add-on may still be patching its vtable (that has crashed the process at EXEC 0x0),
    // and it re-patches after every runtime recreation.
    const bool needs_build12 = !g.frame_ready || w != g.width || h != g.height || cd.Format != g.bb_fmt;
    // Re-arm the grace on a resolution/format change too: that makes the DLSS 5 add-on
    // re-create its own feature, and any NGX interposer downstream (Alex's Toolkit) re-arms
    // with it. Without this the second build races hooks that are only half in place.
    if (g.frame_ready && needs_build12) g.create_grace = 0;
    if (ok && needs_build12 && g.create_grace < g_cfg.create_delay)
    {
        if (++g.create_grace == 1)
            Log("[feed] holding the feature (re)build for %d frames (the DLSS 5 add-on re-arms its hooks asynchronously)",
                g_cfg.create_delay);
        ok = false;
    }

    if (ok && needs_build12)
    {
        Log("[feed] building: %ux%u backbuffer %s (same-device D3D12, depth reversed=%d)", w, h,
            FormatName(cd.Format), g.depth_reversed ? 1 : 0);
        ok = BuildResources12(w, h, cd.Format);
        if (!ok) FeedFail("resource build");
        else g.consecutive_fails = 0;
    }

    if (ok)
    {
        const resource color12  = { reinterpret_cast<uint64_t>(g.tex12[SLOT_COLOR]) };
        const resource output12 = { reinterpret_cast<uint64_t>(g.tex12[SLOT_OUTPUT]) };

        // ReShade renders effects into the backbuffer, so its tracked state here is render_target.
        Breadcrumb("copying the backbuffer (D3D12)");
        {
            const resource       res[2]  = { bb_res, color12 };
            const resource_usage from[2] = { resource_usage::render_target, resource_usage::shader_resource };
            const resource_usage to[2]   = { resource_usage::copy_source, resource_usage::copy_dest };
            cl->barrier(2, res, from, to);
        }
        cl->copy_resource(bb_res, color12);

        if (g_cfg.mode == 1)
        {
            // Transport test: the copied frame goes straight back.
            {
                const resource       res[2]  = { bb_res, color12 };
                const resource_usage from[2] = { resource_usage::copy_source, resource_usage::copy_dest };
                const resource_usage to[2]   = { resource_usage::copy_dest, resource_usage::copy_source };
                cl->barrier(2, res, from, to);
            }
            cl->copy_resource(color12, bb_res);
            {
                const resource       res[2]  = { bb_res, color12 };
                const resource_usage from[2] = { resource_usage::copy_dest, resource_usage::copy_source };
                const resource_usage to[2]   = { resource_usage::render_target, resource_usage::shader_resource };
                cl->barrier(2, res, from, to);
            }
            ++g.frames_done;
        }
        else
        {
            // Park the backbuffer to receive the output; the copy becomes DLSS's colour input.
            {
                const resource       res[2]  = { bb_res, color12 };
                const resource_usage from[2] = { resource_usage::copy_source, resource_usage::copy_dest };
                const resource_usage to[2]   = { resource_usage::copy_dest, resource_usage::shader_resource };
                cl->barrier(2, res, from, to);
            }

            // Everything recorded so far (the motion-vector provider, the feed passes, these copies) goes to
            // the game's queue now; our evaluate follows it on the same queue.
            Breadcrumb("flushing ReShade's command list");
            g.rs_queue->flush_immediate_command_list();

            bool restored = false;
            if (!BeginCommands()) { FeedFail("command list"); }
            else
            {
                const int reset = (g.need_reset || g_cfg.reset_every) ? 1 : 0;
                g.need_reset = false;

                // ReShade parked the effect textures as shader_resource (both SR states on D3D12).
                MvProbeRecord(mv, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
                ep.Feature.pInColor  = g.tex12[SLOT_COLOR];
                ep.Feature.pInOutput = g.tex12[SLOT_OUTPUT];
                ep.Feature.InSharpness = 0.0f;
                ep.pInDepth          = depth;   // the effect textures themselves: zero-copy
                ep.pInMotionVectors  = mv;
                ep.pInBiasCurrentColorMask = g.mask_ok ? mask : nullptr;   // the shader's validation mask
                ep.InJitterOffsetX   = 0.0f;
                ep.InJitterOffsetY   = 0.0f;
                ep.InRenderSubrectDimensions.Width  = g.width;
                ep.InRenderSubrectDimensions.Height = g.height;
                ep.InReset           = reset;
                ep.InMVScaleX        = g_cfg.mv_scale_x;
                ep.InMVScaleY        = g_cfg.mv_scale_y;
                ep.InPreExposure     = 1.0f;
                ep.InExposureScale   = 1.0f;

                Breadcrumb("running the same-device evaluate");
                DWORD ecode = 0;
                NVSDK_NGX_Result re = SafeEvaluateDLSS(&ep, &ecode);
                if (ecode != 0)
                    AbortCommands();  // never execute a list NGX crashed while recording
                else
                    EndCommands();

                if (ecode != 0)
                {
                    Log("[feed] evaluate raised exception 0x%08X (caught; nothing was submitted)", ecode);
                    FeedDisable("the DLSS evaluate crashed (the DLSS 5 add-on may be incompatible with this game/resolution)");
                    g.frame_ready = false;
                }
                else if (NVSDK_NGX_FAILED(re))
                {
                    Log("[feed] evaluate failed 0x%08X (%s)", re, NgxResultName(re));
                    FeedFail("evaluate");
                    g.frame_ready = false;
                }
                else
                {
                    // The copy home is recorded on the (fresh) immediate list: it executes on
                    // the same queue after the evaluate, so no fence is needed.
                    {
                        const resource       res[1]  = { output12 };
                        const resource_usage from[1] = { resource_usage::unordered_access };
                        const resource_usage to[1]   = { resource_usage::copy_source };
                        cl->barrier(1, res, from, to);
                    }
                    cl->copy_resource(output12, bb_res);
                    {
                        const resource       res[2]  = { bb_res, output12 };
                        const resource_usage from[2] = { resource_usage::copy_dest, resource_usage::copy_source };
                        const resource_usage to[2]   = { resource_usage::render_target, resource_usage::unordered_access };
                        cl->barrier(2, res, from, to);
                    }
                    restored = true;

                    const UINT64 n = ++g.frames_done;
                    g.consecutive_fails = 0;
                    if (n <= static_cast<UINT64>(g_cfg.log_frames) || (n % 1800) == 0)
                        Log("[feed] frame %llu delivered (%ux%u, reset=%d, same-device)", n, g.width, g.height, reset);

                    // The DLSS 5 add-on arms its NGX hooks a moment AFTER our first create (seen
                    // in LOTR: hooks +215 ms), which latches it in STANDBY. One warm-up re-create
                    // fixes that -- and it is safe now: it goes through RecreateFeatureOnly, which
                    // keeps the old feature if the new create fails or crashes.
                    if (g_cfg.warmup_rebuild > 0 && !g.warmup_done && !g_renodx_lazy && n >= static_cast<UINT64>(g_cfg.warmup_rebuild))
                    {
                        g.warmup_done = true;
                        g.frame_ready = false;
                        Log("[feed] warm-up: re-creating the DLSS feature once (frame %llu, same-device)", n);
                    }
                }
            }

            if (!restored)
            {
                // Whatever went wrong, hand the backbuffer back in the state ReShade expects.
                const resource       res[1]  = { bb_res };
                const resource_usage from[1] = { resource_usage::copy_dest };
                const resource_usage to[1]   = { resource_usage::render_target };
                cl->barrier(1, res, from, to);
            }
        }
    }

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
}

// ---------------------------------------------------------------------------
// Per frame, Vulkan transport: ReShade's command list carries the copies between
// the game's images and the shared ones, ReShade's queue signal/wait carries the
// cross-API fences, and our private D3D12 list carries only the NGX evaluate.
// ---------------------------------------------------------------------------

static void FeedFrameVk(reshade::api::effect_runtime *rt, reshade::api::command_list *cl, reshade::api::resource_view rtv)
{
    using namespace reshade::api;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    if ((g.frames_done % 60) == 0 && CfgReload()) g.frame_ready = false;
    if (!g_cfg.enabled || g_cfg.mode == 0) return;

    device *dev_api = rt->get_device();

    resource_view mv_srv = {}, mv_srgb = {}, d_srv = {}, d_srgb = {};
    if (g.mv_var.handle != 0)    rt->get_texture_binding(g.mv_var, &mv_srv, &mv_srgb);
    if (g.depth_var.handle != 0) rt->get_texture_binding(g.depth_var, &d_srv, &d_srgb);
    if (mv_srv.handle == 0 || d_srv.handle == 0)
    {
        if (!g.missing_reported)
        {
            g.missing_reported = true;
            Warn("DLSS5_Feed.fx textures not found (technique %s). Install DLSS5_Feed.fx + a motion vector provider and enable both.",
                 g.technique.handle ? "found" : "MISSING");
        }
        return;
    }

    const resource bb_res    = dev_api->get_resource_from_view(rtv);
    const resource mv_res    = dev_api->get_resource_from_view(mv_srv);
    const resource depth_res = dev_api->get_resource_from_view(d_srv);
    if (bb_res.handle == 0 || mv_res.handle == 0 || depth_res.handle == 0) return;

    // Optional validation mask (older shaders have none).
    resource mask_res = {};
    g.mask_ok = false;
    if (g.mask_var.handle != 0)
    {
        resource_view m_srv = {}, m_srgb = {};
        rt->get_texture_binding(g.mask_var, &m_srv, &m_srgb);
        if (m_srv.handle != 0) mask_res = dev_api->get_resource_from_view(m_srv);
        if (mask_res.handle != 0)
        {
            const resource_desc kd = dev_api->get_resource_desc(mask_res);
            const resource_desc bd = dev_api->get_resource_desc(bb_res);
            g.mask_ok = kd.texture.width == bd.texture.width && kd.texture.height == bd.texture.height &&
                        kd.texture.format == format::r8_unorm;
        }
    }

    const resource_desc cd = dev_api->get_resource_desc(bb_res);
    const resource_desc md = dev_api->get_resource_desc(mv_res);
    const resource_desc dd = dev_api->get_resource_desc(depth_res);
    const UINT w = cd.texture.width, h = cd.texture.height;
    if (w != md.texture.width || h != md.texture.height || w != dd.texture.width || h != dd.texture.height ||
        cd.texture.samples != 1 ||
        md.texture.format != format::r16g16_float || dd.texture.format != format::r32_float)
    {
        static bool said_vk = false;
        if (!said_vk)
        {
            said_vk = true;
            Log("[feed] input mismatch: color %ux%u fmt=%u samp=%u | mv %ux%u fmt=%u | depth %ux%u fmt=%u -- skipping",
                w, h, (unsigned)cd.texture.format, cd.texture.samples, md.texture.width, md.texture.height,
                (unsigned)md.texture.format, dd.texture.width, dd.texture.height, (unsigned)dd.texture.format);
        }
        return;
    }

    bool ok = true;
    if (g.session_ready && g.rs_dev != nullptr && g.rs_dev != dev_api)
    {
        Log("[feed] the game recreated its device; rebuilding the session");
        ShutdownSession();
    }
    if (!g.session_ready) ok = InitSessionVk(rt);

    const DXGI_FORMAT bbf = static_cast<DXGI_FORMAT>(cd.texture.format);
    const bool needs_build_vk = !g.frame_ready || w != g.width || h != g.height || bbf != g.bb_fmt;
    // Re-arm the grace on a resolution/format change too: that makes the DLSS 5 add-on
    // re-create its own feature, and any NGX interposer downstream (Alex's Toolkit) re-arms
    // with it. Without this the second build races hooks that are only half in place.
    if (g.frame_ready && needs_build_vk) g.create_grace = 0;
    if (ok && needs_build_vk && g.create_grace < g_cfg.create_delay)
    {
        if (++g.create_grace == 1)
            Log("[feed] holding the feature (re)build for %d frames (the DLSS 5 add-on re-arms its hooks asynchronously)",
                g_cfg.create_delay);
        ok = false;
    }
    if (ok && needs_build_vk)
    {
        Log("[feed] building: %ux%u backbuffer %s (Vulkan transport, depth reversed=%d)", w, h,
            FormatName(bbf), g.depth_reversed ? 1 : 0);
        ok = BuildResourcesVk(w, h, bbf);
        if (!ok) FeedFail("resource build");
        else g.consecutive_fails = 0;
    }

    if (ok && g.frame_ready)
    {
        VkCommandBuffer cb = FeedVkDispatch<VkCommandBuffer>(cl->get_native());
        VkImage bb_img = FeedVkHandle<VkImage>(bb_res.handle);
        VkImage mv_img = FeedVkHandle<VkImage>(mv_res.handle);
        VkImage dp_img = FeedVkHandle<VkImage>(depth_res.handle);

        // Our imported images -> GENERAL (first frame after a build, from UNDEFINED).
        // ReShade never touches them; only these raw barriers do.
        Breadcrumb("copying inputs (Vulkan)");
        {
            const VkImageLayout f = g.vk_layout_init ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            for (int i = 0; i < SLOT_COUNT; ++i)
                FeedVkBarrier(&g.vk, cb, g.vk_img[i], f, VK_IMAGE_LAYOUT_GENERAL);
            g.vk_layout_init = true;
        }
        // Game images -> copy_source via ReShade (its layout tracking stays correct),
        // then raw-copy each into our GENERAL image.
        {
            const resource       res[3]  = { bb_res, mv_res, depth_res };
            const resource_usage from[3] = { resource_usage::render_target, resource_usage::shader_resource, resource_usage::shader_resource };
            const resource_usage to[3]   = { resource_usage::copy_source, resource_usage::copy_source, resource_usage::copy_source };
            cl->barrier(3, res, from, to);
        }
        FeedVkCopyImage(&g.vk, cb, bb_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.vk_img[SLOT_COLOR], VK_IMAGE_LAYOUT_GENERAL, w, h);
        FeedVkCopyImage(&g.vk, cb, mv_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.vk_img[SLOT_MV],    VK_IMAGE_LAYOUT_GENERAL, w, h);
        FeedVkCopyImage(&g.vk, cb, dp_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.vk_img[SLOT_DEPTH], VK_IMAGE_LAYOUT_GENERAL, w, h);
        if (g.mask_ok)
        {
            // The mask goes the same way, and is handed straight back to shader_resource here.
            VkImage mk_img = FeedVkHandle<VkImage>(mask_res.handle);
            {
                const resource       res[1]  = { mask_res };
                const resource_usage from[1] = { resource_usage::shader_resource };
                const resource_usage to[1]   = { resource_usage::copy_source };
                cl->barrier(1, res, from, to);
            }
            FeedVkCopyImage(&g.vk, cb, mk_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.vk_img[SLOT_MASK], VK_IMAGE_LAYOUT_GENERAL, w, h);
            {
                const resource       res[1]  = { mask_res };
                const resource_usage from[1] = { resource_usage::copy_source };
                const resource_usage to[1]   = { resource_usage::shader_resource };
                cl->barrier(1, res, from, to);
            }
        }

        if (g_cfg.mode == 1)
        {
            // Transport test: raw-copy the LEFT half of our COLOR back over the game's
            // backbuffer -- a split screen is unambiguous proof of the round trip.
            {
                const resource       res[3]  = { bb_res, mv_res, depth_res };
                const resource_usage from[3] = { resource_usage::copy_source, resource_usage::copy_source, resource_usage::copy_source };
                const resource_usage to[3]   = { resource_usage::copy_dest, resource_usage::shader_resource, resource_usage::shader_resource };
                cl->barrier(3, res, from, to);
            }
            FeedVkCopyImage(&g.vk, cb, g.vk_img[SLOT_COLOR], VK_IMAGE_LAYOUT_GENERAL, bb_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w / 2, h);
            {
                const resource       res[1]  = { bb_res };
                const resource_usage from[1] = { resource_usage::copy_dest };
                const resource_usage to[1]   = { resource_usage::render_target };
                cl->barrier(1, res, from, to);
            }
            ++g.frames_done;
        }
        else
        {
            // Park the backbuffer as copy_dest to receive the output; restore mv/depth.
            {
                const resource       res[3]  = { bb_res, mv_res, depth_res };
                const resource_usage from[3] = { resource_usage::copy_source, resource_usage::copy_source, resource_usage::copy_source };
                const resource_usage to[3]   = { resource_usage::copy_dest, resource_usage::shader_resource, resource_usage::shader_resource };
                cl->barrier(3, res, from, to);
            }

            const UINT64 n = ++g.vk_frame;
            const int reset = (g.need_reset || g_cfg.reset_every) ? 1 : 0;
            g.need_reset = false;

            Breadcrumb("signalling the game-side fence (Vulkan)");
            g.rs_queue->flush_immediate_command_list();
            g.rs_queue->signal(g.rs_fence_in, n);

            // D3D12: wait for the copies, evaluate, signal back. Unchanged machinery.
            g.queue->Wait(g.fence12_in, n);
            bool done = false;
            if (!BeginCommands()) FeedFail("command list");
            else
            {
                Barrier(g.tex12[SLOT_COLOR],  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_DEPTH],  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_MV],     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (g.mask_ok) Barrier(g.tex12[SLOT_MASK], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                MvProbeRecord(g.tex12[SLOT_MV], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
                ep.Feature.pInColor  = g.tex12[SLOT_COLOR];
                ep.Feature.pInOutput = g.tex12[SLOT_OUTPUT];
                ep.Feature.InSharpness = 0.0f;
                ep.pInDepth          = g.tex12[SLOT_DEPTH];
                ep.pInMotionVectors  = g.tex12[SLOT_MV];
                ep.pInBiasCurrentColorMask = g.mask_ok ? g.tex12[SLOT_MASK] : nullptr;   // the shader's validation mask
                ep.InJitterOffsetX   = 0.0f;
                ep.InJitterOffsetY   = 0.0f;
                ep.InRenderSubrectDimensions.Width  = g.width;
                ep.InRenderSubrectDimensions.Height = g.height;
                ep.InReset           = reset;
                ep.InMVScaleX        = g_cfg.mv_scale_x;
                ep.InMVScaleY        = g_cfg.mv_scale_y;
                ep.InPreExposure     = 1.0f;
                ep.InExposureScale   = 1.0f;

                Breadcrumb("running the D3D12 evaluate (Vulkan transport)");
                DWORD ecode = 0;
                NVSDK_NGX_Result re = SafeEvaluateDLSS(&ep, &ecode);
                if (ecode != 0)
                {
                    AbortCommands();  // never execute a list NGX crashed while recording
                    Log("[feed] evaluate raised exception 0x%08X (caught; nothing was submitted)", ecode);
                    FeedDisable("the DLSS evaluate crashed (the DLSS 5 add-on may be incompatible with this game/resolution)");
                    g.frame_ready = false;
                }
                else
                {
                    Barrier(g.tex12[SLOT_COLOR],  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    Barrier(g.tex12[SLOT_DEPTH],  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    Barrier(g.tex12[SLOT_MV],     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    if (g.mask_ok) Barrier(g.tex12[SLOT_MASK], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    Barrier(g.tex12[SLOT_OUTPUT], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                    EndCommands();
                    if (NVSDK_NGX_FAILED(re))
                    {
                        Log("[feed] evaluate failed 0x%08X (%s)", re, NgxResultName(re));
                        FeedFail("evaluate");
                        g.frame_ready = false;
                    }
                    else
                        done = true;
                }
            }
            if (done)
                g.queue->Signal(g.fence12_out, n);   // after the evaluate, GPU-ordered
            else
                g.fence12_out->Signal(n);            // CPU-signal so the game never hangs on us

            // The copy home lands on the fresh immediate list, which executes on the
            // game's queue after the wait below -- GPU-ordered, no CPU stall.
            Breadcrumb("waiting for the result (Vulkan)");
            g.rs_queue->wait(g.rs_fence_out, n);
            cb = FeedVkDispatch<VkCommandBuffer>(cl->get_native());  // fresh buffer after the flush
            if (done)
            {
                // Prefer the raw copy. vkCmdBlitImage converts, and that conversion is
                // sRGB-aware: blitting our linear-typed output into a VK_FORMAT_*_SRGB
                // swapchain applies a linear->sRGB encode and the frame comes back much
                // brighter with lifted blacks (issue #11). The frame we were handed is
                // already encoded, so the bytes must go home untouched. The blit stays
                // only for the layouts a raw copy genuinely cannot express.
                if (SameTexelLayout(g.output_fmt, g.bb_fmt))
                    FeedVkCopyImage(&g.vk, cb, g.vk_img[SLOT_OUTPUT], VK_IMAGE_LAYOUT_GENERAL,
                                    bb_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w, h);
                else
                    FeedVkBlitImage(&g.vk, cb, g.vk_img[SLOT_OUTPUT], VK_IMAGE_LAYOUT_GENERAL,
                                    bb_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w, h);
            }
            {
                const resource       res[1]  = { bb_res };
                const resource_usage from[1] = { resource_usage::copy_dest };
                const resource_usage to[1]   = { resource_usage::render_target };
                cl->barrier(1, res, from, to);
            }

            if (done)
            {
                const UINT64 fn = ++g.frames_done;
                g.consecutive_fails = 0;
                if (fn <= static_cast<UINT64>(g_cfg.log_frames) || (fn % 1800) == 0)
                    Log("[feed] frame %llu delivered (%ux%u, reset=%d, Vulkan transport)", fn, g.width, g.height, reset);

                if (g_cfg.warmup_rebuild > 0 && !g.warmup_done && !g_renodx_lazy && fn >= static_cast<UINT64>(g_cfg.warmup_rebuild))
                {
                    g.warmup_done = true;
                    g.frame_ready = false;
                    Log("[feed] warm-up: re-creating the DLSS feature once (frame %llu, Vulkan transport)", fn);
                }
            }
        }
    }

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
}

// ---------------------------------------------------------------------------
// Per frame, OpenGL transport. The D3D12 middle is FeedFrameVk's, unchanged; the
// game-side halves are raw GL. No barriers of any kind are needed: every command
// enters the context's single in-order stream, so our reads are already ordered
// after the provider's writes, and the semaphore signal/wait carries the cross-API
// release/acquire. Not one ReShade API call is issued here beyond the lookups.
// ---------------------------------------------------------------------------

static void FeedFrameGl(reshade::api::effect_runtime *rt, reshade::api::command_list * /*cl*/, reshade::api::resource_view rtv)
{
    using namespace reshade::api;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    if ((g.frames_done % 60) == 0 && CfgReload()) g.frame_ready = false;
    if (!g_cfg.enabled || g_cfg.mode == 0) return;

    device *dev_api = rt->get_device();

    resource_view mv_srv = {}, mv_srgb = {}, d_srv = {}, d_srgb = {};
    if (g.mv_var.handle != 0)    rt->get_texture_binding(g.mv_var, &mv_srv, &mv_srgb);
    if (g.depth_var.handle != 0) rt->get_texture_binding(g.depth_var, &d_srv, &d_srgb);
    if (mv_srv.handle == 0 || d_srv.handle == 0)
    {
        if (!g.missing_reported)
        {
            g.missing_reported = true;
            Warn("DLSS5_Feed.fx textures not found (technique %s). Install DLSS5_Feed.fx + a motion vector provider and enable both.",
                 g.technique.handle ? "found" : "MISSING");
        }
        return;
    }

    const resource bb_res    = dev_api->get_resource_from_view(rtv);
    const resource mv_res    = dev_api->get_resource_from_view(mv_srv);
    const resource depth_res = dev_api->get_resource_from_view(d_srv);
    if (mv_res.handle == 0 || depth_res.handle == 0) return;
    // bb_res.handle == 0 is legal on GL and means the DEFAULT framebuffer, which the
    // blit path attaches as FBO 0 + GL_BACK. Only its DESCRIPTION is then unavailable,
    // so the sizes come from the motion vectors instead (which are backbuffer-sized
    // by construction: DLSS5_Feed.fx declares them at BUFFER_WIDTH x BUFFER_HEIGHT).

    // Optional validation mask (older shaders have none).
    resource mask_res = {};
    g.mask_ok = false;
    if (g.mask_var.handle != 0)
    {
        resource_view m_srv = {}, m_srgb = {};
        rt->get_texture_binding(g.mask_var, &m_srv, &m_srgb);
        if (m_srv.handle != 0) mask_res = dev_api->get_resource_from_view(m_srv);
    }

    const resource_desc md = dev_api->get_resource_desc(mv_res);
    const resource_desc dd = dev_api->get_resource_desc(depth_res);
    const bool have_bb_desc = bb_res.handle != 0;
    const resource_desc cd = have_bb_desc ? dev_api->get_resource_desc(bb_res) : md;
    const UINT w = cd.texture.width, h = cd.texture.height;
    // The guides are copied with glCopyImageSubData, which needs real textures on both
    // sides -- effect textures always are, but a wrong assumption here would surface as
    // a silent GL error and a black frame, so it is checked with everything else.
    const bool guides_are_textures = FeedGlHandleType(mv_res.handle) == GL_TEXTURE_2D &&
                                     FeedGlHandleType(depth_res.handle) == GL_TEXTURE_2D;
    if (w != md.texture.width || h != md.texture.height || w != dd.texture.width || h != dd.texture.height ||
        cd.texture.samples != 1 || !guides_are_textures ||
        md.texture.format != format::r16g16_float || dd.texture.format != format::r32_float)
    {
        static bool said_gl = false;
        if (!said_gl)
        {
            said_gl = true;
            Log("[feed] input mismatch: color %ux%u fmt=%u samp=%u | mv %ux%u fmt=%u | depth %ux%u fmt=%u"
                " | mv/depth GL types 0x%04X/0x%04X -- skipping",
                w, h, (unsigned)cd.texture.format, cd.texture.samples, md.texture.width, md.texture.height,
                (unsigned)md.texture.format, dd.texture.width, dd.texture.height, (unsigned)dd.texture.format,
                FeedGlHandleType(mv_res.handle), FeedGlHandleType(depth_res.handle));
        }
        return;
    }
    if (mask_res.handle != 0)
    {
        const resource_desc kd = dev_api->get_resource_desc(mask_res);
        g.mask_ok = kd.texture.width == w && kd.texture.height == h &&
                    kd.texture.format == format::r8_unorm &&
                    FeedGlHandleType(mask_res.handle) == GL_TEXTURE_2D;
    }

    bool ok = true;
    if (g.session_ready && g.rs_dev != nullptr && g.rs_dev != dev_api)
    {
        Log("[feed] the game recreated its device; rebuilding the session");
        ShutdownSession();
    }
    // GL names live in the share group of the context that was current at import. A
    // game that renders through an unshared or recreated context would strand every
    // import, so the session is torn down and rebuilt on the new context instead.
    if (g.session_ready && g.gl.ok && g.gl.wglGetCurrentContext() != g.gl_ctx)
    {
        Log("[feed] the GL context changed (%p -> %p); rebuilding the session on the new one",
            (void *)g.gl_ctx, (void *)g.gl.wglGetCurrentContext());
        ShutdownSession();
    }
    if (!g.session_ready) ok = InitSessionGl(rt);

    const DXGI_FORMAT bbf = have_bb_desc ? static_cast<DXGI_FORMAT>(cd.texture.format)
                                         : DXGI_FORMAT_R8G8B8A8_UNORM;   // default FB: assume 8-bit; the blit converts anyway
    const bool needs_build_gl = !g.frame_ready || w != g.width || h != g.height || bbf != g.bb_fmt;
    // Re-arm the grace on a resolution/format change too: that makes the DLSS 5 add-on
    // re-create its own feature, and any NGX interposer downstream (Alex's Toolkit) re-arms
    // with it. Without this the second build races hooks that are only half in place.
    if (g.frame_ready && needs_build_gl) g.create_grace = 0;
    if (ok && needs_build_gl && g.create_grace < g_cfg.create_delay)
    {
        if (++g.create_grace == 1)
            Log("[feed] holding the feature (re)build for %d frames (the DLSS 5 add-on re-arms its hooks asynchronously)",
                g_cfg.create_delay);
        ok = false;
    }
    if (ok && needs_build_gl)
    {
        Log("[feed] building: %ux%u backbuffer %s (OpenGL transport, depth reversed=%d)", w, h,
            FormatName(bbf), g.depth_reversed ? 1 : 0);
        ok = BuildResourcesGl(w, h, bbf, bb_res.handle);
        if (!ok) FeedFail("resource build");
        else g.consecutive_fails = 0;
    }

    if (ok && g.frame_ready)
    {
        FeedGlStateGuard guard(&g.gl);

        // Capture. MV, Depth and the Mask are exact-format GL_TEXTURE_2Ds, so they go
        // by glCopyImageSubData, which touches no state at all. The colour goes by
        // blit: it converts formats and channel order, and it can read what a raw copy
        // cannot -- a renderbuffer or the default framebuffer.
        Breadcrumb("copying inputs (OpenGL)");
        FeedGlCopy(&g.gl, FeedGlHandleName(mv_res.handle),    g.gl_tex[SLOT_MV],    w, h);
        FeedGlCopy(&g.gl, FeedGlHandleName(depth_res.handle), g.gl_tex[SLOT_DEPTH], w, h);
        if (g.mask_ok)
            FeedGlCopy(&g.gl, FeedGlHandleName(mask_res.handle), g.gl_tex[SLOT_MASK], w, h);
        bool captured = FeedGlBlit(&g.gl, g.gl_fbo_read, g.gl_fbo_draw,
                                   bb_res.handle, false, g.gl_tex[SLOT_COLOR], true, w, h);
        if (!captured)
        {
            static bool said = false;
            if (!said) { said = true; Log("[feed] the colour capture blit could not be set up (incomplete framebuffer)"); }
            FeedFail("colour capture");
            QueryPerformanceCounter(&t1);
            TimingTick(t0.QuadPart, t1.QuadPart);
            return;
        }

        if (g_cfg.mode == 1)
        {
            // Transport test: blit the LEFT half of our captured COLOR straight back
            // over the technique's target -- a split screen is unambiguous proof of
            // the round trip, without NGX in the way.
            FeedGlBlit(&g.gl, g.gl_fbo_read, g.gl_fbo_draw,
                       g.gl_tex[SLOT_COLOR], true, bb_res.handle, false, w / 2, h);
            ++g.frames_done;
        }
        else
        {
            const UINT64 n = ++g.gl_frame;
            const int reset = (g.need_reset || g_cfg.reset_every) ? 1 : 0;
            g.need_reset = false;

            Breadcrumb("signalling the shared fence (OpenGL)");
            {
                const GLuint inputs[4] = { g.gl_tex[SLOT_COLOR], g.gl_tex[SLOT_DEPTH], g.gl_tex[SLOT_MV], g.gl_tex[SLOT_MASK] };
                FeedGlSignal(&g.gl, g.gl_sem_in, n, inputs, g.mask_ok ? 4u : 3u);
            }

            // D3D12: wait for the copies, evaluate, signal back. Unchanged machinery.
            g.queue->Wait(g.fence12_in, n);
            bool done = false;
            if (!BeginCommands()) FeedFail("command list");
            else
            {
                Barrier(g.tex12[SLOT_COLOR],  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_DEPTH],  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_MV],     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (g.mask_ok) Barrier(g.tex12[SLOT_MASK], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                MvProbeRecord(g.tex12[SLOT_MV], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
                ep.Feature.pInColor  = g.tex12[SLOT_COLOR];
                ep.Feature.pInOutput = g.tex12[SLOT_OUTPUT];
                ep.Feature.InSharpness = 0.0f;
                ep.pInDepth          = g.tex12[SLOT_DEPTH];
                ep.pInMotionVectors  = g.tex12[SLOT_MV];
                ep.pInBiasCurrentColorMask = g.mask_ok ? g.tex12[SLOT_MASK] : nullptr;   // the shader's validation mask
                ep.InJitterOffsetX   = 0.0f;
                ep.InJitterOffsetY   = 0.0f;
                ep.InRenderSubrectDimensions.Width  = g.width;
                ep.InRenderSubrectDimensions.Height = g.height;
                ep.InReset           = reset;
                ep.InMVScaleX        = g_cfg.mv_scale_x;
                ep.InMVScaleY        = g_cfg.mv_scale_y;
                ep.InPreExposure     = 1.0f;
                ep.InExposureScale   = 1.0f;

                Breadcrumb("running the D3D12 evaluate (OpenGL transport)");
                DWORD ecode = 0;
                NVSDK_NGX_Result re = SafeEvaluateDLSS(&ep, &ecode);
                if (ecode != 0)
                {
                    AbortCommands();  // never execute a list NGX crashed while recording
                    Log("[feed] evaluate raised exception 0x%08X (caught; nothing was submitted)", ecode);
                    FeedDisable("the DLSS evaluate crashed (the DLSS 5 add-on may be incompatible with this game/resolution)");
                    g.frame_ready = false;
                }
                else
                {
                    Barrier(g.tex12[SLOT_COLOR],  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    Barrier(g.tex12[SLOT_DEPTH],  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    Barrier(g.tex12[SLOT_MV],     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    if (g.mask_ok) Barrier(g.tex12[SLOT_MASK], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    Barrier(g.tex12[SLOT_OUTPUT], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                    EndCommands();
                    if (NVSDK_NGX_FAILED(re))
                    {
                        Log("[feed] evaluate failed 0x%08X (%s)", re, NgxResultName(re));
                        FeedFail("evaluate");
                        g.frame_ready = false;
                    }
                    else
                        done = true;
                }
            }
            if (done)
                g.queue->Signal(g.fence12_out, n);   // after the evaluate, GPU-ordered
            else
                g.fence12_out->Signal(n);            // CPU-signal so the GL stream never hangs on us
                                                     // (glWaitSemaphoreEXT has no timeout)

            Breadcrumb("waiting for the result (OpenGL)");
            {
                const GLuint outputs[1] = { g.gl_tex[SLOT_OUTPUT] };
                FeedGlWait(&g.gl, g.gl_sem_out, n, outputs, 1);   // server-side: the GPU stalls, the CPU does not
            }
            if (done)
                FeedGlBlit(&g.gl, g.gl_fbo_read, g.gl_fbo_draw,
                           g.gl_tex[SLOT_OUTPUT], true, bb_res.handle, false, w, h);

            if (done)
            {
                const UINT64 fn = ++g.frames_done;
                g.consecutive_fails = 0;
                if (fn <= static_cast<UINT64>(g_cfg.log_frames) || (fn % 1800) == 0)
                    Log("[feed] frame %llu delivered (%ux%u, reset=%d, OpenGL transport)", fn, g.width, g.height, reset);

                if (g_cfg.warmup_rebuild > 0 && !g.warmup_done && !g_renodx_lazy && fn >= static_cast<UINT64>(g_cfg.warmup_rebuild))
                {
                    g.warmup_done = true;
                    g.frame_ready = false;
                    Log("[feed] warm-up: re-creating the DLSS feature once (frame %llu, OpenGL transport)", fn);
                }
            }
        }

        // One sweep per frame while the log is still young: a silent GL error here
        // would otherwise only show up as a black frame.
        if (g.frames_done <= static_cast<UINT64>(g_cfg.log_frames))
            if (const GLenum e = FeedGlDrainErrors(&g.gl))
                Log("[feed] GL error 0x%04X during frame %llu", e, g.frames_done);
    }

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
}

// ---------------------------------------------------------------------------
// Per frame, D3D11: the original private-device transport
// ---------------------------------------------------------------------------

static void FeedFrame11(reshade::api::effect_runtime *rt, reshade::api::command_list *cl, reshade::api::resource_view rtv)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    reshade::api::device *dev_api = rt->get_device();

    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cl->get_native());
    if (ctx == nullptr || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;

    if (ApplyPendingWorkResolution()) g.frame_ready = false;
    if ((g.frames_done % 60) == 0 && CfgReload()) g.frame_ready = false;
    if (!g_cfg.enabled || g_cfg.mode == 0) return;

    // Inputs from ReShade: the frame being processed, and the companion effect's guide textures.
    reshade::api::resource_view color_srv = {}, color_srgb = {}, mv_srv = {}, mv_srgb = {}, d_srv = {}, d_srgb = {};
    if (g.color_var.handle != 0) rt->get_texture_binding(g.color_var, &color_srv, &color_srgb);
    if (g.mv_var.handle != 0)    rt->get_texture_binding(g.mv_var, &mv_srv, &mv_srgb);
    if (g.depth_var.handle != 0) rt->get_texture_binding(g.depth_var, &d_srv, &d_srgb);
    if (mv_srv.handle == 0 || d_srv.handle == 0)
    {
        if (!g.missing_reported)
        {
            g.missing_reported = true;
            Warn("DLSS5_Feed.fx textures not found (technique %s). Install DLSS5_Feed.fx + a texMotionVectors provider and enable both.",
                 g.technique.handle ? "found" : "MISSING");
        }
        return;
    }

    auto *color_res = reinterpret_cast<ID3D11Resource *>(dev_api->get_resource_from_view(rtv).handle);
    auto *mv_res    = reinterpret_cast<ID3D11Resource *>(dev_api->get_resource_from_view(mv_srv).handle);
    auto *depth_res = reinterpret_cast<ID3D11Resource *>(dev_api->get_resource_from_view(d_srv).handle);
    auto *rtv11     = reinterpret_cast<ID3D11RenderTargetView *>(rtv.handle);

    // Optional validation mask (older shaders have none).
    ID3D11Resource *mask_res = nullptr;
    reshade::api::resource_view mask_srv = {}, mask_srgb = {};
    if (g.mask_var.handle != 0)
    {
        rt->get_texture_binding(g.mask_var, &mask_srv, &mask_srgb);
        if (mask_srv.handle != 0) mask_res = reinterpret_cast<ID3D11Resource *>(dev_api->get_resource_from_view(mask_srv).handle);
    }

    D3D11_TEXTURE2D_DESC cd = {}, md = {}, dd = {}, kd = {};
    ID3D11Texture2D *color = AsTexture2D(color_res, &cd);
    ID3D11Texture2D *mv    = AsTexture2D(mv_res, &md);
    ID3D11Texture2D *depth = AsTexture2D(depth_res, &dd);
    ID3D11Texture2D *mask  = mask_res != nullptr ? AsTexture2D(mask_res, &kd) : nullptr;
    if (color == nullptr || mv == nullptr || depth == nullptr)
    {
        SafeRelease(color); SafeRelease(mv); SafeRelease(depth); SafeRelease(mask);
        return;
    }
    g.mask_ok = mask != nullptr && kd.Width == cd.Width && kd.Height == cd.Height && kd.Format == DXGI_FORMAT_R8_UNORM;

    bool ok = true;
    if (cd.Width != md.Width || cd.Height != md.Height || cd.Width != dd.Width || cd.Height != dd.Height ||
        cd.SampleDesc.Count != 1 || md.Format != DXGI_FORMAT_R16G16_FLOAT || dd.Format != DXGI_FORMAT_R32_FLOAT)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[feed] input mismatch: color %ux%u %s samp=%u | mv %ux%u %s | depth %ux%u %s -- skipping",
                cd.Width, cd.Height, FormatName(cd.Format), cd.SampleDesc.Count, md.Width, md.Height,
                FormatName(md.Format), dd.Width, dd.Height, FormatName(dd.Format));
        }
        ok = false;
    }

    if (ok)
    {
        ID3D11Device *dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev == nullptr) ok = false;
        else
        {
            if (g.session_ready && g.dev11 != nullptr && dev != g.dev11)
            {
                Log("[feed] the game recreated its D3D11 device; rebuilding the session");
                ShutdownSession();
            }
            if (!g.session_ready) ok = InitSession(dev, ctx);
            dev->Release();
        }
    }

    // The DLSS 5 add-on arms (and re-arms, on every runtime recreation) its NGX hooks
    // asynchronously; calling into NGX while the vtable is being patched has crashed the
    // process (EXEC at 0x0, sometimes fatally on a foreign thread). Hold EVERY build that
    // follows a runtime (re-)init until that settled.
    const UINT work_w = ScaledExtent(cd.Width, g_cfg.work_resolution);
    const UINT work_h = ScaledExtent(cd.Height, g_cfg.work_resolution);
    const bool needs_build11 = !g.frame_ready || work_w != g.width || work_h != g.height ||
                               cd.Width != g.backbuffer_width || cd.Height != g.backbuffer_height ||
                               cd.Format != g.bb_fmt;
    // Re-arm the grace on a resolution/format change too: that makes the DLSS 5 add-on
    // re-create its own feature, and any NGX interposer downstream (Alex's Toolkit) re-arms
    // with it. Without this the second build races hooks that are only half in place.
    if (g.frame_ready && needs_build11) g.create_grace = 0;
    if (ok && needs_build11 && g.create_grace < g_cfg.create_delay)
    {
        if (++g.create_grace == 1)
            Log("[feed] holding the feature (re)build for %d frames (the DLSS 5 add-on re-arms its hooks asynchronously)",
                g_cfg.create_delay);
        ok = false;
    }

    if (ok && needs_build11)
    {
        Log("[feed] building: %ux%u work resolution (%d%%) -> %ux%u backbuffer %s (mv %s, depth %s, depth reversed=%d)",
            work_w, work_h, g_cfg.work_resolution, cd.Width, cd.Height, FormatName(cd.Format),
            FormatName(md.Format), FormatName(dd.Format), g.depth_reversed ? 1 : 0);
        ok = BuildResources(work_w, work_h, cd.Width, cd.Height, cd.Format);
        if (!ok) FeedFail("resource build");
        else g.consecutive_fails = 0;
    }

    if (ok)
    {
        Breadcrumb("preparing work-resolution inputs");
        ok = CopyOrResampleInputs(ctx, color, mv, depth, mask,
                                  nullptr,
                                  reinterpret_cast<ID3D11ShaderResourceView *>(mv_srv.handle),
                                  reinterpret_cast<ID3D11ShaderResourceView *>(d_srv.handle),
                                  reinterpret_cast<ID3D11ShaderResourceView *>(mask_srv.handle),
                                  cd.Width, cd.Height);

        if (ok && g_cfg.mode == 1)
        {
            // Transport test: what went out comes straight back, through the same copy-back path.
            ctx->CopyResource(g.tex11[SLOT_OUTPUT], g.tex11[SLOT_COLOR]);
            BlitOutputToBackbuffer(ctx, rtv11);
            ++g.frames_done;
        }
        else if (ok)
        {
            const UINT64 v_in = ++g.fence_value;
            g.ctx4->Signal(g.fence11, v_in);
            ctx->Flush();
            g.queue->Wait(g.fence12, v_in);

            if (!BeginCommands()) { FeedFail("command list"); ok = false; }
            else
            {
                Barrier(g.tex12[SLOT_COLOR],  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_DEPTH],  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_MV],     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (g.mask_ok) Barrier(g.tex12[SLOT_MASK], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                MvProbeRecord(g.tex12[SLOT_MV], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                const int reset = (g.need_reset || g_cfg.reset_every) ? 1 : 0;
                g.need_reset = false;

                NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
                ep.Feature.pInColor  = g.tex12[SLOT_COLOR];
                ep.Feature.pInOutput = g.tex12[SLOT_OUTPUT];
                ep.Feature.InSharpness = 0.0f;
                ep.pInDepth          = g.tex12[SLOT_DEPTH];
                ep.pInMotionVectors  = g.tex12[SLOT_MV];
                ep.pInBiasCurrentColorMask = g.mask_ok ? g.tex12[SLOT_MASK] : nullptr;   // the shader's validation mask
                ep.InJitterOffsetX   = 0.0f;
                ep.InJitterOffsetY   = 0.0f;
                ep.InRenderSubrectDimensions.Width  = g.width;
                ep.InRenderSubrectDimensions.Height = g.height;
                ep.InReset           = reset;
                ep.InMVScaleX        = g_cfg.mv_scale_x;
                ep.InMVScaleY        = g_cfg.mv_scale_y;
                ep.InPreExposure     = 1.0f;
                ep.InExposureScale   = 1.0f;

                Breadcrumb("running the D3D12 evaluate");
                DWORD ecode = 0;
                NVSDK_NGX_Result re = SafeEvaluateDLSS(&ep, &ecode);

                if (ecode != 0)
                {
                    AbortCommands();  // never execute a list NGX crashed while recording
                    Log("[feed] evaluate raised exception 0x%08X (caught; nothing was submitted)", ecode);
                    FeedDisable("the DLSS evaluate crashed (the DLSS 5 add-on may be incompatible with this game/resolution)");
                    g.frame_ready = false;
                    ok = false;
                }
                else
                {
                Barrier(g.tex12[SLOT_COLOR],  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(g.tex12[SLOT_DEPTH],  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(g.tex12[SLOT_MV],     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                if (g.mask_ok) Barrier(g.tex12[SLOT_MASK], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(g.tex12[SLOT_OUTPUT], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                const UINT64 v_out = EndCommands();

                if (NVSDK_NGX_FAILED(re))
                {
                    Log("[feed] evaluate failed 0x%08X (%s)", re, NgxResultName(re));
                    FeedFail("evaluate");
                    g.frame_ready = false;  // rebuild rather than repeat the same failure
                    ok = false;
                }
                else
                {
                    Breadcrumb("waiting for the D3D12 result");
                    g.ctx4->Wait(g.fence11, v_out);
                    BlitOutputToBackbuffer(ctx, rtv11);
                    const UINT64 n = ++g.frames_done;
                    g.consecutive_fails = 0;
                    if (n <= static_cast<UINT64>(g_cfg.log_frames) || (n % 1800) == 0)
                        Log("[feed] frame %llu delivered (%ux%u at %d%% -> %ux%u, reset=%d)", n,
                            g.width, g.height, g_cfg.work_resolution,
                            g.backbuffer_width, g.backbuffer_height, reset);

                    // The DLSS 5 add-on sometimes latches STANDBY/FAILED on the very first create and only
                    // recovers on a fresh one; re-create once after the pipeline has settled.
                    if (g_cfg.warmup_rebuild > 0 && !g.warmup_done && !g_renodx_lazy && n >= static_cast<UINT64>(g_cfg.warmup_rebuild))
                    {
                        g.warmup_done = true;
                        g.frame_ready = false;
                        Log("[feed] warm-up: re-creating the DLSS feature once (frame %llu)", n);
                    }
                }
                }
            }
        }
    }

    SafeRelease(color);
    SafeRelease(mv);
    SafeRelease(depth);
    SafeRelease(mask);

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
}

static void FeedFrame(reshade::api::effect_runtime *rt, reshade::api::command_list *cl, reshade::api::resource_view rtv)
{
    if (!g_cfg.enabled || g.disabled || g_cfg.mode == 0) return;
    switch (rt->get_device()->get_api())
    {
    case reshade::api::device_api::d3d11: FeedFrame11(rt, cl, rtv); break;
    case reshade::api::device_api::d3d12: FeedFrame12(rt, cl, rtv); break;
    case reshade::api::device_api::vulkan: FeedFrameVk(rt, cl, rtv); break;
    case reshade::api::device_api::opengl: FeedFrameGl(rt, cl, rtv); break;
    default: FeedDisable("only Direct3D 11/12, Vulkan and OpenGL games are supported"); break;
    }
}

// ---------------------------------------------------------------------------
// ReShade events
// ---------------------------------------------------------------------------

static void ResolveHandles(reshade::api::effect_runtime *rt)
{
    g.technique = rt->find_technique(kEffectFile, kTechnique);
    g.color_var = rt->find_texture_variable(kEffectFile, "DLSS5_ColorInput");
    g.mv_var    = rt->find_texture_variable(kEffectFile, "DLSS5_MV");
    g.depth_var = rt->find_texture_variable(kEffectFile, "DLSS5_Depth");
    g.mask_var  = rt->find_texture_variable(kEffectFile, "DLSS5_Mask");
    // Which provider is DLSS5_Feed.fx compiled for, and is a matching provider technique
    // present and enabled? Purely informational, plus a warning when the two disagree
    // (the classic "enabled Launchpad but the shader still reads texMotionVectors").
    const int mode = ReadMvProviderMode(rt);
    g.launchpad = {};
    const char *provider = "none";
    const char *provider_file = nullptr;
    reshade::api::effect_technique other = {};
    const char *other_tech = nullptr;
    int other_mode = -1;
    for (const auto &p : kMvProviders)
    {
        const reshade::api::effect_technique t = rt->find_technique(p.file, p.tech);
        if (t.handle == 0) continue;
        const bool on = rt->get_technique_state(t);
        if (p.mode == mode)
        {
            // Several providers can serve one mode (mode 0 especially); prefer the enabled one.
            if (g.launchpad.handle == 0 || on) { g.launchpad = t; provider = p.tech; provider_file = p.file; }
        }
        else if (on && other.handle == 0) { other = t; other_tech = p.tech; other_mode = p.mode; }
    }
    char compile_error[512] = {};
    const bool provider_broken = provider_file != nullptr && ProviderCompileError(provider_file, compile_error, sizeof(compile_error));

    char v[16] = {};
    g.depth_reversed = true;  // ReShade.fxh's own default when the definition is absent
    if (rt->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", v))
        g.depth_reversed = atoi(v) != 0;

    g.handles_ok = g.technique.handle != 0 && g.mv_var.handle != 0 && g.depth_var.handle != 0;
    g.missing_reported = false;

    // Games can recreate the swapchain (and ReShade its runtime) dozens of times per second;
    // only say something when the situation actually changed.
    const bool provider_on = g.launchpad.handle && rt->get_technique_state(g.launchpad);
    const int signature = (g.technique.handle ? 1 : 0) | (g.color_var.handle ? 2 : 0) |
                          (g.mv_var.handle ? 4 : 0) | (g.depth_var.handle ? 8 : 0) |
                          (g.launchpad.handle ? 16 : 0) | (g.depth_reversed ? 32 : 0) | (provider_on ? 64 : 0) |
                          (mode << 7) | (other.handle ? 1024 : 0) | ((other_mode & 7) << 11) | (provider_broken ? 16384 : 0);
    static int last_signature = -1;
    if (signature == last_signature) return;
    last_signature = signature;

    _snprintf_s(g_mv_status, sizeof(g_mv_status), _TRUNCATE, "DLSS5_MV_PROVIDER=%d (%s) -> %s (%s)",
                mode, kMvModeName[mode], provider,
                g.launchpad.handle ? (provider_broken ? "FAILED TO COMPILE" : provider_on ? "enabled" : "DISABLED") : "not installed");
    g_mv_problem[0] = '\0';

    Log("[feed] effects: %s technique %s, ColorInput %s, DLSS5_MV %s, DLSS5_Depth %s, DLSS5_Mask %s, %s, depth reversed=%d",
        kEffectFile, g.technique.handle ? "found" : "MISSING", g.color_var.handle ? "found" : "MISSING",
        g.mv_var.handle ? "found" : "MISSING",
        g.depth_var.handle ? "found" : "MISSING", g.mask_var.handle ? "found" : "absent (older shader: no bias mask)",
        g_mv_status, g.depth_reversed ? 1 : 0);
    if (!g.handles_ok)
        Warn("DLSS5_Feed.fx is not loaded (technique/textures missing) -- install it into reshade-shaders\\Shaders.");
    else if (g.launchpad.handle == 0)
        _snprintf_s(g_mv_problem, sizeof(g_mv_problem), _TRUNCATE,
                    "DLSS5_Feed.fx is compiled for motion-vector provider %d (%s) but no known %s shader is installed: motion vectors will be zero (still images only). "
                    "Install one, or change the DLSS5_MV_PROVIDER preprocessor definition.", mode, kMvModeName[mode], kMvModeName[mode]);
    else if (provider_broken)
        _snprintf_s(g_mv_problem, sizeof(g_mv_problem), _TRUNCATE,
                    "motion-vector provider %s FAILED TO COMPILE, so it writes nothing and DLSS runs on zero vectors. ReShade.log: %s -- use another provider (VORT: DLSS5_MV_PROVIDER=2).",
                    provider, compile_error);
    else if (!provider_on)
        _snprintf_s(g_mv_problem, sizeof(g_mv_problem), _TRUNCATE,
                    "motion-vector provider %s is installed but DISABLED: enable it above DLSS 5 Feed.", provider);
    if (other.handle != 0)
    {
        char more[320];
        _snprintf_s(more, sizeof(more), _TRUNCATE,
                    "%s%s is enabled, but DLSS5_Feed.fx is compiled for provider %d (%s) and does not read it -- set the DLSS5_MV_PROVIDER preprocessor definition to %d to use it.",
                    g_mv_problem[0] ? " " : "", other_tech, mode, kMvModeName[mode], other_mode);
        strncat_s(g_mv_problem, sizeof(g_mv_problem), more, _TRUNCATE);
    }
    if (g_mv_problem[0]) Warn("%s", g_mv_problem);
}

static void OnInitEffectRuntime(reshade::api::effect_runtime *rt)
{
    g.runtime = rt;
    ResolveHandles(rt);
    // A recreated runtime means the DLSS 5 add-on has re-armed its hooks on our private
    // device: give it a fresh feature (a cheap feature-only re-create -- the textures stay).
    // On the same-device D3D12 path its hooks live on the game's device and survive; the
    // feature must NOT be touched (re-creating a live one is where the add-on crashes).
    if (g.session_ready && g.dev12_owned) g.frame_ready = false;
    // Either way the add-on may be re-patching its NGX hooks right now: hold any upcoming
    // feature create for a fresh grace period.
    g.create_grace = 0;
    static int inits = 0;
    if (++inits <= 8) Log("[feed] effect runtime %p initialised", (void *)rt);
    else if (inits == 9) Log("[feed] (further runtime init/destroy messages suppressed)");
}

static void OnDestroyEffectRuntime(reshade::api::effect_runtime *rt)
{
    if (rt != g.runtime) return;
    static int destroys = 0;
    if (++destroys <= 8) Log("[feed] effect runtime %p destroyed", (void *)rt);
    // D3D11 path: the DLSS 5 add-on re-arms its hooks with the runtime, so the feature is
    // rebuilt anyway. Same-device D3D12: feature and textures live on the GAME's device and
    // survive runtime churn -- keep them. Every feature create near a hook re-arm has been
    // a crash risk (EXEC 0x0 inside the add-on, sometimes fatal on a foreign thread), so
    // the fewer creates, the better.
    if (g.dev12_owned) ReleaseFrameResources();
    g.runtime = nullptr;
    g.technique = {}; g.launchpad = {}; g.color_var = {}; g.mv_var = {}; g.depth_var = {}; g.mask_var = {};
    g.handles_ok = false;
}

static void OnReloadedEffects(reshade::api::effect_runtime *rt)
{
    if (rt == g.runtime || g.runtime == nullptr) { g.runtime = rt; ResolveHandles(rt); }
}

static void OnRenderTechnique(reshade::api::effect_runtime *rt, reshade::api::effect_technique technique,
                              reshade::api::command_list *cl, reshade::api::resource_view rtv,
                              reshade::api::resource_view /*rtv_srgb*/)
{
    if (rt != g.runtime || g.technique.handle == 0 || technique.handle != g.technique.handle) return;
    FeedFrame(rt, cl, rtv);
}

static void OnDestroyDevice(reshade::api::device *dev)
{
    if (g.dev11 != nullptr && reinterpret_cast<ID3D11Device *>(dev->get_native()) == g.dev11)
    {
        Log("[feed] D3D11 device destroyed; shutting the session down");
        g_ngx_dying = true;   // cleared again if a fresh session opens
        ShutdownSession();
    }
    else if (g.session_ready && !g.dev12_owned && g.dev12 != nullptr &&
             reinterpret_cast<ID3D12Device *>(dev->get_native()) == g.dev12)
    {
        Log("[feed] the game's D3D12 device is being destroyed; shutting the session down");
        g_ngx_dying = true;
        ShutdownSession();
    }
    else if (g.session_ready && dev->get_api() == reshade::api::device_api::vulkan && dev == g.rs_dev)
    {
        Log("[feed] the game's Vulkan device is being destroyed; shutting the session down");
        g_ngx_dying = true;
        ShutdownSession();
    }
    else if (g.session_ready && dev->get_api() == reshade::api::device_api::opengl && dev == g.rs_dev)
    {
        Log("[feed] the game's OpenGL device is being destroyed; shutting the session down");
        g_ngx_dying = true;
        ShutdownSession();
    }
}

// ---------------------------------------------------------------------------
// ReShade overlay page: Add-ons tab -> DLSS 5 Feed. Edits dlss5-feed.cfg live and
// saves it immediately, so the usual 60-frame CfgReload() picks up the new values
// (and does not overwrite them with the stale on-disk copy in the meantime).
// ---------------------------------------------------------------------------

static void HelpMarker(const char *desc)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void DrawOverlay(reshade::api::effect_runtime *rt)
{
    bool dirty = false;
    bool enabled = g_cfg.enabled != 0;
    if (ImGui::Checkbox("Enabled", &enabled)) { g_cfg.enabled = enabled ? 1 : 0; dirty = true; }

    ImGui::Separator();
    ImGui::TextUnformatted("Status");
    ImGui::Text("Session: %s", g.disabled ? "disabled (see dlss5-feed.log)" : g.session_ready ? "open" : "not started");
    ImGui::Text("Feature: %s", g.frame_ready ? "ready" : "not built");
    if (g.frames_done > 0) ImGui::Text("Frames delivered: %llu", static_cast<unsigned long long>(g.frames_done));
    ImGui::Text("DLSS 5 add-on: v%s (%s)", g_renodx_ver,
                g_renodx_v46 ? "v4.6+ engine" : g_renodx_lazy ? "v45+ engine" : "classic engine");
    if (g_toolkit_passes >= 2)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Alex's Toolkit %s: %d-pass cascade -- ~%dx temporal history (smearing, slow settle)",
                           g_toolkit_ver, g_toolkit_passes, g_toolkit_passes);
    else if (strcmp(g_toolkit_ver, "not found") != 0)
        ImGui::Text("Alex's Toolkit %s: present, cascade off (single pass)", g_toolkit_ver);
    if (g.disabled && ImGui::Button("Re-enable"))
    {
        g.disabled = false;
        g.consecutive_fails = 0;
        Log("[feed] re-enabled from the overlay");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("DLSS contract");
    static const char *kModes[] = { "Inert", "Transport test (no NGX)", "Full DLSS path" };
    if (ImGui::Combo("Mode", &g_cfg.mode, kModes, 3)) dirty = true;
    const bool adjustable_work_resolution = rt != nullptr &&
        rt->get_device()->get_api() == reshade::api::device_api::d3d11;
    if (adjustable_work_resolution)
    {
        if (g_pending_work_resolution == 0 && g_work_resolution_ui != g_cfg.work_resolution)
            g_work_resolution_ui = g_cfg.work_resolution;
        if (ImGui::SliderInt("Work resolution (%)", &g_work_resolution_ui, 50, 100))
        {
            g_pending_work_resolution = g_work_resolution_ui;
            g_work_resolution_apply_after = GetTickCount64() + 400;
        }
        ImGui::SameLine(); HelpMarker("Scales both axes of the private DLAA + Neural Rendering work textures. "
                                      "The game/backbuffer stays native-sized. Applied once 400 ms after dragging stops.");
        if (g_pending_work_resolution != 0)
            ImGui::TextDisabled("Pending: %d%%", g_pending_work_resolution);
        else if (g.backbuffer_width != 0)
            ImGui::TextDisabled("Active: %ux%u (%d%%) -> %ux%u", g.width, g.height,
                                g_cfg.work_resolution, g.backbuffer_width, g.backbuffer_height);
    }
    else
    {
        ImGui::TextDisabled("Work resolution: 100%% (adjustable path currently supports 64-bit D3D11)");
    }
    static const char *kTri[] = { "Auto", "Force off", "Force on" };
    int hdr_idx = g_cfg.hdr + 1, di_idx = g_cfg.depth_inverted + 1;
    if (ImGui::Combo("HDR", &hdr_idx, kTri, 3)) { g_cfg.hdr = hdr_idx - 1; dirty = true; }
    if (ImGui::Combo("Depth inverted", &di_idx, kTri, 3)) { g_cfg.depth_inverted = di_idx - 1; dirty = true; }
    bool reset_every = g_cfg.reset_every != 0;
    if (ImGui::Checkbox("Reset every frame (diagnostic)", &reset_every)) { g_cfg.reset_every = reset_every ? 1 : 0; dirty = true; }

    ImGui::Separator();
    ImGui::TextUnformatted("DLSS render preset");
    static const char *kPresetNames[] = { "Default", "E (legacy CNN)", "F (legacy CNN)", "J (transformer)", "K (transformer)" };
    static const int   kPresetValues[] = { 0, 5, 6, 10, 11 };
    int preset_idx = 0;
    for (int i = 0; i < 5; ++i) if (kPresetValues[i] == g_cfg.preset) preset_idx = i;
    if (ImGui::Combo("Preset", &preset_idx, kPresetNames, 5)) { g_cfg.preset = kPresetValues[preset_idx]; dirty = true; }
    ImGui::TextWrapped("Presets differ in how hard DLSS clamps history against the current frame. "
                       "If motion warps around transparents (dust, smoke, flames), try E or F.");

    ImGui::Separator();
    ImGui::TextUnformatted("Motion vectors");
    ImGui::TextWrapped("%s", g_mv_status);
    if (g_mv_problem[0])
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f), "%s", g_mv_problem);
    else if (g.handles_ok)
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "provider matches the shader's DLSS5_MV_PROVIDER");
    ImGui::TextWrapped("%s", g_mv_probe);
    ImGui::TextWrapped("Change the provider with DLSS5_Feed.fx's DLSS5_MV_PROVIDER preprocessor definition: "
                       "0 texMotionVectors (qUINT, dh_uber_motion), 1 Launchpad, 2 VORT, 3 LumeniteFX Kernel, 4 LumeniteFX QuantMotion.");
    if (ImGui::SliderFloat("MV scale X", &g_cfg.mv_scale_x, 0.0f, 4.0f)) dirty = true;
    if (ImGui::SliderFloat("MV scale Y", &g_cfg.mv_scale_y, 0.0f, 4.0f)) dirty = true;

    if (ImGui::CollapsingHeader("Advanced"))
    {
        if (ImGui::SliderInt("Create delay (frames)", &g_cfg.create_delay, 0, 300)) dirty = true;
        ImGui::SameLine(); HelpMarker("Frames to hold a feature (re)build after a runtime (re)init -- "
                                       "the DLSS 5 add-on arms its NGX hooks asynchronously.");
        if (!g_renodx_lazy)
        {
            if (ImGui::SliderInt("Warm-up rebuild (frames)", &g_cfg.warmup_rebuild, 0, 600)) dirty = true;
            ImGui::SameLine(); HelpMarker("Re-creates the feature once after N delivered frames -- works around "
                                          "the classic DLSS 5 add-on latching STANDBY on its first create. "
                                          "Skipped automatically on v45+ (not shown as adjustable there).");
        }
        if (ImGui::InputInt("Raw create flags (-1 = auto)", &g_cfg.flags)) dirty = true;
        if (ImGui::SliderInt("Log first N frames", &g_cfg.log_frames, 0, 20)) dirty = true;
        if (ImGui::Button("Force one rebuild")) { ++g_cfg.rebuild; dirty = true; }
    }

    if (dirty) CfgSave();
}

// ---------------------------------------------------------------------------

// Fired by ReShade before the device (for Vulkan: from inside its vkCreateInstance
// hook, i.e. before the game's vkCreateDevice). That is the one moment the interop
// extensions can still be added from in-process -- see feed_vk_hook.h.
static bool OnCreateDevice(reshade::api::device_api api, uint32_t & /*api_version*/)
{
    if (api == reshade::api::device_api::vulkan)
        FeedVkHookInstall();
    return false;   // never change the requested API version
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_log_cs);
        GetModuleFileNameA(module, g_log_path, MAX_PATH);
        if (char *s = strrchr(g_log_path, '\\'))
            strcpy_s(s + 1, MAX_PATH - (s + 1 - g_log_path), "dlss5-feed.log");
        { FILE *f = nullptr; if (fopen_s(&f, g_log_path, "w") == 0 && f) fclose(f); }

        if (!reshade::register_addon(module)) return FALSE;

        g_prev_filter = SetUnhandledExceptionFilter(&CrashFilter);
        Log("dlss5-feed %s (built %s %s) attached.", FEED_VERSION, __DATE__, __TIME__);
        {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            Log("  host: %ls", exe);
        }
        CfgWriteDefault();
        CfgReload();
        DetectRenodxAddon();
        DetectToolkitAddon();

        reshade::register_event<reshade::addon_event::create_device>(OnCreateDevice);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
        reshade::register_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        reshade::register_overlay(nullptr, DrawOverlay);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        reshade::unregister_overlay(nullptr, DrawOverlay);
        reshade::unregister_event<reshade::addon_event::create_device>(OnCreateDevice);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
        reshade::unregister_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
        reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        FeedVkHookRemove();   // before this code is unmapped -- ReShade reloads add-ons per Vulkan instance
        g_ngx_dying = true;   // process is exiting: never call back into NGX
        ShutdownSession();
        reshade::unregister_addon(module);
        Log("shut down cleanly.");
    }
    return TRUE;
}
