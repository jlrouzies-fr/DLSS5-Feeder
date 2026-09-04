// dlss5-feed32 - the 32-bit in-game half of DLSS5-Feeder for 32-bit D3D11, OpenGL
// and Vulkan (DXVK) games.
//
// A 32-bit game cannot load NGX or the DLSS 5 add-on (x64-only), so this add-on
// does none of that. Four GPU textures are shared ACROSS PROCESSES, the frame plus
// the companion effect's depth/motion-vector textures are copied into them, a
// shared fence is signalled, and dlss5-feed-host64.exe -- spawned from the host64\
// subfolder, where ReShade x64 + renodx-dlss5.addon64 live -- runs the DLSS DLAA +
// neural-rendering evaluate. The result comes back through the shared Output
// texture, GPU-fenced, and is blitted over the backbuffer.
//
// Which side CREATES those textures is the driver's call, not ours, and it differs:
//
//  * D3D11: created here, opened by the host -- the phase-0-proven direction.
//  * OpenGL: created by the HOST and imported here, because GL memory objects are
//    import-only (there is no export in GL_EXT_external_objects_win32). The GL half
//    is raw, through the very same src/feed_gl.h the 64-bit add-on uses, compiled
//    x86; both directions are proven by spike/spike-gl32.exe. See PLAN-OPENGL §5.
//  * D3D10: created here as well -- but by a PRIVATE D3D11 RELAY DEVICE, not by the
//    game's. A D3D10.1 device has no NT-handle sharing, no fence and no UAV, so it
//    cannot reach the host at all; it hands its frame to the relay over a legacy
//    keyed-mutex texture (src/feed_d3d10.h) and from there this is the D3D11 client
//    above, unchanged -- same client kind, same protocol version, same host.
//  * Vulkan: created by the HOST too, because D3D12 cannot open what Vulkan exports.
//    The transport is src/feed_vk.h -- again the 64-bit add-on's own header, compiled
//    x86 -- with the queue signal/wait going through ReShade (an api::fence handle IS
//    a VkSemaphore in its Vulkan backend), never a raw vkQueueSubmit. Proven by
//    spike/spike-vkclient32.exe. See PLAN-VULKAN32; the audience is DXVK, which is
//    how most surviving 32-bit games reach Vulkan at all (issue #15).
//
// If the host dies, the pipe breaks and the game just renders normally.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <dwmapi.h>   // the cast: a DWM live thumbnail of the host window, drawn in the game window
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

#define ImTextureID ImU64   // required by reshade_overlay.hpp before including imgui.h
#include <imgui.h>
#include <reshade.hpp>

#include "feed_ipc.h"
#include "feed_fmt.h"  // the DXGI format decisions shared with the host
#include "feed_fsr1.h" // AMD FSR 1 EASU + RCAS: the optional expand-back for work_resolution < 100%
#include "feed_gl.h"   // raw-OpenGL interop, the same header the 64-bit add-on uses
#include "feed_vk.h"   // raw-Vulkan interop, likewise -- compiled x86 here
#include "feed_vk_hook.h"   // in-process vkCreateDevice hook: appends the interop extensions
#include "feed_d3d10.h"     // D3D10.1 <-> D3D11 keyed-mutex bridge + the private relay device
#include "feed_dfc.h"       // Deep Fried Chicken: only the file scan is used here (it lives in host64\)

#define FEED_VERSION "0.13.1-beta.1"

extern "C" __declspec(dllexport) const char *NAME = "DLSS 5 Feed (32-bit) " FEED_VERSION;
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Feeds DLSS 5 neural rendering in 32-bit D3D10, D3D11, OpenGL and Vulkan (DXVK) games without DLSS: ships the frame, depth and "
    "motion vectors to a 64-bit helper process (host64\\dlss5-feed-host64.exe) over cross-process "
    "shared GPU textures, and blits the neural result back. Needs DLSS5_Feed.fx and a motion-vector "
    "provider (DRME, qUINT, Launchpad, VORT or LumeniteFX; pick it with the DLSS5_MV_PROVIDER definition). "
    "Settings in dlss5-feed.cfg.";

// ---------------------------------------------------------------------------
// Logging (same shape as the 64-bit add-on)
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
    EnterCriticalSection(&g_log_cs);
    FILE *f = nullptr;
    if (fopen_s(&f, g_log_path, "a") == 0 && f != nullptr)
    {
        fprintf(f, "%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_cs);
}

static void Warn(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    Log("%s", line);
    char tagged[1100];
    _snprintf_s(tagged, sizeof(tagged), _TRUNCATE, "[DLSS 5 Feed 32] %s", line);
    reshade::log::message(reshade::log::level::warning, tagged);
}

// The initial value has to read as "nothing has happened yet", not as a phase. It used to
// say "starting up", which is what a crash line reports whenever no Breadcrumb has been
// reached -- and every Breadcrumb is inside the feed path, so with the feed off, or before
// the first shared-texture build, it can never say anything else. A reporter (and the
// maintainer answering them) read that as evidence the crash happened during our startup,
// which it is not: the useful half of the line is the faulting module (issue #44).
static const char *volatile g_where = "nothing yet -- no feed work has run in this process";
static void Breadcrumb(const char *what) { g_where = what; }

// The 64-bit add-on has recorded crashes with a breadcrumb since 0.8; this side only
// set the breadcrumb and never read it, so a 32-bit crash left nothing in the log past
// the last ordinary line. Same filter here, plus a minidump next to the log.
//
// dbghelp is resolved EARLY (FeedResolveDbghelp, from an effect-runtime init) rather
// than inside the filter: ReShade refuses a LoadLibrary made from a thread it considers
// deadlock-prone and logs "Ignoring LoadLibrary('dbghelp.dll') call to avoid possible
// deadlock", which is how the one crash worth a dump ended up with none.
typedef BOOL (WINAPI *PFN_MiniDumpWriteDump_)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
static PFN_MiniDumpWriteDump_ g_write_dump;

// Called from an event where a LoadLibrary is safe (never from DllMain, never from the
// exception filter). Cheap and idempotent.
static void FeedResolveDbghelp()
{
    if (g_write_dump != nullptr) return;
    if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll"))
        g_write_dump = reinterpret_cast<PFN_MiniDumpWriteDump_>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
}

static void WriteCrashDump(EXCEPTION_POINTERS *ep)
{
    char path[MAX_PATH];
    strcpy_s(path, g_log_path);
    if (char *s = strrchr(path, '\\')) strcpy_s(s + 1, MAX_PATH - (s + 1 - path), "dlss5-feed-crash.dmp");
    // Last resort only: if the early resolve never ran, try anyway -- ReShade may refuse it.
    PFN_MiniDumpWriteDump_ write = g_write_dump;
    if (write == nullptr)
    {
        if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll"))
            write = reinterpret_cast<PFN_MiniDumpWriteDump_>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    }
    if (write == nullptr) { Log("[feed32] no dbghelp.dll; no crash dump written"); return; }
    // FILE_SHARE_READ: a second thread faulting at the same moment should be able to read
    // this file rather than fail with a sharing violation (error 32).
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log("[feed32] could not create %s (error %lu)", path, GetLastError()); return; }
    struct { DWORD tid; EXCEPTION_POINTERS *ep; BOOL client; } info = { GetCurrentThreadId(), ep, FALSE };
    const int type = 0x0040 | 0x0001 | 0x0004;   // IndirectlyReferencedMemory | DataSegs | HandleData
    const BOOL  ok  = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep != nullptr ? &info : nullptr, nullptr, nullptr);
    const DWORD err = ok ? 0 : GetLastError();   // before CloseHandle, which overwrites it
    CloseHandle(f);
    if (ok) Log("[feed32] crash dump written: %s -- attach it to the issue with this log", path);
    else    Log("[feed32] crash dump FAILED (%s, error %lu)", path, err);
}

static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;
static volatile LONG g_crash_once;
// An access violation carries two extra words, and they are the difference between "the
// game read a null pointer" and "we wrote off the end of something". Free to log, and until
// now only recoverable by opening the minidump by hand -- which is exactly what issue #44
// (Bayonetta) needed to establish that the fault was a read of address 0 in the game's own
// code, before this add-on had fed a single frame.
static void CrashAccessDetail(const EXCEPTION_RECORD *r, char *out, size_t out_size)
{
    out[0] = '\0';
    if (r == nullptr || r->NumberParameters < 2) return;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && r->ExceptionCode != EXCEPTION_IN_PAGE_ERROR) return;
    const char *verb = r->ExceptionInformation[0] == 0 ? "reading"
                     : r->ExceptionInformation[0] == 1 ? "writing"
                     : r->ExceptionInformation[0] == 8 ? "executing" : "accessing";
    _snprintf_s(out, out_size, _TRUNCATE, " (%s address %p)", verb,
                reinterpret_cast<void *>(r->ExceptionInformation[1]));
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    // One record per process, and only one thread may make it. Two ways this is reached
    // more than once: a game whose own handler resumes the faulting instruction comes
    // back every few hundred ms (WormsXHD at exit, 31 times), and a GPU fault takes out
    // every thread inside the driver at once (The Surge 2, eight of them -- they raced
    // for the same dump file and seven got a sharing violation).
    if (InterlockedCompareExchange(&g_crash_once, 1, 0) != 0)
        return g_prev_filter != nullptr ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;

    const void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    wchar_t owner[MAX_PATH] = L"unknown";
    HMODULE mod = nullptr;
    if (addr != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &mod) && mod != nullptr)
        GetModuleFileNameW(mod, owner, MAX_PATH);
    char access[64];
    CrashAccessDetail(ep != nullptr ? ep->ExceptionRecord : nullptr, access, sizeof(access));
    Log("### CRASH RECORDED ###  exception 0x%08X%s at %p in %ls; this add-on was last doing: %s%s "
        "(later faults in this process are not recorded)", code, access, addr,
        owner, g_where, mod == g_self ? " (inside this add-on)" : "");
    WriteCrashDump(ep);
    return g_prev_filter != nullptr ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// NVIDIA Smooth Motion, and the serialization it forces -- the 64-bit add-on's
// note explains the mechanism in full. In short: the driver injects a present
// interposer that wraps the game's swapchain and calls Present more than once
// per game frame from its own pacer thread. ReShade's effect chain, and so
// FeedFrame, runs inside Present, which makes FeedFrame re-entrant and possibly
// off-thread -- and everything it touches (the g struct, the shared textures,
// the host protocol) assumes exactly one caller at a time.
//
// Detection is best-effort here. NVIDIA documents no 32-bit Smooth Motion
// interposer and a WOW64 process cannot load the 64-bit one, so this is likely
// to stay false in practice; the lock below is what actually matters, and it
// costs nothing when there is only ever one thread.
// ---------------------------------------------------------------------------

static bool g_smooth_motion = false;

static bool DetectSmoothMotion()
{
    if (g_smooth_motion) return true;
    static const wchar_t *kNames[] = { L"NvPresent32.dll", L"NvPresent.dll" };
    for (const wchar_t *n : kNames)
    {
        if (GetModuleHandleW(n) == nullptr) continue;
        g_smooth_motion = true;
        Warn("NVIDIA Smooth Motion appears to be active in this process (%ls). It presents more than once "
             "per game frame from its own thread. If the image corrupts or flickers, turn it off for this "
             "game's API only -- NVIDIA Profile Inspector, \"Smooth Motion - Enabled APIs\" (0xB0CC0875).", n);
        return true;
    }
    return false;
}

// One lock for the whole per-frame path plus a busy flag: the lock serializes two
// Present threads, and the flag catches a re-entrant Present on the SAME thread,
// which a recursive CRITICAL_SECTION would wave straight through into a half-built
// frame. A re-entrant call is dropped -- there is one set of shared textures and one
// host request in flight, and driving them twice at once corrupts both.
static CRITICAL_SECTION g_feed_cs;
static bool             g_feed_busy   = false;   // guarded by g_feed_cs
static DWORD            g_feed_thread = 0;
static int              g_feed_offthread_logged = 0;
static int              g_feed_reentry_logged   = 0;
static unsigned         g_feed_reentries = 0;

// Called with g_feed_cs held, so these counters need no synchronization of their own.
static void FeedThreadTrace()
{
    const DWORD tid = GetCurrentThreadId();
    if (g_feed_thread == 0)
    {
        g_feed_thread = tid;
        Log("[feed32] first frame fed from thread %lu", tid);
    }
    else if (tid != g_feed_thread && g_feed_offthread_logged < 8)
    {
        ++g_feed_offthread_logged;
        Log("[feed32] frame fed from thread %lu, not the usual %lu -- Present is off-thread%s", tid, g_feed_thread,
            g_feed_offthread_logged == 8 ? "; further thread changes not logged" : "");
    }
}

// The lock order is always g_feed_cs then g_log_cs (Log's own); nothing takes them
// the other way round, so logging from inside the critical section is safe.
static bool FeedEnter()
{
    EnterCriticalSection(&g_feed_cs);
    if (g_feed_busy)
    {
        ++g_feed_reentries;
        if (g_feed_reentry_logged < 8)
        {
            ++g_feed_reentry_logged;
            Log("[feed32] re-entrant frame on thread %lu dropped (%u so far)%s", GetCurrentThreadId(),
                g_feed_reentries, g_feed_reentry_logged == 8 ? "; further drops not logged" : "");
        }
        LeaveCriticalSection(&g_feed_cs);
        return false;
    }
    g_feed_busy = true;
    return true;
}

static void FeedLeave()
{
    g_feed_busy = false;
    LeaveCriticalSection(&g_feed_cs);
}

// ---------------------------------------------------------------------------
// Configuration: same dlss5-feed.cfg as the 64-bit add-on (extra keys ignored)
// ---------------------------------------------------------------------------

struct Cfg
{
    int   enabled;
    int   mode;            // 0 inert, 1 transport test THROUGH the host (no NGX), 2 full DLSS path
    int   hdr;             // -1 auto, 0/1 force
    int   depth_inverted;  // -1 auto (RESHADE_DEPTH_INPUT_IS_REVERSED), 0/1 force
    int   flags;           // -1 auto, else raw DLSS.Feature.Create.Flags (host applies)
    int   reset_every;
    int   log_frames;
    int   host_window;     // 0 = the host's window sits behind the game, off the taskbar, and its DLSS 5
                           // tuning panel is cast INTO the game window on request (the overlay's "Show the
                           // DLSS 5 panel in-game" button / cast_key). 1 = the host's own visible window,
                           // the old way (press Home there). Read when the host is started.
    int   work_resolution; // 50..100 percent of each backbuffer axis; the game stays native-sized
    int   work_upscale;    // expand-back of the work-size output: 0 = bilinear, 1 = AMD FSR 1
                           // (EASU + RCAS), 2 = DLSS Super Resolution on synthetic jitter (D3D11
                           // clients, IPC v6: the host creates an SR feature, the Output is native
                           // size). Same key and meaning as the 64-bit add-on (issue #34)
    float work_sharpness;  // RCAS strength for work_upscale=1, 0 (off) .. 1 (sharpest)
    int   async_home;      // 1 = the copy home carries the PREVIOUS frame's output and waits on
                           // the fence value of the frame BEFORE this one, so the game's present
                           // never waits on this frame's round trip through the host process.
                           // Costs one frame of DLSS latency, which the temporal history hides.
                           // 0 = the same-frame contract this add-on shipped with. Same name and
                           // meaning as the 64-bit add-on's knob for its Vulkan transport.
    float mv_scale_x, mv_scale_y;
    int   cast_key;        // virtual-key code that shows/hides the cast DLSS 5 panel in-game; 0 = none.
                           // Bound from the overlay page ("Set key").
    int   cast_scale;      // size of the cast panel, 25..100 percent of the largest size that fits the game
                           // window (the host's tab column at 1:1 or shrunk to the game's height)
    int   cast_mode;       // how the panel is displayed: 0 = desktop-compositor thumbnail of the host window
                           // (windowed / borderless only), 1 = the host's shared panel texture drawn by the
                           // game's ReShade (IPC v7; works in exclusive fullscreen; all three client APIs)
    int   host_creates;    // 0 = auto: this side creates the shared set and falls back to the host only when
                           // its device refuses one (a feature-level 10.x game, issue #33/#43). 1 = always
                           // let the host create it, which is the only way to exercise that path on a device
                           // that does not need it. Parse-only, not written back, not on the overlay.
};

static Cfg g_cfg = { 1, 2, -1, -1, -1, 0, 3, 0, 100, 0, 0.3f, 1, 1.0f, 1.0f, 0, 100, 0, 0 };
static int       g_work_resolution_ui = 100;
static int       g_pending_work_resolution = 0;
static ULONGLONG g_work_resolution_apply_after = 0;

// NGX work textures use even dimensions; 100% must return the native extent untouched.
static UINT ScaledExtent(UINT native_extent, int percent)
{
    if (percent >= 100) return native_extent;
    UINT extent = (native_extent * static_cast<UINT>(percent)) / 100u;
    extent &= ~1u;
    return extent >= 2u ? extent : 2u;
}

// Same, rounding UP to even: what a DLSS Super Resolution build asks for, so the work
// size never falls below the preset's minimum render size (ceil of 50% of the output).
static UINT ScaledExtentUp(UINT native_extent, int percent)
{
    if (percent >= 100) return native_extent;
    UINT extent = (native_extent * static_cast<UINT>(percent) + 99u) / 100u;
    extent = (extent + 1u) & ~1u;
    return extent < native_extent ? extent : native_extent;
}

// Halton(2,3) low-discrepancy sequence, centred on the pixel: each value in [-0.5, 0.5).
// Index 0 (and every wrap) is the unshifted sample, which is what a history reset starts from.
static void HaltonJitter(UINT index, UINT phases, float *x, float *y)
{
    if (phases == 0) phases = 8;
    const UINT k = index % phases;
    if (k == 0) { *x = 0.0f; *y = 0.0f; return; }
    float fx = 0.0f, inv = 0.5f;
    for (UINT n = k; n != 0; n /= 2) { fx += inv * static_cast<float>(n % 2); inv *= 0.5f; }
    float fy = 0.0f; inv = 1.0f / 3.0f;
    for (UINT n = k; n != 0; n /= 3) { fy += inv * static_cast<float>(n % 3); inv /= 3.0f; }
    *x = fx - 0.5f;
    *y = fy - 0.5f;
}

static const char *SrQualityName(uint32_t q)
{
    switch (q)
    {
    case 0: return "Performance";       // NVSDK_NGX_PerfQuality_Value_MaxPerf
    case 1: return "Balanced";
    case 2: return "Quality";
    case 3: return "Ultra Performance";
    case 4: return "Ultra Quality";
    default: return "?";
    }
}

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
    fprintf(f, "enabled=%d\nmode=%d\nhdr=%d\ndepth_inverted=%d\nflags=%d\nreset_every=%d\nlog_frames=%d\n"
               "host_window=%d\nwork_resolution=%d\nwork_upscale=%d\nwork_sharpness=%.2f\nasync_home=%d\nmv_scale_x=%.3f\nmv_scale_y=%.3f\ncast_key=%d\ncast_scale=%d\ncast_mode=%d\n",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
            g_cfg.log_frames, g_cfg.host_window, g_cfg.work_resolution, g_cfg.work_upscale, g_cfg.work_sharpness,
            g_cfg.async_home, g_cfg.mv_scale_x, g_cfg.mv_scale_y, g_cfg.cast_key, g_cfg.cast_scale, g_cfg.cast_mode);
    fclose(f);
}

// The keys CfgSave() writes. Anything else in the file is carried over untouched: comments,
// and any key a different build of this add-on understands. (The 64-bit side lost hand-set
// diagnostics this way -- see its CfgSave.)
static const char *const kCfgSavedKeys[] = {
    "enabled", "mode", "hdr", "depth_inverted", "flags", "reset_every", "log_frames",
    "host_window", "work_resolution", "work_upscale", "work_sharpness", "async_home",
    "mv_scale_x", "mv_scale_y", "cast_key", "cast_scale", "cast_mode",
};

static bool CfgKeyIsSaved(const char *key)
{
    for (const char *k : kCfgSavedKeys)
        if (_stricmp(k, key) == 0) return true;
    return false;
}

// Writes every current value -- used by the overlay page so an edit made there survives the
// next CfgReload() instead of being read back off the stale on-disk copy 60 frames later.
// Everything the file holds that is not one of ours is preserved rather than truncated away.
static void CfgSave()
{
    char path[MAX_PATH];
    CfgPath(path);

    std::string carried;
    FILE *r = nullptr;
    if (fopen_s(&r, path, "r") == 0 && r != nullptr)
    {
        char line[256];
        while (fgets(line, sizeof(line), r) != nullptr)
        {
            char key[64] = {};
            const char *eq = strchr(line, '=');
            bool ours = false;
            if (eq != nullptr && sscanf_s(line, "%63[^=]", key, static_cast<unsigned>(sizeof(key))) == 1)
            {
                size_t n = strlen(key);
                while (n > 0 && (key[n - 1] == ' ' || key[n - 1] == '\t')) key[--n] = '\0';
                const char *k = key;
                while (*k == ' ' || *k == '\t') ++k;
                ours = CfgKeyIsSaved(k);
            }
            if (ours) continue;   // rewritten below from g_cfg
            carried += line;
            if (carried.back() != '\n') carried += '\n';
        }
        fclose(r);
    }

    FILE *f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || f == nullptr) return;
    fprintf(f, "enabled=%d\nmode=%d\nhdr=%d\ndepth_inverted=%d\nflags=%d\nreset_every=%d\nlog_frames=%d\n"
               "host_window=%d\nwork_resolution=%d\nwork_upscale=%d\nwork_sharpness=%.2f\nasync_home=%d\nmv_scale_x=%.3f\nmv_scale_y=%.3f\ncast_key=%d\ncast_scale=%d\ncast_mode=%d\n",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
            g_cfg.log_frames, g_cfg.host_window, g_cfg.work_resolution, g_cfg.work_upscale, g_cfg.work_sharpness,
            g_cfg.async_home, g_cfg.mv_scale_x, g_cfg.mv_scale_y, g_cfg.cast_key, g_cfg.cast_scale, g_cfg.cast_mode);
    if (!carried.empty()) fputs(carried.c_str(), f);
    fclose(f);
}

// The slider drives a full shared-texture rebuild and a host round trip, so apply it
// once the user stops dragging rather than once per intermediate value.
static bool ApplyPendingWorkResolution()
{
    if (g_pending_work_resolution == 0 || GetTickCount64() < g_work_resolution_apply_after) return false;
    const int next = g_pending_work_resolution;
    g_pending_work_resolution = 0;
    g_work_resolution_apply_after = 0;
    if (next == g_cfg.work_resolution) return false;
    g_cfg.work_resolution = next;
    CfgSave();
    Log("[feed32] settled work resolution=%d%%; rebuilding the shared set", g_cfg.work_resolution);
    return true;
}

static bool CfgReload()   // true when a build-affecting value changed
{
    char path[MAX_PATH];
    CfgPath(path);
    // Runs on the game thread every 60 frames. Opening and parsing the file each time
    // is a needless visit to the filesystem when, almost always, nothing has changed --
    // so ask for the stamp first and go no further if it matches. The overlay's own
    // CfgSave rewrites the file, which moves the stamp, so edits still land.
    {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        static FILETIME last_write = {};
        static DWORD    last_size  = 0xFFFFFFFFu;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        {
            if (fad.ftLastWriteTime.dwLowDateTime  == last_write.dwLowDateTime &&
                fad.ftLastWriteTime.dwHighDateTime == last_write.dwHighDateTime &&
                fad.nFileSizeLow == last_size)
                return false;
            last_write = fad.ftLastWriteTime;
            last_size  = fad.nFileSizeLow;
        }
    }
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
        else if (_stricmp(key, "log_frames")     == 0) next.log_frames     = iv;
        else if (_stricmp(key, "host_window")    == 0) next.host_window    = iv;
        else if (_stricmp(key, "work_resolution")== 0) next.work_resolution = iv;
        else if (_stricmp(key, "work_upscale")   == 0) next.work_upscale   = iv;
        else if (_stricmp(key, "work_sharpness") == 0) next.work_sharpness = val;
        else if (_stricmp(key, "async_home")     == 0) next.async_home     = iv;
        else if (_stricmp(key, "mv_scale_x")     == 0) next.mv_scale_x     = val;
        else if (_stricmp(key, "mv_scale_y")     == 0) next.mv_scale_y     = val;
        else if (_stricmp(key, "cast_key")       == 0) next.cast_key       = (iv > 0 && iv < 256) ? iv : 0;
        else if (_stricmp(key, "cast_scale")     == 0) next.cast_scale     = iv < 25 ? 25 : iv > 300 ? 300 : iv;
        else if (_stricmp(key, "cast_mode")      == 0) next.cast_mode      = iv == 1 ? 1 : 0;
        else if (_stricmp(key, "host_creates")   == 0) next.host_creates   = iv == 1 ? 1 : 0;
    }
    fclose(f);
    if (next.work_resolution < 50 || next.work_resolution > 100) next.work_resolution = g_cfg.work_resolution;
    if (next.work_upscale < 0 || next.work_upscale > 2) next.work_upscale = g_cfg.work_upscale;
    if (next.work_sharpness < 0.0f || next.work_sharpness > 1.0f) next.work_sharpness = g_cfg.work_sharpness;
    const bool rebuild = next.mode != g_cfg.mode || next.hdr != g_cfg.hdr ||
                         next.depth_inverted != g_cfg.depth_inverted || next.flags != g_cfg.flags ||
                         next.mv_scale_x != g_cfg.mv_scale_x || next.mv_scale_y != g_cfg.mv_scale_y;
    const bool changed = memcmp(&next, &g_cfg, sizeof(Cfg)) != 0;
    if (changed)
    {
        g_cfg = next;
        // Every knob, not a selection of them. async_home in particular decides the whole
        // handoff contract, and no DXVK report could be triaged without asking the reporter
        // what they had set (issue #15). The 64-bit side has always printed its full set.
        Log("[feed32] config: enabled=%d mode=%d hdr=%d depth_inverted=%d flags=%d reset_every=%d log_frames=%d "
            "host_window=%d work_resolution=%d%% work_upscale=%d work_sharpness=%.2f async_home=%d "
            "mv_scale=%.3f,%.3f cast_key=%d cast_scale=%d cast_mode=%d host_creates=%d",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
            g_cfg.log_frames, g_cfg.host_window, g_cfg.work_resolution, g_cfg.work_upscale, g_cfg.work_sharpness,
            g_cfg.async_home, g_cfg.mv_scale_x, g_cfg.mv_scale_y, g_cfg.cast_key, g_cfg.cast_scale,
            g_cfg.cast_mode, g_cfg.host_creates);
    }
    return rebuild;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static const char *kEffectFile    = "DLSS5_Feed.fx";
static const char *kTechnique     = "DLSS5_Feed";
// Known motion-vector providers, keyed by the DLSS5_MV_PROVIDER value DLSS5_Feed.fx
// was compiled with (0 texMotionVectors, 1 Launchpad, 2 VORT, 3 LumeniteFX Kernel,
// 4 LumeniteFX QuantMotion). Name checks only, for the status line and a mismatch warning.
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

static char g_mv_status[192]  = "not checked yet";
static char g_mv_problem[640] = "";

// A provider whose effect failed to compile is still listed (and can be "enabled") but writes
// nothing. ReShade logs the compiler error next to the game; the last line about that file
// -- an error or a "Successfully compiled" -- is the current state. Same as the 64-bit add-on.
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
    const long take = size < 512 * 1024 ? size : 512 * 1024;
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

static int ReadMvProviderMode(reshade::api::effect_runtime *rt)
{
    char v[16] = {};
    int mode = 0;
    if (rt->get_preprocessor_definition_for_effect(kEffectFile, "DLSS5_MV_PROVIDER", v) ||
        rt->get_preprocessor_definition("DLSS5_MV_PROVIDER", v))
        mode = atoi(v);
    return (mode < 0 || mode >= kMvModeCount) ? 0 : mode;
}

struct Feed32
{
    reshade::api::effect_runtime          *runtime;
    reshade::api::effect_technique         technique;
    reshade::api::effect_technique         launchpad;
    reshade::api::effect_texture_variable  mv_var;
    reshade::api::effect_texture_variable  depth_var;

    bool depth_reversed;
    bool handles_ok;
    bool missing_reported;

    bool disabled;
    int  consecutive_fails;

    // host + pipe
    HANDLE hproc;
    HANDLE pipe;

    // shared textures (created HERE, opened by the host -- unless host_creates)
    ID3D11Texture2D *tex[FEED_SLOTS];
    HANDLE           tex_handle[FEED_SLOTS];
    bool             host_creates;  // D3D11 client: the game's device refused the shared set once; the host creates it now
    bool             no_uav;        // ... and cannot bind UAVs at all (feature level 10.x): the Output is shared without one
    ID3D11ShaderResourceView *output_srv;
    ID3D11Texture2D          *panel_tex;   // v7: the panel texture, created HERE at the host's frame size; the host
    ID3D11ShaderResourceView *panel_srv;   // copies every presented frame into it (cast_mode=1 draws it)
    HANDLE                    panel_handle;
    UINT                      panel_w, panel_h;   // from the hello ack; 0 = the host has no panel
    GLuint                    gl_panel_tex, gl_panel_memobj;   // GL / Vulkan: the HOST-created panel, imported
    VkImage                   vk_panel;                        // (they cannot export one for the host to open)
    VkDeviceMemory            vk_panel_mem;
    bool                      vk_panel_init;                   // moved UNDEFINED -> GENERAL once
    ID3D11RenderTargetView   *input_rtv[FEED_SLOTS];  // work-resolution resample targets
    ID3D11Texture2D          *color_stage;            // native-size copy of the frame (the only SRV-able source)
    ID3D11ShaderResourceView *color_stage_srv;
    ID3D11Texture2D          *easu_tex;               // work_upscale=1: native-size EASU result, RCAS reads it
    ID3D11RenderTargetView   *easu_rtv;
    ID3D11ShaderResourceView *easu_srv;
    ID3D11Fence     *fence_in;    // we signal        (D3D11 client)
    ID3D11Fence     *fence_out;   // host signals     (D3D11 client)
    HANDLE           fence_in_handle, fence_out_handle;   // GL client: kept for the GL import
    bool             fence_wait_queued;  // a GPU-side Wait(fence_out, frame_n) is outstanding
    ID3D11DeviceContext4 *ctx4;
    ID3D11Device    *dev;         // not owned
    ID3D11Multithread *mt;        // immediate-context serialization, restored on teardown
    bool             mt_was_on;   // what the game had set before we turned it on

    // OpenGL client: the host creates the shared textures and duplicates the handles
    // in; we import them raw (feed_gl.h) and never touch D3D11 at all. tex_handle[]
    // above holds the received handles, closed by ReleaseShared exactly as before.
    bool   is_gl;
    FeedGl gl;
    HGLRC  gl_ctx;               // the context the imports live in (share-group check)
    GLuint gl_tex[FEED_SLOTS], gl_memobj[FEED_SLOTS];
    GLuint gl_sem_in, gl_sem_out;
    GLuint gl_fbo_read, gl_fbo_draw;

    // Vulkan client: the host creates the shared textures here too (D3D12 cannot open
    // Vulkan-exported memory), and we import them as VkImages. The per-frame COPIES are
    // raw vkCmd* recorded into ReShade's own command buffer, but every QUEUE operation
    // goes through ReShade -- an api::fence handle IS a VkSemaphore in its Vulkan
    // backend -- so signal/wait stay inside its locks and never race the game's submits.
    bool                       is_vulkan;
    FeedVk                     vk;
    reshade::api::device       *rs_dev;     // not owned
    reshade::api::command_queue *rs_queue;  // not owned
    reshade::api::fence         rs_fence_in, rs_fence_out;   // our vk_sem_* punned back
    VkImage                    vk_img[FEED_SLOTS];
    VkDeviceMemory             vk_mem[FEED_SLOTS];
    VkSemaphore                vk_sem_in, vk_sem_out;
    bool                       vk_layout_init;   // our images transitioned UNDEFINED->GENERAL once
    bool                       vk_released;      // our images are released to VK_QUEUE_FAMILY_EXTERNAL
                                                 // (the host's D3D12 device owns them until the next acquire)

    // D3D10 client: not really a client at all. A private D3D11 relay device (feed_d3d10.h)
    // is what talks to the host, so g.dev, g.ctx4, g.tex[] and g.fence_* below are all the
    // RELAY's -- everything from BuildShared down is the D3D11 path, byte for byte. The
    // game's own D3D10.1 device only ever does CopyResource, into and out of the bridges.
    bool            is_d3d10;
    FeedD3D10       d10;
    FeedD3D10Bridge d10_bridge[FEED_SLOTS];   // native-sized; FEED_OUTPUT is the one with an RTV
    ID3D10Device1  *d10_dev;                  // not owned, and only for the log line

    bool        built;
    UINT        width, height;                  // the work resolution DLSS runs at
    UINT        output_width, output_height;    // the Output slot: == work (DLAA) or native (work_upscale=2)
    // work_upscale=2: DLSS Super Resolution on synthetic jitter (D3D11 client, IPC v6)
    bool        sr_requested;      // what the current build asked the host for
    bool        sr_active;         // the host created an SR feature (FEED_ACK_SR_ACTIVE)
    bool        sr_unavailable;    // the host said no preset covers this ratio; cleared on a size change
    uint32_t    sr_quality;        // the host's NVSDK_NGX_PerfQuality_Value
    UINT        jitter_index;      // position in the Halton sequence, restarts with the DLSS history
    UINT        jitter_phases;
    float       jitter_x, jitter_y;   // this frame's grid shift, in work pixels
    UINT        backbuffer_width, backbuffer_height;
    DXGI_FORMAT bb_fmt, color_fmt, output_fmt;
    UINT64      frame_n;
    // async_home bookkeeping. frame_n counts every frame this add-on has ever sent and
    // never restarts; the FENCES do, because each host process creates its own pair from
    // zero. So the value to wait on is the last frame THIS host accepted, not frame_n - 1:
    // after a respawn the latter is a value the new fence will never reach, and the wait
    // sits in front of the very submit whose signal would let the host produce it -- a
    // deadlock that ends in a TDR rather than a stall.
    UINT64      sent_n;       // last frame message this host session took (0 = none yet)
    UINT64      wait_n;       // newest GPU-side wait queued on fence_out; what HostDrain owes
    bool        out_valid;    // Output holds a host-written frame that is safe to carry home
    bool        need_reset;

