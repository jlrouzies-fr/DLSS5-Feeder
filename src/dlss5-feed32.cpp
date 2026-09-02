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
#include "feed_dfc.h"       // Deep Fried Chicken: only the file scan is used here (it lives in host64\)

#define FEED_VERSION "0.11.0-beta.2"

extern "C" __declspec(dllexport) const char *NAME = "DLSS 5 Feed (32-bit) " FEED_VERSION;
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Feeds DLSS 5 neural rendering in 32-bit D3D11, OpenGL and Vulkan (DXVK) games without DLSS: ships the frame, depth and "
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

static const char *volatile g_where = "starting up";
static void Breadcrumb(const char *what) { g_where = what; }

// The 64-bit add-on has recorded crashes with a breadcrumb since 0.8; this side only
// set the breadcrumb and never read it, so a 32-bit crash left nothing in the log past
// the last ordinary line. Same filter here, plus a minidump next to the log (dbghelp
// loaded on demand -- the process is already dying when it is needed).
typedef BOOL (WINAPI *PFN_MiniDumpWriteDump_)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
static void WriteCrashDump(EXCEPTION_POINTERS *ep)
{
    char path[MAX_PATH];
    strcpy_s(path, g_log_path);
    if (char *s = strrchr(path, '\\')) strcpy_s(s + 1, MAX_PATH - (s + 1 - path), "dlss5-feed-crash.dmp");
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    auto write = dbghelp ? reinterpret_cast<PFN_MiniDumpWriteDump_>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (write == nullptr) { Log("[feed32] no dbghelp.dll; no crash dump written"); return; }
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log("[feed32] could not create %s (error %lu)", path, GetLastError()); return; }
    struct { DWORD tid; EXCEPTION_POINTERS *ep; BOOL client; } info = { GetCurrentThreadId(), ep, FALSE };
    const int type = 0x0040 | 0x0001 | 0x0004;   // IndirectlyReferencedMemory | DataSegs | HandleData
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep != nullptr ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
    Log(ok ? "[feed32] crash dump written: %s -- attach it to the issue with this log"
           : "[feed32] crash dump FAILED (%s, error %lu)", path, GetLastError());
}

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
    int   host_window;     // 1 = show the host's window (it carries the DLSS 5 tuning panel: press Home there)
    int   work_resolution; // 50..100 percent of each backbuffer axis; the game stays native-sized
    int   work_upscale;    // expand-back of the work-size output: 0 = bilinear, 1 = AMD FSR 1
                           // (EASU + RCAS). Same key and meaning as the 64-bit add-on (issue #34)
    float work_sharpness;  // RCAS strength for work_upscale=1, 0 (off) .. 1 (sharpest)
    int   async_home;      // 1 = the copy home carries the PREVIOUS frame's output and waits on
                           // the fence value of the frame BEFORE this one, so the game's present
                           // never waits on this frame's round trip through the host process.
                           // Costs one frame of DLSS latency, which the temporal history hides.
                           // 0 = the same-frame contract this add-on shipped with. Same name and
                           // meaning as the 64-bit add-on's knob for its Vulkan transport.
    float mv_scale_x, mv_scale_y;
};

static Cfg g_cfg = { 1, 2, -1, -1, -1, 0, 3, 1, 100, 0, 0.3f, 1, 1.0f, 1.0f };
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
               "host_window=%d\nwork_resolution=%d\nwork_upscale=%d\nwork_sharpness=%.2f\nasync_home=%d\nmv_scale_x=%.3f\nmv_scale_y=%.3f\n",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
            g_cfg.log_frames, g_cfg.host_window, g_cfg.work_resolution, g_cfg.work_upscale, g_cfg.work_sharpness,
            g_cfg.async_home, g_cfg.mv_scale_x, g_cfg.mv_scale_y);
    fclose(f);
}