    // blit
    ID3D11VertexShader *blit_vs;
    ID3D11PixelShader  *blit_ps;
    ID3D11PixelShader  *resample_ps;
    ID3D11SamplerState *blit_sampler;
    ID3D11SamplerState *point_sampler;
    ID3D11Buffer       *resample_cb;

    // work_upscale=1 (feed_fsr1.h). Optional: when the compile fails the blit stays bilinear.
    ID3D11PixelShader  *easu_ps;
    ID3D11PixelShader  *rcas_ps;
    ID3D11Buffer       *fsr_cb;
    bool   fsr_ok;
    UINT   fsr_in_w, fsr_in_h, fsr_out_w, fsr_out_h;   // what fsr_cb currently describes
    float  fsr_sharpness;

    UINT64   frames_done;
    LONGLONG qpf, cpu_ticks, span_start;
    UINT64   timed_frames;
};

static Feed32 g;

template <typename T> static void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

static DXGI_FORMAT TypedColorFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
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

static DXGI_FORMAT OutputFormatFor(DXGI_FORMAT color_typed)
{
    switch (color_typed)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:                             return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

// OpenGL has no sized BGRA8 internal format, so the GL path never asks the host for
// one. The colour moves by blit, which is component-wise, so a BGRA-flavoured game
// surface lands correctly in an RGBA8 shared texture and comes home the same way.
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

// Kept for the overlay, which otherwise can only point at a log file.
static char g_disable_why[256] = "";

static void FeedDisable(const char *why)
{
    if (g.disabled) return;
    g.disabled = true;
    _snprintf_s(g_disable_why, sizeof(g_disable_why), _TRUNCATE, "%s", why);
    Warn("stopped: %s. The game renders normally. See dlss5-feed.log for the detail.", why);
}

// A build can fail transiently -- the game is mid-resolution-change, or the host's NGX
// needs a reinit first. Retrying every frame just hammers a broken NGX (and spams the
// log), so back off exponentially instead of disabling the feed for the whole session:
// the user should not have to restart the game because one mode switch went wrong.
static UINT64 g_retry_at;   // GetTickCount64 deadline

static void FeedFail(const char *what)
{
    const int n = ++g.consecutive_fails;
    const DWORD wait_ms = n <= 3 ? 1000u : (n <= 6 ? 5000u : 30000u);
    g_retry_at = GetTickCount64() + wait_ms;
    Log("[feed32] failure: %s (attempt %d; retrying in %lu ms)", what, n, wait_ms);
}

// ---------------------------------------------------------------------------
// Host process + pipe
//
// Starting the helper, shaking hands with it and asking it to build all used to happen on
// the render thread, inside Present. Starting it meant spinning on CreateFile for up to
// 15 s and then blocking on the hello ack; every build blocked on an ack the host only
// sends once NGX is up and the feature is created, which on a cold host includes the
// ~165 MB model load. So every Apply, restart, resize and work-resolution change froze the
// game for seconds while the overlay's own text claimed it kept rendering. And a host that
// hung rather than died froze it for good: the pipe calls had no timeout, and HostAlive()
// cannot tell a hang from health.
//
// So: a worker thread does the waiting, the frame path polls it, and every pipe transfer
// is bounded. The worker touches nothing but Win32 and the pipe -- never ReShade, never the
// game's device -- and the pipe has exactly one user at a time by construction, not by
// lock: the render thread only writes a frame message once a build has finished, and a
// build only finishes after the job that carried it was consumed here.
// ---------------------------------------------------------------------------

enum { LINK_IDLE = 0, LINK_RUNNING, LINK_DONE, LINK_FAILED };
enum { JOB_CONNECT = 1, JOB_BUILD };

struct HostLink
{
    HANDLE        thread;
    volatile LONG state;        // LINK_*; the worker only ever moves RUNNING -> DONE/FAILED
    volatile LONG abort;        // set by the render thread; the worker checks it between steps
    HANDLE        abort_event;  // and it wakes every wait the worker is parked in
    int           job;          // JOB_*
    FeedBuild     build;        // JOB_BUILD in
    FeedBuildAck  ack;          // JOB_BUILD out
    HANDLE        pipe;         // JOB_CONNECT out -- adopted into g.pipe / g.hproc by the
    HANDLE        proc;         //   render thread, on success AND on failure (so HostClose
    uint32_t      panel_w;      //   disposes of a half-made link the usual way)
    uint32_t      panel_h;
    HMODULE       mod_ref;      // a reference on THIS module, held for the worker's lifetime
    bool          fatal;        // true = FeedDisable(why), false = HostLost(why)
    char          why[192];
    DWORD         ms;           // how long the job took, for the log
};
static HostLink g_link;

// How long we are willing to wait for the host at each step. The first two are the worker's,
// so the game never feels them. The last one IS on the frame path: 21 bytes into a 1 KB pipe
// buffer can only block if the host has stopped reading it, but a busy host can be a little
// late, and running out of patience there ends the session (HostLost). So: generous enough
// that reaching it really does mean the host is not coming back, short enough that the worst
// case is a survivable hitch rather than the indefinite freeze this replaced.
static const DWORD kPipeHelloMs = 15000;
static const DWORD kPipeBuildMs = 60000;
static const DWORD kPipeFrameMs = 2000;

static bool HostLinkStop();   // below: abort and join the worker, from HostClose
static bool g_detaching;      // DLL_PROCESS_DETACH: the loader lock is held, so never join

static bool HostRequestPending();   // below: an overlay button is waiting to be acted on
static void HostConsumeRequest();   // below: act on it, on the render thread, inside the lock

// A build is in flight (waiting for the host to connect, or for its answer). The frame path
// skips feeding while this is set WITHOUT counting a failure: the exponential backoff in
// FeedFail is for a build that went wrong, not for one that has simply not come back yet.
static bool g_build_pending;

static void HostDrain()
{
    // A GPU-side Wait(fence_out, wait_n) is queued on the immediate context every frame
    // BEFORE the host has signalled it -- wait_n is this frame under the same-frame
    // contract, and the one before it under async_home. If the host goes away first,
    // that wait can never be satisfied, and everything queued behind it -- Present --
    // wedges until the driver TDRs (seen as a system-wide freeze when it happened on
    // a settings apply). Never let go of a live host before the last submitted frame
    // has been signalled. The host also catch-up-signals fence_out on its way out,
    // so with both sides healthy this resolves in milliseconds.
    if (!g.fence_wait_queued) return;
    if (g.is_vulkan)
    {
        // The imported timeline semaphore IS the host's D3D12 fence, so it can be both
        // read and waited on with a deadline -- the same shape as the D3D11 arm below.
        g.fence_wait_queued = false;
        if (!g.vk.ok || g.vk_sem_out == VK_NULL_HANDLE) return;
        if (!FeedVkHasTimelineQueries(&g.vk))
        {
            // No way to ask or wait on the value (a device that imported a D3D12 fence
            // without vkWaitSemaphores should not exist, but do not guess). Driving the
            // queue to completion answers the same question the long way round.
            Log("[feed32] drain: no timeline query on this device; draining the queue instead");
            if (g.rs_queue != nullptr) g.rs_queue->wait_idle();
            return;
        }
        if (FeedVkTimelineValue(&g.vk, g.vk_sem_out) >= g.wait_n) return;
        if (g.hproc == nullptr || WaitForSingleObject(g.hproc, 0) != WAIT_TIMEOUT)
        {
            Log("[feed32] drain: host died before signalling frame %llu",
                static_cast<unsigned long long>(g.wait_n));
            return;
        }
        if (!FeedVkWaitTimeline(&g.vk, g.vk_sem_out, g.wait_n, 2000))
            Log("[feed32] drain: frame %llu never signalled by the host",
                static_cast<unsigned long long>(g.wait_n));
        return;
    }
    if (g.is_gl)
    {
        // On OpenGL the outstanding wait is a glWaitSemaphoreEXT, which has no timeout
        // and no readable value -- so instead of asking the fence, drive the stream to
        // completion with a deadline. Same intent, same 2 s, no way to hang the game.
        // Only the context that queued the wait can drain it; from anywhere else the
        // wait dies with the context anyway.
        g.fence_wait_queued = false;
        if (!g.gl.ok || g.gl.wglGetCurrentContext() != g.gl_ctx || g.gl_ctx == nullptr) return;
        if (!FeedGlWaitIdle(&g.gl, 2000))
            Log("[feed32] drain: the GL stream did not finish within 2 s (frame %llu never signalled by the host)",
                static_cast<unsigned long long>(g.wait_n));
        return;
    }
    if (g.fence_out == nullptr) return;
    g.fence_wait_queued = false;
    if (g.fence_out->GetCompletedValue() >= g.wait_n) return;
    if (g.hproc == nullptr || WaitForSingleObject(g.hproc, 0) != WAIT_TIMEOUT)
    {
        // The host is already dead and can no longer signal: the wait (if the GPU
        // reached it) is unsatisfiable and there is nothing we can do from D3D11.
        Log("[feed32] drain: host died before signalling frame %llu",
            static_cast<unsigned long long>(g.wait_n));
        return;
    }
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (evt == nullptr) return;
    if (SUCCEEDED(g.fence_out->SetEventOnCompletion(g.wait_n, evt)) &&
        WaitForSingleObject(evt, 2000) != WAIT_OBJECT_0)
        Log("[feed32] drain: frame %llu never signalled by the host",
            static_cast<unsigned long long>(g.wait_n));
    CloseHandle(evt);
}

static void CastHostLost();       // below: drop the thumbnail of a window that is going away
static void CastReleasePanel();   // below: and our view of its panel texture
static void CastFlushInput();     // below: release any key/button the host still thinks is held

static void HostClose()
{
    // First: nothing else here may run beside a worker still using the pipe. When it says
    // false an orphan survived, and g.pipe is the handle a JOB_BUILD worker holds.
    const bool worker_done = HostLinkStop();
    g_build_pending = false;   // whatever was in flight went with the link
    CastHostLost();
    CastReleasePanel();
    HostDrain();   // BEFORE the pipe closes: the host must still be around to signal
    // The next host starts its fences at zero, so nothing this session sent is a value
    // the new pair will ever reach. Forget them, and stop trusting Output's contents.
    g.sent_n    = 0;
    g.wait_n    = 0;
    g.out_valid = false;
    if (g.hproc != nullptr)
    {
        // Ending the host first is what unblocks an orphan worker parked on the pipe, and it
        // is always safe: no worker ever touches g.hproc. 4 s, because the host releases its
        // swapchain on the way out so ReShade x64 can save its ini (the overlay layout), and
        // ReShade's own unhook takes about a second.
        if (WaitForSingleObject(g.hproc, 4000) != WAIT_OBJECT_0)
            TerminateProcess(g.hproc, 0);      // it did not exit on the pipe break
        CloseHandle(g.hproc);
        g.hproc = nullptr;
    }
    if (g.pipe != nullptr)
    {
        // Closing it under a live worker would let the value be reused and the orphan's next
        // write land in an unrelated handle. Leaking one pipe handle is the cheaper mistake;
        // the host it named is gone by now, so the orphan's I/O fails and it exits.
        if (worker_done) CloseHandle(g.pipe);
        else Log("[feed32] leaking the pipe handle: a host worker may still be using it");
        g.pipe = nullptr;
    }
    // The fences belong to the host that just went away; a new host creates new ones.
    // Releasing them on EVERY close (not just the apply path) is what makes a respawn
    // reopen from the new host's BuildAck instead of waiting on dead fences forever.
    SafeRelease(g.fence_in);
    SafeRelease(g.fence_out);
    if (g.gl.ok && g.gl.wglGetCurrentContext() == g.gl_ctx && g.gl_ctx != nullptr)
    {
        if (g.gl_sem_in  != 0) { g.gl.DeleteSemaphoresEXT(1, &g.gl_sem_in);  g.gl_sem_in  = 0; }
        if (g.gl_sem_out != 0) { g.gl.DeleteSemaphoresEXT(1, &g.gl_sem_out); g.gl_sem_out = 0; }
    }
    g.gl_sem_in = g.gl_sem_out = 0;
    if (g.vk.ok)
    {
        // HostDrain above waits on the semaphore's VALUE, which the HOST's D3D12 queue
        // advances -- it says nothing about the GAME's queue having retired the batches
        // that signal fence_in and wait on fence_out, and vkDestroySemaphore requires
        // exactly that. The gap is not theoretical: if the frame message fails to write,
        // FeedFrameVk has already enqueued signal(rs_fence_in, n) but never set
        // fence_wait_queued, so the drain returns instantly and we would destroy a
        // semaphore with a signal still in flight on it.
        if (g.rs_queue != nullptr) g.rs_queue->wait_idle();
        if (g.vk_sem_in  != VK_NULL_HANDLE) { g.vk.DestroySemaphore(g.vk.dev, g.vk_sem_in,  nullptr); }
        if (g.vk_sem_out != VK_NULL_HANDLE) { g.vk.DestroySemaphore(g.vk.dev, g.vk_sem_out, nullptr); }
    }
    g.vk_sem_in = g.vk_sem_out = VK_NULL_HANDLE;
    g.rs_fence_in = g.rs_fence_out = {};
    if (g.fence_in_handle  != nullptr) { CloseHandle(g.fence_in_handle);  g.fence_in_handle  = nullptr; }
    if (g.fence_out_handle != nullptr) { CloseHandle(g.fence_out_handle); g.fence_out_handle = nullptr; }
    // A respawned host creates NEW fences and, on the paths where it owns them, new
    // textures as well. Force the next frame through a full rebuild so both are
    // re-imported instead of aliasing objects that died with the old host. The D3D11
    // path creates its own textures and is deliberately left as it was.
    if (g.is_gl || g.is_vulkan) g.built = false;
}

// Set when a restart is initiated from the overlay, so the game's own window can be put
// back in front once the replacement host is up. Windows only honours SetForegroundWindow
// from a process that is already foreground -- which the game is at the moment the user
// clicks Apply, so we capture it there and spend it a couple of seconds later.
static HWND g_restore_focus;

static void CaptureGameFocus()
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg != nullptr && GetWindowThreadProcessId(fg, &pid) != 0 && pid == GetCurrentProcessId())
        g_restore_focus = fg;   // only ever restore a window that is ours
}

static void RestoreGameFocus()
{
    if (g_restore_focus == nullptr) return;
    HWND w = g_restore_focus;
    g_restore_focus = nullptr;
    if (!IsWindow(w) || GetForegroundWindow() == w) return;
    SetForegroundWindow(w);
    Log("[feed32] focus returned to the game window");
}

static void HostLost(const char *why)
{
    DWORD code = 0;
    if (g.hproc != nullptr && GetExitCodeProcess(g.hproc, &code) && code != STILL_ACTIVE)
        Log("[feed32] host lost: %s (exit code %lu)", why, code);
    else
        Log("[feed32] host lost: %s", why);
    HostClose();
    FeedDisable("the 64-bit host went away -- its own dlss5-feed-host.log (in host64\\) names the reason");
}

static bool HostAlive()
{
    return g.hproc != nullptr && WaitForSingleObject(g.hproc, 0) == WAIT_TIMEOUT;
}

// ---------------------------------------------------------------------------
// The cast: the host's window, shown INSIDE the game window, clickable.
//
// The neural consumer's tuning panel (RenoDX's or Deep Fried Chicken's) is drawn by
// ReShade x64 over the host's own window. Reaching it used to mean alt-tabbing to that
// window, which some games do not survive, and the settings mirror on this page has to
// restart the host to apply anything, so changes are never seen live. Instead:
//
//  * Display: a DWM live thumbnail of the host window, registered on the game window.
//    The compositor paints the host's surface into a rect of the game window -- no
//    pixel transport, no bitness issue, and the host keeps its own swapchain. Only the
//    tab column of the host's overlay layout is shown (PrepareHostOverlay on the host
//    side gives the tabs everything but 96 px). DWM cannot draw over an exclusive-
//    fullscreen swapchain: the panel needs windowed / borderless.
//  * Input: while the panel is shown the game loses the mouse (block_input_next_frame
//    every frame, as ReShade's own overlay does -- otherwise a mouselook game hides and
//    re-centres the cursor and it can never reach the panel), a cursor is drawn in the
//    game by ReShade x86's ImGui, and while it is over the panel (or a button pressed
//    there is still held) the mouse and keys -- read through ReShade's runtime API --
//    are posted to the host window as the WM_* messages a real mouse would produce.
//    ReShade x64 in the host reads its input from the message queue (WH_GETMESSAGE),
//    which is also how the host opens its own overlay: it posts itself a Home key.
//
// The host window is found by class name + process id, so the pipe protocol is
// untouched. The host runs with --behind (host_window=0): shown, since DWM needs a
// shown non-minimized source, but a tool window (no taskbar button, never activated)
// parked at the bottom of the Z-order and moved under the game window from here.
// ---------------------------------------------------------------------------

static bool       g_cast_wanted;          // the user's intent; survives a host restart
static HTHUMBNAIL g_cast_thumb;
static HWND       g_cast_hwnd;            // the host's window
static HWND       g_cast_dest;            // the game window the thumbnail is registered on
static RECT       g_cast_rect;            // destination rect, game client coordinates
static SIZE       g_cast_src;             // source crop, host client pixels
static float      g_cast_scale = 1.0f;    // destination / source
static bool       g_cast_hover;           // the cursor was over the panel last frame
static bool       g_cast_captured;        // a button went down over the panel and is still held
static bool       g_cast_placed;          // the host window was moved under the game once
static bool       g_cast_fullscreen;      // the game's swapchain went exclusive fullscreen
static POINT      g_cast_last = { -1, -1 };
static POINT      g_cast_cursor = { -1, -1 };   // ReShade's mouse position this frame, game client coordinates
static bool       g_cast_capture_key;     // the overlay's "Set key" is waiting for a key
static bool       g_cast_texture;         // the mode the current layout was made in (cfg cast_mode)
static bool       g_game_overlay_open;    // ReShade x86's own overlay is up (it draws a cursor then)
static bool       g_cast_hid_cursor;      // we switched ReShade x86's software cursor off this frame
static bool       g_cast_close_hover;     // the cursor is over the panel's own close button

// The panel's close button: a square at its top-right corner, drawn by this add-on and
// handled before any click is forwarded, so a panel scaled over the whole window (and
// over the game's ReShade overlay with the Hide button) can still be dismissed.
static const int kCastCloseSize = 28;
static RECT CastCloseRect()
{
    const int s = static_cast<int>(kCastCloseSize * (g_cast_scale > 1.0f ? g_cast_scale : 1.0f));
    return { g_cast_rect.right - s, g_cast_rect.top, g_cast_rect.right, g_cast_rect.top + s };
}
static bool       g_cast_shown;           // something is on screen this frame (thumbnail or texture)
static char       g_cast_status[160] = "hidden";

// What this add-on has told the host is currently held down. Key and button messages are
// only forwarded while the cursor is over the panel, so a key released after the cursor
// left it -- or while the panel was being hidden by the toggle key, Escape, the close
// button or a host restart -- never got its WM_KEYUP, and ReShade x64's ImGui went on
// believing it was down: every later click became a Ctrl+click, text fields repeated,
// Shift stuck. CastFlushInput() releases whatever is still marked here.
static bool       g_cast_key_down[256];
static UINT       g_cast_btn_down;        // bit i = kCastButtons[i] is down in the host
static int16_t    g_cast_wheel_pending;   // wheel notches seen in reshade_overlay, consumed by CastInput

static HWND CastFindHostWindow()
{
    if (g.hproc == nullptr) return nullptr;
    const DWORD host_pid = GetProcessId(g.hproc);
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"dlss5feedhost", nullptr)) != nullptr;)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == host_pid) return w;
    }
    return nullptr;
}

static void CastRelease()   // the thumbnail only; the host window stays known
{
    // Before the panel goes away: hand the host a key-up for everything it still thinks is
    // held. This is the path every hide takes -- toggle key, Escape, Alt+F4, the close
    // button, a lost host, DLL detach -- so it is the one place that has to do it.
    CastFlushInput();
    if (g_cast_thumb != nullptr) { DwmUnregisterThumbnail(g_cast_thumb); g_cast_thumb = nullptr; }
    g_cast_dest     = nullptr;
    g_cast_rect     = {};
    g_cast_hover    = false;
    g_cast_captured = false;
    g_cast_shown    = false;
    g_cast_last     = { -1, -1 };
    g_cast_cursor   = { -1, -1 };
}

static void CastHostLost()
{
    CastRelease();
    g_cast_hwnd   = nullptr;
    g_cast_placed = false;
}

// cast_mode=1: the panel texture (IPC v7), created on the game's D3D11 device at the size
// the hello ack named and handed to the host with every build, which copies its presented
// frame into it. This direction because the other one is refused: OpenSharedResource1 on a
// D3D12-created texture came back E_INVALIDARG on this driver (Fable, 2026-09-02).
static void CastReleasePanel()
{
    SafeRelease(g.panel_srv);
    SafeRelease(g.panel_tex);
    if (g.panel_handle != nullptr) { CloseHandle(g.panel_handle); g.panel_handle = nullptr; }
    g.panel_w = g.panel_h = 0;
}

// Returns the handle value to put in FeedBuild::panel_tex (0 = none).
static uint64_t CastMakePanel()
{
    // Not on D3D10. The panel is drawn by handing its shader resource view to the GAME's
    // ReShade as an ImTextureID, and on a D3D10 game that ImGui backend wants an
    // ID3D10ShaderResourceView -- ours would be a D3D11 view from the relay wearing the
    // wrong vtable. Refusing here leaves CastPanelAvailable() false, which is what makes
    // the cast fall back to the compositor thumbnail; see the note in CastLayout.
    if (g.is_d3d10) return 0;
    if (g.dev == nullptr || g.panel_w == 0 || g.panel_h == 0) return 0;
    if (g.panel_handle != nullptr) return reinterpret_cast<uintptr_t>(g.panel_handle);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = g.panel_w;
    td.Height           = g.panel_h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = g.dev->CreateTexture2D(&td, nullptr, &g.panel_tex);
    if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(g.panel_tex, nullptr, &g.panel_srv);
    IDXGIResource1 *r = nullptr;
    if (SUCCEEDED(hr)) hr = g.panel_tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&r));
    if (SUCCEEDED(hr))
    {
        hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &g.panel_handle);
        r->Release();
    }
    if (FAILED(hr))
    {
        Log("[feed32] cast: panel texture %ux%u failed 0x%08X; texture mode unavailable", g.panel_w, g.panel_h, hr);
        SafeRelease(g.panel_srv);
        SafeRelease(g.panel_tex);
        g.panel_w = g.panel_h = 0;   // do not retry every build
        return 0;
    }
    Log("[feed32] cast: panel texture %ux%u created and handed to the host", g.panel_w, g.panel_h);
    return reinterpret_cast<uintptr_t>(g.panel_handle);
}

// GL / Vulkan clients: import the host-created panel a build ack carries. The GL and
// Vulkan objects are dropped in ReleaseShared with the four slots, so this runs per build.
static void CastImportPanelGl(const FeedBuildAck &ack)
{
    if (ack.panel_tex == 0) return;
    HANDLE hnd = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.panel_tex));
    // The host duplicates a FRESH handle into this process on every build. When we are not
    // importing it -- the panel is already there, or the hello ack named no size -- it still
    // has to be closed here, or every rebuild leaks one.
    if (g.panel_w == 0 || g.gl_panel_tex != 0) { CloseHandle(hnd); return; }
    if (!FeedGlImportImage(&g.gl, hnd, ack.panel_size, static_cast<GLsizei>(g.panel_w), static_cast<GLsizei>(g.panel_h),
                           GL_RGBA8, &g.gl_panel_tex, &g.gl_panel_memobj))
        Log("[feed32] cast: panel import FAILED (GL error 0x%04X); texture mode unavailable", FeedGlDrainErrors(&g.gl));
    else
        Log("[feed32] cast: host panel texture imported (OpenGL, %ux%u)", g.panel_w, g.panel_h);
    CloseHandle(hnd);
}

static void CastImportPanelVk(const FeedBuildAck &ack)
{
    if (ack.panel_tex == 0) return;
    HANDLE hnd = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.panel_tex));
    if (g.panel_w == 0 || g.vk_panel != VK_NULL_HANDLE) { CloseHandle(hnd); return; }   // see CastImportPanelGl
    if (!FeedVkImportImage(&g.vk, hnd, g.panel_w, g.panel_h, VK_FORMAT_R8G8B8A8_UNORM, false, &g.vk_panel, &g.vk_panel_mem))
        Log("[feed32] cast: panel import FAILED (Vulkan external-memory import); texture mode unavailable");
    else
        Log("[feed32] cast: host panel texture imported (Vulkan, %ux%u)", g.panel_w, g.panel_h);
    g.vk_panel_init = false;
    CloseHandle(hnd);
}

// A D3D11 client whose own device refused the shared set (feature level 10.x, issue #33)
// gets a host-created panel as well: the host takes the same branch as a GL/Vulkan client
// and never looks at FeedBuild::panel_tex, so a panel made on this side would be a texture
// nobody ever writes -- texture cast showed a frozen frame, and the handle the host
// duplicated in was dropped on the floor once per build. Open the host's instead.
static void CastAdoptHostPanel11(ID3D11Device1 *dev1, const FeedBuildAck &ack)
{
    if (ack.panel_tex == 0) return;
    HANDLE hnd = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.panel_tex));
    // Not on D3D10, for the reason CastMakePanel gives: the view would be the relay's.
    if (dev1 == nullptr || g.panel_w == 0 || g.is_d3d10) { CloseHandle(hnd); return; }

    const UINT pw = g.panel_w, ph = g.panel_h;   // CastReleasePanel clears these; the size has not changed
    CastReleasePanel();
    g.panel_w = pw; g.panel_h = ph;

    ID3D11Texture2D *tex = nullptr;
    HRESULT hr = dev1->OpenSharedResource1(hnd, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
    if (SUCCEEDED(hr)) hr = g.dev->CreateShaderResourceView(tex, nullptr, &g.panel_srv);
    if (FAILED(hr))
    {
        Log("[feed32] cast: could not open the host's panel texture 0x%08X; texture mode unavailable", hr);
        SafeRelease(g.panel_srv);
        if (tex != nullptr) tex->Release();
        CloseHandle(hnd);
        g.panel_w = g.panel_h = 0;   // do not retry every build
        return;
    }
    g.panel_tex    = tex;
    g.panel_handle = hnd;   // CastReleasePanel closes it
    Log("[feed32] cast: host panel texture opened (%ux%u, host-created set)", g.panel_w, g.panel_h);
}

static bool CastPanelAvailable()
{
    return g.panel_srv != nullptr || g.gl_panel_tex != 0 || g.vk_panel != VK_NULL_HANDLE;
}

// The blit rectangles for the GL / Vulkan draw: the panel's crop -> the layout rect,
// clipped to the backbuffer (above 100% the panel runs past the window's edges).
static bool CastPanelRects(int bbw, int bbh, RECT *src, RECT *dst)
{
    if (!g_cast_shown || !g_cast_texture || g_cast_src.cx <= 0 || g_cast_src.cy <= 0) return false;
    RECT d = g_cast_rect, s = { 0, 0, g_cast_src.cx, g_cast_src.cy };
    if (d.right <= d.left || d.bottom <= d.top) return false;
    const float sx = static_cast<float>(s.right) / static_cast<float>(d.right - d.left);
    const float sy = static_cast<float>(s.bottom) / static_cast<float>(d.bottom - d.top);
    if (d.left < 0)     { s.left   += static_cast<LONG>(-d.left * sx);          d.left   = 0; }
    if (d.top < 0)      { s.top    += static_cast<LONG>(-d.top * sy);           d.top    = 0; }
    if (d.right > bbw)  { s.right  -= static_cast<LONG>((d.right - bbw) * sx);  d.right  = bbw; }
    if (d.bottom > bbh) { s.bottom -= static_cast<LONG>((d.bottom - bbh) * sy); d.bottom = bbh; }
    if (d.right <= d.left || d.bottom <= d.top || s.right <= s.left || s.bottom <= s.top) return false;
    *src = s;
    *dst = d;
    return true;
}

// OpenGL: blit the panel over the backbuffer, inside the frame's FeedGlStateGuard. No
// vertical flip: the target ReShade hands us is its own effect surface, which it keeps
// top-down like D3D (it flips the game's frame in and out around the effects), and the
// imported texture's row 0 is the host's top row too. A flipped blit put the panel upside
// down at the bottom of the window (WormsXHD, 2026-09-02).
static void CastGlDrawPanel(uint64_t bb_handle, int bbw, int bbh)
{
    RECT s, d;
    if (g.gl_panel_tex == 0 || !CastPanelRects(bbw, bbh, &s, &d)) return;
    if (!FeedGlAttach(&g.gl, GL_READ_FRAMEBUFFER, g.gl_fbo_read, g.gl_panel_tex, true)) return;
    if (!FeedGlAttach(&g.gl, GL_DRAW_FRAMEBUFFER, g.gl_fbo_draw, bb_handle, false)) return;
    g.gl.BlitFramebuffer(s.left, s.top, s.right, s.bottom, d.left, d.top, d.right, d.bottom,
                         GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

// Vulkan: same, on the frame's command buffer while the backbuffer is parked as a copy
// destination. The host writes the panel unfenced; the acquire/release pair around the
// blit is what makes its writes visible here.
// The command buffer is fetched HERE, not passed in: the pipelined branch flushes
// ReShade's immediate list to signal the host before this runs, and recording into the
// buffer that flush submitted crashed the driver (Castlevania under DXVK, 2026-09-02).
static void CastVkDrawPanel(reshade::api::command_list *cl, VkImage bb, uint32_t family, int bbw, int bbh)
{
    RECT s, d;
    if (g.vk_panel == VK_NULL_HANDLE || cl == nullptr || !CastPanelRects(bbw, bbh, &s, &d)) return;
    const VkCommandBuffer cb = FeedVkDispatch<VkCommandBuffer>(cl->get_native());
    if (cb == VK_NULL_HANDLE) return;
    if (!g.vk_panel_init)
    {
        FeedVkBarrier(&g.vk, cb, g.vk_panel, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        g.vk_panel_init = true;
    }
    else
        FeedVkExternalTransfer(&g.vk, cb, g.vk_panel, family, false /*acquire*/);
    VkImageBlit bl = {};
    bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bl.srcOffsets[0]  = { s.left, s.top, 0 };
    bl.srcOffsets[1]  = { s.right, s.bottom, 1 };
    bl.dstOffsets[0]  = { d.left, d.top, 0 };
    bl.dstOffsets[1]  = { d.right, d.bottom, 1 };
    g.vk.CmdBlitImage(cb, g.vk_panel, VK_IMAGE_LAYOUT_GENERAL, bb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    FeedVkExternalTransfer(&g.vk, cb, g.vk_panel, family, true /*release*/);
}

static void CastKeyName(int vk, char *out, size_t n)
{
    if (vk <= 0) { strcpy_s(out, n, "none"); return; }
    UINT scan = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    switch (vk)   // extended keys, or GetKeyNameText names the numpad twin
    {
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
    case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        scan |= 0x100; break;
    default: break;
    }
    if (GetKeyNameTextA(static_cast<LONG>(scan << 16), out, static_cast<int>(n)) == 0)
        sprintf_s(out, n, "key %d", vk);
}

static void CastPostKey(UINT msg, UINT vk, bool up)
{
    if (g_cast_hwnd == nullptr) return;
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16);
    if (up) lp |= (1 << 30) | (1u << 31);
    PostMessageW(g_cast_hwnd, msg, vk, lp);
}

// ReShade's mouse button order: 0 left, 1 middle, 2 right.
static const struct { uint32_t idx; UINT down, up; } kCastButtons[] = {
    { 0, WM_LBUTTONDOWN, WM_LBUTTONUP },
    { 1, WM_MBUTTONDOWN, WM_MBUTTONUP },
    { 2, WM_RBUTTONDOWN, WM_RBUTTONUP },
};

// Release everything the host still believes is held. Called when the cursor leaves the
// panel and whenever the panel is taken down, so no key or button can stay stuck in ReShade
// x64's ImGui after this side stops forwarding.
static void CastFlushInput()
{
    int keys = 0, buttons = 0;
    for (UINT vk = 0; vk < 256; ++vk)
        if (g_cast_key_down[vk]) { g_cast_key_down[vk] = false; ++keys; CastPostKey(WM_KEYUP, vk, true); }
    if (g_cast_btn_down != 0 && g_cast_hwnd != nullptr)
    {
        const LPARAM at = MAKELPARAM(g_cast_last.x >= 0 ? g_cast_last.x : 0,
                                     g_cast_last.y >= 0 ? g_cast_last.y : 0);
        for (const auto &b : kCastButtons)
            if (g_cast_btn_down & (1u << b.idx)) { ++buttons; PostMessageW(g_cast_hwnd, b.up, 0, at); }
    }
    g_cast_btn_down = 0;
    if ((keys != 0 || buttons != 0) && g_cfg.log_frames > 0)
        Log("[feed32] cast: released %d key(s) and %d button(s) still held in the host", keys, buttons);
}

// Finds the host window, registers the thumbnail on the game window and lays the panel
// out: right-aligned at the top, the host's tab column at 1:1 or shrunk to fit the game
// client area. Returns false when there is nothing on screen (status says why).
static bool CastLayout()
{
    HWND game = g.runtime != nullptr ? static_cast<HWND>(g.runtime->get_hwnd()) : nullptr;
    if (game == nullptr || !IsWindow(game)) { strcpy_s(g_cast_status, "no game window"); return false; }

    if (g_cast_hwnd != nullptr && !IsWindow(g_cast_hwnd)) CastHostLost();
    if (g_cast_hwnd == nullptr)
    {
        g_cast_hwnd = CastFindHostWindow();
        if (g_cast_hwnd == nullptr) { strcpy_s(g_cast_status, "waiting for the host window"); return false; }
        Log("[feed32] cast: host window %p found", (void *)g_cast_hwnd);
    }
    if (!g_cast_placed)
    {
        // Under the game window, at its position: on a second monitor the parked host
        // window would otherwise be in plain view. Never activated, never resized.
        RECT gr = {};
        if (GetWindowRect(game, &gr))
            SetWindowPos(g_cast_hwnd, HWND_BOTTOM, gr.left, gr.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        g_cast_placed = true;
    }

    // D3D10 has no texture mode -- the panel view cannot cross into the game's ImGui --
    // so the cast is always the compositor thumbnail there.
    const bool texture = g_cfg.cast_mode == 1 && !g.is_d3d10;
    if (texture != g_cast_texture)   // the mode changed while shown: start over in the other one
    {
        CastRelease();
        g_cast_texture = texture;
    }

    RECT hc = {}, gc = {};
    GetClientRect(game, &gc);
    if (gc.right < 64 || gc.bottom < 64) { strcpy_s(g_cast_status, "the game window is minimized"); return false; }
    if (texture)
    {
        if (!CastPanelAvailable())
        {
            strcpy_s(g_cast_status, "waiting for the panel texture (it is set up with the feed's first build)");
            return false;
        }
        hc.right  = static_cast<LONG>(g.panel_w);
        hc.bottom = static_cast<LONG>(g.panel_h);
    }
    else
    {
        GetClientRect(g_cast_hwnd, &hc);
        if (hc.right < 200 || hc.bottom < 100 || IsIconic(g_cast_hwnd))
        { strcpy_s(g_cast_status, "the host window is minimized"); return false; }
    }

    const SIZE src = { hc.right - 96 >= 200 ? hc.right - 96 : hc.right, hc.bottom };
    float s = 1.0f;
    if (src.cy > gc.bottom) s = static_cast<float>(gc.bottom) / static_cast<float>(src.cy);
    if (src.cx * s > gc.right) s = static_cast<float>(gc.right) / static_cast<float>(src.cx);
    s *= static_cast<float>(g_cfg.cast_scale) / 100.0f;   // the user's size, relative to the fit
    const int dw = static_cast<int>(src.cx * s + 0.5f), dh = static_cast<int>(src.cy * s + 0.5f);
    const RECT dest = { gc.right - dw, 0, gc.right, dh };

    if (texture)
    {
        // Nothing to register: OnOverlay draws the texture at g_cast_rect every frame.
        if (g_cast_dest != game) { g_cast_dest = game; Log("[feed32] cast: drawing the host's panel texture on game window %p", (void *)game); }
        if (!EqualRect(&dest, &g_cast_rect) || src.cx != g_cast_src.cx || src.cy != g_cast_src.cy)
            Log("[feed32] cast: %ldx%ld of the panel texture shown at %dx%d (scale %.2f)", src.cx, src.cy, dw, dh, s);
        g_cast_rect  = dest;
        g_cast_src   = src;
        g_cast_scale = s;
        g_cast_shown = true;
        sprintf_s(g_cast_status, "shown at %dx%d (texture)%s", dw, dh, g_cast_hover ? " (cursor over it)" : "");
        return true;
    }
    if (g_cast_thumb == nullptr || g_cast_dest != game)
    {
        CastRelease();
        const HRESULT hr = DwmRegisterThumbnail(game, g_cast_hwnd, &g_cast_thumb);
        if (FAILED(hr) || g_cast_thumb == nullptr)
        {
            g_cast_thumb = nullptr;
            sprintf_s(g_cast_status, "the desktop compositor refused the thumbnail (0x%08lX)", static_cast<unsigned long>(hr));
            return false;
        }
        g_cast_dest = game;
        Log("[feed32] cast: thumbnail registered on game window %p", (void *)game);
    }
    g_cast_shown = true;
    if (!EqualRect(&dest, &g_cast_rect) || src.cx != g_cast_src.cx || src.cy != g_cast_src.cy)
    {
        DWM_THUMBNAIL_PROPERTIES p = {};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_VISIBLE |
                    DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcDestination         = dest;
        p.rcSource              = { 0, 0, src.cx, src.cy };
        p.opacity               = 255;
        p.fVisible              = TRUE;
        p.fSourceClientAreaOnly = TRUE;
        const HRESULT hr = DwmUpdateThumbnailProperties(g_cast_thumb, &p);
        if (FAILED(hr))
        {
            sprintf_s(g_cast_status, "thumbnail layout failed (0x%08lX)", static_cast<unsigned long>(hr));
            return false;
        }
        g_cast_rect  = dest;
        g_cast_src   = src;
        g_cast_scale = s;
        Log("[feed32] cast: %ldx%ld of the host window shown at %dx%d (scale %.2f)", src.cx, src.cy, dw, dh, s);
    }
    if (g_cast_fullscreen)
        strcpy_s(g_cast_status, "shown, but the game is exclusive fullscreen: the compositor cannot draw over it -- use borderless");
    else
        sprintf_s(g_cast_status, "shown at %dx%d%s", dw, dh, g_cast_hover ? " (cursor over it)" : "");
    return true;
}

// Forwards the game's mouse and keys to the host window while the cursor is over the
// panel, and keeps them from the game meanwhile.
static void CastInput(reshade::api::effect_runtime *rt)
{
    // The panel owns the mouse for as long as it is shown, like ReShade's own overlay does:
    // a game in mouselook hides the cursor and re-centres it every frame, so a cursor that
    // is only claimed once it hovers the panel never gets there. With the mouse blocked,
    // ReShade x86 swallows the game's SetCursorPos/ClipCursor and its raw/DirectInput reads,
    // and keeps tracking the position itself.
    //
    // ReShade's block covers the keyboard too (there is no mouse-only block), and it works
    // by nulling the messages before the game's window sees them -- so while the panel is
    // up, Alt+F4 does nothing and Escape never opens the game's menu (Castlevania: the user
    // had to stop the game from Steam). Hence: Escape away from the panel hides it, and
    // Alt+F4 hides it AND hands the close to the game window itself.
    const bool alt_f4 = rt->is_key_down(VK_MENU) && rt->is_key_pressed(VK_F4);
    if (alt_f4 || (rt->is_key_pressed(VK_ESCAPE) && !g_cast_hover && !g_cast_captured))
    {
        g_cast_wanted = false;
        Log("[feed32] cast: hidden (%s)", alt_f4 ? "Alt+F4, passed on to the game" : "Escape");
        if (alt_f4 && g_cast_dest != nullptr) PostMessageW(g_cast_dest, WM_SYSCOMMAND, SC_CLOSE, 0);
        return;
    }
    rt->block_input_next_frame();

    // ReShade's own position (client coordinates, from the window messages it sees), not
    // GetCursorPos: the game may have hidden or moved the OS cursor.
    uint32_t cx = 0, cy = 0;
    int16_t  wheel = 0;
    rt->get_mouse_cursor_position(&cx, &cy, &wheel);   // the wheel comes in notches
    if (wheel == 0) wheel = g_cast_wheel_pending;      // OnOverlay's earlier read, if this one came up empty
    g_cast_wheel_pending = 0;
    const POINT p = { static_cast<LONG>(cx), static_cast<LONG>(cy) };
    g_cast_cursor = p;
    const bool inside = PtInRect(&g_cast_rect, p) != FALSE;
    const bool l = rt->is_mouse_button_down(0), m = rt->is_mouse_button_down(1), r = rt->is_mouse_button_down(2);
    if (g_cast_captured && !(l || m || r)) g_cast_captured = false;

    // The close button comes first and is never forwarded.
    const RECT close = CastCloseRect();
    g_cast_close_hover = !g_cast_captured && PtInRect(&close, p) != FALSE;
    if (g_cast_close_hover)
    {
        if (rt->is_mouse_button_pressed(0))
        {
            g_cast_wanted = false;
            Log("[feed32] cast: hidden (close button)");
        }
        g_cast_hover = false;
        g_cast_last  = { -1, -1 };
        CastFlushInput();
        return;
    }
    if (!inside && !g_cast_captured)
    {
        // Leaving the panel stops the forwarding, so anything still held has to be released
        // now -- otherwise its key-up happens out there and the host never hears it.
        g_cast_hover = false;
        g_cast_last  = { -1, -1 };
        CastFlushInput();
        return;
    }

    const bool ctrl = rt->is_key_down(VK_CONTROL), shift = rt->is_key_down(VK_SHIFT);
    const WPARAM mk = (l ? MK_LBUTTON : 0) | (m ? MK_MBUTTON : 0) | (r ? MK_RBUTTON : 0) |
                      (ctrl ? MK_CONTROL : 0) | (shift ? MK_SHIFT : 0);
    const int hx = static_cast<int>((p.x - g_cast_rect.left) / g_cast_scale);
    const int hy = static_cast<int>((p.y - g_cast_rect.top)  / g_cast_scale);
    const LPARAM at = MAKELPARAM(hx, hy);
    if (!g_cast_hover || hx != g_cast_last.x || hy != g_cast_last.y)
    {
        PostMessageW(g_cast_hwnd, WM_MOUSEMOVE, mk, at);
        g_cast_last = { hx, hy };
    }
    g_cast_hover = true;

    // Press and release land on different frames by construction -- what ImGui needs
    // to see a click.
    for (const auto &b : kCastButtons)
    {
        if (rt->is_mouse_button_pressed(b.idx))
        {
            PostMessageW(g_cast_hwnd, b.down, mk, at);
            g_cast_captured = true;
            g_cast_btn_down |= 1u << b.idx;
        }
        if (rt->is_mouse_button_released(b.idx))
        {
            PostMessageW(g_cast_hwnd, b.up, mk, at);
            g_cast_btn_down &= ~(1u << b.idx);
        }
    }

    if (wheel != 0)
    {
        // WM_MOUSEWHEEL carries the cursor in SCREEN coordinates, and ReShade x64 turns the
        // position in the message into its ImGui mouse position. Sending the real cursor's
        // screen position put that somewhere over the game instead of over the panel, so the
        // scroll landed on whatever ImGui window happened to be under a point the user was
        // not pointing at -- usually none, which is why nothing scrolled. Send the panel
        // position mapped into the host's own screen space, and a move first so ImGui has
        // the hover before the wheel arrives.
        POINT s = { hx, hy };
        ClientToScreen(g_cast_hwnd, &s);
        PostMessageW(g_cast_hwnd, WM_MOUSEMOVE, mk, at);
        g_cast_last = { hx, hy };
        // One message per notch: ImGui accumulates them, and some builds clamp a single
        // oversized delta to one line.
        const int notches = wheel > 0 ? wheel : -wheel;
        const int step    = wheel > 0 ? WHEEL_DELTA : -WHEEL_DELTA;
        for (int i = 0; i < notches && i < 16; ++i)
            PostMessageW(g_cast_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(mk, step), MAKELPARAM(s.x, s.y));
        if (g_cfg.log_frames > 0)
            Log("[feed32] cast: wheel %d notch(es) -> host client %d,%d (screen %ld,%ld)", wheel, hx, hy, s.x, s.y);
    }

    // Keys, so Ctrl+click text entry, typing a value and the host's own overlay key work.
    // The cast toggle key never reaches the host, and neither do the Windows keys.
    BYTE ks[256];
    const bool have_ks = GetKeyboardState(ks) != FALSE;
    for (UINT vk = 8; vk < 256; ++vk)
    {
        if (static_cast<int>(vk) == g_cfg.cast_key || vk == VK_LWIN || vk == VK_RWIN) continue;
        if (rt->is_key_pressed(vk))
        {
            CastPostKey(WM_KEYDOWN, vk, false);
            g_cast_key_down[vk] = true;   // so CastFlushInput can release it if the cursor leaves
            if (have_ks && !ctrl)
            {
                wchar_t chars[4] = {};
                const int n = ToUnicode(vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), ks, chars, 4, 0);
                for (int i = 0; i < n; ++i)
                    if (chars[i] >= 32) PostMessageW(g_cast_hwnd, WM_CHAR, chars[i], 1);
            }
        }
        if (rt->is_key_released(vk)) { CastPostKey(WM_KEYUP, vk, true); g_cast_key_down[vk] = false; }
    }
}

// Once per frame, after ReShade drew (reshade_present): the toggle key, the "Set key"
// capture, the layout and the input forwarding.
static void CastTick(reshade::api::effect_runtime *rt)
{
    if (rt != g.runtime || rt == nullptr) return;

    if (g_cast_capture_key)
    {
        for (UINT vk = 8; vk < 256; ++vk)
        {
            if (vk == VK_LWIN || vk == VK_RWIN || !rt->is_key_pressed(vk)) continue;
            g_cast_capture_key = false;
            if (vk == VK_ESCAPE) break;
            g_cfg.cast_key = vk == VK_BACK ? 0 : static_cast<int>(vk);
            CfgSave();
            char name[64];
            CastKeyName(g_cfg.cast_key, name, sizeof(name));
            Log("[feed32] cast: toggle key set to %s (%d)", name, g_cfg.cast_key);
            break;
        }
    }
    else if (g_cfg.cast_key > 0 && rt->is_key_pressed(static_cast<uint32_t>(g_cfg.cast_key)))
    {
        g_cast_wanted = !g_cast_wanted;
        Log("[feed32] cast: %s (key)", g_cast_wanted ? "shown" : "hidden");
    }

    if (!g_cast_wanted)
    {
        if (g_cast_thumb != nullptr || g_cast_shown) CastRelease();
        strcpy_s(g_cast_status, "hidden");
        return;
    }
    if (!HostAlive())
    {
        CastHostLost();
        strcpy_s(g_cast_status, "the host is not running");
        return;
    }
    if (!CastLayout()) return;
    CastInput(rt);
}

static bool OnSetFullscreenState(reshade::api::swapchain *, bool fullscreen, void *)
{
    g_cast_fullscreen = fullscreen;
    return false;   // never interfere with the game's choice
}

// Defined far below with the other ReShade callbacks; OnPresent and DrawOverlay call it to
// re-adopt the runtime when enabled goes 0 -> 1, since no event will fire again for it.
static void OnInitEffectRuntime(reshade::api::effect_runtime *rt);

static bool OnOpenOverlay(reshade::api::effect_runtime *rt, bool open, reshade::api::input_source)
{
    if (g_cfg.enabled && rt == g.runtime) g_game_overlay_open = open;
    return false;   // never veto
}

static void OnPresent(reshade::api::effect_runtime *rt)
{
    // The host buttons are drained HERE, not in FeedFrame. FeedFrame only runs when the
    // DLSS5_Feed technique renders, so with the effect missing or disabled, effects toggled
    // off, or mode=0, "Start the DLSS 5 host" would have done nothing at all -- and that is
    // exactly the state a user presses it in. This callback runs every frame regardless.
    // Ahead of the enabled gate, and the only CfgReload that is: the other three are in the
    // frame path, which enabled=0 never reaches, so a cfg file edited back to enabled=1
    // could never be picked up -- only the overlay checkbox could undo it. Time-throttled
    // rather than frame-counted, because the frame counter only advances when feeding.
    {
        static ULONGLONG next = 0;
        const ULONGLONG now = GetTickCount64();
        if (now >= next)
        {
            next = now + 1000;
            const int was = g_cfg.enabled;
            if (CfgReload() || was != g_cfg.enabled) g.built = false;
            if (was == 0 && g_cfg.enabled != 0 && rt != nullptr)
            { Log("[feed32] enabled=1 read back from dlss5-feed.cfg; re-adopting the effect runtime"); OnInitEffectRuntime(rt); }
        }
    }
    if (!g_cfg.enabled) return;
    if (rt == g.runtime && HostRequestPending() && FeedEnter())
    {
        HostConsumeRequest();   // inside the lock: it tears down what a frame uses
        FeedLeave();
    }
    CastTick(rt);
}

// A cursor for the panel, drawn in the game by ReShade x86's ImGui: the game has usually
// hidden the OS cursor, and the host draws none since the OS cursor is never over its
// window. Drawn on the foreground list so it sits above the game's own ReShade overlay.
static void OnOverlay(reshade::api::effect_runtime *rt)
{
    if (!g_cfg.enabled || rt != g.runtime) return;
    // ReShade's wheel delta is a per-frame value. This callback runs inside its GUI draw,
    // earlier in the frame than reshade_present where CastInput reads it, so take it here
    // too: if the delta has already been cleared by then, this is the copy that survives.
    // CastInput prefers its own read and only falls back to this one.
    {
        uint32_t wx = 0, wy = 0;
        int16_t  w  = 0;
        rt->get_mouse_cursor_position(&wx, &wy, &w);
        if (w != 0) g_cast_wheel_pending = w;
    }
    if (!g_cast_shown)
    {
        if (g_cast_hid_cursor) { ImGui::GetIO().MouseDrawCursor = true; g_cast_hid_cursor = false; }
        return;
    }
    ImDrawList *dl = ImGui::GetForegroundDrawList(nullptr);
    if (dl == nullptr) return;
    if (g_cast_texture && !g.is_d3d10 && g.panel_srv != nullptr && g.panel_w != 0)
    {
        // ReShade's D3D11 ImGui backend takes the view pointer itself as the texture id
        // (the same convention as the rtv.handle casts in FeedFrame11).
        const ImVec2 pmin(static_cast<float>(g_cast_rect.left), static_cast<float>(g_cast_rect.top));
        const ImVec2 pmax(static_cast<float>(g_cast_rect.right), static_cast<float>(g_cast_rect.bottom));
        const ImVec2 uv1(static_cast<float>(g_cast_src.cx) / static_cast<float>(g.panel_w),
                         static_cast<float>(g_cast_src.cy) / static_cast<float>(g.panel_h));
        dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g.panel_srv)), pmin, pmax, ImVec2(0.0f, 0.0f), uv1);
    }
    // The close button, on top of everything.
    {
        const RECT c = CastCloseRect();
        const ImVec2 a(static_cast<float>(c.left), static_cast<float>(c.top)), b(static_cast<float>(c.right), static_cast<float>(c.bottom));
        dl->AddRectFilled(a, b, g_cast_close_hover ? IM_COL32(200, 40, 40, 230) : IM_COL32(30, 30, 30, 200));
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 160));
        const float pad = (b.x - a.x) * 0.3f;
        dl->AddLine(ImVec2(a.x + pad, a.y + pad), ImVec2(b.x - pad, b.y - pad), IM_COL32(255, 255, 255, 255), 2.0f);
        dl->AddLine(ImVec2(b.x - pad, a.y + pad), ImVec2(a.x + pad, b.y - pad), IM_COL32(255, 255, 255, 255), 2.0f);
    }

    // One cursor at a time. Over the panel the host's ReShade draws its own at the
    // forwarded position (a pump behind ours, which showed as two arrows that did not
    // quite agree), so neither this add-on's arrow nor -- when the game's ReShade overlay
    // is open, as it is right after pressing the button -- ReShade x86's software cursor
    // may show there. That one is an ImGui io flag ReShade sets before the frame; it is
    // read at render time, so flipping it here is enough, and it is restored when the
    // cursor leaves the panel.
    if (g_cast_cursor.x < 0) return;
    ImGuiIO &io = ImGui::GetIO();
    if (PtInRect(&g_cast_rect, g_cast_cursor))
    {
        if (io.MouseDrawCursor) { io.MouseDrawCursor = false; g_cast_hid_cursor = true; }
        return;
    }
    if (g_cast_hid_cursor) { io.MouseDrawCursor = true; g_cast_hid_cursor = false; }
    if (g_game_overlay_open) return;
    const float x = static_cast<float>(g_cast_cursor.x), y = static_cast<float>(g_cast_cursor.y);
    const ImVec2 arrow[] = { { x, y }, { x, y + 17.0f }, { x + 4.5f, y + 13.0f }, { x + 7.5f, y + 19.5f },
                             { x + 10.5f, y + 18.0f }, { x + 7.5f, y + 11.5f }, { x + 13.0f, y + 11.5f } };
    dl->AddConvexPolyFilled(arrow, 7, IM_COL32(255, 255, 255, 255));
    dl->AddPolyline(arrow, 7, IM_COL32(0, 0, 0, 255), ImDrawFlags_Closed, 1.5f);
}

// One bounded transfer. The pipe is opened FILE_FLAG_OVERLAPPED, so every read and write in
// this file goes through here: a host that stops answering costs the caller `timeout_ms`
// and an error, where a synchronous call would have parked it forever -- on the render
// thread, mid-Present. `ev` is the caller's own manual-reset event (the worker and the
// render thread never transfer at the same time, but they do not share one either).
static bool PipeXfer(HANDLE pipe, HANDLE ev, bool write, void *buf, DWORD len, DWORD timeout_ms)
{
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || ev == nullptr) return false;
    BYTE *p = static_cast<BYTE *>(buf);
    DWORD left = len;
    while (left > 0)
    {
        OVERLAPPED ov = {};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD moved = 0;
        const BOOL done = write ? WriteFile(pipe, p, left, &moved, &ov)
                                : ReadFile(pipe, p, left, &moved, &ov);
        if (!done)
        {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            const HANDLE waits[2] = { ev, g_link.abort_event };
            const DWORD  n = g_link.abort_event != nullptr ? 2u : 1u;
            if (WaitForMultipleObjects(n, waits, FALSE, timeout_ms) != WAIT_OBJECT_0)
            {
                // This OVERLAPPED is on the stack, so the I/O has to be finished with before
                // the frame goes -- otherwise the kernel writes into memory that is gone.
                // Once cancelled (or already complete) the event is signalled, so the wait
                // returns at once; it is unbounded on purpose, because the alternative is
                // memory corruption.
                CancelIoEx(pipe, &ov);
                WaitForSingleObject(ev, INFINITE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &ov, &moved, FALSE)) return false;
        }
        if (moved == 0) return false;   // the far end closed
        p    += moved;
        left -= moved;
    }
    return true;
}

// The render thread's own event for PipeXfer, made on first use.
static HANDLE g_pipe_ev;
static HANDLE PipeEvent()
{
    if (g_pipe_ev == nullptr) g_pipe_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return g_pipe_ev;
}

static bool PipeWrite(const void *buf, DWORD len)
{
    return PipeXfer(g.pipe, PipeEvent(), true, const_cast<void *>(buf), len, kPipeFrameMs);
}

// The tag and the message go in ONE write. Two writes put two round trips through the
// pipe on the frame path, and the host reads them back-to-back anyway; the wire format
// is byte-for-byte what it always was.
#pragma pack(push, 1)
struct FeedTaggedFrame { BYTE tag; FeedFrameMsg fm; };
#pragma pack(pop)

static bool PipeWriteFrame(const FeedFrameMsg &fm)
{
    const FeedTaggedFrame msg = { 'F', fm };
    return PipeWrite(&msg, sizeof(msg));
}

// ---------------------------------------------------------------------------
// Deep Fried Chicken in host64\ -- the neural consumer on the split path, when it is
// installed there instead of renodx-dlss5.addon64. The x86 side never negotiates with it
// (the host does, see feed_dfc.h); this add-on only needs to know it is there so the
// overlay stops mirroring RenoDX's [RenoDX.DLSS5] keys, which Chicken does not read, and
// points the user at the host window instead. A few of Chicken's own top-level keys are
// shown read-only from host64\deep-fried-chicken.cfg (plain key=value, no INI section).
// ---------------------------------------------------------------------------
static bool   g_chicken_host        = false;
static char   g_chicken_host_ver[64] = "not found";
static char   g_chicken_cfg_path[MAX_PATH];
static UINT64 g_chicken_cfg_read_at = 0;
static int    g_chicken_arm = -1, g_chicken_enabled = -1, g_chicken_layers = -1, g_chicken_work_percent = -1;

static void DetectChickenHost()
{
    char dir[MAX_PATH];
    GetModuleFileNameA(g_self, dir, MAX_PATH);
    if (char *s = strrchr(dir, '\\')) *(s + 1) = '\0';
    char h64[MAX_PATH];
    sprintf_s(h64, "%shost64\\", dir);
    g_chicken_host = DfcScanFile(h64, g_chicken_host_ver, sizeof(g_chicken_host_ver));
    sprintf_s(g_chicken_cfg_path, "%sdeep-fried-chicken.cfg", h64);
    if (!g_chicken_host) { Log("[feed32] Deep Fried Chicken: not present in host64\\"); return; }
    Log("[feed32] Deep Fried Chicken %s: present in host64\\ -- it is the neural consumer; the host negotiates "
        "with it (ABI 1, HostMode=1) and its full panel lives in the host window (Home key there)", g_chicken_host_ver);

    char stray[MAX_PATH];
    sprintf_s(stray, "%s" DFC_ADDON_FILENAME, dir);
    if (GetFileAttributesA(stray) != INVALID_FILE_ATTRIBUTES)
        Warn("deep-fried-chicken.addon64 is next to the 32-bit game exe, where a 32-bit process cannot load it. "
             "It belongs in host64\\ (it is already there too). Remove the copy next to the game.");
}

// Re-read the four headline keys at most every 2 s while the overlay is open.
static void ChickenCfgRefresh()
{
    const UINT64 now = GetTickCount64();
    if (now - g_chicken_cfg_read_at < 2000) return;
    g_chicken_cfg_read_at = now;
    FILE *f = nullptr;
    if (fopen_s(&f, g_chicken_cfg_path, "rb") != 0 || f == nullptr) return;
    char line[256];
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        char *eq = strchr(line, '=');
        if (eq == nullptr) continue;
        *eq = '\0';
        const int v = atoi(eq + 1);
        if      (_stricmp(line, "arm") == 0)                 g_chicken_arm = v;
        else if (_stricmp(line, "enabled") == 0)             g_chicken_enabled = v;
        else if (_stricmp(line, "layers") == 0)              g_chicken_layers = v;
        else if (_stricmp(line, "neural_work_percent") == 0) g_chicken_work_percent = v;
    }
    fclose(f);
}

static const char *HostClientKindName()
{
    return g.is_vulkan ? "Vulkan" : g.is_gl ? "OpenGL" : g.is_d3d10 ? "D3D10 (via a D3D11 relay)" : "D3D11";
}

// Worker side of JOB_CONNECT: spawn the helper, wait for its pipe, shake hands. Everything
// it opens goes into g_link so the render thread can adopt it -- on failure too, so a
// half-made link is disposed of by the usual HostClose path rather than by hand here.
static bool HostWorkerConnect(HANDLE ev)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(g_self, dir, MAX_PATH);
    if (char *s = strrchr(dir, '\\')) *(s + 1) = '\0';

    char exe[MAX_PATH], cmd[MAX_PATH + 32], wd[MAX_PATH];
    sprintf_s(exe, "%shost64\\dlss5-feed-host64.exe", dir);
    sprintf_s(wd, "%shost64", dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES)
    {
        g_link.fatal = true;
        strcpy_s(g_link.why, "the 64-bit host is not installed");
        return false;
    }
    // host_window=0: the host still makes its window (the cast needs a shown one), but as a
    // tool window parked behind everything -- --behind; 1: its own plain window.
    sprintf_s(cmd, "\"%s\" %lu%s", exe, GetCurrentProcessId(), g_cfg.host_window ? "" : " --behind");

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, wd, &si, &pi))
    {
        Log("[feed32] CreateProcess failed %lu", GetLastError());
        g_link.fatal = true;
        strcpy_s(g_link.why, "could not start the 64-bit host");
        return false;
    }
    CloseHandle(pi.hThread);
    g_link.proc = pi.hProcess;
    // The command line, not just the pid: when the host exits at its usage line, this is
    // the half of the evidence that survives -- the host truncates its own log on every
    // launch, so a later manual run can erase the failing one (issue #46).
    Log("[feed32] host spawned (pid %lu): %s", pi.dwProcessId, cmd);

    char name[128];
    sprintf_s(name, FEED_PIPE_FMT, static_cast<unsigned long>(GetCurrentProcessId()));
    for (int i = 0; i < 150 && g_link.pipe == nullptr; ++i)   // up to 15 s (host loads ReShade + NGX)
    {
        // FILE_FLAG_OVERLAPPED: every transfer after this is bounded (see PipeXfer).
        HANDLE p = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (p != INVALID_HANDLE_VALUE) { g_link.pipe = p; break; }
        if (WaitForSingleObject(g_link.proc, 0) != WAIT_TIMEOUT)
        {
            // The host's own return value said why (1 = it rejected its arguments, 3 = the
            // device was removed); it used to be discarded, leaving "went away" as the
            // whole story.
            DWORD code = 0;
            if (GetExitCodeProcess(g_link.proc, &code))
                sprintf_s(g_link.why, "exited during startup with code %lu%s", code,
                          code == 1 ? " (it rejected its own command line -- see dlss5-feed-host.log)" : "");
            else
                strcpy_s(g_link.why, "exited during startup");
            return false;
        }
        if (WaitForSingleObject(g_link.abort_event, 100) == WAIT_OBJECT_0)
        { strcpy_s(g_link.why, "cancelled while starting"); return false; }
    }
    if (g_link.pipe == nullptr) { strcpy_s(g_link.why, "pipe never appeared"); return false; }

    // D3D10 is deliberately absent: by the time the host hears from us the frame is
    // already on a D3D11 relay device, so it IS a D3D11 client -- same kind, same
    // protocol version, nothing on the host side to change.
    const uint32_t kind = g.is_vulkan ? FEED_CLIENT_VULKAN : g.is_gl ? FEED_CLIENT_GL : FEED_CLIENT_D3D11;
    // Hand the host a handle to this process instead of making it OpenProcess(pid): a
    // protective DACL on the game (anti-cheat/DRM; seen on vanilla WoW) denies that with
    // error 5, and this duplication never consults the game's DACL -- both process handles
    // involved are ours (the pseudo-handle, and the one CreateProcess just returned).
    HANDLE self_in_host = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), g_link.proc, &self_in_host,
                         PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
        Log("[feed32] could not duplicate our process handle into the host (%lu); it will fall back to OpenProcess",
            GetLastError());
    FeedHello hello = { FEED_IPC_MAGIC, FEED_IPC_VERSION, GetCurrentProcessId(), kind,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self_in_host)) };
    FeedHelloAck ack = {};
    if (!PipeXfer(g_link.pipe, ev, true,  &hello, sizeof(hello), kPipeHelloMs) ||
        !PipeXfer(g_link.pipe, ev, false, &ack,   sizeof(ack),   kPipeHelloMs) ||
        ack.magic != FEED_IPC_MAGIC)
    { strcpy_s(g_link.why, "handshake failed"); return false; }
    if (ack.version != FEED_IPC_VERSION)
    {
        // The message structs after the hello changed size between versions, so a mismatched
        // pair would not just misbehave, it would desync the pipe. Both sides refuse.
        Log("[feed32] the host in host64\\ speaks protocol v%u, this add-on v%u", ack.version, FEED_IPC_VERSION);
        g_link.fatal = true;
        strcpy_s(g_link.why, "the host64\\ folder is from a different release -- reinstall both halves together");
        return false;
    }
    g_link.panel_w = ack.panel_width;   // v7: the size the panel texture has to be, if any
    g_link.panel_h = ack.panel_height;
    return true;
}

// Worker side of JOB_BUILD: the 'B' exchange, nothing else. g.pipe is set and untouched by
// the render thread for as long as this job is in flight.
static bool HostWorkerBuild(HANDLE ev)
{
    const BYTE tag = 'B';
    if (!PipeXfer(g.pipe, ev, true,  const_cast<BYTE *>(&tag), 1, kPipeBuildMs) ||
        !PipeXfer(g.pipe, ev, true,  &g_link.build, sizeof(g_link.build), kPipeBuildMs) ||
        !PipeXfer(g.pipe, ev, false, &g_link.ack,   sizeof(g_link.ack),   kPipeBuildMs))
    { strcpy_s(g_link.why, "build exchange failed"); return false; }
    return true;
}

static DWORD WINAPI HostWorker(void *)
{
    const ULONGLONG t0 = GetTickCount64();
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    if (ev == nullptr) strcpy_s(g_link.why, "could not create the worker's I/O event");
    else ok = g_link.job == JOB_CONNECT ? HostWorkerConnect(ev) : HostWorkerBuild(ev);
    if (ev != nullptr) CloseHandle(ev);
    g_link.ms = static_cast<DWORD>(GetTickCount64() - t0);
    // Last write: everything above must be visible to the render thread before it sees this.
    InterlockedExchange(&g_link.state, ok ? LINK_DONE : LINK_FAILED);
    // And the last act: drop the module reference taken in HostLinkStart. This is what makes
    // it safe for DLL_PROCESS_DETACH to walk away without joining -- the module cannot be
    // unmapped while this thread is still in it, and FreeLibraryAndExitThread is the one call
    // that releases the reference and exits atomically. Nothing in this DLL runs after it.
    const HMODULE ref = g_link.mod_ref;
    if (ref != nullptr) FreeLibraryAndExitThread(ref, 0);
    return 0;
}