// Writes every current value, overwriting the file -- used by the overlay page so an
// edit made there survives the next CfgReload() instead of being read back off the
// stale on-disk copy 60 frames later.
static void CfgSave()
{
    char path[MAX_PATH];
    CfgPath(path);
    FILE *f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || f == nullptr) return;
    fprintf(f, "enabled=%d\nmode=%d\nhdr=%d\ndepth_inverted=%d\nflags=%d\nreset_every=%d\nlog_frames=%d\n"
               "host_window=%d\nwork_resolution=%d\nwork_upscale=%d\nwork_sharpness=%.2f\nasync_home=%d\nmv_scale_x=%.3f\nmv_scale_y=%.3f\n",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
            g_cfg.log_frames, g_cfg.host_window, g_cfg.work_resolution, g_cfg.work_upscale, g_cfg.work_sharpness,
            g_cfg.async_home, g_cfg.mv_scale_x, g_cfg.mv_scale_y);
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
    }
    fclose(f);
    if (next.work_resolution < 50 || next.work_resolution > 100) next.work_resolution = g_cfg.work_resolution;
    if (next.work_upscale < 0 || next.work_upscale > 1) next.work_upscale = g_cfg.work_upscale;
    if (next.work_sharpness < 0.0f || next.work_sharpness > 1.0f) next.work_sharpness = g_cfg.work_sharpness;
    const bool rebuild = next.mode != g_cfg.mode || next.hdr != g_cfg.hdr ||
                         next.depth_inverted != g_cfg.depth_inverted || next.flags != g_cfg.flags ||
                         next.mv_scale_x != g_cfg.mv_scale_x || next.mv_scale_y != g_cfg.mv_scale_y;
    const bool changed = memcmp(&next, &g_cfg, sizeof(Cfg)) != 0;
    if (changed)
    {
        g_cfg = next;
        Log("[feed32] config: enabled=%d mode=%d hdr=%d depth_inverted=%d flags=%d reset_every=%d work_resolution=%d%% work_upscale=%d work_sharpness=%.2f",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
            g_cfg.work_resolution, g_cfg.work_upscale, g_cfg.work_sharpness);
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

    bool        built;
    UINT        width, height;                  // the work resolution DLSS runs at
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
// ---------------------------------------------------------------------------

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

static void HostClose()
{
    HostDrain();   // BEFORE the pipe closes: the host must still be around to signal
    // The next host starts its fences at zero, so nothing this session sent is a value
    // the new pair will ever reach. Forget them, and stop trusting Output's contents.
    g.sent_n    = 0;
    g.wait_n    = 0;
    g.out_valid = false;
    if (g.pipe != nullptr)  { CloseHandle(g.pipe); g.pipe = nullptr; }
    if (g.hproc != nullptr)
    {
        if (WaitForSingleObject(g.hproc, 2000) != WAIT_OBJECT_0)
            TerminateProcess(g.hproc, 0);      // it did not exit on the pipe break
        CloseHandle(g.hproc);
        g.hproc = nullptr;
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
    Log("[feed32] host lost: %s", why);
    HostClose();
    FeedDisable("the 64-bit host went away -- its own dlss5-feed-host.log (in host64\\) names the reason");
}

static bool HostAlive()
{
    return g.hproc != nullptr && WaitForSingleObject(g.hproc, 0) == WAIT_TIMEOUT;
}

static bool PipeWrite(const void *buf, DWORD len)
{
    DWORD put = 0;
    return g.pipe != nullptr && WriteFile(g.pipe, buf, len, &put, nullptr) && put == len;
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

static bool PipeRead(void *buf, DWORD len)
{
    DWORD got = 0;
    return g.pipe != nullptr && ReadFile(g.pipe, buf, len, &got, nullptr) && got == len;
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

static bool EnsureHost()
{
    if (g.pipe != nullptr && HostAlive()) return true;
    HostClose();

    char dir[MAX_PATH];
    GetModuleFileNameA(g_self, dir, MAX_PATH);
    if (char *s = strrchr(dir, '\\')) *(s + 1) = '\0';

    char exe[MAX_PATH], cmd[MAX_PATH + 32], wd[MAX_PATH];
    sprintf_s(exe, "%shost64\\dlss5-feed-host64.exe", dir);
    sprintf_s(wd, "%shost64", dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES)
    {
        Warn("host64\\dlss5-feed-host64.exe not found next to the add-on");
        FeedDisable("the 64-bit host is not installed");
        return false;
    }
    sprintf_s(cmd, "\"%s\" %lu%s", exe, GetCurrentProcessId(), g_cfg.host_window ? "" : " --hide");

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    Breadcrumb("spawning the 64-bit host");
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, wd, &si, &pi))
    {
        Log("[feed32] CreateProcess failed %lu", GetLastError());
        FeedDisable("could not start the 64-bit host");
        return false;
    }
    CloseHandle(pi.hThread);
    g.hproc = pi.hProcess;
    Log("[feed32] host spawned (pid %lu)", pi.dwProcessId);

    char name[128];
    sprintf_s(name, FEED_PIPE_FMT, static_cast<unsigned long>(GetCurrentProcessId()));
    for (int i = 0; i < 150 && g.pipe == nullptr; ++i)   // up to 15 s (host loads ReShade + NGX)
    {
        HANDLE p = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (p != INVALID_HANDLE_VALUE) { g.pipe = p; break; }
        if (!HostAlive()) { HostLost("exited during startup"); return false; }
        Sleep(100);
    }
    if (g.pipe == nullptr) { HostLost("pipe never appeared"); return false; }

    const uint32_t kind = g.is_vulkan ? FEED_CLIENT_VULKAN : g.is_gl ? FEED_CLIENT_GL : FEED_CLIENT_D3D11;
    const char *kind_name = g.is_vulkan ? "Vulkan" : g.is_gl ? "OpenGL" : "D3D11";
    // Hand the host a handle to this process instead of making it OpenProcess(pid): a
    // protective DACL on the game (anti-cheat/DRM; seen on vanilla WoW) denies that with
    // error 5, and this duplication never consults the game's DACL -- both process handles
    // involved are ours (the pseudo-handle, and the one CreateProcess just returned).
    HANDLE self_in_host = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), g.hproc, &self_in_host,
                         PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
        Log("[feed32] could not duplicate our process handle into the host (%lu); it will fall back to OpenProcess",
            GetLastError());
    FeedHello hello = { FEED_IPC_MAGIC, FEED_IPC_VERSION, GetCurrentProcessId(), kind,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self_in_host)) };
    FeedHelloAck ack = {};
    if (!PipeWrite(&hello, sizeof(hello)) || !PipeRead(&ack, sizeof(ack)) || ack.magic != FEED_IPC_MAGIC)
    { HostLost("handshake failed"); return false; }
    if (ack.version != FEED_IPC_VERSION)
    {
        // The message structs after the hello changed size between versions, so a
        // mismatched pair would not just misbehave, it would desync the pipe. Both
        // sides refuse rather than guess.
        Log("[feed32] the host in host64\\ speaks protocol v%u, this add-on v%u", ack.version, FEED_IPC_VERSION);
        HostClose();
        FeedDisable("the host64\\ folder is from a different release -- reinstall both halves together");
        return false;
    }
    Log("[feed32] host connected (protocol v%u, %s client)", ack.version, kind_name);
    RestoreGameFocus();   // the replacement host is up; take the foreground back if we lost it
    return true;
}