static bool HostLinkStart(int job)
{
    if (g_link.thread != nullptr) return false;
    if (g_link.abort_event == nullptr)
    {
        g_link.abort_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_link.abort_event == nullptr) return false;
    }
    ResetEvent(g_link.abort_event);
    InterlockedExchange(&g_link.abort, 0);
    g_link.job     = job;
    g_link.fatal   = false;
    g_link.why[0]  = '\0';
    g_link.ms      = 0;
    g_link.pipe    = nullptr;
    g_link.proc    = nullptr;
    // Pin this module for as long as the worker runs in it (see HostWorker's tail).
    g_link.mod_ref = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&HostWorker), &g_link.mod_ref);
    InterlockedExchange(&g_link.state, LINK_RUNNING);
    g_link.thread = CreateThread(nullptr, 0, HostWorker, nullptr, 0, nullptr);
    if (g_link.thread == nullptr)
    {
        if (g_link.mod_ref != nullptr) { FreeLibrary(g_link.mod_ref); g_link.mod_ref = nullptr; }
        InterlockedExchange(&g_link.state, LINK_IDLE);
        g_build_pending = false;
        Log("[feed32] could not start the host worker thread (%lu)", GetLastError());
        return false;
    }
    return true;
}

static void HostLinkJoin()
{
    if (g_link.thread == nullptr) return;
    WaitForSingleObject(g_link.thread, INFINITE);   // it has already published its result
    CloseHandle(g_link.thread);
    g_link.thread = nullptr;
}

// Returns true when the worker is definitely finished, so everything it could have been
// touching is safe to close. False means an orphan is still running and the CALLER must leak
// the pipe rather than close it -- see HostClose.
static bool HostLinkStop()
{
    if (g_link.thread == nullptr) { InterlockedExchange(&g_link.state, LINK_IDLE); return true; }
    InterlockedExchange(&g_link.abort, 1);
    if (g_link.abort_event != nullptr) SetEvent(g_link.abort_event);
    // Wakes a transfer that is parked on the pipe; the worker's own OVERLAPPED wait then
    // returns and it unwinds. Cancel on both handles, since a connect uses its own.
    if (g.pipe != nullptr)      CancelIoEx(g.pipe, nullptr);
    if (g_link.pipe != nullptr) CancelIoEx(g_link.pipe, nullptr);

    // NEVER wait from DLL_PROCESS_DETACH. A thread cannot finish exiting while another holds
    // the loader lock, so a join there blocks for the whole timeout and then unmaps this
    // module out from under a thread still executing it -- and ReShade unloads and reloads
    // this add-on per Vulkan instance, so that is a real path, not a theoretical one. The
    // worker holds its own reference to the module and drops it as its last act
    // (FreeLibraryAndExitThread), so walking away here is safe.
    if (g_detaching)
    {
        Log("[feed32] detaching with the host worker still running; it holds a reference to this "
            "module and will unload it when it finishes");
        return false;
    }
    // Otherwise every wait it can be in is bounded and wakes on the abort or the cancel, so
    // this should return almost at once. If it somehow does not, the one thing that must not
    // happen is a handle closed or reused under it: leave the orphan and everything it holds
    // strictly alone. g_link.thread stays set, so HostLinkStart refuses from here on.
    if (WaitForSingleObject(g_link.thread, 5000) != WAIT_OBJECT_0)
    {
        Log("[feed32] the host worker did not stop within 5 s; abandoning it and everything it holds");
        return false;
    }
    CloseHandle(g_link.thread);
    g_link.thread = nullptr;
    // It is finished, so anything it half-opened is ours. Break the pipe first and give the
    // host the same few seconds HostClose does: a host that has just connected still has a
    // ReShade ini to save, and killing it outright loses the overlay layout.
    if (g_link.pipe != nullptr) { CloseHandle(g_link.pipe); g_link.pipe = nullptr; }
    if (g_link.proc != nullptr)
    {
        if (WaitForSingleObject(g_link.proc, 4000) != WAIT_OBJECT_0) TerminateProcess(g_link.proc, 0);
        CloseHandle(g_link.proc);
        g_link.proc = nullptr;
    }
    InterlockedExchange(&g_link.state, LINK_IDLE);
    InterlockedExchange(&g_link.abort, 0);
    if (g_link.abort_event != nullptr) ResetEvent(g_link.abort_event);
    return true;
}

// True when the pipe is up and the host is alive. Otherwise it starts (or keeps waiting on)
// the connect and returns false: the caller renders this frame without a feed, which is the
// whole point -- this used to be up to 15 s of frozen game.
static bool HostConnectReady()
{
    if (g.pipe != nullptr && HostAlive()) return true;

    if (g_link.state == LINK_RUNNING) return false;
    if (g_link.state == LINK_IDLE)
    {
        if (g.pipe != nullptr || g.hproc != nullptr) HostClose();   // a stale half-link
        if (HostLinkStart(JOB_CONNECT))
            Log("[feed32] starting the 64-bit host in the background (the game keeps rendering)");
        return false;
    }
    if (g_link.job != JOB_CONNECT) return false;   // a build result; not ours to consume

    const bool  ok    = g_link.state == LINK_DONE;
    const DWORD ms    = g_link.ms;
    const bool  fatal = g_link.fatal;
    char why[192];
    strcpy_s(why, g_link.why[0] != '\0' ? g_link.why : "the host could not be started");
    HostLinkJoin();
    // Adopt whatever it opened either way, so the failure path disposes of it the usual way.
    if (g_link.pipe != nullptr) { g.pipe  = g_link.pipe; g_link.pipe = nullptr; }
    if (g_link.proc != nullptr) { g.hproc = g_link.proc; g_link.proc = nullptr; }
    InterlockedExchange(&g_link.state, LINK_IDLE);

    if (!ok)
    {
        if (fatal) { HostClose(); FeedDisable(why); }
        else       HostLost(why);
        return false;
    }
    g.panel_w = g_link.panel_w;
    g.panel_h = g_link.panel_h;
    Log("[feed32] host connected in %lu ms (protocol v%u, %s client)", ms, FEED_IPC_VERSION, HostClientKindName());
    RestoreGameFocus();   // the replacement host is up; take the foreground back if we lost it
    return true;
}

// The build exchange, from the render thread's side.
enum HostXfer { XFER_NONE, XFER_BUSY, XFER_DONE, XFER_FAILED };

static HostXfer HostBuildPoll(FeedBuildAck *ack)
{
    if (g_link.state == LINK_RUNNING) return XFER_BUSY;   // a connect counts as "not now" too
    if (g_link.job != JOB_BUILD) return XFER_NONE;
    if (g_link.state == LINK_DONE)
    {
        HostLinkJoin();
        *ack = g_link.ack;
        InterlockedExchange(&g_link.state, LINK_IDLE);
        Log("[feed32] the host answered the build in %lu ms", g_link.ms);
        return XFER_DONE;
    }
    if (g_link.state == LINK_FAILED)
    {
        HostLinkJoin();
        InterlockedExchange(&g_link.state, LINK_IDLE);
        return XFER_FAILED;
    }
    return XFER_NONE;
}

static bool HostBuildSubmit(const FeedBuild &b)
{
    if (g_link.state != LINK_IDLE) return false;
    g_link.build = b;
    if (!HostLinkStart(JOB_BUILD)) return false;
    Breadcrumb("waiting for the host's build (off the render thread)");
    return true;
}

// ---------------------------------------------------------------------------
// The host's DLSS 5 settings, controlled from the game's own ReShade panel.
// The renodx add-on reads [RenoDX.DLSS5] from the HOST's ReShade.ini at startup
// (only its own panel can change them live), so applying = write that ini and
// cycle the host. The gap is usually a couple of seconds but can reach ~15 s -- the
// replacement host has to re-init NGX and reload the ~165 MB DLSSNR model before it can
// serve a frame. The game really does render normally throughout now: the spawn, the
// handshake and the build all happen on the worker (see "Host process + pipe" above), where
// they used to hold the render thread for exactly that long.
// ---------------------------------------------------------------------------

// This table mirrors the DLSS 5 add-on's own panel one-for-one: same order, same
// labels, same widget kinds, same dropdown entries. Labels and combo entries were
// taken verbatim from the add-on binary's string table, so they read identically
// here and in the host window -- a slider here where the add-on has a dropdown is
// how NRStyle=2 got written into a two-entry ("Natural"/"Cinematic") setting.
//
// The order and the labels follow v4.7, the newest generation (it renamed most of
// the strength sliders and added NRGlobalTone and NRDiffuseWhiteNits). v4.6 and
// older builds still read every key here, and v4.7 still reads the three colour-
// transfer keys it dropped from its own panel, so the union is safe in both
// directions: a build that does not know a key simply never reads it, and this
// panel never writes a key the user did not touch.
//
// Ranges: the add-on does not publish its min/max, so these are supersets of every
// value observed in real ReShade.ini files written by the add-on's own panel on this
// machine. Where a range is a judgement call the tooltip says so, and the host
// window's own panel stays the final authority.
enum NRKind { NR_BOOL, NR_COMBO, NR_FLOAT };

struct NRSetting
{
    const char        *key;
    const char        *label;
    NRKind             kind;
    float              def, lo, hi;
    const char        *format;
    const char *const *items;
    int                item_count;
    const char        *tooltip;
};

static const char *const kNRPresetItems[] = { "Default", "Preset #1", "Preset #2", "Preset #3" };
// "Default" is shared with the preset list above -- the compiler pools the literal, which is
// why only one copy of it appears in the add-on's string table.
static const char *const kNRStyleItems[]  = { "Default", "Natural", "Cinematic" };
static const char *const kNRDepthItems[]  = { "Use game NGX flag", "Force normal depth", "Force inverted depth" };

// Section boundaries, matching the add-on's own grouping.
enum { NR_BRIDGE_FIRST = 11, NR_LEGACY_FIRST = 12, NR_GUIDE_FIRST = 15, NR_COUNT = 18 };

static const NRSetting kNR[NR_COUNT] = {
    { "NeuralUplift",      "Enable DLSS Neural Rendering", NR_BOOL,   1.0f,  0.0f,  1.0f, nullptr, nullptr, 0,
      "The add-on's master switch. Off leaves the DLAA output untouched." },
    { "NREnableUpscaling", "Enable Upscaling",             NR_BOOL,   0.0f,  0.0f,  1.0f, nullptr, nullptr, 0,
      "Lets the neural pass also upscale. The feed already hands the host a native-sized "
      "contract, so this is normally off; use 'Work resolution' above instead." },
    { "NRPreset",          "NR Preset",                    NR_COMBO,  0.0f,  0.0f,  3.0f, nullptr, kNRPresetItems, 4,
      "Maps to the DLSSNR render-preset hint." },
    { "NRStyle",           "NR Style",                     NR_COMBO,  0.0f,  0.0f,  2.0f, nullptr, kNRStyleItems, 3,
      "Default keeps the add-on's own choice; Natural and Cinematic force it." },
    { "NRIntensity",       "Overall Intensity",            NR_FLOAT,  1.0f,  0.0f,  2.0f, "%.2f", nullptr, 0,
      "Overall strength of the relighting (\"NR Intensity\" before v4.7). Community guides "
      "suggest 1.00-1.05; the slider allows the full observed 0-2 range." },
    { "NRGlobalTone",      "Global Tone Intensity",        NR_FLOAT,  1.0f,  0.0f,  2.0f, "%.2f", nullptr, 0,
      "v4.7 and newer only. Older add-on builds never read this key, so setting it there "
      "does nothing." },
    { "NRLocalTone",       "Local Tone Intensity",         NR_FLOAT,  1.0f,  0.0f,  2.0f, "%.2f", nullptr, 0, nullptr },
    { "NRLocalStructure",  "Structure Intensity",          NR_FLOAT,  1.0f,  0.0f,  2.0f, "%.2f", nullptr, 0, nullptr },
    { "NRSkinStructure",   "Character/Skin Structure",     NR_FLOAT, -1.0f, -1.0f,  1.0f, "%.2f", nullptr, 0,
      "Facial detail. Defaults to -1; positive values sharpen skin." },
    { "NRAutoMask",        "Automatic / Character Mask",   NR_BOOL,   1.0f,  0.0f,  1.0f, nullptr, nullptr, 0, nullptr },
    { "NRUICorrection",    "NR UI Correction",             NR_BOOL,   1.0f,  0.0f,  1.0f, nullptr, nullptr, 0,
      "Keeps HUD/UI from being re-lit. The feed inserts before ReShade's UI pass, so leave on." },

    { "NRDiffuseWhiteNits","Diffuse White (nits)",         NR_FLOAT, 203.0f, 80.0f, 1000.0f, "%.0f", nullptr, 0,
      "v4.7 and newer only. The nit level the bridge treats as diffuse white when the "
      "contract is HDR; ignored on the SDR sRGB bridge, which is what this feed publishes "
      "for an SDR game. The value shown when the key is absent is this panel's guess, not "
      "a value read from the add-on -- the host window's own panel is the authority." },

    { "NRPaperWhiteScale", "Scene Paper-White Scale",      NR_FLOAT,  1.0f,  0.0f, 10.0f, "%.3f", nullptr, 0,
      "Normalises scene brightness for the neural pass. 1.0 for an SDR game like this one; "
      "for HDR titles match the game's own paper-white." },
    { "NRTransferStrength","HDR Transfer Strength",        NR_FLOAT,  1.0f,  0.0f,  1.0f, "%.2f", nullptr, 0, nullptr },
    { "NRColorStrength",   "Color Strength",               NR_FLOAT,  1.0f,  0.0f,  1.0f, "%.2f", nullptr, 0, nullptr },

    { "NRDepthMode",       "Depth Convention",             NR_COMBO,  0.0f,  0.0f,  2.0f, nullptr, kNRDepthItems, 3,
      "Independent of the feed's own 'Depth inverted' above: this one overrides what the "
      "HOST tells NGX. Leave on 'Use game NGX flag'." },
    { "NRMVecScaleX",      "Motion Scale X Multiplier",    NR_FLOAT,  1.0f,  0.0f,  4.0f, "%.2f", nullptr, 0,
      "Applied by the add-on on top of the feed's own 'MV scale X' above -- they multiply." },
    { "NRMVecScaleY",      "Motion Scale Y Multiplier",    NR_FLOAT,  1.0f,  0.0f,  4.0f, "%.2f", nullptr, 0,
      "Applied by the add-on on top of the feed's own 'MV scale Y' above -- they multiply." },
};

static void HostIniPath(char *out)
{
    GetModuleFileNameA(g_self, out, MAX_PATH);
    if (char *s = strrchr(out, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - out), "host64\\ReShade.ini");
}

// The host's own window size (host/dlss5-feed-host64.cpp's FitWindowToWorkArea): a real
// resize of its window, swapchain and the panel texture this add-on casts, not just the
// scaled-picture "Panel size (%)" slider below. Same [DLSS5Host] keys the host itself
// reads and writes defaults for, so editing here and editing the ini by hand are the same
// thing. Takes effect the next time the host (re)starts -- it only sizes its window once,
// at startup -- so this is a plain read/write on the file, no IPC round trip.
static int  g_host_win_w = 620, g_host_win_h;   // 0 = auto (fill the work area), the host's own default
static bool g_host_win_loaded;

static void ReadHostWindowSize()
{
    char p[MAX_PATH];
    HostIniPath(p);
    g_host_win_w = GetPrivateProfileIntA("DLSS5Host", "WindowWidth", 620, p);
    g_host_win_h = GetPrivateProfileIntA("DLSS5Host", "WindowHeight", 0, p);
}

static void WriteHostWindowSize()
{
    char p[MAX_PATH], buf[16];
    HostIniPath(p);
    sprintf_s(buf, "%d", g_host_win_w);
    WritePrivateProfileStringA("DLSS5Host", "WindowWidth", buf, p);
    sprintf_s(buf, "%d", g_host_win_h);
    WritePrivateProfileStringA("DLSS5Host", "WindowHeight", buf, p);
}

// Cache of the host's settings, shown and edited on the ReShade overlay page (Add-ons
// tab -> DLSS 5 Feed). Loaded from the host's ini on first resolve so the panel always
// starts from what is actually active, never a stale default.
static float g_nr[NR_COUNT];
static bool  g_nr_present[NR_COUNT];   // the key existed in the host's ini
static bool  g_nr_touched[NR_COUNT];   // edited here since the last load
static bool  g_host_nr_loaded;

static void ReadHostNR()
{
    char p[MAX_PATH], buf[64];
    HostIniPath(p);
    for (int i = 0; i < NR_COUNT; ++i)
    {
        // A sentinel default separates "absent" from "present and equal to our default":
        // we must not write back a guessed default over a key the add-on owns.
        GetPrivateProfileStringA("RenoDX.DLSS5", kNR[i].key, "\x01", buf, sizeof(buf), p);
        g_nr_present[i] = (buf[0] != '\x01');
        g_nr[i]         = g_nr_present[i] ? static_cast<float>(atof(buf)) : kNR[i].def;
        g_nr_touched[i] = false;
    }
}

static void WriteHostNR()
{
    char p[MAX_PATH], buf[64];
    HostIniPath(p);
    for (int i = 0; i < NR_COUNT; ++i)
    {
        // Only keys the add-on already had, or that the user actually moved here. Writing
        // our guessed default for an untouched key would silently overwrite the add-on's
        // own (unpublished) default with ours.
        if (!g_nr_present[i] && !g_nr_touched[i]) continue;
        if (kNR[i].kind == NR_FLOAT) sprintf_s(buf, "%g", g_nr[i]);
        else                         sprintf_s(buf, "%d", static_cast<int>(g_nr[i]));
        WritePrivateProfileStringA("RenoDX.DLSS5", kNR[i].key, buf, p);
        g_nr_present[i] = true;
        g_nr_touched[i] = false;
    }
}

static void LogHostNR(const char *what)
{
    char line[768];   // 18 keys with "(default)" markers overflow 512 and stop the loop early
    int  n = sprintf_s(line, "[feed32] %s:", what);
    for (int i = 0; i < NR_COUNT && n > 0 && n < static_cast<int>(sizeof(line)) - 48; ++i)
    {
        if (kNR[i].kind == NR_FLOAT) n += sprintf_s(line + n, sizeof(line) - n, " %s=%g", kNR[i].key, g_nr[i]);
        else                         n += sprintf_s(line + n, sizeof(line) - n, " %s=%d", kNR[i].key, static_cast<int>(g_nr[i]));
        if (!g_nr_present[i] && !g_nr_touched[i]) n += sprintf_s(line + n, sizeof(line) - n, "(default)");
    }
    Log("%s", line);
}

static void HostClose();   // below

// Close the helper and let the next frame respawn it (EnsureHost). This is the recovery
// path when the host died, was never started, or latched off -- it writes no settings, so
// it is the right button for any neural consumer, not just RenoDX. HostApplySettings below
// repeats these steps rather than calling this, because it has to write the host's ini in
// the middle of the sequence and the order there is load-bearing.
// The overlay runs on the render thread but OUTSIDE the feed lock, and HostClose drains
// fences, tears down shared state and stops the worker -- none of which may happen beside a
// frame that is using them. So the buttons only record what they want; FeedFrame consumes
// it at the top of the next frame, inside the lock.
enum { HOST_REQ_NONE = 0, HOST_REQ_RESTART, HOST_REQ_APPLY };
static volatile LONG g_host_request;
static char          g_host_request_why[160];

static void HostRequest(int req, const char *why)
{
    if (why != nullptr) strcpy_s(g_host_request_why, why);
    InterlockedExchange(&g_host_request, req);
    CaptureGameFocus();   // spent once the replacement host has connected
}

// Note both of these still pay HostClose's shutdown wait (up to 4 s, so the old helper's
// ReShade can save its ini before the replacement claims the pipe -- the pipe name is per
// game PID and only one instance may own it, so the two cannot overlap). What they no longer
// pay is the START: that is the worker's, and the game renders through it.
static void HostRestart(const char *why)
{
    HostClose();          // drains the in-flight frame and releases the shared fences
    g.built = false;
    g.disabled = false;
    g.consecutive_fails = 0;
    g_retry_at = 0;
    g_disable_why[0] = '\0';
    Warn("%s -- the replacement starts in the background", why);
}

static void HostApplySettings()
{
    LogHostNR("applying DLSS 5 host settings");

    // Order matters: the host's ReShade saves its ini ON EXIT and would clobber our
    // values -- close the host first (HostClose drains the in-flight frame and
    // releases the shared fences), write after, respawn on the next frame.
    HostClose();
    WriteHostNR();

    g.built = false;
    g.disabled = false;
    g.consecutive_fails = 0;
    g_retry_at = 0;
    Warn("DLSS 5 settings applied -- the replacement host starts in the background");
}

static bool HostRequestPending() { return g_host_request != HOST_REQ_NONE; }

// Called from OnPresent, on the render thread and inside the feed lock.
static void HostConsumeRequest()
{
    const LONG req = InterlockedExchange(&g_host_request, HOST_REQ_NONE);
    if (req == HOST_REQ_RESTART) HostRestart(g_host_request_why);
    else if (req == HOST_REQ_APPLY) HostApplySettings();
}

// ---------------------------------------------------------------------------
// Shared resources (created on the game's D3D11 device)
// ---------------------------------------------------------------------------

static void ReleaseShared()
{
    // Vulkan: drop our VkImage aliases of the host's D3D12 textures. The memory belongs
    // to the host's resource; freeing the import does not free it. Nothing may still be
    // reading them, and the frame in flight is only one of the things that could be --
    // so drain the whole queue, not just our own fence.
    if (g.vk.ok)
    {
        if (g.rs_queue != nullptr) g.rs_queue->wait_idle();
        for (int i = 0; i < FEED_SLOTS; ++i)
        {
            if (g.vk_img[i] != VK_NULL_HANDLE) { g.vk.DestroyImage(g.vk.dev, g.vk_img[i], nullptr); g.vk_img[i] = VK_NULL_HANDLE; }
            if (g.vk_mem[i] != VK_NULL_HANDLE) { g.vk.FreeMemory(g.vk.dev, g.vk_mem[i], nullptr);   g.vk_mem[i] = VK_NULL_HANDLE; }
        }
        if (g.vk_panel     != VK_NULL_HANDLE) { g.vk.DestroyImage(g.vk.dev, g.vk_panel, nullptr);    g.vk_panel     = VK_NULL_HANDLE; }
        if (g.vk_panel_mem != VK_NULL_HANDLE) { g.vk.FreeMemory(g.vk.dev, g.vk_panel_mem, nullptr);  g.vk_panel_mem = VK_NULL_HANDLE; }
        g.vk_panel_init  = false;
        g.vk_layout_init = false;   // the next set starts from UNDEFINED again
        g.vk_released    = false;
    }
    // OpenGL: drop our aliases of the host's textures. Deleting them frees the import,
    // not the host's D3D12 resource. GL objects can only be deleted from the context
    // they live in; from anywhere else the driver reclaims them with the context.
    if (g.gl.ok)
    {
        if (g.gl.wglGetCurrentContext() == g.gl_ctx && g.gl_ctx != nullptr)
        {
            g.gl.Finish();   // the shared textures must be idle before the handles go
            for (int i = 0; i < FEED_SLOTS; ++i)
            {
                if (g.gl_tex[i]    != 0) { g.gl.DeleteTextures(1, &g.gl_tex[i]);            g.gl_tex[i]    = 0; }
                if (g.gl_memobj[i] != 0) { g.gl.DeleteMemoryObjectsEXT(1, &g.gl_memobj[i]); g.gl_memobj[i] = 0; }
            }
            if (g.gl_panel_tex    != 0) { g.gl.DeleteTextures(1, &g.gl_panel_tex);            g.gl_panel_tex    = 0; }
            if (g.gl_panel_memobj != 0) { g.gl.DeleteMemoryObjectsEXT(1, &g.gl_panel_memobj); g.gl_panel_memobj = 0; }
        }
        else
        {
            bool any = false;
            for (int i = 0; i < FEED_SLOTS; ++i) if (g.gl_tex[i] != 0) { any = true; g.gl_tex[i] = 0; g.gl_memobj[i] = 0; }
            if (g.gl_panel_tex != 0) { any = true; g.gl_panel_tex = 0; g.gl_panel_memobj = 0; }
            if (any) Log("[feed32] the GL context is not current here; the imported textures are left to the driver");
        }
    }
    // D3D10: the bridges are sized by the build, so they go out with it. Unbind first --
    // the relay may still hold the Output's render target view or a guide SRV from the
    // last frame, and a released view that is still bound is exactly what the debug layer
    // catches and a release driver quietly tolerates until it does not.
    if (g.d10.ok)
    {
        if (g.d10.relay_ctx != nullptr) { g.d10.relay_ctx->ClearState(); g.d10.relay_ctx->Flush(); }
        for (int i = 0; i < FEED_SLOTS; ++i) FeedD3D10ReleaseBridge(&g.d10_bridge[i]);
    }
    SafeRelease(g.output_srv);
    g.sr_active = false;              // the next build decides again
    g.output_width = g.output_height = 0;
    SafeRelease(g.color_stage_srv);
    SafeRelease(g.color_stage);
    SafeRelease(g.easu_srv);
    SafeRelease(g.easu_rtv);
    SafeRelease(g.easu_tex);
    for (int i = 0; i < FEED_SLOTS; ++i)
    {
        SafeRelease(g.input_rtv[i]);
        SafeRelease(g.tex[i]);
        if (g.tex_handle[i] != nullptr) { CloseHandle(g.tex_handle[i]); g.tex_handle[i] = nullptr; }
    }
    g.built     = false;
    g.out_valid = false;
}

static bool MakeShared(int slot, UINT w, UINT h, DXGI_FORMAT fmt, bool uav, bool render_target)
{
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
    HRESULT hr = g.dev->CreateTexture2D(&td, nullptr, &g.tex[slot]);
    if (FAILED(hr)) { Log("[feed32] tex %d CreateTexture2D failed 0x%08X", slot, hr); return false; }

    IDXGIResource1 *r = nullptr;
    hr = g.tex[slot]->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&r));
    if (SUCCEEDED(hr))
    {
        hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                   &g.tex_handle[slot]);
        r->Release();
    }
    if (FAILED(hr)) { Log("[feed32] tex %d CreateSharedHandle failed 0x%08X", slot, hr); return false; }
    return true;
}

static bool MakeBlitShaders()
{
    if (g.blit_vs != nullptr && g.blit_ps != nullptr) return true;
    static const char kSrc[] =
        "Texture2D<float4> src_color : register(t0);\n"
        "Texture2D<float2> src_mv : register(t1);\n"
        "Texture2D<float> src_depth : register(t2);\n"
        "SamplerState smp : register(s0);\n"
        "SamplerState point_smp : register(s1);\n"
        // jitter_uv: work_upscale=2 shifts the whole sampling grid by a sub-pixel amount
        // each frame (the synthetic jitter DLSS reconstructs from); zero otherwise. The
        // guides move with the colour so depth and vectors stay aligned with the sample.
        "cbuffer ResampleConstants : register(b0) { float2 mv_scale; float2 jitter_uv; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(VSOut i) : SV_Target { return float4(src_color.Sample(smp, i.uv).rgb, 1.0); }\n"
        // Motion vectors are in pixels, so they scale with the resolution ratio; depth is a
        // point sample (interpolating across a silhouette would invent geometry).
        "struct ResampleOut { float4 color : SV_Target0; float2 mv : SV_Target1; float depth : SV_Target2; };\n"
        "ResampleOut ps_resample(VSOut i) { ResampleOut o; float2 uv = i.uv + jitter_uv;\n"
        "  o.color = src_color.SampleLevel(smp, uv, 0);\n"
        "  o.mv = src_mv.SampleLevel(point_smp, uv, 0) * mv_scale;\n"
        "  o.depth = src_depth.SampleLevel(point_smp, uv, 0); return o; }\n";
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (compile == nullptr) { Log("[feed32] d3dcompiler_47.dll unavailable"); return false; }
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "feedblit", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { Log("[feed32] blit VS compile failed 0x%08X", hr); SafeRelease(err); return false; }
    SafeRelease(err);
    hr = compile(kSrc, sizeof(kSrc) - 1, "feedblit", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, &err);
    if (FAILED(hr)) { Log("[feed32] blit PS compile failed 0x%08X", hr); SafeRelease(err); SafeRelease(vs); return false; }
    SafeRelease(err);
    ID3DBlob *resample = nullptr;
    hr = compile(kSrc, sizeof(kSrc) - 1, "feedblit", nullptr, nullptr, "ps_resample", "ps_4_0", 0, 0, &resample, &err);
    if (FAILED(hr))
    {
        Log("[feed32] resample PS compile failed 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : "");
        SafeRelease(err); SafeRelease(vs); SafeRelease(ps); return false;
    }
    SafeRelease(err);

    hr = g.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.blit_vs);
    if (SUCCEEDED(hr)) hr = g.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.blit_ps);
    if (SUCCEEDED(hr)) hr = g.dev->CreatePixelShader(resample->GetBufferPointer(), resample->GetBufferSize(), nullptr, &g.resample_ps);
    vs->Release();
    ps->Release();
    resample->Release();
    if (FAILED(hr)) { Log("[feed32] blit shader creation failed 0x%08X", hr); return false; }

    D3D11_SAMPLER_DESC sd = {};
    // Linear: below 100% this sampler both downsamples the colour and upscales the result.
    // At 100% every tap lands on a texel centre, so it stays bit-identical to the old point filter.
    sd.Filter   = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(g.dev->CreateSamplerState(&sd, &g.blit_sampler))) { Log("[feed32] blit sampler failed"); return false; }
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(g.dev->CreateSamplerState(&sd, &g.point_sampler))) { Log("[feed32] point sampler failed"); return false; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = 16;
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g.dev->CreateBuffer(&cbd, nullptr, &g.resample_cb))) { Log("[feed32] resample constant buffer failed"); return false; }

    // FSR 1 is optional: a failure here only pins work_upscale to the bilinear path.
    // ps_4_0 on purpose -- the source uses integer Loads only, so feature level 10 is enough.
    ID3DBlob *easu = nullptr, *rcas = nullptr;
    hr = compile(kFsr1Src, sizeof(kFsr1Src) - 1, "feedfsr1", nullptr, nullptr, "ps_easu", "ps_4_0", 0, 0, &easu, &err);
    if (SUCCEEDED(hr)) { SafeRelease(err); hr = compile(kFsr1Src, sizeof(kFsr1Src) - 1, "feedfsr1", nullptr, nullptr, "ps_rcas", "ps_4_0", 0, 0, &rcas, &err); }
    if (SUCCEEDED(hr)) hr = g.dev->CreatePixelShader(easu->GetBufferPointer(), easu->GetBufferSize(), nullptr, &g.easu_ps);
    if (SUCCEEDED(hr)) hr = g.dev->CreatePixelShader(rcas->GetBufferPointer(), rcas->GetBufferSize(), nullptr, &g.rcas_ps);
    if (SUCCEEDED(hr)) { cbd.ByteWidth = sizeof(FsrConstants); hr = g.dev->CreateBuffer(&cbd, nullptr, &g.fsr_cb); }
    g.fsr_ok = SUCCEEDED(hr);
    if (!g.fsr_ok)
    {
        Log("[feed32] fsr1 shaders: failed 0x%08X: %s -- work_upscale=1 falls back to bilinear", hr,
            err ? (const char *)err->GetBufferPointer() : "");
        SafeRelease(g.easu_ps); SafeRelease(g.rcas_ps); SafeRelease(g.fsr_cb);
    }
    else Log("[feed32] fsr1 shaders: ok (EASU + RCAS expand-back available)");
    SafeRelease(err); SafeRelease(easu); SafeRelease(rcas);
    g.fsr_in_w = g.fsr_in_h = g.fsr_out_w = g.fsr_out_h = 0;
    g.fsr_sharpness = -1.0f;
    return true;
}

static bool BuildShared(UINT w, UINT h, UINT backbuffer_w, UINT backbuffer_h, DXGI_FORMAT bb_fmt)
{
    // The exchange with the host runs on the worker, so this function is entered twice per
    // build: once to prepare and hand it over, and once more -- some frames later -- to
    // finish with the ack. Everything before the hand-over is the same code it always was.
    FeedBuildAck ack = {};
    const HostXfer st = HostBuildPoll(&ack);
    if (st == XFER_BUSY)   { g_build_pending = true;  return false; }
    if (st == XFER_FAILED) { g_build_pending = false; HostLost("build exchange failed"); return false; }
    if (st != XFER_DONE)
    {
    Breadcrumb("building the shared textures");
    // Connect first, and off this thread: nothing below is worth doing without a host, and
    // re-creating the shared textures on every frame of a 15 s spawn would be worse still.
    if (!HostConnectReady()) { g_build_pending = true; return false; }
    ReleaseShared();

    g.width      = w;
    g.height     = h;
    g.backbuffer_width  = backbuffer_w;
    g.backbuffer_height = backbuffer_h;
    g.bb_fmt     = bb_fmt;
    g.color_fmt  = TypedColorFormat(bb_fmt);
    // Transport test copies Color->Output host-side with CopyResource: same format then.
    g.output_fmt = g_cfg.mode == 1 ? g.color_fmt : OutputFormatFor(g.color_fmt);
    if (g.color_fmt == DXGI_FORMAT_UNKNOWN)
    { g_build_pending = false; FeedDisable("unsupported backbuffer format"); return false; }
    const bool hdr      = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : IsHdrFormat(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    // work_upscale=2: the Output is native-sized and the host creates a DLSS Super
    // Resolution feature that expands the work-size frame itself (IPC v6). Not in the
    // transport test (it copies Color -> Output, sizes must match), and not once the host
    // has said no preset covers this ratio.
    const bool want_sr = g_cfg.work_upscale == 2 && g_cfg.mode == 2 && !g.sr_unavailable &&
                         (w != backbuffer_w || h != backbuffer_h);
    g.sr_requested  = want_sr;
    g.sr_active     = false;
    g.output_width  = want_sr ? backbuffer_w : w;
    g.output_height = want_sr ? backbuffer_h : h;
    g.jitter_index  = 0;
    g.jitter_x = g.jitter_y = 0.0f;

    // host_creates=1 in the cfg forces the route a feature-level 10.x device is pushed onto,
    // on a device that does not need it. It is the only way to exercise that path on a healthy
    // FL11 machine -- issue #43 shipped a fallback nobody here could run. Parse-only, not
    // written back, not on the overlay: a diagnostic, like the 64-bit side's jitter_sign.
    if (g_cfg.host_creates == 1 && !g.host_creates)
    {
        g.host_creates = true;
        g.no_uav       = true;   // the interesting half: it is what makes the Output differ
        Log("[feed32] host_creates=1: forcing the host to create the shared set (issue #43 test path)");
    }

    if (!g.host_creates)
    {
        int failed = -1;
        if      (!MakeShared(FEED_COLOR,  w, h, g.color_fmt,              false, true))  failed = FEED_COLOR;
        else if (!MakeShared(FEED_OUTPUT, g.output_width, g.output_height, g.output_fmt, true, false)) failed = FEED_OUTPUT;
        else if (!MakeShared(FEED_DEPTH,  w, h, DXGI_FORMAT_R32_FLOAT,    false, true))  failed = FEED_DEPTH;
        else if (!MakeShared(FEED_MV,     w, h, DXGI_FORMAT_R16G16_FLOAT, false, true))  failed = FEED_MV;
        if (failed >= 0)
        {
            // The game's device will not create what the host needs to open. Seen on a
            // feature-level 10.x device (NFS Most Wanted 2012, issue #33): Color went
            // through and Output -- the one slot with a UAV bind -- came back E_INVALIDARG,
            // because UAVs are a feature-level 11 feature. Retrying the same desc every
            // 30 s forever was the old behaviour. Instead, switch to the route the GL and
            // Vulkan clients always use: the host creates the set on D3D12 and hands the
            // handles in; this side opens them (OpenSharedResource1 needs no bind flags
            // of its own), and where UAVs are the problem the host keeps the Output's UAV
            // on its side and copies into a plain shared texture.
            const D3D_FEATURE_LEVEL fl = g.dev->GetFeatureLevel();
            ReleaseShared();
            g.host_creates = true;
            g.no_uav       = failed == FEED_OUTPUT || fl < D3D_FEATURE_LEVEL_11_0;
            Log("[feed32] the game's D3D11 device (feature level %d_%d) refused the shared %s texture; the host will "
                "create the shared set instead%s",
                (fl >> 12) & 0xF, (fl >> 8) & 0xF, FeedSlotName(failed),
                g.no_uav ? ", keeping the DLSS output's UAV on its own side (this device cannot bind one)" : "");
        }
    }

    FeedBuild b = {};
    b.width          = w;
    b.height         = h;
    b.color_fmt      = g.color_fmt;
    b.output_fmt     = g.output_fmt;
    b.hdr            = hdr ? 1 : 0;
    b.depth_inverted = inverted ? 1 : 0;
    b.flags_override = g_cfg.flags;
    b.transport      = g_cfg.mode == 1 ? 1 : 0;
    b.mv_scale_x     = g_cfg.mv_scale_x;
    b.mv_scale_y     = g_cfg.mv_scale_y;
    if (want_sr) { b.target_width = g.output_width; b.target_height = g.output_height; }
    if (g_cfg.async_home) b.client_flags |= FEED_BUILD_ASYNC_HOME;
    if (g.host_creates)
        b.client_flags |= FEED_BUILD_HOST_CREATES | (g.no_uav ? FEED_BUILD_OUTPUT_NO_UAV : 0);
    else
        for (int i = 0; i < FEED_SLOTS; ++i)
            b.tex[i] = reinterpret_cast<uintptr_t>(g.tex_handle[i]);
    // v7: the in-game panel's texture (0 when the host has none). Not when the host creates
    // the set -- it then makes the panel too and ignores this field, so making one here would
    // only burn a texture nobody writes; CastAdoptHostPanel11 opens the host's from the ack.
    b.panel_tex = g.host_creates ? 0 : CastMakePanel();

    // Hand it to the worker and let the frame go. The ack lands on a later frame, which is
    // where this function picks up again.
    if (!HostBuildSubmit(b)) { g_build_pending = false; FeedFail("could not hand the build to the host"); return false; }
    g_build_pending = true;
    Log("[feed32] building: %ux%u work resolution (%d%%) -> %ux%u backbuffer fmt=%u (depth reversed=%d, mode=%d) "
        "-- handed to the host, the game keeps rendering",
        w, h, g_cfg.work_resolution, backbuffer_w, backbuffer_h, bb_fmt, g.depth_reversed ? 1 : 0, g_cfg.mode);
    return false;
    }

    // ---- the host has answered: everything below runs with `ack` in hand ----
    //
    // Finish against what was actually BUILT, not against this frame's arguments. The caller
    // re-derives those from the current backbuffer every frame, and the resolution can have
    // changed while the build was in flight -- finishing against the new size would size the
    // staging texture and the RTVs for one resolution and the shared set for another. The
    // prepare half recorded the real ones, and nothing else writes them. A build that has
    // been overtaken is spotted by the caller's own size test on the very next frame and
    // simply rebuilt.
    g_build_pending = false;
    w = g.width;                     h = g.height;
    backbuffer_w = g.backbuffer_width; backbuffer_h = g.backbuffer_height;
    bb_fmt = g.bb_fmt;

    // Take ownership of every handle the host duplicated in, BEFORE any early return can drop
    // it. The host fills ack.tex[] whenever it created the textures, whether or not the
    // feature create that follows succeeded -- so a build that fails here used to leak four
    // handles, plus the panel's, every attempt. ReleaseShared() closes g.tex_handle[], and
    // BuildShared starts with a ReleaseShared(), so a retry is covered. (The GL and Vulkan
    // builds have always done this; the D3D11 host-creates path never did.)
    if (g.host_creates)
        for (int i = 0; i < FEED_SLOTS; ++i)
            g.tex_handle[i] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.tex[i]));

    if (!ack.ok && (ack.flags & FEED_ACK_SR_UNAVAILABLE) != 0)
    {
        // The host found no DLSS preset for this ratio: build again as DLAA + FSR 1, once.
        Log("[feed32] work_upscale=2: no DLSS preset covers %ux%u -> %ux%u; staying on DLAA + FSR 1 for this size", w, h, backbuffer_w, backbuffer_h);
        g.sr_unavailable = true;
        if (ack.panel_tex != 0) CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.panel_tex)));
        return BuildShared(w, h, backbuffer_w, backbuffer_h, bb_fmt);
    }
    if (!ack.ok)
    {
        Log("[feed32] host build failed (ngx 0x%08X)", ack.ngx_result);
        if (ack.panel_tex != 0) CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.panel_tex)));
        return false;
    }

    if (g.host_creates)
    {
        // The handles were taken above; ReleaseShared closes them.
        ID3D11Device1 *dev1 = nullptr;
        if (FAILED(g.dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1))) || dev1 == nullptr)
        {
            if (ack.panel_tex != 0) CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.panel_tex)));
            FeedDisable("ID3D11Device1 unavailable, so the host-created textures cannot be opened (Windows 8+ D3D11.1 runtime required)");
            return false;
        }
        // Try every slot before giving up. Returning on the first failure meant a report
        // only ever named one slot and never said whether the rest would have opened --
        // and that single line was the entire evidence for issue #43. The host logs the
        // D3D12 flags it used for each; together the two say which flag a device refuses.
        int  failed_mask = 0;
        char failed_names[64] = "";
        char opened_names[64] = "";
        static_assert(sizeof(failed_names) == sizeof(opened_names), "both lists share one size below");
        for (int i = 0; i < FEED_SLOTS; ++i)
        {
            const HRESULT hr = g.tex_handle[i] != nullptr
                ? dev1->OpenSharedResource1(g.tex_handle[i], __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&g.tex[i]))
                : E_HANDLE;
            char *const names = FAILED(hr) ? failed_names : opened_names;
            if (names[0] != '\0') strcat_s(names, sizeof(failed_names), ", ");
            strcat_s(names, sizeof(failed_names), FeedSlotName(i));
            if (FAILED(hr))
            {
                failed_mask |= 1 << i;
                Log("[feed32] OpenSharedResource1(%s) failed 0x%08X", FeedSlotName(i), hr);
            }
        }
        if (failed_mask != 0)
        {
            Log("[feed32] this device cannot open the host's shared textures: %s failed; %s opened. "
                "host64\\dlss5-feed-host.log lists the D3D12 flags each was created with.",
                failed_names, opened_names[0] != '\0' ? opened_names : "none");
            if (ack.panel_tex != 0) CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.panel_tex)));
            dev1->Release();
            ReleaseShared();
            FeedDisable("the shared textures cannot be created on the game's device or opened from the host's");
            return false;
        }
        CastAdoptHostPanel11(dev1, ack);   // the host made the panel too; ours would never be written
        dev1->Release();
        if (ack.output_fmt != 0 && static_cast<DXGI_FORMAT>(ack.output_fmt) != g.output_fmt)
        {
            Log("[feed32] host created the Output as fmt=%u (asked for %u)", ack.output_fmt, g.output_fmt);
            g.output_fmt = static_cast<DXGI_FORMAT>(ack.output_fmt);
        }
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format              = g.output_fmt;
    sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(g.dev->CreateShaderResourceView(g.tex[FEED_OUTPUT], &sv, &g.output_srv)))
    { Log("[feed32] output SRV failed"); ReleaseShared(); return false; }
    if (!MakeBlitShaders()) { ReleaseShared(); return false; }

    // Below 100% the guides are resampled into the shared set rather than copied, which
    // needs an RTV per input and an SRV-able source. ReShade's backbuffer has neither
    // D3D11_BIND_SHADER_RESOURCE nor a resource behind `DLSS5_ColorInput : COLOR`, so
    // stage a native-size copy we own and downsample from that.
    if (backbuffer_w != w || backbuffer_h != h)
    {
        const int input_slots[] = { FEED_COLOR, FEED_MV, FEED_DEPTH };
        for (const int slot : input_slots)
        {
            D3D11_RENDER_TARGET_VIEW_DESC rv = {};
            rv.Format = slot == FEED_COLOR ? g.color_fmt
                      : (slot == FEED_MV ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32_FLOAT);
            rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(g.dev->CreateRenderTargetView(g.tex[slot], &rv, &g.input_rtv[slot])))
            { Log("[feed32] input RTV %d failed", slot); ReleaseShared(); return false; }
        }

        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width            = backbuffer_w;
        sd.Height           = backbuffer_h;
        sd.MipLevels        = 1;
        sd.ArraySize        = 1;
        sd.Format           = bb_fmt;          // exact backbuffer format, so CopyResource accepts it
        sd.SampleDesc.Count = 1;
        sd.Usage            = D3D11_USAGE_DEFAULT;
        sd.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g.dev->CreateTexture2D(&sd, nullptr, &g.color_stage)))
        { Log("[feed32] work-resolution staging texture failed (%ux%u fmt=%u)", backbuffer_w, backbuffer_h, bb_fmt); ReleaseShared(); return false; }

        D3D11_SHADER_RESOURCE_VIEW_DESC ss = {};
        ss.Format              = g.color_fmt;  // typed view, in case the backbuffer is TYPELESS
        ss.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        ss.Texture2D.MipLevels = 1;
        if (FAILED(g.dev->CreateShaderResourceView(g.color_stage, &ss, &g.color_stage_srv)))
        { Log("[feed32] work-resolution staging SRV failed"); ReleaseShared(); return false; }

        Log("[feed32] work-resolution source: %ux%u staging copy -> %ux%u", backbuffer_w, backbuffer_h, w, h);

        // work_upscale=1 needs somewhere native-sized for EASU to write and RCAS to read.
        // Created regardless of the current setting so toggling it later is free.
        D3D11_TEXTURE2D_DESC ed = sd;
        ed.Format    = g.output_fmt;
        ed.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(g.dev->CreateTexture2D(&ed, nullptr, &g.easu_tex)))
        {
            D3D11_RENDER_TARGET_VIEW_DESC rv = {};
            rv.Format = g.output_fmt;
            rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            D3D11_SHADER_RESOURCE_VIEW_DESC es = {};
            es.Format              = g.output_fmt;
            es.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
            es.Texture2D.MipLevels = 1;
            if (FAILED(g.dev->CreateRenderTargetView(g.easu_tex, &rv, &g.easu_rtv)) ||
                FAILED(g.dev->CreateShaderResourceView(g.easu_tex, &es, &g.easu_srv)))
            { SafeRelease(g.easu_rtv); SafeRelease(g.easu_srv); SafeRelease(g.easu_tex); }
        }
        if (g.easu_tex == nullptr)
            Log("[feed32] fsr1 intermediate (%ux%u fmt=%u) failed; work_upscale=1 falls back to bilinear", backbuffer_w, backbuffer_h, g.output_fmt);
    }

    if (g.fence_in == nullptr || g.fence_out == nullptr)
    {
        ID3D11Device5 *dev5 = nullptr;
        if (FAILED(g.dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5))) || dev5 == nullptr)
        { FeedDisable("ID3D11Device5 unavailable (Windows 10 1703+ required)"); return false; }
        HRESULT h1 = dev5->OpenSharedFence(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.fence_in)),
                                           __uuidof(ID3D11Fence), reinterpret_cast<void **>(&g.fence_in));
        HRESULT h2 = dev5->OpenSharedFence(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.fence_out)),
                                           __uuidof(ID3D11Fence), reinterpret_cast<void **>(&g.fence_out));
        dev5->Release();
        if (FAILED(h1) || FAILED(h2)) { Log("[feed32] OpenSharedFence failed 0x%08X/0x%08X", h1, h2); return false; }
    }

    Log("[feed32] shared set ready: %ux%u (%d%% of %ux%u) color fmt=%u output fmt=%u (host ngx 0x%08X, %s)",
        w, h, g_cfg.work_resolution, backbuffer_w, backbuffer_h,
        g.color_fmt, g.output_fmt, ack.ngx_result, g_cfg.mode == 1 ? "transport" : "DLSS");
    // g.sr_requested is what the prepare half asked for; `want_sr` itself is out of scope
    // here, since the two halves are now separated by the worker's round trip.
    if (g.sr_requested && (ack.flags & FEED_ACK_SR_ACTIVE) != 0)
    {
        g.sr_active  = true;
        g.sr_quality = ack.sr_quality;
        const float ratio = static_cast<float>(backbuffer_w) / static_cast<float>(w);
        g.jitter_phases = static_cast<UINT>(ceilf(8.0f * ratio * ratio));
        Log("[feed32] work_upscale=2: DLSS %s, %ux%u -> %ux%u, Halton(2,3) over %u phases",
            SrQualityName(g.sr_quality), w, h, backbuffer_w, backbuffer_h, g.jitter_phases);
    }
    else if (g.sr_requested)
        Log("[feed32] work_upscale=2 was asked for but the host built DLAA (an older host?); expand-back stays spatial");
    if (g.host_creates)
        Log("[feed32] the shared set is host-created and opened here%s", g.no_uav ? "; the DLSS output is copied into it host-side" : "");
    g.built      = true;
    g.need_reset = true;
    g.out_valid  = false;   // a fresh Output slot holds nothing worth carrying home
    g.consecutive_fails = 0;
    return true;
}

// ---------------------------------------------------------------------------
// OpenGL client: the host creates the shared set and duplicates the handles in,
// because GL memory objects are import-only. Everything else about the protocol is
// unchanged -- the host's 'B' and 'F' handlers do not care which API asked.
// ---------------------------------------------------------------------------

static bool BuildSharedGl(UINT w, UINT h, DXGI_FORMAT bb_fmt, uint64_t rtv_handle)
{
    // Same two-part shape as BuildShared: prepare and hand over, then finish with the ack a
    // few frames later. See the comment there.
    FeedBuildAck ack = {};
    const HostXfer st = HostBuildPoll(&ack);
    if (st == XFER_BUSY)   { g_build_pending = true;  return false; }
    if (st == XFER_FAILED) { g_build_pending = false; HostLost("build exchange failed"); return false; }
    if (st != XFER_DONE)
    {
    Breadcrumb("building the shared textures (OpenGL)");
    if (!HostConnectReady()) { g_build_pending = true; return false; }
    ReleaseShared();

    g.width  = w;
    g.height = h;
    g.backbuffer_width  = w;   // v1 GL is DLAA at 100%: no work-resolution scaling
    g.backbuffer_height = h;
    g.bb_fmt     = bb_fmt;
    g.color_fmt  = GlSafeColorFormat(TypedColorFormat(bb_fmt));
    g.output_fmt = g_cfg.mode == 1 ? g.color_fmt : GlSafeColorFormat(OutputFormatFor(g.color_fmt));
    if (g.color_fmt == DXGI_FORMAT_UNKNOWN)
    { g_build_pending = false; FeedDisable("unsupported backbuffer format"); return false; }
    const bool hdr      = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : IsHdrFormat(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    FeedBuild b = {};
    b.width          = w;
    b.height         = h;
    b.color_fmt      = g.color_fmt;
    b.output_fmt     = g.output_fmt;
    b.hdr            = hdr ? 1 : 0;
    b.depth_inverted = inverted ? 1 : 0;
    b.flags_override = g_cfg.flags;
    b.transport      = g_cfg.mode == 1 ? 1 : 0;
    b.mv_scale_x     = g_cfg.mv_scale_x;
    b.mv_scale_y     = g_cfg.mv_scale_y;
    if (g_cfg.async_home) b.client_flags |= FEED_BUILD_ASYNC_HOME;
    // b.tex stays zero: on this path the host creates, and answers with its handles.

    if (!HostBuildSubmit(b)) { g_build_pending = false; FeedFail("could not hand the build to the host"); return false; }
    g_build_pending = true;
    Log("[feed32] building: %ux%u backbuffer fmt=%u (OpenGL, depth reversed=%d, mode=%d) "
        "-- handed to the host, the game keeps rendering",
        w, h, bb_fmt, g.depth_reversed ? 1 : 0, g_cfg.mode);
    return false;
    }

    // ---- the host has answered ----
    // Against what was built, not this frame's arguments -- see BuildShared.
    g_build_pending = false;
    w = g.width; h = g.height; bb_fmt = g.bb_fmt;

    // Own the duplicated handles before any early return can drop them -- see the same
    // step in BuildSharedVk for why the failing build is exactly when this bites.
    for (int i = 0; i < FEED_SLOTS; ++i)
        g.tex_handle[i] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.tex[i]));

    if (!ack.ok)
    {
        Log("[feed32] host build failed (ngx 0x%08X)", ack.ngx_result);
        return false;
    }

    // The host owns the Output format on every path where it owns the texture. Today
    // this always agrees with what we asked for -- GlSafeColorFormat has already ruled
    // out the one format (BGRA8) the host's typed-UAV-store fallback can change -- but
    // trusting its answer rather than our assumption is what keeps the two in step.
    if (ack.output_fmt != 0 && static_cast<DXGI_FORMAT>(ack.output_fmt) != g.output_fmt)
    {
        Log("[feed32] the host created the Output as %s, not the requested %s",
            FeedFmtName(static_cast<DXGI_FORMAT>(ack.output_fmt)), FeedFmtName(g.output_fmt));
        g.output_fmt = static_cast<DXGI_FORMAT>(ack.output_fmt);
    }

    // The fences are per session, not per build: import them once.
    if (g.gl_sem_in == 0 || g.gl_sem_out == 0)
    {
        g.fence_in_handle  = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.fence_in));
        g.fence_out_handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.fence_out));
        g.gl_sem_in  = FeedGlImportFence(&g.gl, g.fence_in_handle);
        g.gl_sem_out = FeedGlImportFence(&g.gl, g.fence_out_handle);
        Log("[feed32] D3D12 fence -> GL semaphore import: in=%s out=%s",
            g.gl_sem_in ? "OK" : "FAILED", g.gl_sem_out ? "OK" : "FAILED");
        if (g.gl_sem_in == 0 || g.gl_sem_out == 0)
        { FeedDisable("cross-process fence import failed (see dlss5-feed.log)"); return false; }
    }

    static const DXGI_FORMAT kFmt[FEED_SLOTS] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
                                                  DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT };
    for (int i = 0; i < FEED_SLOTS; ++i)
    {
        const DXGI_FORMAT f = i == FEED_COLOR ? g.color_fmt : i == FEED_OUTPUT ? g.output_fmt : kFmt[i];
        const GLenum glf = FeedGlFormat(f);
        if (glf == 0 || g.tex_handle[i] == nullptr || ack.tex_size[i] == 0 ||
            !FeedGlImportImage(&g.gl, g.tex_handle[i], ack.tex_size[i],
                               static_cast<GLsizei>(w), static_cast<GLsizei>(h), glf,
                               &g.gl_tex[i], &g.gl_memobj[i]))
        {
            Log("[feed32] texture import FAILED: slot %d %ux%u fmt=%u (%llu bytes, GL error 0x%04X)",
                i, w, h, f, static_cast<unsigned long long>(ack.tex_size[i]), FeedGlDrainErrors(&g.gl));
            ReleaseShared();
            return false;
        }
    }

    if (g.gl_fbo_read == 0) g.gl.GenFramebuffers(1, &g.gl_fbo_read);
    if (g.gl_fbo_draw == 0) g.gl.GenFramebuffers(1, &g.gl_fbo_draw);
    if (g.gl_fbo_read == 0 || g.gl_fbo_draw == 0)
    { Log("[feed32] glGenFramebuffers failed"); ReleaseShared(); return false; }
    CastImportPanelGl(ack);   // v7: the host-created panel texture, if the host has one

    {
        FeedGlStateGuard guard(&g.gl);
        const GLenum ty = FeedGlHandleType(rtv_handle);
        const GLint enc = FeedGlColorEncoding(&g.gl, g.gl_fbo_read, rtv_handle);
        Log("[feed32] technique target: %s (GL object type 0x%04X), colour encoding %s",
            rtv_handle == 0 ? "the DEFAULT framebuffer" :
            ty == GL_RENDERBUFFER ? "a renderbuffer" :
            ty == GL_TEXTURE_2D ? "a GL_TEXTURE_2D" : "an unexpected GL object",
            ty, enc == GL_SRGB ? "GL_SRGB" : enc == GL_LINEAR ? "GL_LINEAR" : "unknown");
    }

    Log("[feed32] shared set ready (OpenGL): %ux%u color fmt=%u output fmt=%u (host ngx 0x%08X, %s)",
        w, h, g.color_fmt, g.output_fmt, ack.ngx_result, g_cfg.mode == 1 ? "transport" : "DLSS");
    g.built      = true;
    g.need_reset = true;
    g.out_valid  = false;   // a fresh Output slot holds nothing worth carrying home
    g.consecutive_fails = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Vulkan client (issue #15: 32-bit games on DXVK). The host creates the shared set
// here too -- D3D12 cannot open memory Vulkan exported -- and we import it as
// VkImages, exactly as the 64-bit add-on's Vulkan transport does in-process.
//
// The division of labour is the 64-bit path's, unchanged: raw vkCmd* for the copies
// (recorded into ReShade's own command buffer, so they are simply more commands in
// the buffer it is already building), and ReShade for every QUEUE operation. That
// second half matters -- a raw vkQueueSubmit would race ReShade's and the game's
// submits -- and it is possible because in ReShade's Vulkan backend an api::fence
// handle IS a VkSemaphore, so our imported timeline semaphores can be handed straight
// back to it.
// ---------------------------------------------------------------------------

// Resolve the raw entry points from the game's own VkDevice. Once per device; the
// failure path is where a missing interop extension is diagnosed.
static bool EnsureVulkanLoaded(reshade::api::effect_runtime *rt)
{
    if (g.vk.ok) return true;

    g.rs_dev   = rt->get_device();
    g.rs_queue = rt->get_command_queue();
    if (g.rs_dev == nullptr || g.rs_queue == nullptr)
    { FeedDisable("the ReShade device/queue is not reachable"); return false; }

    if (FeedVkLoad(&g.vk, FeedVkDispatch<VkDevice>(g.rs_dev->get_native())))
    {
        Log("[feed32] Vulkan: interop entry points resolved on device %p (thread %lu)",
            (void *)g.vk.dev, GetCurrentThreadId());
        return true;
    }

    // The KHR external-interop extensions were not enabled at vkCreateDevice. The
    // add-on's own hook (feed_vk_hook.h) normally appends them; if it never saw this
    // device -- DXVK resolved vkCreateDevice some way the hook does not cover, or the
    // hook could not be installed at all -- the out-of-process layer is the fallback.
    Log("[feed32] the Vulkan external-memory/semaphore entry points are missing: the KHR external-interop");
    Log("[feed32] extensions were not enabled on this device at vkCreateDevice.");
    if (g_vk_create_device_target == nullptr)
        Log("[feed32] The add-on's vkCreateDevice hook was NOT installed (see the hook lines above).");
    else if (g_vk_hook_devices == 0)
        Log("[feed32] The add-on's vkCreateDevice hook was installed but never called: this game creates its device some way it does not intercept.");
    else
        Log("[feed32] The hook did run (%d vkCreateDevice call(s)); check its per-extension lines above for what the driver refused.", g_vk_hook_devices);
    Log("[feed32] FALLBACK: launch the game through layer\\x86\\run-with-feed-layer32.bat (the 32-bit VK_LAYER_feed_vk appends them from outside).");
    FeedDisable("the Vulkan interop extensions are missing on this device -- see dlss5-feed.log");
    return false;
}

static bool BuildSharedVk(UINT w, UINT h, DXGI_FORMAT bb_fmt)
{
    // Same two-part shape as BuildShared: prepare and hand over, then finish with the ack a
    // few frames later. See the comment there.
    FeedBuildAck ack = {};
    const HostXfer st = HostBuildPoll(&ack);
    if (st == XFER_BUSY)   { g_build_pending = true;  return false; }
    if (st == XFER_FAILED) { g_build_pending = false; HostLost("build exchange failed"); return false; }
    if (st != XFER_DONE)
    {
    Breadcrumb("building the shared textures (Vulkan)");
    if (!HostConnectReady()) { g_build_pending = true; return false; }
    ReleaseShared();

    g.width  = w;
    g.height = h;
    g.backbuffer_width  = w;   // v1 Vulkan is DLAA at 100%: no work-resolution scaling
    g.backbuffer_height = h;
    g.bb_fmt     = bb_fmt;
    g.color_fmt  = FeedFmtTypedColor(bb_fmt);
    if (g.color_fmt == DXGI_FORMAT_UNKNOWN)
    {
        Log("[feed32] backbuffer format %u (%s) is not supported", bb_fmt, FeedFmtName(bb_fmt));
        g_build_pending = false;
        FeedDisable("unsupported backbuffer format");
        return false;
    }
    // Transport test copies Color->Output host-side with CopyTextureRegion: same format
    // then. Otherwise ask for the channel order the backbuffer has, so the way home is a
    // raw vkCmdCopyImage -- the host gets the final say (see ack.output_fmt below).
    const DXGI_FORMAT want_output = g_cfg.mode == 1 ? g.color_fmt : FeedFmtOutputFor(g.color_fmt);
    g.output_fmt = want_output;   // what we asked for; the finish half compares the ack against it
    const bool hdr      = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : FeedFmtIsHdr(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    FeedBuild b = {};
    b.width          = w;
    b.height         = h;
    b.color_fmt      = g.color_fmt;
    b.output_fmt     = want_output;
    b.hdr            = hdr ? 1 : 0;
    b.depth_inverted = inverted ? 1 : 0;
    b.flags_override = g_cfg.flags;
    b.transport      = g_cfg.mode == 1 ? 1 : 0;
    b.mv_scale_x     = g_cfg.mv_scale_x;
    b.mv_scale_y     = g_cfg.mv_scale_y;
    // Under async_home the game copies home the PREVIOUS frame's result and is never
    // waiting on the evaluate the host is running, so the host may take its own time to
    // get a present slot rather than dropping the present. That drop is what starved the
    // neural consumer on DXVK (issue #15).
    if (g_cfg.async_home) b.client_flags |= FEED_BUILD_ASYNC_HOME;
    // b.tex stays zero: on this path the host creates, and answers with its handles.

    if (!HostBuildSubmit(b)) { g_build_pending = false; FeedFail("could not hand the build to the host"); return false; }
    g_build_pending = true;
    Log("[feed32] building: %ux%u backbuffer %s (Vulkan, depth reversed=%d, mode=%d) "
        "-- handed to the host, the game keeps rendering",
        w, h, FeedFmtName(bb_fmt), g.depth_reversed ? 1 : 0, g_cfg.mode);
    return false;
    }

    // ---- the host has answered ----
    // Against what was built, not this frame's arguments -- see BuildShared.
    g_build_pending = false;
    w = g.width; h = g.height; bb_fmt = g.bb_fmt;

    // Take ownership of the duplicated handles NOW, before any early return can drop
    // them on the floor. The host duplicates all four into this process and fills
    // ack.tex[] whether or not the build as a whole succeeded -- the common failure is
    // the textures being made fine and then CreateFeature failing -- and g.tex_handle[]
    // is the only thing ReleaseShared closes. Every path out of here runs ReleaseShared
    // first on the next attempt, so this is what keeps a retry loop from leaking four
    // handles a go. Zeros from a host that did not create anything are harmless.
    for (int i = 0; i < FEED_SLOTS; ++i)
        g.tex_handle[i] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.tex[i]));

    if (!ack.ok)
    {
        Log("[feed32] host build failed (ngx 0x%08X)", ack.ngx_result);
        return false;
    }

    // The host owns the Output format: only its device can be asked whether a typed UAV
    // store to BGRA8 exists on this GPU, and it falls back to RGBA8 where it does not.
    const DXGI_FORMAT requested = g.output_fmt;   // the prepare half's want_output, out of scope here
    g.output_fmt = ack.output_fmt != 0 ? static_cast<DXGI_FORMAT>(ack.output_fmt) : requested;
    if (g.output_fmt != requested)
        Log("[feed32] the host created the Output as %s, not the requested %s",
            FeedFmtName(g.output_fmt), FeedFmtName(requested));

    // The fences are per session, not per build: import them once.
    if (g.vk_sem_in == VK_NULL_HANDLE || g.vk_sem_out == VK_NULL_HANDLE)
    {
        g.fence_in_handle  = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.fence_in));
        g.fence_out_handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.fence_out));
        g.vk_sem_in  = FeedVkImportFence(&g.vk, g.fence_in_handle);
        g.vk_sem_out = FeedVkImportFence(&g.vk, g.fence_out_handle);
        Log("[feed32] D3D12 fence -> Vulkan timeline semaphore import: in=%s out=%s",
            g.vk_sem_in  != VK_NULL_HANDLE ? "OK" : "FAILED",
            g.vk_sem_out != VK_NULL_HANDLE ? "OK" : "FAILED");
        if (g.vk_sem_in == VK_NULL_HANDLE || g.vk_sem_out == VK_NULL_HANDLE)
        { FeedDisable("cross-process fence import failed (see dlss5-feed.log)"); return false; }
        // Hand them back to ReShade as api::fence handles, which is what they already are.
        g.rs_fence_in  = { FeedVkValue(g.vk_sem_in) };
        g.rs_fence_out = { FeedVkValue(g.vk_sem_out) };
    }

    static const DXGI_FORMAT kFmt[FEED_SLOTS] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
                                                  DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT };
    for (int i = 0; i < FEED_SLOTS; ++i)
    {
        const DXGI_FORMAT f = i == FEED_COLOR ? g.color_fmt : i == FEED_OUTPUT ? g.output_fmt : kFmt[i];
        const VkFormat vkf = FeedVkFormat(f);
        // Only the Output is written through a UAV, so only it asks for storage usage --
        // the same split the 64-bit add-on makes, matching the D3D12 resource's flags.
        const bool storage = i == FEED_OUTPUT;
        if (vkf == VK_FORMAT_UNDEFINED || g.tex_handle[i] == nullptr ||
            !FeedVkImportImage(&g.vk, g.tex_handle[i], w, h, vkf, storage,
                               &g.vk_img[i], &g.vk_mem[i]))
        {
            Log("[feed32] texture import FAILED: slot %d %ux%u %s (raw Vulkan external-memory import)",
                i, w, h, FeedFmtName(f));
            if (storage && f == DXGI_FORMAT_B8G8R8A8_UNORM)
                Log("[feed32] the Output is the one slot imported with VK_IMAGE_USAGE_STORAGE_BIT, and this is "
                    "a BGRA8 one -- if the driver does not support storage images in that format, that is why. "
                    "Forcing an RGBA8 backbuffer, or a game/DXVK setting that yields one, works around it.");
            ReleaseShared();
            return false;
        }
    }

    CastImportPanelVk(ack);   // v7: the host-created panel texture, if the host has one

    Log("[feed32] copy home: %s (output %s -> backbuffer %s)",
        FeedFmtSameTexelLayout(g.output_fmt, bb_fmt) ? "raw vkCmdCopyImage"
                                                     : "vkCmdBlitImage (CONVERTS: expect issue #11 washout)",
        FeedFmtName(g.output_fmt), FeedFmtName(bb_fmt));
    Log("[feed32] shared set ready (Vulkan): %ux%u color %s output %s (host ngx 0x%08X, %s)",
        w, h, FeedFmtName(g.color_fmt), FeedFmtName(g.output_fmt), ack.ngx_result,
        g_cfg.mode == 1 ? "transport" : "DLSS");
    g.built      = true;
    g.need_reset = true;
    g.out_valid  = false;   // a fresh Output slot holds nothing worth carrying home
    g.consecutive_fails = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Guide preparation: a straight copy at 100%, one resample pass below it