// ---------------------------------------------------------------------------
// The host's DLSS 5 settings, controlled from the game's own ReShade panel.
// The renodx add-on reads [RenoDX.DLSS5] from the HOST's ReShade.ini at startup
// (only its own panel can change them live), so applying = write that ini and
// cycle the host. The game renders normally during the gap, which is usually a couple of
// seconds but can reach ~15 s -- the replacement host has to re-init NGX and reload the
// ~165 MB DLSSNR model before it can serve a frame.
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

static void HostApplySettings()
{
    LogHostNR("applying DLSS 5 host settings");
    CaptureGameFocus();   // spent once the replacement host has connected

    // Order matters: the host's ReShade saves its ini ON EXIT and would clobber our
    // values -- close the host first (HostClose drains the in-flight frame and
    // releases the shared fences), write after, respawn on the next frame.
    HostClose();
    WriteHostNR();

    g.built = false;
    g.disabled = false;
    g.consecutive_fails = 0;
    g_retry_at = 0;
    Warn("DLSS 5 settings applied -- restarting the host (up to 15 s)");
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
        }
        else
        {
            bool any = false;
            for (int i = 0; i < FEED_SLOTS; ++i) if (g.gl_tex[i] != 0) { any = true; g.gl_tex[i] = 0; g.gl_memobj[i] = 0; }
            if (any) Log("[feed32] the GL context is not current here; the imported textures are left to the driver");
        }
    }
    SafeRelease(g.output_srv);
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
        "cbuffer ResampleConstants : register(b0) { float2 mv_scale; float2 _pad; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(VSOut i) : SV_Target { return float4(src_color.Sample(smp, i.uv).rgb, 1.0); }\n"
        // Motion vectors are in pixels, so they scale with the resolution ratio; depth is a
        // point sample (interpolating across a silhouette would invent geometry).
        "struct ResampleOut { float4 color : SV_Target0; float2 mv : SV_Target1; float depth : SV_Target2; };\n"
        "ResampleOut ps_resample(VSOut i) { ResampleOut o;\n"
        "  o.color = src_color.SampleLevel(smp, i.uv, 0);\n"
        "  o.mv = src_mv.SampleLevel(point_smp, i.uv, 0) * mv_scale;\n"
        "  o.depth = src_depth.SampleLevel(point_smp, i.uv, 0); return o; }\n";
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
    Breadcrumb("building the shared textures");
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
    { FeedDisable("unsupported backbuffer format"); return false; }
    const bool hdr      = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : IsHdrFormat(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    if (!g.host_creates)
    {
        int failed = -1;
        if      (!MakeShared(FEED_COLOR,  w, h, g.color_fmt,              false, true))  failed = FEED_COLOR;
        else if (!MakeShared(FEED_OUTPUT, w, h, g.output_fmt,             true,  false)) failed = FEED_OUTPUT;
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
            static const char *const slot_name[FEED_SLOTS] = { "Color", "Output", "Depth", "MV" };
            const D3D_FEATURE_LEVEL fl = g.dev->GetFeatureLevel();
            ReleaseShared();
            g.host_creates = true;
            g.no_uav       = failed == FEED_OUTPUT || fl < D3D_FEATURE_LEVEL_11_0;
            Log("[feed32] the game's D3D11 device (feature level %d_%d) refused the shared %s texture; the host will "
                "create the shared set instead%s",
                (fl >> 12) & 0xF, (fl >> 8) & 0xF, slot_name[failed],
                g.no_uav ? ", keeping the DLSS output's UAV on its own side (this device cannot bind one)" : "");
        }
    }

    if (!EnsureHost()) return false;

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
    if (g.host_creates)
        b.client_flags = FEED_BUILD_HOST_CREATES | (g.no_uav ? FEED_BUILD_OUTPUT_NO_UAV : 0);
    else
        for (int i = 0; i < FEED_SLOTS; ++i)
            b.tex[i] = reinterpret_cast<uintptr_t>(g.tex_handle[i]);

    Breadcrumb("asking the host to build");
    BYTE tag = 'B';
    FeedBuildAck ack = {};
    if (!PipeWrite(&tag, 1) || !PipeWrite(&b, sizeof(b)) || !PipeRead(&ack, sizeof(ack)))
    { HostLost("build exchange failed"); return false; }
    if (!ack.ok)
    {
        Log("[feed32] host build failed (ngx 0x%08X)", ack.ngx_result);
        return false;
    }

    if (g.host_creates)
    {
        // The handles are already duplicated into this process; ReleaseShared closes them.
        ID3D11Device1 *dev1 = nullptr;
        if (FAILED(g.dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1))) || dev1 == nullptr)
        { FeedDisable("ID3D11Device1 unavailable, so the host-created textures cannot be opened (Windows 8+ D3D11.1 runtime required)"); return false; }
        for (int i = 0; i < FEED_SLOTS; ++i)
        {
            g.tex_handle[i] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ack.tex[i]));
            const HRESULT hr = g.tex_handle[i] != nullptr
                ? dev1->OpenSharedResource1(g.tex_handle[i], __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&g.tex[i]))
                : E_HANDLE;
            if (FAILED(hr))
            {
                Log("[feed32] OpenSharedResource1(tex %d) failed 0x%08X -- this device cannot open the host's textures either",
                    i, hr);
                dev1->Release();
                ReleaseShared();
                FeedDisable("the shared textures cannot be created on the game's device or opened from the host's");
                return false;
            }
        }
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
    Breadcrumb("building the shared textures (OpenGL)");
    ReleaseShared();

    g.width  = w;
    g.height = h;
    g.backbuffer_width  = w;   // v1 GL is DLAA at 100%: no work-resolution scaling
    g.backbuffer_height = h;
    g.bb_fmt     = bb_fmt;
    g.color_fmt  = GlSafeColorFormat(TypedColorFormat(bb_fmt));
    g.output_fmt = g_cfg.mode == 1 ? g.color_fmt : GlSafeColorFormat(OutputFormatFor(g.color_fmt));
    if (g.color_fmt == DXGI_FORMAT_UNKNOWN)
    { FeedDisable("unsupported backbuffer format"); return false; }
    const bool hdr      = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : IsHdrFormat(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    if (!EnsureHost()) return false;

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
    // b.tex stays zero: on this path the host creates, and answers with its handles.

    Breadcrumb("asking the host to build (OpenGL)");
    BYTE tag = 'B';
    FeedBuildAck ack = {};
    if (!PipeWrite(&tag, 1) || !PipeWrite(&b, sizeof(b)) || !PipeRead(&ack, sizeof(ack)))
    { HostLost("build exchange failed"); return false; }

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
    Breadcrumb("building the shared textures (Vulkan)");
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
        FeedDisable("unsupported backbuffer format");
        return false;
    }
    // Transport test copies Color->Output host-side with CopyTextureRegion: same format
    // then. Otherwise ask for the channel order the backbuffer has, so the way home is a
    // raw vkCmdCopyImage -- the host gets the final say (see ack.output_fmt below).
    const DXGI_FORMAT want_output = g_cfg.mode == 1 ? g.color_fmt : FeedFmtOutputFor(g.color_fmt);
    const bool hdr      = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : FeedFmtIsHdr(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    if (!EnsureHost()) return false;

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
    // b.tex stays zero: on this path the host creates, and answers with its handles.

    Breadcrumb("asking the host to build (Vulkan)");
    BYTE tag = 'B';
    FeedBuildAck ack = {};
    if (!PipeWrite(&tag, 1) || !PipeWrite(&b, sizeof(b)) || !PipeRead(&ack, sizeof(ack)))
    { HostLost("build exchange failed"); return false; }

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
    g.output_fmt = ack.output_fmt != 0 ? static_cast<DXGI_FORMAT>(ack.output_fmt) : want_output;
    if (g.output_fmt != want_output)
        Log("[feed32] the host created the Output as %s, not the requested %s",
            FeedFmtName(g.output_fmt), FeedFmtName(want_output));

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
    const float constants[4] = {
        static_cast<float>(g.width)  / static_cast<float>(source_w),
        static_cast<float>(g.height) / static_cast<float>(source_h), 0.0f, 0.0f
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

    const bool scaled = g.width != g.backbuffer_width || g.height != g.backbuffer_height;
    const bool fsr    = g_cfg.work_upscale == 1 && g.fsr_ok && g.easu_ps != nullptr;
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
        UpdateFsrConstants(ctx, easu ? g.width : g.backbuffer_width, easu ? g.height : g.backbuffer_height);
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
            Log("[feed32] building: %ux%u backbuffer fmt=%u (OpenGL, depth reversed=%d, mode=%d)",
                w, h, bbf, g.depth_reversed ? 1 : 0, g_cfg.mode);
            ok = BuildSharedGl(w, h, bbf, bb_res.handle);
            if (ok) g.consecutive_fails = 0;
            else if (!g.disabled) FeedFail("shared build");
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
            Log("[feed32] building: %ux%u backbuffer %s (Vulkan, depth reversed=%d, mode=%d)",
                w, h, FeedFmtName(bbf), g.depth_reversed ? 1 : 0, g_cfg.mode);
            ok = BuildSharedVk(w, h, bbf);
            if (ok) g.consecutive_fails = 0;
            else if (!g.disabled) FeedFail("shared build");
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
    if (dev_api->get_api() != reshade::api::device_api::d3d11)
    { FeedDisable("only Direct3D 11, OpenGL and Vulkan games are supported by the 32-bit add-on"); return; }

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

    const UINT work_w = ScaledExtent(cd.Width, g_cfg.work_resolution);
    const UINT work_h = ScaledExtent(cd.Height, g_cfg.work_resolution);
    if (ok && (!g.built || work_w != g.width || work_h != g.height ||
               cd.Width != g.backbuffer_width || cd.Height != g.backbuffer_height || cd.Format != g.bb_fmt))
    {
        if (GetTickCount64() < g_retry_at)
            ok = false;                       // backing off after a failed build
        else
        {
            Log("[feed32] building: %ux%u work resolution (%d%%) -> %ux%u backbuffer fmt=%u (depth reversed=%d, mode=%d)",
                work_w, work_h, g_cfg.work_resolution, cd.Width, cd.Height, cd.Format, g.depth_reversed ? 1 : 0, g_cfg.mode);
            ok = BuildShared(work_w, work_h, cd.Width, cd.Height, cd.Format);
            if (ok) g.consecutive_fails = 0;
            else if (!g.disabled) FeedFail("shared build");
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
    if (rt->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", v))
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

static void OnInitEffectRuntime(reshade::api::effect_runtime *rt)
{
    g.runtime = rt;
    ResolveHandles(rt);
    DetectSmoothMotion();   // a present interposer can arrive after this add-on did
    static int inits = 0;
    if (++inits <= 8) Log("[feed32] effect runtime %p initialised", (void *)rt);
}

static void OnDestroyEffectRuntime(reshade::api::effect_runtime *rt)
{
    if (rt != g.runtime) return;
    // The shared textures live on the game's device and survive runtime churn; keep them.
    g.runtime = nullptr;
    g.technique = {}; g.launchpad = {}; g.mv_var = {}; g.depth_var = {};
    g.handles_ok = false;
}

static void OnReloadedEffects(reshade::api::effect_runtime *rt)
{
    if (rt == g.runtime || g.runtime == nullptr)
    {
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
    if (rt != g.runtime || g.technique.handle == 0 || technique.handle != g.technique.handle) return;
    FeedFrame(rt, cl, rtv);
}

static void OnDestroyDevice(reshade::api::device *dev)
{
    const bool ours = (g.dev != nullptr && reinterpret_cast<ID3D11Device *>(dev->get_native()) == g.dev) ||
                      (g.is_gl && dev->get_api() == reshade::api::device_api::opengl) ||
                      (g.is_vulkan && dev == g.rs_dev);
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

static void DrawOverlay(reshade::api::effect_runtime *)
{
    bool dirty = false;
    bool enabled = g_cfg.enabled != 0;
    if (ImGui::Checkbox("Enabled", &enabled)) { g_cfg.enabled = enabled ? 1 : 0; dirty = true; }

    ImGui::Separator();
    ImGui::TextUnformatted("Status");
    ImGui::Text("Feed: %s", g.disabled ? "disabled" : g.built ? "built" : "not built");
    if (g.disabled && g_disable_why[0])
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f), "Stopped: %s", g_disable_why);
    ImGui::Text("Render API: %s", g.is_vulkan ? "Vulkan" : g.is_gl ? "OpenGL" : "Direct3D 11");
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
        bool fsr = g_cfg.work_upscale == 1;
        if (ImGui::Checkbox("FSR 1 expand-back (EASU + RCAS)", &fsr)) { g_cfg.work_upscale = fsr ? 1 : 0; dirty = true; }
        ImGui::SameLine(); HelpMarker("Replaces the bilinear stretch of the work-size output with AMD FSR 1 spatial "
                                      "upscaling and RCAS sharpening. A better filter for the cost knob above, not DLSS "
                                      "Quality: the result can never exceed the native frame. At 100% only the "
                                      "sharpening runs.");
        if (fsr)
        {
            ImGui::SliderFloat("Sharpness", &g_cfg.work_sharpness, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) dirty = true;
        }
        if (!fsr_available)
        {
            ImGui::EndDisabled();
            ImGui::TextDisabled("FSR 1 shaders failed to compile (see the log); expand-back stays bilinear.");
        }
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

    bool show_host_window = g_cfg.host_window != 0;
    if (ImGui::Checkbox("Show the DLSS 5 host window", &show_host_window)) { g_cfg.host_window = show_host_window ? 1 : 0; dirty = true; }
    ImGui::SameLine(); HelpMarker("The helper process's own window. Only needed for settings not listed here.");

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
                           "For Chicken's, tick \"Show the DLSS 5 host window\" above, then press Home in that "
                           "window and open its Deep Fried Chicken tab.");
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
        if (dirty) CfgSave();
        return;
    }

    ImGui::Separator();
    ImGui::TextUnformatted("DLSS 5 neural-rendering settings (on the host)");
    ImGui::SameLine();
    HelpMarker("The same settings, in the same order, as the \"DLSS 5 Neural Rendering\" panel in "
               "the host window -- mirrored here so you do not have to alt-tab. They live in the "
               "host's own ReShade.ini, which it reads at startup, so applying them restarts the "
               "host. Settings you never touch here are left exactly as the add-on wrote them.");

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
        HostApplySettings();
    ImGui::SameLine();
    if (ImGui::Button("Reload from host"))
    {
        ReadHostNR();
        LogHostNR("host DLSS 5 settings reloaded from the overlay page");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(applying restarts the helper process; up to 15 s without DLSS)");

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
        InitializeCriticalSection(&g_feed_cs);
        GetModuleFileNameA(module, g_log_path, MAX_PATH);
        if (char *s = strrchr(g_log_path, '\\'))
            strcpy_s(s + 1, MAX_PATH - (s + 1 - g_log_path), "dlss5-feed.log");
        { FILE *f = nullptr; if (fopen_s(&f, g_log_path, "w") == 0 && f) fclose(f); }

        if (!reshade::register_addon(module)) return FALSE;
        g_prev_filter = SetUnhandledExceptionFilter(&CrashFilter);
        Log("dlss5-feed32 %s (built %s %s) attached.", FEED_VERSION, __DATE__, __TIME__);
        {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            Log("  host game: %ls", exe);
        }
        CfgWriteDefault();
        CfgReload();
        DetectChickenHost();

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
        HostClose();
        reshade::unregister_addon(module);
        Log("shut down cleanly.");
    }
    return TRUE;
}