// ---------------------------------------------------------------------------

static bool CopyOrResampleInputs(ID3D11DeviceContext *ctx,
                                 ID3D11Texture2D *color, ID3D11Texture2D *mv, ID3D11Texture2D *depth,
                                 ID3D11ShaderResourceView *mv_srv, ID3D11ShaderResourceView *depth_srv,
                                 UINT source_w, UINT source_h)
{
    if (source_w == g.width && source_h == g.height)
    {
        ctx->CopyResource(g.tex[FEED_COLOR], color);
        ctx->CopyResource(g.tex[FEED_DEPTH], depth);
        ctx->CopyResource(g.tex[FEED_MV], mv);
        return true;
    }

    if (g.color_stage == nullptr || g.color_stage_srv == nullptr || g.resample_ps == nullptr) return false;
    if (mv_srv == nullptr || depth_srv == nullptr) return false;
    ctx->CopyResource(g.color_stage, color);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(g.resample_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    { Log("[feed32] resample constant-buffer map failed"); return false; }
    // A shift of j work pixels is j / work_size in uv, whatever the source size is.
    const float constants[4] = {
        static_cast<float>(g.width)  / static_cast<float>(source_w),
        static_cast<float>(g.height) / static_cast<float>(source_h),
        g.sr_active ? g.jitter_x / static_cast<float>(g.width)  : 0.0f,
        g.sr_active ? g.jitter_y / static_cast<float>(g.height) : 0.0f
    };
    memcpy(mapped.pData, constants, sizeof(constants));
    ctx->Unmap(g.resample_cb, 0);

    ID3D11RenderTargetView   *old_rtvs[3] = {};
    ID3D11DepthStencilView   *old_dsv = nullptr;
    ID3D11VertexShader       *old_vs = nullptr;
    ID3D11PixelShader        *old_ps = nullptr;
    ID3D11ShaderResourceView *old_srvs[3] = {};
    ID3D11SamplerState       *old_samplers[2] = {};
    ID3D11Buffer             *old_cb = nullptr;
    ID3D11InputLayout        *old_il = nullptr;
    ID3D11BlendState         *old_bs = nullptr; FLOAT old_bf[4] = {}; UINT old_mask = 0;
    ID3D11DepthStencilState  *old_ds = nullptr; UINT old_sref = 0;
    ID3D11RasterizerState    *old_rs = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY  old_topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    UINT nvp = 1; D3D11_VIEWPORT old_vp = {};

    ctx->OMGetRenderTargets(3, old_rtvs, &old_dsv);
    ctx->VSGetShader(&old_vs, nullptr, nullptr);
    ctx->PSGetShader(&old_ps, nullptr, nullptr);
    ctx->PSGetShaderResources(0, 3, old_srvs);
    ctx->PSGetSamplers(0, 2, old_samplers);
    ctx->PSGetConstantBuffers(0, 1, &old_cb);
    ctx->IAGetInputLayout(&old_il);
    ctx->IAGetPrimitiveTopology(&old_topo);
    ctx->OMGetBlendState(&old_bs, old_bf, &old_mask);
    ctx->OMGetDepthStencilState(&old_ds, &old_sref);
    ctx->RSGetState(&old_rs);
    ctx->RSGetViewports(&nvp, &old_vp);

    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(g.width);
    vp.Height   = static_cast<float>(g.height);
    vp.MaxDepth = 1.0f;
    ID3D11RenderTargetView   *rtvs[3] = { g.input_rtv[FEED_COLOR], g.input_rtv[FEED_MV], g.input_rtv[FEED_DEPTH] };
    ID3D11ShaderResourceView *srvs[3] = { g.color_stage_srv, mv_srv, depth_srv };
    ID3D11SamplerState       *samplers[2] = { g.blit_sampler, g.point_sampler };

    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(3, rtvs, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.blit_vs, nullptr, 0);
    ctx->PSSetShader(g.resample_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 3, srvs);
    ctx->PSSetSamplers(0, 2, samplers);
    ctx->PSSetConstantBuffers(0, 1, &g.resample_cb);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView *null_srvs[3] = {};
    ID3D11RenderTargetView   *null_rtvs[3] = {};
    ctx->PSSetShaderResources(0, 3, null_srvs);
    ctx->OMSetRenderTargets(3, null_rtvs, nullptr);

    ctx->OMSetRenderTargets(3, old_rtvs, old_dsv);
    ctx->VSSetShader(old_vs, nullptr, 0);
    ctx->PSSetShader(old_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 3, old_srvs);
    ctx->PSSetSamplers(0, 2, old_samplers);
    ctx->PSSetConstantBuffers(0, 1, &old_cb);
    ctx->IASetInputLayout(old_il);
    ctx->IASetPrimitiveTopology(old_topo);
    ctx->OMSetBlendState(old_bs, old_bf, old_mask);
    ctx->OMSetDepthStencilState(old_ds, old_sref);
    ctx->RSSetState(old_rs);
    if (nvp != 0) ctx->RSSetViewports(1, &old_vp);

    for (auto *r : old_rtvs) SafeRelease(r);
    SafeRelease(old_dsv); SafeRelease(old_vs); SafeRelease(old_ps);
    for (auto *r : old_srvs) SafeRelease(r);
    for (auto *r : old_samplers) SafeRelease(r);
    SafeRelease(old_cb); SafeRelease(old_il); SafeRelease(old_bs); SafeRelease(old_ds); SafeRelease(old_rs);
    return true;
}

// ---------------------------------------------------------------------------
// Copy-back blit (verbatim from the 64-bit add-on)
// ---------------------------------------------------------------------------

// Refill the FSR constant buffer when the sizes or the sharpness it describes changed.
static void UpdateFsrConstants(ID3D11DeviceContext *ctx, UINT in_w, UINT in_h)
{
    if (in_w == g.fsr_in_w && in_h == g.fsr_in_h && g.backbuffer_width == g.fsr_out_w &&
        g.backbuffer_height == g.fsr_out_h && g_cfg.work_sharpness == g.fsr_sharpness) return;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(g.fsr_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    FsrFillConstants(static_cast<FsrConstants *>(mapped.pData), in_w, in_h,
                     g.backbuffer_width, g.backbuffer_height, g_cfg.work_sharpness);
    ctx->Unmap(g.fsr_cb, 0);
    g.fsr_in_w = in_w; g.fsr_in_h = in_h;
    g.fsr_out_w = g.backbuffer_width; g.fsr_out_h = g.backbuffer_height;
    g.fsr_sharpness = g_cfg.work_sharpness;
}

// Expands the work-size Output over the native backbuffer. work_upscale=0: one bilinear
// draw (at 100% every tap lands on a texel centre, so it is a copy). work_upscale=1: EASU
// upsamples into easu_tex and RCAS sharpens from there into the backbuffer; at 100% EASU
// has nothing to do and RCAS runs alone straight from the Output; with sharpness 0 EASU
// writes the backbuffer directly. Either way the game and ReShade never see a size change.
static void BlitOutputToBackbuffer(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv)
{
    ID3D11RenderTargetView   *old_rtv = nullptr;
    ID3D11DepthStencilView   *old_dsv = nullptr;
    ID3D11VertexShader       *old_vs  = nullptr;
    ID3D11PixelShader        *old_ps  = nullptr;
    ID3D11ShaderResourceView *old_srv = nullptr;
    ID3D11SamplerState       *old_smp = nullptr;
    ID3D11Buffer             *old_cb  = nullptr;
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
    ctx->PSGetConstantBuffers(0, 1, &old_cb);
    ctx->IAGetInputLayout(&old_il);
    ctx->IAGetPrimitiveTopology(&old_topo);
    ctx->OMGetBlendState(&old_bs, old_bf, &old_mask);
    ctx->OMGetDepthStencilState(&old_ds, &old_sref);
    ctx->RSGetState(&old_rs);
    ctx->RSGetViewports(&nvp, &old_vp);

    // The Output is work-sized under DLAA and native-sized under work_upscale=2, where DLSS
    // did the expanding and only the optional RCAS pass is left for us.
    const UINT out_w = g.output_width  != 0 ? g.output_width  : g.width;
    const UINT out_h = g.output_height != 0 ? g.output_height : g.height;
    const bool scaled = out_w != g.backbuffer_width || out_h != g.backbuffer_height;
    const bool fsr    = g_cfg.work_upscale != 0 && g.fsr_ok && g.easu_ps != nullptr;
    const bool easu   = fsr && scaled && g.easu_rtv != nullptr;
    const bool rcas   = fsr && g_cfg.work_sharpness > 0.0f && (easu || !scaled);   // RCAS reads at native texel indices

    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(g.backbuffer_width);
    vp.Height   = static_cast<float>(g.backbuffer_height);
    vp.MaxDepth = 1.0f;
    ID3D11SamplerState *smps[] = { g.blit_sampler };
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.blit_vs, nullptr, 0);
    ctx->PSSetSamplers(0, 1, smps);
    if (easu || rcas)
    {
        UpdateFsrConstants(ctx, easu ? out_w : g.backbuffer_width, easu ? out_h : g.backbuffer_height);
        ctx->PSSetConstantBuffers(0, 1, &g.fsr_cb);
    }

    ID3D11ShaderResourceView *src = g.output_srv;
    if (easu)
    {
        ID3D11RenderTargetView *target[] = { rcas ? g.easu_rtv : rtv };
        ctx->OMSetRenderTargets(1, target, nullptr);
        ctx->PSSetShader(g.easu_ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &src);
        ctx->Draw(3, 0);
        src = g.easu_srv;
    }
    if (rcas || !easu)
    {
        ID3D11ShaderResourceView *unbind = nullptr;
        ctx->PSSetShaderResources(0, 1, &unbind);      // easu_tex leaves the OM before it enters the PS
        ID3D11RenderTargetView *target[] = { rtv };
        ctx->OMSetRenderTargets(1, target, nullptr);
        ctx->PSSetShader(rcas ? g.rcas_ps : g.blit_ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &src);
        ctx->Draw(3, 0);
    }

    ID3D11ShaderResourceView *no_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &no_srv);
    ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
    ctx->VSSetShader(old_vs, nullptr, 0);
    ctx->PSSetShader(old_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &old_srv);
    ctx->PSSetSamplers(0, 1, &old_smp);
    ctx->PSSetConstantBuffers(0, 1, &old_cb);
    ctx->IASetInputLayout(old_il);
    ctx->IASetPrimitiveTopology(old_topo);
    ctx->OMSetBlendState(old_bs, old_bf, old_mask);
    ctx->OMSetDepthStencilState(old_ds, old_sref);
    ctx->RSSetState(old_rs);
    if (nvp) ctx->RSSetViewports(1, &old_vp);
    SafeRelease(old_rtv); SafeRelease(old_dsv); SafeRelease(old_vs); SafeRelease(old_ps); SafeRelease(old_srv);
    SafeRelease(old_smp); SafeRelease(old_cb); SafeRelease(old_il); SafeRelease(old_bs); SafeRelease(old_ds); SafeRelease(old_rs);
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
    Log("[feed32] 600 frames: feed CPU %.2f ms/frame | frame interval %.2f ms (%.1f fps) | feed is %.0f%% of the frame",
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
    return tex;
}

// The OpenGL sibling of FeedFrame below: same protocol, same host, raw GL instead of
// D3D11. No barriers of any kind -- every command enters the context's single in-order
// stream, and the semaphores carry the cross-process release/acquire.
static void FeedFrameGl(reshade::api::effect_runtime *rt, reshade::api::resource_view rtv)
{
    using namespace reshade::api;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    if ((g.frames_done % 60) == 0 && CfgReload()) g.built = false;
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
            Warn("DLSS5_Feed.fx textures not found. Install DLSS5_Feed.fx + a texMotionVectors provider and enable both.");
        }
        return;
    }

    const resource bb_res    = dev_api->get_resource_from_view(rtv);
    const resource mv_res    = dev_api->get_resource_from_view(mv_srv);
    const resource depth_res = dev_api->get_resource_from_view(d_srv);
    if (mv_res.handle == 0 || depth_res.handle == 0) return;
    // bb_res.handle == 0 is legal on GL and means the DEFAULT framebuffer, which the
    // blit path attaches as FBO 0 + GL_BACK; only its description is then unavailable,
    // so the sizes come from the motion vectors (backbuffer-sized by construction).

    const resource_desc md = dev_api->get_resource_desc(mv_res);
    const resource_desc dd = dev_api->get_resource_desc(depth_res);
    const bool have_bb_desc = bb_res.handle != 0;
    const resource_desc cd = have_bb_desc ? dev_api->get_resource_desc(bb_res) : md;
    const UINT w = cd.texture.width, h = cd.texture.height;
    // glCopyImageSubData needs real textures on both sides; effect textures always are,
    // but checking it here turns a wrong assumption into a log line, not a black frame.
    const bool guides_are_textures = FeedGlHandleType(mv_res.handle) == GL_TEXTURE_2D &&
                                     FeedGlHandleType(depth_res.handle) == GL_TEXTURE_2D;
    if (w != md.texture.width || h != md.texture.height || w != dd.texture.width || h != dd.texture.height ||
        cd.texture.samples != 1 || !guides_are_textures ||
        md.texture.format != format::r16g16_float || dd.texture.format != format::r32_float)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[feed32] input mismatch: color %ux%u fmt=%u samp=%u | mv %ux%u fmt=%u | depth %ux%u fmt=%u"
                " | mv/depth GL types 0x%04X/0x%04X",
                w, h, (unsigned)cd.texture.format, cd.texture.samples, md.texture.width, md.texture.height,
                (unsigned)md.texture.format, dd.texture.width, dd.texture.height, (unsigned)dd.texture.format,
                FeedGlHandleType(mv_res.handle), FeedGlHandleType(depth_res.handle));
        }
        return;
    }

    if (!g.gl.ok)
    {
        if (!FeedGlLoad(&g.gl))
        {
            Log("[feed32] OpenGL interop unavailable: %s", g.gl.missing);
            Log("[feed32] renderer=\"%s\" version=\"%s\"", g.gl.renderer, g.gl.version);
            Log("[feed32] extension query: %s", g.gl.diag);
            Log("[feed32] GL_EXT_memory_object_win32 + GL_EXT_semaphore_win32 are NVIDIA-supported on every");
            Log("[feed32] DLSS-capable driver. Their absence means this frame is not being rendered on the");
            Log("[feed32] NVIDIA GPU -- on a hybrid laptop, force the game onto it (Windows graphics settings).");
            FeedDisable("the OpenGL interop extensions are missing on the rendering GPU -- see dlss5-feed.log");
            return;
        }
        g.gl_ctx = g.gl.wglGetCurrentContext();
        Log("[feed32] OpenGL: renderer=\"%s\" version=\"%s\" context=%p thread=%lu (interop extensions present)",
            g.gl.renderer, g.gl.version, (void *)g.gl_ctx, GetCurrentThreadId());
        Log("[feed32] extension query: %s", g.gl.diag);
    }
    // GL names live in the share group of the context current at import: a context
    // change strands every one of them, so start over on the new one.
    if (g.gl.wglGetCurrentContext() != g.gl_ctx)
    {
        Log("[feed32] the GL context changed (%p -> %p); rebuilding on the new one",
            (void *)g.gl_ctx, (void *)g.gl.wglGetCurrentContext());
        HostClose();
        ReleaseShared();
        g.gl_ctx = g.gl.wglGetCurrentContext();
        g.gl_fbo_read = g.gl_fbo_draw = 0;
    }

    const DXGI_FORMAT bbf = have_bb_desc ? static_cast<DXGI_FORMAT>(cd.texture.format)
                                         : DXGI_FORMAT_R8G8B8A8_UNORM;
    bool ok = true;
    if (!g.built || w != g.width || h != g.height || bbf != g.bb_fmt)
    {
        if (GetTickCount64() < g_retry_at)
            ok = false;                       // backing off after a failed build
        else
        {
            ok = BuildSharedGl(w, h, bbf, bb_res.handle);
            if (ok) g.consecutive_fails = 0;
            // A build that is merely still with the host is not a failure: no backoff, and
            // no log line for it -- this runs every frame until the ack lands.
            else if (!g_build_pending && !g.disabled) FeedFail("shared build");
        }
    }

    if (ok && g.built)
    {
        if (!HostAlive()) { HostLost("process died"); }
        else
        {
            FeedGlStateGuard guard(&g.gl);

            // async_home: wait for the frame we sent last, not this one -- see the Vulkan
            // sibling below for why the wait moves to the top and the copy home with it.
            const bool async_home = g_cfg.async_home != 0;
            if (async_home && g.sent_n != 0)
            {
                Breadcrumb("waiting for the previous result (OpenGL)");
                const GLuint outputs[1] = { g.gl_tex[FEED_OUTPUT] };
                FeedGlWait(&g.gl, g.gl_sem_out, g.sent_n, outputs, 1);   // server-side; no CPU stall
                g.fence_wait_queued = true;
                g.wait_n            = g.sent_n;
            }

            Breadcrumb("copying inputs (OpenGL)");
            FeedGlCopy(&g.gl, FeedGlHandleName(mv_res.handle),    g.gl_tex[FEED_MV],    w, h);
            FeedGlCopy(&g.gl, FeedGlHandleName(depth_res.handle), g.gl_tex[FEED_DEPTH], w, h);
            if (!FeedGlBlit(&g.gl, g.gl_fbo_read, g.gl_fbo_draw,
                            bb_res.handle, false, g.gl_tex[FEED_COLOR], true, w, h))
            {
                static bool said = false;
                if (!said) { said = true; Log("[feed32] the colour capture blit could not be set up (incomplete framebuffer)"); }
                FeedFail("colour capture");
                QueryPerformanceCounter(&t1);
                TimingTick(t0.QuadPart, t1.QuadPart);
                return;
            }

            const UINT64 n = ++g.frame_n;
            const int reset = (g.need_reset || g_cfg.reset_every) ? 1 : 0;
            g.need_reset = false;

            // The copy home goes before the in-fence signal: that signal is the host's
            // permission to overwrite Output, so our read of it must already be in the
            // stream. (Same-frame mode blits below instead, after waiting on n.)
            const bool carried = async_home && g.out_valid;
            if (carried)
                FeedGlBlit(&g.gl, g.gl_fbo_read, g.gl_fbo_draw,
                           g.gl_tex[FEED_OUTPUT], true, bb_res.handle, false, w, h);

            {
                // async_home also releases Output across the signal, so the host's next
                // write is ordered against the blit above.
                const GLuint inputs[4] = { g.gl_tex[FEED_COLOR], g.gl_tex[FEED_DEPTH], g.gl_tex[FEED_MV],
                                           g.gl_tex[FEED_OUTPUT] };
                FeedGlSignal(&g.gl, g.gl_sem_in, n, inputs, async_home ? 4 : 3);   // includes the glFlush the host's wait needs
            }

            const FeedFrameMsg fm = { n, static_cast<uint32_t>(reset) };
            if (!PipeWriteFrame(fm))
                HostLost("frame message failed");
            else if (async_home)
            {
                g.sent_n    = n;
                g.out_valid = true;
                if (carried)
                {
                    const UINT64 done = ++g.frames_done;
                    g.consecutive_fails = 0;
                    if (done <= static_cast<UINT64>(g_cfg.log_frames) || (done % 1800) == 0)
                        Log("[feed32] frame %llu delivered (%ux%u, reset=%d, OpenGL)", done, g.width, g.height, reset);
                }
            }
            else
            {
                Breadcrumb("waiting for the host's result (OpenGL)");
                const GLuint outputs[1] = { g.gl_tex[FEED_OUTPUT] };
                FeedGlWait(&g.gl, g.gl_sem_out, n, outputs, 1);   // server-side; the host CPU-signals on failure
                g.fence_wait_queued = true;                       // HostDrain must resolve this before any close
                g.wait_n            = n;
                g.sent_n            = n;
                FeedGlBlit(&g.gl, g.gl_fbo_read, g.gl_fbo_draw,
                           g.gl_tex[FEED_OUTPUT], true, bb_res.handle, false, w, h);
                g.out_valid = true;
                const UINT64 done = ++g.frames_done;
                g.consecutive_fails = 0;
                if (done <= static_cast<UINT64>(g_cfg.log_frames) || (done % 1800) == 0)
                    Log("[feed32] frame %llu delivered (%ux%u, reset=%d, OpenGL)", done, g.width, g.height, reset);
            }

            // The cast panel (cast_mode=1), over whatever went home this frame.
            CastGlDrawPanel(bb_res.handle, static_cast<int>(w), static_cast<int>(h));

            if (g.frames_done <= static_cast<UINT64>(g_cfg.log_frames))
                if (const GLenum e = FeedGlDrainErrors(&g.gl))
                    Log("[feed32] GL error 0x%04X during frame %llu", e, g.frames_done);
        }
    }

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
}

// The Vulkan sibling of FeedFrame below: the same protocol and the same host, with
// raw vkCmd* copies recorded into ReShade's command buffer and every queue operation
// handed back to ReShade. Structurally this is the 64-bit add-on's FeedFrameVk with
// the D3D12 middle replaced by the pipe -- and with no MASK slot, which the 32-bit
// protocol has never carried.
static void FeedFrameVk(reshade::api::effect_runtime *rt, reshade::api::command_list *cl,
                        reshade::api::resource_view rtv)
{
    using namespace reshade::api;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    if ((g.frames_done % 60) == 0 && CfgReload()) g.built = false;
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
            Warn("DLSS5_Feed.fx textures not found. Install DLSS5_Feed.fx + a texMotionVectors provider and enable both.");
        }
        return;
    }

    const resource bb_res    = dev_api->get_resource_from_view(rtv);
    const resource mv_res    = dev_api->get_resource_from_view(mv_srv);
    const resource depth_res = dev_api->get_resource_from_view(d_srv);
    if (bb_res.handle == 0 || mv_res.handle == 0 || depth_res.handle == 0) return;

    const resource_desc cd = dev_api->get_resource_desc(bb_res);
    const resource_desc md = dev_api->get_resource_desc(mv_res);
    const resource_desc dd = dev_api->get_resource_desc(depth_res);
    const UINT w = cd.texture.width, h = cd.texture.height;
    if (w != md.texture.width || h != md.texture.height || w != dd.texture.width || h != dd.texture.height ||
        cd.texture.samples != 1 ||
        md.texture.format != format::r16g16_float || dd.texture.format != format::r32_float)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[feed32] input mismatch: color %ux%u fmt=%u samp=%u | mv %ux%u fmt=%u | depth %ux%u fmt=%u",
                w, h, (unsigned)cd.texture.format, cd.texture.samples, md.texture.width, md.texture.height,
                (unsigned)md.texture.format, dd.texture.width, dd.texture.height, (unsigned)dd.texture.format);
        }
        return;
    }

    // A recreated device strands every import: the images, the semaphores and the
    // entry points all belong to the old one. Drop them WITHOUT calling into it --
    // they died with it -- and start over on the new device.
    if (g.vk.ok && g.rs_dev != nullptr && g.rs_dev != dev_api)
    {
        Log("[feed32] the game recreated its Vulkan device; rebuilding on the new one");
        g.fence_wait_queued = false;
        for (int i = 0; i < FEED_SLOTS; ++i) { g.vk_img[i] = VK_NULL_HANDLE; g.vk_mem[i] = VK_NULL_HANDLE; }
        // The cast's panel image is an import of the old device too, and it has to be dropped
        // HERE: `g.vk = {}` below clears the entry points, and ReleaseShared's panel cleanup
        // is gated on g.vk.ok, so it would be skipped. Leaving them set meant the next build
        // saw a panel already imported (early return, new handle leaked) and CastVkDrawPanel
        // blitted a VkImage belonging to a destroyed device.
        g.vk_panel     = VK_NULL_HANDLE;
        g.vk_panel_mem = VK_NULL_HANDLE;
        g.vk_panel_init = false;
        g.vk = {};
        g.vk_sem_in = g.vk_sem_out = VK_NULL_HANDLE;
        g.rs_fence_in = g.rs_fence_out = {};
        g.vk_layout_init = false;
        g.vk_released    = false;
        g.rs_queue = nullptr;
        HostClose();
        ReleaseShared();
    }
    if (!EnsureVulkanLoaded(rt)) return;

    const DXGI_FORMAT bbf = static_cast<DXGI_FORMAT>(cd.texture.format);
    bool ok = true;
    if (!g.built || w != g.width || h != g.height || bbf != g.bb_fmt)
    {
        if (GetTickCount64() < g_retry_at)
            ok = false;                       // backing off after a failed build
        else
        {
            ok = BuildSharedVk(w, h, bbf);
            if (ok) g.consecutive_fails = 0;
            else if (!g_build_pending && !g.disabled) FeedFail("shared build");   // see the OpenGL path
        }
    }

    if (ok && g.built)
    {
        if (!HostAlive()) { HostLost("process died"); }
        else
        {
            VkCommandBuffer cb = FeedVkDispatch<VkCommandBuffer>(cl->get_native());
            const VkImage bb_img = FeedVkHandle<VkImage>(bb_res.handle);
            const VkImage mv_img = FeedVkHandle<VkImage>(mv_res.handle);
            const VkImage dp_img = FeedVkHandle<VkImage>(depth_res.handle);

            // The transfers to/from VK_QUEUE_FAMILY_EXTERNAL below need the graphics
            // queue family. The vkCreateDevice hook captured it; family 0 is graphics
            // on every Windows desktop driver, so that is the (loudly logged) fallback.
            uint32_t gfx_family = g_vk_gfx_family;
            if (gfx_family == VK_QUEUE_FAMILY_IGNORED)
            {
                static bool said_family = false;
                if (!said_family)
                {
                    said_family = true;
                    Log("[feed32] the vkCreateDevice hook never saw this device; assuming graphics queue family 0 for the external ownership transfers");
                }
                gfx_family = 0;
            }

            // async_home: wait for the result of the frame we sent LAST, not the one we
            // are about to send. Everything below -- the input copies AND the copy home --
            // then rides a submit that only ever waits on work the host has had a whole
            // frame to finish, so the game's present stops carrying a round trip through
            // another process. The flush first puts the effect passes before us into their
            // own submit, so the wait orders only our own recording and not theirs.
            const bool async_home = g_cfg.async_home != 0;
            if (async_home && g.sent_n != 0)
            {
                Breadcrumb("waiting for the previous result (Vulkan)");
                g.rs_queue->flush_immediate_command_list();
                g.rs_queue->wait(g.rs_fence_out, g.sent_n);
                g.fence_wait_queued = true;
                g.wait_n            = g.sent_n;
                cb = FeedVkDispatch<VkCommandBuffer>(cl->get_native());   // fresh buffer after the flush
            }

            // The shared set moves as a group, so collect it once and transition it in
            // one barrier rather than four.
            VkImage group[FEED_SLOTS];
            uint32_t group_n = 0;
            for (int i = 0; i < FEED_SLOTS; ++i)
                if (g.vk_img[i] != VK_NULL_HANDLE) group[group_n++] = g.vk_img[i];

            // Our imported images live permanently in GENERAL; only these raw barriers
            // ever move them, and only the first frame after a build starts UNDEFINED.
            // A frame that released them to VK_QUEUE_FAMILY_EXTERNAL and then bailed
            // before acquiring them back (host lost mid-frame) is picked up here.
            Breadcrumb("copying inputs (Vulkan)");
            if (!g.vk_layout_init)
            {
                FeedVkBarrierN(&g.vk, cb, group, group_n, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                g.vk_layout_init = true;
                g.vk_released    = false;   // fresh images; nothing has been handed to the host yet
            }
            else if (g.vk_released)
            {
                FeedVkExternalTransferN(&g.vk, cb, group, group_n, gfx_family, false /*acquire*/);
                g.vk_released = false;
            }
            else
            {
                FeedVkBarrierN(&g.vk, cb, group, group_n, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            }
            // The game's own images go through ReShade's barrier API so its layout
            // tracking stays correct; the copies themselves are raw.
            {
                const resource       res[3]  = { bb_res, mv_res, depth_res };
                const resource_usage from[3] = { resource_usage::render_target, resource_usage::shader_resource,
                                                 resource_usage::shader_resource };
                const resource_usage to[3]   = { resource_usage::copy_source, resource_usage::copy_source,
                                                 resource_usage::copy_source };
                cl->barrier(3, res, from, to);
            }
            FeedVkCopyImage(&g.vk, cb, bb_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.vk_img[FEED_COLOR], VK_IMAGE_LAYOUT_GENERAL, w, h);
            FeedVkCopyImage(&g.vk, cb, mv_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.vk_img[FEED_MV],    VK_IMAGE_LAYOUT_GENERAL, w, h);
            FeedVkCopyImage(&g.vk, cb, dp_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.vk_img[FEED_DEPTH], VK_IMAGE_LAYOUT_GENERAL, w, h);

            // Park the backbuffer as copy_dest to receive the result; hand mv/depth back.
            {
                const resource       res[3]  = { bb_res, mv_res, depth_res };
                const resource_usage from[3] = { resource_usage::copy_source, resource_usage::copy_source,
                                                 resource_usage::copy_source };
                const resource_usage to[3]   = { resource_usage::copy_dest, resource_usage::shader_resource,
                                                 resource_usage::shader_resource };
                cl->barrier(3, res, from, to);
            }

            const UINT64 n = ++g.frame_n;
            const int reset = (g.need_reset || g_cfg.reset_every) ? 1 : 0;
            g.need_reset = false;
            // Presents-vs-frames probe (feed_vk_hook.h): the 64-bit add-on has published its
            // frame count to it since 0.9.0; this side never did, so a DXVK user could not
            // tell an external pacer from a slow host (issue #15).
            FeedVkPresentTick(n, 120);

            // async_home: carry home what the host produced for the frame we waited on
            // above. It has to be recorded HERE -- after the input copies, because the
            // backbuffer is still the Color source until they are done, and before the
            // in-fence signal below, because that signal is the host's permission to
            // start overwriting Output for frame n. Between those two points the read is
            // exclusive, which is why one shared Output slot is enough and the 64-bit
            // transport's double-slotted home buffer is not needed here.
            const bool carried = async_home && g.out_valid;
            if (carried)
            {
                Breadcrumb("carrying the previous result home (Vulkan)");
                if (FeedFmtSameTexelLayout(g.output_fmt, g.bb_fmt))
                    FeedVkCopyImage(&g.vk, cb, g.vk_img[FEED_OUTPUT], VK_IMAGE_LAYOUT_GENERAL,
                                    bb_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w, h);
                else
                    FeedVkBlitImage(&g.vk, cb, g.vk_img[FEED_OUTPUT], VK_IMAGE_LAYOUT_GENERAL,
                                    bb_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w, h);
            }

            // Hand the images to the host's D3D12 device: release ownership to
            // VK_QUEUE_FAMILY_EXTERNAL. This is what makes this frame's input copies
            // *available* to the evaluate over there -- the in-fence below only orders
            // the work, it does not publish the bytes. Recorded before the flush so it
            // rides the same submit as the copies.
            FeedVkExternalTransferN(&g.vk, cb, group, group_n, gfx_family, true /*release*/);
            g.vk_released = true;

            Breadcrumb("signalling the host (Vulkan)");
            g.rs_queue->flush_immediate_command_list();
            g.rs_queue->signal(g.rs_fence_in, n);

            const FeedFrameMsg fm = { n, static_cast<uint32_t>(reset) };
            bool delivered = false;
            if (!PipeWriteFrame(fm))
                HostLost("frame message failed");
            else if (async_home)
            {
                // Nothing more to do this frame: the result of n is picked up at the top
                // of the next one. The images stay released to the host until then.
                g.sent_n    = n;
                g.out_valid = true;
                delivered   = carried;   // nothing was carried home on the first frame of a build
            }
            else
            {
                Breadcrumb("waiting for the host's result (Vulkan)");
                g.rs_queue->wait(g.rs_fence_out, n);   // GPU-side; the host CPU-signals on failure
                g.fence_wait_queued = true;            // HostDrain must resolve this before any close
                g.wait_n            = n;
                g.sent_n            = n;
                cb = FeedVkDispatch<VkCommandBuffer>(cl->get_native());   // fresh buffer after the flush
                // Take the images back from the host: acquire from
                // VK_QUEUE_FAMILY_EXTERNAL, making the evaluate's output writes visible
                // to the copy home. This submit waits on the out-fence, so the acquire
                // is GPU-ordered after the evaluate.
                FeedVkExternalTransferN(&g.vk, cb, group, group_n, gfx_family, false /*acquire*/);
                g.vk_released = false;
                // Prefer the raw copy: vkCmdBlitImage CONVERTS, and that conversion is
                // sRGB-aware, so blitting our linear-typed output into a VK_FORMAT_*_SRGB
                // swapchain re-encodes it and the frame comes back washed out (issue #11).
                // The frame we were handed is already encoded; the bytes must go home
                // untouched. The blit stays only for layouts a raw copy cannot express.
                if (FeedFmtSameTexelLayout(g.output_fmt, g.bb_fmt))
                    FeedVkCopyImage(&g.vk, cb, g.vk_img[FEED_OUTPUT], VK_IMAGE_LAYOUT_GENERAL,
                                    bb_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w, h);
                else
                    FeedVkBlitImage(&g.vk, cb, g.vk_img[FEED_OUTPUT], VK_IMAGE_LAYOUT_GENERAL,
                                    bb_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w, h);
                g.out_valid = true;
                delivered   = true;
            }

            // The cast panel (cast_mode=1), while the backbuffer is still copy_dest.
            CastVkDrawPanel(cl, bb_img, gfx_family, static_cast<int>(w), static_cast<int>(h));

            // The backbuffer goes back to render_target whether or not the round trip
            // happened: leaving ReShade's tracking believing it is still copy_dest would
            // break every pass after ours, including its own UI.
            {
                const resource       res[1]  = { bb_res };
                const resource_usage from[1] = { resource_usage::copy_dest };
                const resource_usage to[1]   = { resource_usage::render_target };
                cl->barrier(1, res, from, to);
            }

            if (delivered)
            {
                const UINT64 done = ++g.frames_done;
                g.consecutive_fails = 0;
                if (done <= static_cast<UINT64>(g_cfg.log_frames) || (done % 1800) == 0)
                    Log("[feed32] frame %llu delivered (%ux%u, reset=%d, Vulkan)", done, g.width, g.height, reset);
            }
        }
    }

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
}

static ID3D10Texture2D *AsTexture2D10(ID3D10Resource *res, D3D10_TEXTURE2D_DESC *desc)
{
    if (res == nullptr) return nullptr;
    ID3D10Texture2D *tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D10Texture2D), reinterpret_cast<void **>(&tex))) || tex == nullptr)
        return nullptr;
    tex->GetDesc(desc);
    return tex;
}

// The D3D10 bridges are native-sized -- the resample to the work resolution happens on
// the relay, out of the Color bridge, exactly as it happens out of the game's own texture
// on D3D11. Built after the shared set so a failure here backs off the same way.
static bool BuildD3D10Bridges(UINT native_w, UINT native_h)
{
    for (int i = 0; i < FEED_SLOTS; ++i) FeedD3D10ReleaseBridge(&g.d10_bridge[i]);

    const struct { int slot; DXGI_FORMAT fmt; bool rtv; } want[FEED_SLOTS] = {
        { FEED_COLOR,  g.color_fmt,              false },
        { FEED_OUTPUT, g.color_fmt,              true  },   // the blit chain draws into this one
        { FEED_DEPTH,  DXGI_FORMAT_R32_FLOAT,    false },
        { FEED_MV,     DXGI_FORMAT_R16G16_FLOAT, false },
    };
    for (int i = 0; i < FEED_SLOTS; ++i)
    {
        if (!FeedD3D10MakeBridge(&g.d10, &g.d10_bridge[want[i].slot], native_w, native_h, want[i].fmt, want[i].rtv))
        {
            Log("[feed32] D3D10 %s bridge failed at %s: 0x%08X",
                FeedSlotName(want[i].slot), g.d10.where != nullptr ? g.d10.where : "?", g.d10.hr);
            return false;
        }
    }

    Log("[feed32] D3D10 bridges built: %ux%u, colour %s", native_w, native_h, FeedFmtName(g.color_fmt));
    return true;
}

// The frame comes home in two hops. The relay draws the finished image into the Output
// bridge -- the same blit chain every other client uses, scaling and FSR1 included --
// and then the game's own device copies that bridge into its render target. The D3D10
// side binds nothing: no shader, no viewport, no render target. That matters more here
// than anywhere else in this file, because D3D10 has no ID3D10DeviceContext and state
// lives directly on the device, so anything we set the game would silently inherit.
static bool BlitHome10(ID3D10Resource *dst)
{
    FeedD3D10Bridge *out = &g.d10_bridge[FEED_OUTPUT];
    if (out->rtv11 == nullptr) return false;
    BlitOutputToBackbuffer(g.d10.relay_ctx, out->rtv11);
    return FeedD3D10Collect(&g.d10, out, dst);   // drains the relay before the game copies
}

// The Direct3D 10 sibling of FeedFrameDispatch's D3D11 branch. Everything from
// CopyOrResampleInputs onwards is that same client running on the private relay device;
// what is new is only the two API crossings at either end. See src/feed_d3d10.h.
static void FeedFrame10(reshade::api::effect_runtime *rt, reshade::api::resource_view rtv)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    reshade::api::device *dev_api = rt->get_device();

    if (ApplyPendingWorkResolution()) g.built = false;
    if ((g.frames_done % 60) == 0 && CfgReload()) g.built = false;
    if (!g_cfg.enabled || g_cfg.mode == 0) return;

    // One-time: stand up the relay. This is the only place the game's own device is
    // interrogated for anything but a copy, and it goes after the gate above so that
    // mode=0 does not leave a private D3D11 device sitting in the game's process.
    if (!g.d10.ok)
    {
        if (!FeedD3D10Open(&g.d10, reinterpret_cast<ID3D10Device *>(dev_api->get_native())))
        {
            char why[256];
            _snprintf_s(why, sizeof(why), _TRUNCATE, "Direct3D 10: %s failed (0x%08X)",
                        g.d10.where != nullptr ? g.d10.where : "?", g.d10.hr);
            FeedDisable(why);
            return;
        }
        g.d10_dev = g.d10.game;
        g.dev     = g.d10.relay;   // not owned: g.d10 owns it, and FeedD3D10Close ends it
        Log("[feed32] Direct3D 10.1 game: private D3D11 relay device created on adapter LUID %08lX:%08lX, "
            "feature level %d_%d",
            static_cast<unsigned long>(g.d10.luid.HighPart), static_cast<unsigned long>(g.d10.luid.LowPart),
            (g.d10.relay_fl >> 12) & 0xF, (g.d10.relay_fl >> 8) & 0xF);
        if (FAILED(g.d10.relay_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4),
                                                   reinterpret_cast<void **>(&g.ctx4))))
        { FeedDisable("ID3D11DeviceContext4 unavailable on the relay device (Windows 10 1703+ required)"); return; }
        // No ID3D11Multithread step, and no D3D10 equivalent of one: the relay is ours, it
        // is only ever driven from the render thread inside FeedEnter/FeedLeave, and the
        // game's device is free-threaded in D3D10 by definition.
    }

    ID3D11DeviceContext *ctx = g.d10.relay_ctx;

    reshade::api::resource_view mv_srv = {}, mv_srgb = {}, d_srv = {}, d_srgb = {};
    if (g.mv_var.handle != 0)    rt->get_texture_binding(g.mv_var, &mv_srv, &mv_srgb);
    if (g.depth_var.handle != 0) rt->get_texture_binding(g.depth_var, &d_srv, &d_srgb);
    if (mv_srv.handle == 0 || d_srv.handle == 0)
    {
        if (!g.missing_reported)
        {
            g.missing_reported = true;
            Warn("DLSS5_Feed.fx textures not found. Install DLSS5_Feed.fx + a texMotionVectors provider and enable both.");
        }
        return;
    }

    auto *color_res = reinterpret_cast<ID3D10Resource *>(dev_api->get_resource_from_view(rtv).handle);
    auto *mv_res    = reinterpret_cast<ID3D10Resource *>(dev_api->get_resource_from_view(mv_srv).handle);
    auto *depth_res = reinterpret_cast<ID3D10Resource *>(dev_api->get_resource_from_view(d_srv).handle);

    D3D10_TEXTURE2D_DESC cd = {}, md = {}, dd = {};
    ID3D10Texture2D *color = AsTexture2D10(color_res, &cd);
    ID3D10Texture2D *mv    = AsTexture2D10(mv_res, &md);
    ID3D10Texture2D *depth = AsTexture2D10(depth_res, &dd);
    if (color == nullptr || mv == nullptr || depth == nullptr)
    { SafeRelease(color); SafeRelease(mv); SafeRelease(depth); return; }

    bool ok = true;
    if (cd.Width != md.Width || cd.Height != md.Height || cd.Width != dd.Width || cd.Height != dd.Height ||
        cd.SampleDesc.Count != 1 || md.Format != DXGI_FORMAT_R16G16_FLOAT || dd.Format != DXGI_FORMAT_R32_FLOAT)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[feed32] input mismatch: color %ux%u fmt=%u samp=%u | mv %ux%u fmt=%u | depth %ux%u fmt=%u (D3D10)",
                cd.Width, cd.Height, cd.Format, cd.SampleDesc.Count, md.Width, md.Height, md.Format,
                dd.Width, dd.Height, dd.Format);
        }
        ok = false;
    }

    // Same rounding rule as the D3D11 path; see the note there.
    const bool sr_wanted = g_cfg.work_upscale == 2 && g_cfg.mode == 2 && g_cfg.work_resolution < 100;
    const UINT work_w = sr_wanted ? ScaledExtentUp(cd.Width,  g_cfg.work_resolution) : ScaledExtent(cd.Width,  g_cfg.work_resolution);
    const UINT work_h = sr_wanted ? ScaledExtentUp(cd.Height, g_cfg.work_resolution) : ScaledExtent(cd.Height, g_cfg.work_resolution);
    const bool size_changed = work_w != g.width || work_h != g.height ||
                              cd.Width != g.backbuffer_width || cd.Height != g.backbuffer_height;
    if (size_changed) g.sr_unavailable = false;
    const bool want_sr = sr_wanted && !g.sr_unavailable && (work_w != cd.Width || work_h != cd.Height);
    if (ok && (!g.built || size_changed || cd.Format != g.bb_fmt || want_sr != g.sr_requested))
    {
        if (GetTickCount64() < g_retry_at)
            ok = false;
        else
        {
            ok = BuildShared(work_w, work_h, cd.Width, cd.Height, cd.Format);
            // BuildShared sets g.built on its own, so a bridge failure has to clear it or
            // the next frame would sail past this block and copy out of null bridges.
            if (ok && !BuildD3D10Bridges(cd.Width, cd.Height)) { g.built = false; ok = false; }
            if (ok) g.consecutive_fails = 0;
            else if (!g_build_pending && !g.disabled) FeedFail("shared build");
        }
    }

    if (ok && g.built && g.d10_bridge[FEED_COLOR].tex11 != nullptr && g.d10_bridge[FEED_OUTPUT].rtv11 != nullptr)
    {
        if (!HostAlive()) { HostLost("process died"); }
        else
        {
            const bool async_home = g_cfg.async_home != 0;
            if (async_home && g.sent_n != 0)
            {
                Breadcrumb("waiting for the previous result");
                g.ctx4->Wait(g.fence_out, g.sent_n);
                g.fence_wait_queued = true;
                g.wait_n            = g.sent_n;
            }

            if (g.sr_active)
            {
                if (g.need_reset || g_cfg.reset_every) g.jitter_index = 0;
                HaltonJitter(g.jitter_index, g.jitter_phases, &g.jitter_x, &g.jitter_y);
            }

            // Cross into the relay's world. Queue all three copies on the game's device
            // and then pay for ONE drain: with no fence to hand the relay, an event query
            // is the only thing that can say the copies have landed, and it stops the
            // whole device -- so doing it three times would cost three pipeline bubbles
            // for one frame's worth of ordering.
            Breadcrumb("handing the frame to the relay device");
            FeedD3D10Deposit(&g.d10, &g.d10_bridge[FEED_COLOR], color_res);
            FeedD3D10Deposit(&g.d10, &g.d10_bridge[FEED_MV],    mv_res);
            FeedD3D10Deposit(&g.d10, &g.d10_bridge[FEED_DEPTH], depth_res);
            const bool got = FeedD3D10SyncGame(&g.d10);

            bool prepared = false;
            if (got)
            {
                Breadcrumb("preparing work-resolution inputs");
                prepared = CopyOrResampleInputs(ctx,
                                                g.d10_bridge[FEED_COLOR].tex11,
                                                g.d10_bridge[FEED_MV].tex11,
                                                g.d10_bridge[FEED_DEPTH].tex11,
                                                g.d10_bridge[FEED_MV].srv11,
                                                g.d10_bridge[FEED_DEPTH].srv11,
                                                cd.Width, cd.Height);
            }
            if (!got || !prepared)
            {
                FeedFail(got ? "work-resolution resample" : "D3D10 input sync");
                SafeRelease(color); SafeRelease(mv); SafeRelease(depth);
                QueryPerformanceCounter(&t1);
                TimingTick(t0.QuadPart, t1.QuadPart);
                return;
            }

            const UINT64 n = ++g.frame_n;
            const int reset = (g.need_reset || g_cfg.reset_every) ? 1 : 0;
            g.need_reset = false;

            // As on D3D11, the blit home goes before the in-fence signal: the relay's
            // context is in-order, so the host's permission to overwrite Output cannot
            // pass our read of it.
            const bool carried = async_home && g.out_valid;
            bool home_ok = true;
            if (carried) home_ok = BlitHome10(color_res);

            g.ctx4->Signal(g.fence_in, n);
            ctx->Flush();

            const FeedFrameMsg fm = { n, static_cast<uint32_t>(reset),
                                      g.sr_active ? g.jitter_x : 0.0f, g.sr_active ? g.jitter_y : 0.0f };
            const bool sent = PipeWriteFrame(fm);
            if (sent && g.sr_active) ++g.jitter_index;
            if (!sent)
                HostLost("frame message failed");
            else if (!home_ok)
                FeedFail("D3D10 blit home");
            else if (async_home)
            {
                g.sent_n    = n;
                g.out_valid = true;
                if (carried)
                {
                    const UINT64 done = ++g.frames_done;
                    g.consecutive_fails = 0;
                    if (done <= static_cast<UINT64>(g_cfg.log_frames) || (done % 1800) == 0)
                        Log("[feed32] frame %llu delivered (%ux%u, reset=%d, D3D10)", done, g.width, g.height, reset);
                }
            }
            else
            {
                Breadcrumb("waiting for the host's result");
                g.ctx4->Wait(g.fence_out, n);
                g.fence_wait_queued = true;
                g.wait_n            = n;
                g.sent_n            = n;
                if (!BlitHome10(color_res)) FeedFail("D3D10 blit home");
                else
                {
                    g.out_valid = true;
                    const UINT64 done = ++g.frames_done;
                    g.consecutive_fails = 0;
                    if (done <= static_cast<UINT64>(g_cfg.log_frames) || (done % 1800) == 0)
                        Log("[feed32] frame %llu delivered (%ux%u, reset=%d, D3D10)", done, g.width, g.height, reset);
                }
            }
        }
    }

    SafeRelease(color);
    SafeRelease(mv);
    SafeRelease(depth);

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
}

static void FeedFrameDispatch(reshade::api::effect_runtime *rt, reshade::api::command_list *cl,
                              reshade::api::resource_view rtv)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    reshade::api::device *dev_api = rt->get_device();
    if (dev_api->get_api() == reshade::api::device_api::opengl)
    { g.is_gl = true; FeedFrameGl(rt, rtv); return; }
    if (dev_api->get_api() == reshade::api::device_api::vulkan)
    { g.is_vulkan = true; FeedFrameVk(rt, cl, rtv); return; }
    // D3D10 goes through a private D3D11 relay device and never touches cl: on this API
    // ReShade's command list IS the device (there are no deferred contexts), so the cast
    // below would be an ID3D10Device wearing a D3D11 vtable. Claim the frame first.
    if (dev_api->get_api() == reshade::api::device_api::d3d10)
    { g.is_d3d10 = true; FeedFrame10(rt, rtv); return; }
    if (dev_api->get_api() != reshade::api::device_api::d3d11)
    { FeedDisable("only Direct3D 10, Direct3D 11, OpenGL and Vulkan games are supported by the 32-bit add-on"); return; }

    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cl->get_native());
    if (ctx == nullptr || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;

    if (ApplyPendingWorkResolution()) g.built = false;
    if ((g.frames_done % 60) == 0 && CfgReload()) g.built = false;
    if (!g_cfg.enabled || g_cfg.mode == 0) return;

    reshade::api::resource_view mv_srv = {}, mv_srgb = {}, d_srv = {}, d_srgb = {};
    if (g.mv_var.handle != 0)    rt->get_texture_binding(g.mv_var, &mv_srv, &mv_srgb);
    if (g.depth_var.handle != 0) rt->get_texture_binding(g.depth_var, &d_srv, &d_srgb);
    if (mv_srv.handle == 0 || d_srv.handle == 0)
    {
        if (!g.missing_reported)
        {
            g.missing_reported = true;
            Warn("DLSS5_Feed.fx textures not found. Install DLSS5_Feed.fx + a texMotionVectors provider and enable both.");
        }
        return;
    }

    auto *color_res = reinterpret_cast<ID3D11Resource *>(dev_api->get_resource_from_view(rtv).handle);
    auto *mv_res    = reinterpret_cast<ID3D11Resource *>(dev_api->get_resource_from_view(mv_srv).handle);
    auto *depth_res = reinterpret_cast<ID3D11Resource *>(dev_api->get_resource_from_view(d_srv).handle);
    auto *rtv11     = reinterpret_cast<ID3D11RenderTargetView *>(rtv.handle);

    D3D11_TEXTURE2D_DESC cd = {}, md = {}, dd = {};
    ID3D11Texture2D *color = AsTexture2D(color_res, &cd);
    ID3D11Texture2D *mv    = AsTexture2D(mv_res, &md);
    ID3D11Texture2D *depth = AsTexture2D(depth_res, &dd);
    if (color == nullptr || mv == nullptr || depth == nullptr)
    { SafeRelease(color); SafeRelease(mv); SafeRelease(depth); return; }

    bool ok = true;
    if (cd.Width != md.Width || cd.Height != md.Height || cd.Width != dd.Width || cd.Height != dd.Height ||
        cd.SampleDesc.Count != 1 || md.Format != DXGI_FORMAT_R16G16_FLOAT || dd.Format != DXGI_FORMAT_R32_FLOAT)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[feed32] input mismatch: color %ux%u fmt=%u samp=%u | mv %ux%u fmt=%u | depth %ux%u fmt=%u",
                cd.Width, cd.Height, cd.Format, cd.SampleDesc.Count, md.Width, md.Height, md.Format,
                dd.Width, dd.Height, dd.Format);
        }
        ok = false;
    }

    if (ok && g.dev == nullptr)
    {
        ctx->GetDevice(&g.dev);
        if (g.dev != nullptr) g.dev->Release();   // not owned; the game outlives us
        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&g.ctx4))))
        { FeedDisable("ID3D11DeviceContext4 unavailable (Windows 10 1703+ required)"); ok = false; }
        // The immediate context is not thread-safe unless asked, and BlitOutputToBackbuffer
        // save/restores a slice of device state around its own draw -- which a second
        // present thread would tear. A game that already had protection on is left alone.
        if (ok && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void **>(&g.mt))) &&
            g.mt != nullptr)
        {
            g.mt_was_on = g.mt->SetMultithreadProtected(TRUE) != FALSE;
            Log("[feed32] D3D11 multithread protection enabled (the game had it %s)", g.mt_was_on ? "on" : "off");
        }
    }

    // DLSS's dynamic render range starts at ceil(50%) of the output; the cost knob rounds
    // DOWN to even, which at exactly 50% lands one pixel short (1788x1006 vs 1789x1007 on
    // Fable) and no preset covers it. Under work_upscale=2 round UP to even instead.
    // The rounding does not depend on the host's answer: a DLAA fallback at the rounded-up
    // size is harmless, and rounding down again after "unavailable" would change the size,
    // clear the latch, and rebuild forever.
    const bool sr_wanted = g_cfg.work_upscale == 2 && g_cfg.mode == 2 && g_cfg.work_resolution < 100;
    const UINT work_w = sr_wanted ? ScaledExtentUp(cd.Width,  g_cfg.work_resolution) : ScaledExtent(cd.Width,  g_cfg.work_resolution);
    const UINT work_h = sr_wanted ? ScaledExtentUp(cd.Height, g_cfg.work_resolution) : ScaledExtent(cd.Height, g_cfg.work_resolution);
    const bool size_changed = work_w != g.width || work_h != g.height ||
                              cd.Width != g.backbuffer_width || cd.Height != g.backbuffer_height;
    if (size_changed) g.sr_unavailable = false;   // a new ratio deserves a new answer from the host
    const bool want_sr = sr_wanted && !g.sr_unavailable && (work_w != cd.Width || work_h != cd.Height);
    if (ok && (!g.built || size_changed || cd.Format != g.bb_fmt || want_sr != g.sr_requested))
    {
        if (GetTickCount64() < g_retry_at)
            ok = false;                       // backing off after a failed build
        else
        {
            ok = BuildShared(work_w, work_h, cd.Width, cd.Height, cd.Format);
            if (ok) g.consecutive_fails = 0;
            else if (!g_build_pending && !g.disabled) FeedFail("shared build");   // see the OpenGL path
        }
    }

    if (ok && g.built)
    {
        if (!HostAlive()) { HostLost("process died"); }
        else
        {
            // async_home: wait for the frame we sent last, not this one. The wait goes
            // ahead of the input copies as well -- the host may still be READING Color,
            // Depth and MV for that evaluate, and this is the only thing ordering our
            // overwrite of them against it.
            const bool async_home = g_cfg.async_home != 0;
            if (async_home && g.sent_n != 0)
            {
                Breadcrumb("waiting for the previous result");
                g.ctx4->Wait(g.fence_out, g.sent_n);   // GPU-side; the host CPU-signals on failure
                g.fence_wait_queued = true;
                g.wait_n            = g.sent_n;
            }

            // work_upscale=2: this frame's grid shift. The sequence restarts with the DLSS
            // history so a reset frame is the unshifted one.
            if (g.sr_active)
            {
                if (g.need_reset || g_cfg.reset_every) g.jitter_index = 0;
                HaltonJitter(g.jitter_index, g.jitter_phases, &g.jitter_x, &g.jitter_y);
            }
            Breadcrumb("preparing work-resolution inputs");
            if (!CopyOrResampleInputs(ctx, color, mv, depth,
                                      reinterpret_cast<ID3D11ShaderResourceView *>(mv_srv.handle),
                                      reinterpret_cast<ID3D11ShaderResourceView *>(d_srv.handle),
                                      cd.Width, cd.Height))
            {
                // Cannot prepare the guides (only reachable below 100%): count it as a
                // failure so the usual backoff applies, and leave the frame untouched.
                FeedFail("work-resolution resample");
                SafeRelease(color); SafeRelease(mv); SafeRelease(depth);
                QueryPerformanceCounter(&t1);
                TimingTick(t0.QuadPart, t1.QuadPart);
                return;
            }

            const UINT64 n = ++g.frame_n;
            const int reset = (g.need_reset || g_cfg.reset_every) ? 1 : 0;
            g.need_reset = false;

            // The blit home goes before the in-fence signal: the immediate context is
            // in-order, so the signal -- the host's permission to overwrite Output --
            // cannot pass our read of it.
            const bool carried = async_home && g.out_valid;
            if (carried) BlitOutputToBackbuffer(ctx, rtv11);

            g.ctx4->Signal(g.fence_in, n);
            ctx->Flush();

            // Jitter sign: the shift was applied to the sampling grid, so a sample sits at
            // pixel centre + jitter -- the convention the SDK's jitter offset describes.
            const FeedFrameMsg fm = { n, static_cast<uint32_t>(reset),
                                      g.sr_active ? g.jitter_x : 0.0f, g.sr_active ? g.jitter_y : 0.0f };
            const bool sent = PipeWriteFrame(fm);
            if (sent && g.sr_active) ++g.jitter_index;
            if (!sent)
                HostLost("frame message failed");
            else if (async_home)
            {
                g.sent_n    = n;
                g.out_valid = true;
                if (carried)
                {
                    const UINT64 done = ++g.frames_done;
                    g.consecutive_fails = 0;
                    if (done <= static_cast<UINT64>(g_cfg.log_frames) || (done % 1800) == 0)
                        Log("[feed32] frame %llu delivered (%ux%u, reset=%d)", done, g.width, g.height, reset);
                }
            }
            else
            {
                Breadcrumb("waiting for the host's result");
                g.ctx4->Wait(g.fence_out, n);       // GPU-side; the host CPU-signals on failure
                g.fence_wait_queued = true;         // HostDrain must resolve this before any close
                g.wait_n            = n;
                g.sent_n            = n;
                BlitOutputToBackbuffer(ctx, rtv11);
                g.out_valid = true;
                const UINT64 done = ++g.frames_done;
                g.consecutive_fails = 0;
                if (done <= static_cast<UINT64>(g_cfg.log_frames) || (done % 1800) == 0)
                    Log("[feed32] frame %llu delivered (%ux%u, reset=%d)", done, g.width, g.height, reset);
            }
        }
    }

    SafeRelease(color);
    SafeRelease(mv);
    SafeRelease(depth);

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
}

// The one place the feed is serialized: every backend above reaches the shared
// textures and the host protocol through here, and all of it assumes a single
// caller per frame. See the Smooth Motion note next to FeedEnter.
static void FeedFrame(reshade::api::effect_runtime *rt, reshade::api::command_list *cl, reshade::api::resource_view rtv)
{
    // Overlay host requests are drained in OnPresent, which runs whether or not this does.
    if (!g_cfg.enabled || g.disabled || g_cfg.mode == 0) return;

    if (!FeedEnter()) return;   // logs the dropped call, with its thread id
    FeedThreadTrace();
    FeedFrameDispatch(rt, cl, rtv);
    FeedLeave();
}

// ---------------------------------------------------------------------------
// ReShade events
// ---------------------------------------------------------------------------

static void ResolveHandles(reshade::api::effect_runtime *rt)
{
    g.technique = rt->find_technique(kEffectFile, kTechnique);
    g.mv_var    = rt->find_texture_variable(kEffectFile, "DLSS5_MV");
    g.depth_var = rt->find_texture_variable(kEffectFile, "DLSS5_Depth");
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
            if (g.launchpad.handle == 0 || on) { g.launchpad = t; provider = p.tech; provider_file = p.file; }
        }
        else if (on && other.handle == 0) { other = t; other_tech = p.tech; other_mode = p.mode; }
    }
    char compile_error[512] = {};
    const bool provider_broken = provider_file != nullptr && ProviderCompileError(provider_file, compile_error, sizeof(compile_error));

    if (!g_host_nr_loaded)
    {
        ReadHostNR();
        g_host_nr_loaded = true;
        LogHostNR("host DLSS 5 settings loaded into the overlay page");
    }

    char v[16] = {};
    g.depth_reversed = true;
    // Per-effect scope first, like the provider lookup: see the 64-bit add-on's note.
    if (rt->get_preprocessor_definition_for_effect(kEffectFile, "RESHADE_DEPTH_INPUT_IS_REVERSED", v) ||
        rt->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", v))
        g.depth_reversed = atoi(v) != 0;

    g.handles_ok = g.technique.handle != 0 && g.mv_var.handle != 0 && g.depth_var.handle != 0;
    g.missing_reported = false;

    const bool provider_on = g.launchpad.handle && rt->get_technique_state(g.launchpad);
    const int signature = (g.technique.handle ? 1 : 0) | (g.mv_var.handle ? 2 : 0) | (g.depth_var.handle ? 4 : 0) |
                          (g.launchpad.handle ? 8 : 0) | (g.depth_reversed ? 16 : 0) | (provider_on ? 32 : 0) |
                          (mode << 6) | (other.handle ? 512 : 0) | ((other_mode & 7) << 10) | (provider_broken ? 8192 : 0);
    static int last_signature = -1;
    if (signature == last_signature) return;
    last_signature = signature;

    _snprintf_s(g_mv_status, sizeof(g_mv_status), _TRUNCATE, "DLSS5_MV_PROVIDER=%d (%s) -> %s (%s)",
                mode, kMvModeName[mode], provider,
                g.launchpad.handle ? (provider_broken ? "FAILED TO COMPILE" : provider_on ? "enabled" : "DISABLED") : "not installed");
    g_mv_problem[0] = '\0';
    Log("[feed32] effects: technique %s, DLSS5_MV %s, DLSS5_Depth %s, %s, depth reversed=%d",
        g.technique.handle ? "found" : "MISSING", g.mv_var.handle ? "found" : "MISSING",
        g.depth_var.handle ? "found" : "MISSING", g_mv_status, g.depth_reversed ? 1 : 0);
    if (g.handles_ok && g.launchpad.handle == 0)
        _snprintf_s(g_mv_problem, sizeof(g_mv_problem), _TRUNCATE,
                    "DLSS5_Feed.fx is compiled for motion-vector provider %d (%s) but no known %s shader is installed: motion vectors will be zero. "
                    "Install one, or change the DLSS5_MV_PROVIDER preprocessor definition.", mode, kMvModeName[mode], kMvModeName[mode]);
    else if (g.handles_ok && provider_broken)
        _snprintf_s(g_mv_problem, sizeof(g_mv_problem), _TRUNCATE,
                    "motion-vector provider %s FAILED TO COMPILE, so it writes nothing and DLSS runs on zero vectors. ReShade.log: %s -- use another provider (VORT: DLSS5_MV_PROVIDER=2).",
                    provider, compile_error);
    else if (g.handles_ok && !provider_on)
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

// ---------------------------------------------------------------------------
// Which effect runtime is ours -- the 32-bit half of issue #1, which the 64-bit add-on
// fixed in 0.11.0-beta.2 and this one never got.
//
// A process can hold several ReShade effect runtimes. NVIDIA Smooth Motion is the common
// cause: its present interposer creates its OWN device and an invisible proxy swapchain
// ("InvisibleWindowClassNvPresent"), so ReShade builds a runtime on each and gives them
// separate configs -- the first gets ReShade.ini (the user's preset), the second
// ReShade2.ini. The old rule here was "the last runtime to initialise is ours", which
// bound to whichever came second, resolved DLSS5_Feed.fx as MISSING there, and then
// silently ignored every render of the technique on the other one: a healthy-looking log
// and no neural rendering at all. The rule now matches the 64-bit add-on's -- the runtime
// that RENDERS DLSS5_Feed is ours.
// ---------------------------------------------------------------------------
struct RuntimeSlot
{
    reshade::api::effect_runtime  *rt;
    reshade::api::effect_technique technique;    // this runtime's DLSS5_Feed, or 0
    void                          *dev;          // native device, for the log
    char                           wclass[48];   // window class of the swapchain's HWND
    bool                           proxy;        // Smooth Motion's invisible proxy swapchain
    ULONGLONG                      last_resolve; // GetTickCount64 of the last find_technique from the render path
};
static RuntimeSlot g_runtimes[6];
static int         g_runtime_count;
static ULONGLONG   g_bound_last_render;   // GetTickCount64 of the bound runtime's last DLSS5_Feed render

static RuntimeSlot *FindRuntime(reshade::api::effect_runtime *rt)
{
    for (int i = 0; i < g_runtime_count; ++i)
        if (g_runtimes[i].rt == rt) return &g_runtimes[i];
    return nullptr;
}

static RuntimeSlot *TrackRuntime(reshade::api::effect_runtime *rt)
{
    RuntimeSlot *s = FindRuntime(rt);
    if (s == nullptr)
    {
        if (g_runtime_count == static_cast<int>(sizeof(g_runtimes) / sizeof(g_runtimes[0])))
            --g_runtime_count;   // overflow: recycle the last slot rather than lose track
        s = &g_runtimes[g_runtime_count++];
        *s = {};
        s->rt = rt;
        reshade::api::device *dev = rt->get_device();
        s->dev = dev != nullptr ? reinterpret_cast<void *>(dev->get_native()) : nullptr;
        HWND hwnd = static_cast<HWND>(rt->get_hwnd());
        if (hwnd != nullptr && !GetClassNameA(hwnd, s->wclass, sizeof(s->wclass))) s->wclass[0] = '\0';
        if (hwnd == nullptr) strcpy_s(s->wclass, "(no window)");
        s->proxy = strstr(s->wclass, "NvPresent") != nullptr;
    }
    s->technique = rt->find_technique(kEffectFile, kTechnique);
    return s;
}

static void UntrackRuntime(reshade::api::effect_runtime *rt)
{
    for (int i = 0; i < g_runtime_count; ++i)
        if (g_runtimes[i].rt == rt)
        {
            g_runtimes[i] = g_runtimes[--g_runtime_count];
            g_runtimes[g_runtime_count] = {};
            return;
        }
}

// This side captures g.dev from the first frame's immediate context and never re-checks it,
// so adopting a runtime on a DIFFERENT device once the shared set is built would feed the
// wrong device's textures. In the case this fix is for, that cannot happen: the runtime
// rendering the technique is the game's, and it is adopted before anything is built. Refuse
// the other case rather than pretend it works.
static bool RuntimeDeviceCompatible(const RuntimeSlot *slot)
{
    if (slot == nullptr || slot->dev == nullptr) return true;
    // On D3D10 g.dev is the private relay, which no runtime will ever report as its own.
    // The device that has to match is the game's -- the one the bridges were made on.
    if (g.is_d3d10) return g.d10_dev == nullptr || slot->dev == static_cast<void *>(g.d10_dev);
    return g.dev == nullptr || reinterpret_cast<ID3D11Device *>(slot->dev) == g.dev;
}

// enabled=0 means enabled=0. Everything below this line queries the runtime, reads files,
// loads dbghelp, scans modules or draws -- none of which a user who set enabled=0 to take
// this add-on out of the picture expects to still be happening (issue #44, and README's
// "0 disables everything"). Only the overlay page stays, so the checkbox can undo it.
static void OnInitEffectRuntime(reshade::api::effect_runtime *rt)
{
    if (!g_cfg.enabled) return;
    RuntimeSlot *slot = TrackRuntime(rt);
    DetectSmoothMotion();   // a present interposer can arrive after this add-on did
    // Not in DllMain (LoadLibrary under the loader lock) and not in the exception filter
    // (ReShade refuses a LoadLibrary from there): this is what makes a dump possible.
    FeedResolveDbghelp();
    static int inits = 0;
    if (++inits <= 8)
        Log("[feed32] effect runtime %p initialised (device %p, window class '%s'%s; %d runtime%s in this process)",
            (void *)rt, slot->dev, slot->wclass,
            slot->proxy ? " -- NVIDIA Smooth Motion's proxy swapchain" : "",
            g_runtime_count, g_runtime_count == 1 ? "" : "s");
    else if (inits == 9)
        Log("[feed32] (further runtime init/destroy messages suppressed)");

    // Bind: the first runtime, or a re-init of the bound one. Another runtime only takes
    // over when the bound one has no DLSS5_Feed and this one does; otherwise it is tracked,
    // and OnRenderTechnique adopts it the moment it renders the technique.
    if (g.runtime == nullptr || rt == g.runtime || (g.technique.handle == 0 && slot->technique.handle != 0))
    {
        g.runtime = rt;
        ResolveHandles(rt);
    }
}

static void OnDestroyEffectRuntime(reshade::api::effect_runtime *rt)
{
    // Logged before the bound test: with several runtimes churning, a log that only ever
    // reported the bound one could never show the topology changing underneath.
    const bool was_bound = rt == g.runtime;
    UntrackRuntime(rt);
    static int destroys = 0;
    if (++destroys <= 8)
        Log("[feed32] effect runtime %p destroyed%s (%d runtime%s left)", (void *)rt,
            was_bound ? " -- it was the bound one" : "", g_runtime_count, g_runtime_count == 1 ? "" : "s");
    else if (destroys == 9)
        Log("[feed32] (further runtime init/destroy messages suppressed)");
    if (!was_bound) return;
    CastRelease();   // the thumbnail is registered on this runtime's window
    // The shared textures live on the game's device and survive runtime churn; keep them.
    g.runtime = nullptr;
    g.technique = {}; g.launchpad = {}; g.mv_var = {}; g.depth_var = {};
    g.handles_ok = false;
}

static void OnReloadedEffects(reshade::api::effect_runtime *rt)
{
    if (!g_cfg.enabled) return;
    RuntimeSlot *slot = TrackRuntime(rt);
    if (rt == g.runtime || g.runtime == nullptr || (g.technique.handle == 0 && slot->technique.handle != 0))
    {
        if (g.runtime != nullptr && rt != g.runtime)
            Log("[feed32] effect runtime %p takes over from %p: its reload produced DLSS5_Feed.fx, the bound one has none",
                (void *)rt, (void *)g.runtime);
        g.runtime = rt;
        ResolveHandles(rt);
        // A reload recompiles the MV provider, which writes zero vectors until its own
        // history re-fills -- discard the DLSS history built on those frames.
        g.need_reset = true;
    }
}

static void OnRenderTechnique(reshade::api::effect_runtime *rt, reshade::api::effect_technique technique,
                              reshade::api::command_list *cl, reshade::api::resource_view rtv,
                              reshade::api::resource_view /*rtv_srgb*/)
{
    if (!g_cfg.enabled) return;
    if (rt != g.runtime)
    {
        // Another runtime is rendering DLSS5_Feed. Adopt it -- unless the bound one rendered
        // the technique within the last second, in which case both are drawing it and
        // flip-flopping every frame would rebuild the session each time.
        RuntimeSlot *slot = FindRuntime(rt);
        // A slot's technique handle is only written by TrackRuntime (init and reload). One
        // that changes without either reaching us leaves the slot stale for the rest of the
        // session, and the feed stops with nothing logged until a swapchain re-init. Cheap
        // to re-resolve, throttled to once a second per runtime.
        if (slot != nullptr && technique.handle != slot->technique.handle)
        {
            const ULONGLONG t = GetTickCount64();
            if (t - slot->last_resolve >= 1000)
            {
                slot->last_resolve = t;
                const reshade::api::effect_technique fresh = rt->find_technique(kEffectFile, kTechnique);
                if (fresh.handle != slot->technique.handle)
                {
                    Log("[feed32] effect runtime %p: DLSS5_Feed handle changed under us (%llu -> %llu); re-resolved",
                        (void *)rt, (unsigned long long)slot->technique.handle, (unsigned long long)fresh.handle);
                    slot->technique = fresh;
                }
            }
        }
        // Still not this runtime's DLSS5_Feed: it is one of the other techniques in the
        // preset, which arrive here constantly and are nothing to report.
        if (slot == nullptr || slot->technique.handle == 0 || technique.handle != slot->technique.handle) return;
        const ULONGLONG now = GetTickCount64();
        if (g.technique.handle != 0 && now - g_bound_last_render < 1000)
        {
            static int both = 0;
            if (++both <= 3)
                Log("[feed32] effect runtime %p (window class '%s') also renders DLSS5_Feed; feeding %p only%s",
                    (void *)rt, slot->wclass, (void *)g.runtime, both == 3 ? " (further notices suppressed)" : "");
            return;
        }
        if (!RuntimeDeviceCompatible(slot))
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                Log("[feed32] effect runtime %p renders DLSS5_Feed but on device %p, and the shared set is already "
                    "built on %p; staying on %p. Restart the game if the wrong window is being processed.",
                    (void *)rt, slot->dev, (void *)g.dev, (void *)g.runtime);
            }
            return;
        }
        Log("[feed32] binding to effect runtime %p (device %p, window class '%s'): it is the one rendering DLSS5_Feed; %p was bound",
            (void *)rt, slot->dev, slot->wclass, (void *)g.runtime);
        g.runtime = rt;
        ResolveHandles(rt);
        g.need_reset = true;
    }
    if (g.technique.handle == 0 || technique.handle != g.technique.handle)
    {
        // Nearly every render arriving here is one of the OTHER techniques in the user's
        // preset -- ordinary, and silent. The case worth catching is the same stale-handle
        // hole one level up: g.technique no longer matching the bound runtime's DLSS5_Feed
        // drops every one of ITS renders forever, with nothing in the frame path to
        // re-resolve it. Ask ReShade at most once a second, and only speak when the fresh
        // handle proves this render was ours after all.
        const ULONGLONG t = GetTickCount64();
        static ULONGLONG last_bound_resolve = 0;
        if (rt == g.runtime && t - last_bound_resolve >= 1000)
        {
            last_bound_resolve = t;
            const reshade::api::effect_technique fresh = rt->find_technique(kEffectFile, kTechnique);
            if (fresh.handle == technique.handle && fresh.handle != g.technique.handle)
            {
                Log("[feed32] bound runtime %p: DLSS5_Feed handle changed under us (%llu -> %llu); re-resolving",
                    (void *)rt, (unsigned long long)g.technique.handle, (unsigned long long)fresh.handle);
                ResolveHandles(rt);
            }
        }
        if (g.technique.handle == 0 || technique.handle != g.technique.handle) return;
    }
    g_bound_last_render = GetTickCount64();
    FeedFrame(rt, cl, rtv);
}

static void OnDestroyDevice(reshade::api::device *dev)
{
    const bool ours = (g.dev != nullptr && reinterpret_cast<ID3D11Device *>(dev->get_native()) == g.dev) ||
                      (g.is_gl && dev->get_api() == reshade::api::device_api::opengl) ||
                      (g.is_vulkan && dev == g.rs_dev) ||
                      // g.dev is the RELAY on D3D10, so it never matches the game's device
                      (g.is_d3d10 && dev->get_api() == reshade::api::device_api::d3d10);
    if (!ours) return;

    Log("[feed32] game device destroyed; shutting down");
    HostClose();     // drain + end the host while the fences are still alive
    ReleaseShared();
    g.vk = {};
    g.rs_dev = nullptr;
    g.rs_queue = nullptr;
    g.vk_layout_init = false;
    g.vk_released    = false;
    if (g.gl.ok && g.gl.wglGetCurrentContext() == g.gl_ctx && g.gl_ctx != nullptr)
    {
        if (g.gl_fbo_read != 0) { g.gl.DeleteFramebuffers(1, &g.gl_fbo_read); g.gl_fbo_read = 0; }
        if (g.gl_fbo_draw != 0) { g.gl.DeleteFramebuffers(1, &g.gl_fbo_draw); g.gl_fbo_draw = 0; }
    }
    g.gl_fbo_read = g.gl_fbo_draw = 0;
    g.gl = {};
    g.gl_ctx = nullptr;
    if (g.mt != nullptr)
    {
        if (!g.mt_was_on) g.mt->SetMultithreadProtected(FALSE);
        SafeRelease(g.mt);
        g.mt_was_on = false;
    }
    SafeRelease(g.ctx4);
    SafeRelease(g.blit_vs);
    SafeRelease(g.blit_ps);
    SafeRelease(g.resample_ps);
    SafeRelease(g.blit_sampler);
    SafeRelease(g.point_sampler);
    SafeRelease(g.resample_cb);
    SafeRelease(g.easu_ps);
    SafeRelease(g.rcas_ps);
    SafeRelease(g.fsr_cb);
    g.fsr_ok = false;
    // Last, because everything released above -- the blit shaders, the constant buffers,
    // ctx4 -- was created on the relay and must not outlive it.
    FeedD3D10Close(&g.d10);
    g.d10_dev  = nullptr;
    g.is_d3d10 = false;
    g.dev = nullptr;
}

// ---------------------------------------------------------------------------
// ReShade overlay page (Add-ons tab -> DLSS 5 Feed): the local dlss5-feed.cfg, and --
// unlike the 64-bit add-on -- the DLSS 5 host's own neural-rendering settings, which
// on this path live in a separate process's ReShade.ini. Replaces the old approach of
// bridging them through hidden shader uniforms in DLSS5_Feed.fx.
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
    if (ImGui::Checkbox("Enabled", &enabled))
    {
        g_cfg.enabled = enabled ? 1 : 0;
        dirty = true;
        // Turning it back on has to re-adopt the runtime by hand. Every other adoption path
        // is an event that has already fired for this runtime and will not fire again --
        // init, an effect reload, a resolution change -- so without this the add-on would
        // sit inert until one of those happened to come round.
        if (enabled && rt != nullptr) { Log("[feed32] enabled from the overlay; re-adopting the effect runtime"); OnInitEffectRuntime(rt); }
        else if (!enabled) Log("[feed32] disabled from the overlay: no frames are fed and nothing is queried. "
                               "An already-installed Vulkan interop hook stays until the game exits.");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Status");
    ImGui::Text("Feed: %s", g.disabled ? "disabled" : g.built ? "built" : "not built");
    if (g.disabled && g_disable_why[0])
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f), "Stopped: %s", g_disable_why);
    ImGui::Text("Render API: %s", g.is_vulkan ? "Vulkan" : g.is_gl ? "OpenGL" :
                                  g.is_d3d10 ? "Direct3D 10.1 (through a private Direct3D 11 relay device)" :
                                  "Direct3D 11");
    ImGui::Text("Handoff: %s", g_cfg.async_home ? "pipelined (+1 frame)" : "same frame");
    ImGui::Text("Host process: %s", HostAlive() ? "running" : "not running");
    if (g_chicken_host)
        ImGui::Text("Neural consumer: Deep Fried Chicken %s (in host64\\)", g_chicken_host_ver);
    if (g.frames_done > 0) ImGui::Text("Frames delivered: %llu", static_cast<unsigned long long>(g.frames_done));
    ImGui::TextWrapped("Motion vectors: %s", g_mv_status);
    if (g_mv_problem[0])
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f), "%s", g_mv_problem);
    if (g_smooth_motion)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "NVIDIA Smooth Motion is active -- unverified with this add-on. If the image "
                           "corrupts or flickers, disable it for this game's API only: Profile Inspector, "
                           "\"Smooth Motion - Enabled APIs\" (1=DX12, 2=DX11, 4=Vulkan).");
    if (g.disabled && ImGui::Button("Re-enable"))
    {
        g.disabled = false;
        g.consecutive_fails = 0;
        g_retry_at = 0;
        g_disable_why[0] = '\0';
        // HostClose() keeps g.built on the D3D11 path (the game owns those textures, so a
        // replacement host can re-open them). But if the host is what died, "built" with no
        // host means the frame path skips the rebuild and then trips over the dead host on
        // the very next frame -- disabled again, and only "Restart the host" ever recovered.
        // A build is cheap here; the host it needs gets spawned by EnsureHost as usual.
        if (!HostAlive() && g.built)
        {
            g.built = false;
            Log("[feed32] re-enable: the host is gone, so the shared set is rebuilt from scratch");
        }
        Log("[feed32] re-enabled from the overlay");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("DLSS contract");
    static const char *kModes[] = { "Inert", "Transport test (no NGX, left half only)", "Full DLSS path" };
    if (ImGui::Combo("Mode", &g_cfg.mode, kModes, 3)) dirty = true;

    // Below 100% the guides have to be resampled into the shared set, which is a
    // D3D11 pixel-shader pass this add-on only has on the D3D11 path. The OpenGL and
    // Vulkan transports run DLAA at the native size, so the slider would lie.
    if (g.is_gl || g.is_vulkan)
    {
        ImGui::BeginDisabled();
        int fixed = 100;
        ImGui::SliderInt("Work resolution (%)", &fixed, 50, 100);
        ImGui::EndDisabled();
        ImGui::SameLine(); HelpMarker("Fixed at 100% on the OpenGL and Vulkan transports: DLSS runs at the "
                                      "game's native resolution there.");
    }
    else
    {
        if (g_pending_work_resolution == 0 && g_work_resolution_ui != g_cfg.work_resolution)
            g_work_resolution_ui = g_cfg.work_resolution;
        if (ImGui::SliderInt("Work resolution (%)", &g_work_resolution_ui, 50, 100))
        {
            g_pending_work_resolution = g_work_resolution_ui;
            g_work_resolution_apply_after = GetTickCount64() + 400;
        }
        ImGui::SameLine(); HelpMarker("Scales both axes of the shared DLAA + Neural Rendering work textures the host "
                                      "runs on. The game and its backbuffer stay native-sized. Applied once 400 ms "
                                      "after dragging stops, since each change rebuilds the shared set.");
        if (g_pending_work_resolution != 0)
            ImGui::TextDisabled("Pending: %d%%", g_pending_work_resolution);
        else if (g.backbuffer_width != 0)
            ImGui::TextDisabled("Active: %ux%u (%d%%) -> %ux%u", g.width, g.height,
                                g_cfg.work_resolution, g.backbuffer_width, g.backbuffer_height);

        const bool fsr_available = g.blit_vs == nullptr || g.fsr_ok;   // unknown until the shaders compile
        if (!fsr_available) ImGui::BeginDisabled();
        // work_upscale=2 (DLSS reconstruction on synthetic jitter) is deliberately NOT on the
        // overlay: measured on Fable Anniversary it costs as much as 100% -- DLSS SR scales
        // with the output size and the neural consumer runs on the resolved native output --
        // and the image shimmers. It stays reachable from the cfg as the experiment it is.
        bool fsr = g_cfg.work_upscale != 0;
        if (ImGui::Checkbox("FSR 1 expand-back (EASU + RCAS)", &fsr)) { g_cfg.work_upscale = fsr ? 1 : 0; dirty = true; }
        ImGui::SameLine(); HelpMarker("Replaces the bilinear stretch of the work-size output with AMD FSR 1 spatial "
                                      "upscaling and RCAS sharpening: much crisper than the stretch at 50-75%. A better "
                                      "filter for the cost knob above, not DLSS Quality: the result can never exceed "
                                      "the native frame. At 100% only the sharpening runs.");
        if (fsr)
        {
            ImGui::SliderFloat("Sharpness", &g_cfg.work_sharpness, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) dirty = true;
        }
        if (!fsr_available)
        {
            ImGui::EndDisabled();
            ImGui::TextDisabled("FSR 1 shaders failed to compile (see the log); the spatial expand-back stays bilinear.");
        }
        if (g_cfg.work_upscale == 2 && g.backbuffer_width != 0)
            ImGui::TextDisabled("work_upscale=2 (cfg only): %s", g.sr_active ? "DLSS reconstruction active -- costs as much as 100%" :
                                g.sr_unavailable ? "no DLSS preset covers this ratio; DLAA + FSR 1" : "DLAA + FSR 1");
    }

    static const char *kTri[] = { "Auto", "Force off", "Force on" };
    int hdr_idx = g_cfg.hdr + 1, di_idx = g_cfg.depth_inverted + 1;
    if (ImGui::Combo("HDR", &hdr_idx, kTri, 3)) { g_cfg.hdr = hdr_idx - 1; dirty = true; }
    if (ImGui::Combo("Depth inverted", &di_idx, kTri, 3)) { g_cfg.depth_inverted = di_idx - 1; dirty = true; }
    bool reset_every = g_cfg.reset_every != 0;
    bool async_home = g_cfg.async_home != 0;
    if (ImGui::Checkbox("Pipelined handoff", &async_home)) { g_cfg.async_home = async_home ? 1 : 0; dirty = true; }
    ImGui::SameLine(); HelpMarker("Shows the frame DLSS finished LAST, instead of making the game wait for the "
                                  "one it is presenting now. The helper process then works alongside the game "
                                  "instead of inside its frame, which is what lifts the frame-rate ceiling. "
                                  "Costs one frame of latency on the DLSS output; the temporal history hides it. "
                                  "Turn it off to get the original same-frame behaviour back.");
    if (ImGui::Checkbox("Reset every frame (diagnostic)", &reset_every)) { g_cfg.reset_every = reset_every ? 1 : 0; dirty = true; }
    if (ImGui::SliderFloat("MV scale X", &g_cfg.mv_scale_x, 0.0f, 4.0f)) dirty = true;
    if (ImGui::SliderFloat("MV scale Y", &g_cfg.mv_scale_y, 0.0f, 4.0f)) dirty = true;

    ImGui::Separator();
    ImGui::TextUnformatted("DLSS 5 panel in-game");
    ImGui::SameLine();
    HelpMarker("Shows the helper process's own tuning panel (the neural consumer's tab column) inside this "
               "game window, at the top right. While it is shown the mouse and keyboard belong to the panel, "
               "not the game: clicks, wheel and keys are forwarded to it while the cursor is over it, Escape "
               "away from it hides it, and Alt+F4 hides it and closes the game as usual. Changes "
               "made there apply live, with no host restart. Needs a windowed or borderless game: the "
               "desktop compositor cannot draw over exclusive fullscreen.");
    // Two ways in. The compositor thumbnail needs no build and works for every client
    // API; the texture works in exclusive fullscreen and on any monitor, but needs the
    // host's v7 panel texture, which a D3D11 game opens with its first build.
    const bool shown_thumb = g_cast_wanted && g_cfg.cast_mode == 0, shown_tex = g_cast_wanted && g_cfg.cast_mode == 1;
    if (ImGui::Button(shown_thumb ? "Hide the DLSS 5 panel" : "Show the DLSS 5 panel in-game"))
    {
        g_cast_wanted = !shown_thumb;
        if (g_cfg.cast_mode != 0) { g_cfg.cast_mode = 0; dirty = true; }
        Log("[feed32] cast: %s (overlay, compositor)", g_cast_wanted ? "shown" : "hidden");
    }
    ImGui::SameLine(); HelpMarker("Drawn by the desktop compositor: windowed or borderless games only.");
    ImGui::SameLine();
    // The texture mode hands a shader resource view to the GAME's ImGui backend, and on
    // D3D10 that backend wants an ID3D10 view while ours is the relay's D3D11 one. There
    // is no conversion to make: offer only the thumbnail there, and say why.
    if (g.is_d3d10) ImGui::BeginDisabled();
    if (ImGui::Button(shown_tex ? "Hide the DLSS 5 panel " : "Show as texture (fullscreen too)"))
    {
        g_cast_wanted = !shown_tex;
        if (g_cfg.cast_mode != 1) { g_cfg.cast_mode = 1; dirty = true; }
        Log("[feed32] cast: %s (overlay, texture)", g_cast_wanted ? "shown" : "hidden");
    }
    if (g.is_d3d10) ImGui::EndDisabled();
    ImGui::SameLine(); HelpMarker(g.is_d3d10
                                  ? "Not available on Direct3D 10: the panel would have to be handed to this game's "
                                    "ReShade as a Direct3D 11 texture. Use the button on the left, or host_window=1."
                                  : "Drawn by this game's ReShade from a copy of the host's frame: works in exclusive "
                                    "fullscreen and on any monitor. The panel appears once the feed has built (its "
                                    "texture is set up with the first build).");
    ImGui::TextDisabled("%s", g_cast_status);
    if (ImGui::SliderInt("Panel size (%)", &g_cfg.cast_scale, 25, 300)) dirty = true;
    ImGui::SameLine(); HelpMarker("Relative to the largest size that fits this window (the host's tab column at 1:1, "
                                  "or shrunk to the game's height); above 100% the panel may run past the window's "
                                  "left and bottom edges, and cover this overlay. Applied live; the panel stays "
                                  "anchored at the top right, and the X in its top-right corner always closes it.");
    {
        char name[64];
        CastKeyName(g_cfg.cast_key, name, sizeof(name));
        ImGui::Text("Toggle key: %s", name);
        ImGui::SameLine();
        if (g_cast_capture_key)
            ImGui::TextDisabled("press a key (Esc cancels, Backspace clears)");
        else if (ImGui::Button("Set key"))
            g_cast_capture_key = true;
        ImGui::SameLine(); HelpMarker("A key that shows and hides the panel without opening this overlay. "
                                      "Saved as cast_key in dlss5-feed.cfg.");
    }
    bool show_host_window = g_cfg.host_window != 0;
    if (ImGui::Checkbox("Show the DLSS 5 host window", &show_host_window)) { g_cfg.host_window = show_host_window ? 1 : 0; dirty = true; }
    ImGui::SameLine(); HelpMarker("The helper process's own separate window, the old way in. Not needed for the "
                                  "in-game panel above. Takes effect when the host is next started.");

    if (!g_host_win_loaded) { ReadHostWindowSize(); g_host_win_loaded = true; }
    bool win_size_touched = false;
    if (ImGui::SliderInt("Host window width", &g_host_win_w, 300, 2000)) win_size_touched = true;
    ImGui::SameLine(); HelpMarker("A REAL resize of the host window, its swapchain and the panel texture cast "
                                  "above -- ReShade's own tab column actually gets more room to lay out in, "
                                  "not just a bigger-drawn copy of the same pixels like \"Panel size (%)\" "
                                  "below. Written straight into host64\\ReShade.ini's [DLSS5Host] section; "
                                  "takes effect the next time the host (re)starts.");
    if (ImGui::SliderInt("Host window height (0 = auto, full screen)", &g_host_win_h, 0, 3000)) win_size_touched = true;
    ImGui::SameLine(); HelpMarker("0 fills the primary monitor's work area (the default). This window is "
                                  "normally hidden behind the game, never shown on the desktop at OS size, so "
                                  "taller than the screen is fine if you want more room and less scrolling.");
    if (win_size_touched) WriteHostWindowSize();

    if (ImGui::CollapsingHeader("Advanced"))
    {
        if (ImGui::InputInt("Raw create flags (-1 = auto)", &g_cfg.flags)) dirty = true;
        if (ImGui::SliderInt("Log first N frames", &g_cfg.log_frames, 0, 20)) dirty = true;
    }

    if (g_chicken_host)
    {
        // Chicken does not read [RenoDX.DLSS5]; mirroring that panel here would be a lie.
        ImGui::Separator();
        ImGui::TextUnformatted("Deep Fried Chicken settings (on the host)");
        ImGui::TextWrapped("Deep Fried Chicken has far more options than fit on this page -- up to 30 neural "
                           "passes, each with its own preset, style and four strengths, plus colour, HDR and "
                           "work-scale controls -- so only the feeder's own DLSS settings above are shown here. "
                           "For Chicken's, press \"Show the DLSS 5 panel in-game\" above and open its Deep Fried "
                           "Chicken tab in that panel.");
        ChickenCfgRefresh();
        if (g_chicken_arm >= 0)
        {
            ImGui::Text("Current (host64\\deep-fried-chicken.cfg): arm=%d  enabled=%d  passes=%d  work scale=%d%%",
                        g_chicken_arm, g_chicken_enabled, g_chicken_layers, g_chicken_work_percent);
            if (g_chicken_arm == 0)
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f),
                                   "arm=0 is a hard disarm: Chicken installs no hooks and does nothing until it is set "
                                   "to 1 and the host is restarted.");
            else if (g_chicken_enabled == 0)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "enabled=0: neural passes are switched off live.");
        }
        else
            ImGui::TextDisabled("(host64\\deep-fried-chicken.cfg not readable yet)");
    }
    else
    {
        ImGui::Separator();
        ImGui::TextUnformatted("DLSS 5 neural-rendering settings (on the host)");
        ImGui::SameLine();
        HelpMarker("The same settings, in the same order, as the \"DLSS 5 Neural Rendering\" panel in "
                   "the host window -- mirrored here so you do not have to alt-tab. They live in the "
                   "host's own ReShade.ini, which it reads at startup, so applying them restarts the "
                   "host. Settings you never touch here are left exactly as the add-on wrote them. "
                   "To change them live instead, use \"Show the DLSS 5 panel in-game\" above.");

        // The overlay can be opened before the first effect-runtime resolve has loaded these.
        if (!g_host_nr_loaded) { ReadHostNR(); g_host_nr_loaded = true; }

        for (int i = 0; i < NR_COUNT; ++i)
        {
            if (i == NR_BRIDGE_FIRST)
            {
                ImGui::Spacing();
                ImGui::TextDisabled("HDR color bridge (v4.7 and newer add-on builds)");
            }
            else if (i == NR_LEGACY_FIRST)
            {
                ImGui::Spacing();
                ImGui::TextDisabled("Control-compatible color transfer (v4.6 and older; v4.7 replaced it above)");
            }
            else if (i == NR_GUIDE_FIRST)
            {
                ImGui::Spacing();
                ImGui::TextDisabled("Guide overrides (leave at defaults unless diagnostics require them)");
            }

            const NRSetting &s = kNR[i];
            bool edited = false;
            if (s.kind == NR_BOOL)
            {
                bool b = g_nr[i] != 0.0f;
                if (ImGui::Checkbox(s.label, &b)) { g_nr[i] = b ? 1.0f : 0.0f; edited = true; }
            }
            else if (s.kind == NR_COMBO)
            {
                int idx = static_cast<int>(g_nr[i]);
                if (idx < 0 || idx >= s.item_count) idx = 0;   // a value the add-on could not have written
                if (ImGui::Combo(s.label, &idx, s.items, s.item_count)) { g_nr[i] = static_cast<float>(idx); edited = true; }
            }
            else
            {
                if (ImGui::SliderFloat(s.label, &g_nr[i], s.lo, s.hi, s.format)) edited = true;
            }
            if (edited) g_nr_touched[i] = true;

            if (s.tooltip != nullptr) { ImGui::SameLine(); HelpMarker(s.tooltip); }

            // Flag a stored value the add-on's own widget could never produce (an older build of
            // this panel wrote NRStyle=2 into a two-entry dropdown).
            if (s.kind == NR_COMBO && (g_nr[i] < 0.0f || g_nr[i] >= static_cast<float>(s.item_count)))
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f),
                                   "   stored value %d is out of range - pick one above to correct it",
                                   static_cast<int>(g_nr[i]));
            else if (!g_nr_present[i] && !g_nr_touched[i])
                { ImGui::SameLine(); ImGui::TextDisabled("(add-on default)"); }
        }

        ImGui::Spacing();
        if (ImGui::Button("Apply to the DLSS 5 host"))
            HostRequest(HOST_REQ_APPLY, nullptr);
        ImGui::SameLine();
        if (ImGui::Button("Reload from host"))
        {
            ReadHostNR();
            LogHostNR("host DLSS 5 settings reloaded from the overlay page");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(applying restarts the helper; a brief pause while it saves, then it comes back in the background)");
    }   // end of the RenoDX-only settings mirror

    // Host process controls, both consumers. The helper is a separate process: it can die,
    // be blocked at startup, or be shut down with the feed disabled, and then nothing on
    // this page would bring it back. Under RenoDX this used to be reachable only as a side
    // effect of "Apply to the DLSS 5 host".
    ImGui::Separator();
    ImGui::TextUnformatted("Host process");
    if (ImGui::Button(HostAlive() ? "Restart the DLSS 5 host" : "Start the DLSS 5 host"))
        HostRequest(HOST_REQ_RESTART, HostAlive() ? "host restart requested from the overlay"
                                                  : "host start requested from the overlay");
    ImGui::SameLine();
    if (HostAlive())
        ImGui::TextDisabled("(running -- the replacement starts in the background; only its shutdown pauses the game)");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "not running -- press this to start it again");
    // What the worker is doing, so the wait is visible rather than mysterious.
    if (g_link.state == LINK_RUNNING)
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                           g_link.job == JOB_CONNECT ? "starting the host and shaking hands (the game keeps rendering)"
                                                     : "waiting for the host to build (the game keeps rendering)");
    else if (g_build_pending)
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "a build is in progress");
    ImGui::TextDisabled("Its own log is host64\\dlss5-feed-host.log; the neural consumer's panel lives in "
                        "its window (\"Show the DLSS 5 panel in-game\" above brings it here).");

    if (dirty) CfgSave();
}

// ---------------------------------------------------------------------------

// Fired by ReShade before the device (for Vulkan: from inside its vkCreateInstance
// hook, i.e. before the game's vkCreateDevice). That is the one moment the interop
// extensions can still be added from in-process -- see feed_vk_hook.h.
static bool OnCreateDevice(reshade::api::device_api api, uint32_t & /*api_version*/)
{
    // Gated on enabled: this is the one thing here that patches another module's code
    // (MinHook trampolines over vulkan-1's exports) and appends extensions to every device
    // the game creates. It used to run at enabled=0, which made "set enabled=0 and see if
    // it still crashes" a test that proved nothing (issue #44). It cannot be installed
    // later either -- the game's vkCreateDevice has been and gone -- so turning the add-on
    // back on from the overlay needs a restart to get the Vulkan transport, and says so.
    if (api == reshade::api::device_api::vulkan)
    {
        if (g_cfg.enabled) FeedVkHookInstall();
        else Log("[feed32] enabled=0: the Vulkan interop hook is NOT installed. Turning this add-on "
                 "back on mid-session cannot install it -- a Vulkan game needs a restart with enabled=1.");
    }
    return false;   // never change the requested API version
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_log_cs);
        InitializeCriticalSection(&g_feed_cs);
        GetModuleFileNameA(module, g_log_path, MAX_PATH);
        if (char *s = strrchr(g_log_path, '\\'))
            strcpy_s(s + 1, MAX_PATH - (s + 1 - g_log_path), "dlss5-feed.log");
        // A fresh log per process -- but not per attach: ReShade can load this add-on a
        // second time in the same process (a second GL context at exit did, WormsXHD), and
        // truncating then wiped the session that had just ended. The marker is per process.
        const bool reattached = GetEnvironmentVariableA("DLSS5_FEED32_ATTACHED", nullptr, 0) != 0;
        SetEnvironmentVariableA("DLSS5_FEED32_ATTACHED", "1");
        if (!reattached) { FILE *f = nullptr; if (fopen_s(&f, g_log_path, "w") == 0 && f) fclose(f); }

        if (!reshade::register_addon(module)) return FALSE;
        g_prev_filter = SetUnhandledExceptionFilter(&CrashFilter);
        Log("dlss5-feed32 %s (built %s %s) attached%s.", FEED_VERSION, __DATE__, __TIME__,
            reattached ? " AGAIN in the same process (ReShade loaded the add-on a second time; the log above is the earlier attach)" : "");
        {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            Log("  host game: %ls", exe);
        }
        CfgWriteDefault();
        CfgReload();
        // Said once, plainly, because "enabled=0 but it still crashed" is only evidence if
        // the reader knows what enabled=0 actually leaves behind (issue #44).
        if (!g_cfg.enabled)
            Log("[feed32] enabled=0: no frames are fed, no runtime is queried, no textures are created and "
                "the Vulkan interop hook is not installed. The add-on stays registered so the overlay's "
                "Enabled checkbox can undo this; nothing else runs.");
        DetectChickenHost();

        reshade::register_event<reshade::addon_event::create_device>(OnCreateDevice);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
        reshade::register_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        reshade::register_event<reshade::addon_event::reshade_present>(OnPresent);
        reshade::register_event<reshade::addon_event::reshade_overlay>(OnOverlay);
        reshade::register_event<reshade::addon_event::set_fullscreen_state>(OnSetFullscreenState);
        reshade::register_event<reshade::addon_event::reshade_open_overlay>(OnOpenOverlay);
        reshade::register_overlay(nullptr, DrawOverlay);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // First, before anything else can fault: CrashFilter lives in code that is about to
        // be unmapped. ReShade reloads add-ons per Vulkan instance (see feed_vk_hook.h), so
        // leaving it installed means a later crash jumps into freed memory and the game's own
        // handler never sees the real fault.
        SetUnhandledExceptionFilter(g_prev_filter);
        // We are under the loader lock: HostLinkStop must not try to join the worker here.
        g_detaching = true;
        CastRelease();
        reshade::unregister_overlay(nullptr, DrawOverlay);
        reshade::unregister_event<reshade::addon_event::reshade_present>(OnPresent);
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(OnOverlay);
        reshade::unregister_event<reshade::addon_event::set_fullscreen_state>(OnSetFullscreenState);
        reshade::unregister_event<reshade::addon_event::reshade_open_overlay>(OnOpenOverlay);
        reshade::unregister_event<reshade::addon_event::create_device>(OnCreateDevice);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
        reshade::unregister_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
        reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        FeedVkHookRemove();   // before this code is unmapped -- ReShade reloads add-ons per Vulkan instance
        HostClose();
        reshade::unregister_addon(module);
        Log("shut down cleanly.");
        // Last, after the final Log: these are re-initialised on every attach, and ReShade
        // attaches this add-on again per Vulkan instance, so not deleting them leaks one pair
        // per load cycle. Nothing may log or feed past this point.
        DeleteCriticalSection(&g_feed_cs);
        DeleteCriticalSection(&g_log_cs);
    }
    return TRUE;
}
