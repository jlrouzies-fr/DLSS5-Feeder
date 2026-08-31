// dlss5-feed32 - the 32-bit in-game half of DLSS5-Feeder for 32-bit D3D11 games.
//
// A 32-bit game cannot load NGX or the DLSS 5 add-on (x64-only), so this add-on
// does none of that. It creates four GPU textures shared ACROSS PROCESSES (the
// phase-0-proven direction: created here on D3D11, opened by the host on D3D12),
// copies the frame + the companion effect's depth/motion-vector textures into
// them, signals a shared fence, and lets dlss5-feed-host64.exe -- spawned from
// the host64\ subfolder, where ReShade x64 + renodx-dlss5.addon64 live -- run
// the DLSS DLAA + neural-rendering evaluate. The result comes back through the
// shared Output texture, GPU-fenced, and is blitted over the backbuffer.
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

#define FEED_VERSION "0.6.0-beta.1"

extern "C" __declspec(dllexport) const char *NAME = "DLSS 5 Feed (32-bit) " FEED_VERSION;
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Feeds DLSS 5 neural rendering in 32-bit D3D11 games without DLSS: ships the frame, depth and "
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
    float mv_scale_x, mv_scale_y;
};

static Cfg g_cfg = { 1, 2, -1, -1, -1, 0, 3, 1, 1.0f, 1.0f };

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
               "host_window=%d\nmv_scale_x=%.3f\nmv_scale_y=%.3f\n",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
            g_cfg.log_frames, g_cfg.host_window, g_cfg.mv_scale_x, g_cfg.mv_scale_y);
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
               "host_window=%d\nmv_scale_x=%.3f\nmv_scale_y=%.3f\n",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
            g_cfg.log_frames, g_cfg.host_window, g_cfg.mv_scale_x, g_cfg.mv_scale_y);
    fclose(f);
}

static bool CfgReload()   // true when a build-affecting value changed
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
        else if (_stricmp(key, "log_frames")     == 0) next.log_frames     = iv;
        else if (_stricmp(key, "host_window")    == 0) next.host_window    = iv;
        else if (_stricmp(key, "mv_scale_x")     == 0) next.mv_scale_x     = val;
        else if (_stricmp(key, "mv_scale_y")     == 0) next.mv_scale_y     = val;
    }
    fclose(f);
    const bool rebuild = next.mode != g_cfg.mode || next.hdr != g_cfg.hdr ||
                         next.depth_inverted != g_cfg.depth_inverted || next.flags != g_cfg.flags ||
                         next.mv_scale_x != g_cfg.mv_scale_x || next.mv_scale_y != g_cfg.mv_scale_y;
    const bool changed = memcmp(&next, &g_cfg, sizeof(Cfg)) != 0;
    if (changed)
    {
        g_cfg = next;
        Log("[feed32] config: enabled=%d mode=%d hdr=%d depth_inverted=%d flags=%d reset_every=%d",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every);
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

    // shared textures (created HERE, opened by the host)
    ID3D11Texture2D *tex[FEED_SLOTS];
    HANDLE           tex_handle[FEED_SLOTS];
    ID3D11ShaderResourceView *output_srv;
    ID3D11Fence     *fence_in;    // we signal
    ID3D11Fence     *fence_out;   // host signals
    ID3D11DeviceContext4 *ctx4;
    ID3D11Device    *dev;         // not owned

    bool        built;
    UINT        width, height;
    DXGI_FORMAT bb_fmt, color_fmt, output_fmt;
    UINT64      frame_n;
    bool        need_reset;

    // blit
    ID3D11VertexShader *blit_vs;
    ID3D11PixelShader  *blit_ps;
    ID3D11SamplerState *blit_sampler;

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

static bool IsHdrFormat(DXGI_FORMAT typed)
{
    return typed == DXGI_FORMAT_R16G16B16A16_FLOAT || typed == DXGI_FORMAT_R11G11B10_FLOAT;
}

static void FeedDisable(const char *why)
{
    if (g.disabled) return;
    g.disabled = true;
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

static void HostClose()
{
    if (g.pipe != nullptr)  { CloseHandle(g.pipe); g.pipe = nullptr; }
    if (g.hproc != nullptr)
    {
        if (WaitForSingleObject(g.hproc, 2000) != WAIT_OBJECT_0)
            TerminateProcess(g.hproc, 0);      // it did not exit on the pipe break
        CloseHandle(g.hproc);
        g.hproc = nullptr;
    }
}

static void HostLost(const char *why)
{
    Log("[feed32] host lost: %s", why);
    HostClose();
    FeedDisable("the 64-bit host went away");
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

static bool PipeRead(void *buf, DWORD len)
{
    DWORD got = 0;
    return g.pipe != nullptr && ReadFile(g.pipe, buf, len, &got, nullptr) && got == len;
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

    FeedHello hello = { FEED_IPC_MAGIC, FEED_IPC_VERSION, GetCurrentProcessId() };
    FeedHelloAck ack = {};
    if (!PipeWrite(&hello, sizeof(hello)) || !PipeRead(&ack, sizeof(ack)) || ack.magic != FEED_IPC_MAGIC)
    { HostLost("handshake failed"); return false; }
    Log("[feed32] host connected (protocol v%u)", ack.version);
    return true;
}

// ---------------------------------------------------------------------------
// The host's DLSS 5 settings, controlled from the game's own ReShade panel.
// The renodx add-on reads [RenoDX.DLSS5] from the HOST's ReShade.ini at startup
// (only its own panel can change them live), so applying = write that ini and
// cycle the host. The game renders normally during the ~2 s gap.
// ---------------------------------------------------------------------------

struct HostNR
{
    int   uplift, intensity, style, automask, uicorr;
    float structure_, tone;
};

static void HostIniPath(char *out)
{
    GetModuleFileNameA(g_self, out, MAX_PATH);
    if (char *s = strrchr(out, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - out), "host64\\ReShade.ini");
}

static void ReadHostNR(HostNR *v)
{
    char p[MAX_PATH], buf[64];
    HostIniPath(p);
    v->uplift    = GetPrivateProfileIntA("RenoDX.DLSS5", "NeuralUplift", 1, p);
    v->intensity = GetPrivateProfileIntA("RenoDX.DLSS5", "NRIntensity", 2, p);
    v->style     = GetPrivateProfileIntA("RenoDX.DLSS5", "NRStyle", 0, p);
    v->automask  = GetPrivateProfileIntA("RenoDX.DLSS5", "NRAutoMask", 1, p);
    v->uicorr    = GetPrivateProfileIntA("RenoDX.DLSS5", "NRUICorrection", 1, p);
    GetPrivateProfileStringA("RenoDX.DLSS5", "NRLocalStructure", "0.99", buf, sizeof(buf), p);
    v->structure_ = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("RenoDX.DLSS5", "NRLocalTone", "0.45", buf, sizeof(buf), p);
    v->tone = static_cast<float>(atof(buf));
}

static void WriteHostNR(const HostNR &v)
{
    char p[MAX_PATH], buf[64];
    HostIniPath(p);
    sprintf_s(buf, "%d", v.uplift);        WritePrivateProfileStringA("RenoDX.DLSS5", "NeuralUplift", buf, p);
    sprintf_s(buf, "%d", v.intensity);     WritePrivateProfileStringA("RenoDX.DLSS5", "NRIntensity", buf, p);
    sprintf_s(buf, "%d", v.style);         WritePrivateProfileStringA("RenoDX.DLSS5", "NRStyle", buf, p);
    sprintf_s(buf, "%d", v.automask);      WritePrivateProfileStringA("RenoDX.DLSS5", "NRAutoMask", buf, p);
    sprintf_s(buf, "%d", v.uicorr);        WritePrivateProfileStringA("RenoDX.DLSS5", "NRUICorrection", buf, p);
    sprintf_s(buf, "%.6f", v.structure_);  WritePrivateProfileStringA("RenoDX.DLSS5", "NRLocalStructure", buf, p);
    sprintf_s(buf, "%.6f", v.tone);        WritePrivateProfileStringA("RenoDX.DLSS5", "NRLocalTone", buf, p);
}

// Cache of the host's settings, shown and edited on the ReShade overlay page (Add-ons
// tab -> DLSS 5 Feed). Loaded from the host's ini on first resolve so the panel always
// starts from what is actually active, never a stale default.
static HostNR g_host_nr;
static bool   g_host_nr_loaded;

static void HostClose();   // below

static void HostApplySettings()
{
    Log("[feed32] applying DLSS 5 host settings: uplift=%d intensity=%d style=%d structure=%.2f tone=%.2f automask=%d uicorr=%d",
        g_host_nr.uplift, g_host_nr.intensity, g_host_nr.style, g_host_nr.structure_, g_host_nr.tone,
        g_host_nr.automask, g_host_nr.uicorr);

    // Order matters: the host's ReShade saves its ini ON EXIT and would clobber our
    // values -- close the host first, write after, respawn on the next frame.
    HostClose();
    WriteHostNR(g_host_nr);

    SafeRelease(g.fence_in);    // the new host creates new fences; reopen from its BuildAck
    SafeRelease(g.fence_out);
    g.built = false;
    g.disabled = false;
    g.consecutive_fails = 0;
    g_retry_at = 0;
    Warn("DLSS 5 settings applied -- restarting the host (~2 s)");
}

// ---------------------------------------------------------------------------
// Shared resources (created on the game's D3D11 device)
// ---------------------------------------------------------------------------

static void ReleaseShared()
{
    SafeRelease(g.output_srv);
    for (int i = 0; i < FEED_SLOTS; ++i)
    {
        SafeRelease(g.tex[i]);
        if (g.tex_handle[i] != nullptr) { CloseHandle(g.tex_handle[i]); g.tex_handle[i] = nullptr; }
    }
    g.built = false;
}

static bool MakeShared(int slot, UINT w, UINT h, DXGI_FORMAT fmt, bool uav)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
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
        "Texture2D<float4> src : register(t0);\n"
        "SamplerState smp : register(s0);\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(VSOut i) : SV_Target { return float4(src.Sample(smp, i.uv).rgb, 1.0); }\n";
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
    hr = g.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.blit_vs);
    if (SUCCEEDED(hr)) hr = g.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.blit_ps);
    vs->Release();
    ps->Release();
    if (FAILED(hr)) { Log("[feed32] blit shader creation failed 0x%08X", hr); return false; }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    return SUCCEEDED(g.dev->CreateSamplerState(&sd, &g.blit_sampler));
}

static bool BuildShared(UINT w, UINT h, DXGI_FORMAT bb_fmt)
{
    Breadcrumb("building the shared textures");
    ReleaseShared();

    g.width      = w;
    g.height     = h;
    g.bb_fmt     = bb_fmt;
    g.color_fmt  = TypedColorFormat(bb_fmt);
    // Transport test copies Color->Output host-side with CopyResource: same format then.
    g.output_fmt = g_cfg.mode == 1 ? g.color_fmt : OutputFormatFor(g.color_fmt);
    if (g.color_fmt == DXGI_FORMAT_UNKNOWN)
    { FeedDisable("unsupported backbuffer format"); return false; }
    const bool hdr      = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : IsHdrFormat(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    if (!MakeShared(FEED_COLOR, w, h, g.color_fmt, false) ||
        !MakeShared(FEED_OUTPUT, w, h, g.output_fmt, true) ||
        !MakeShared(FEED_DEPTH, w, h, DXGI_FORMAT_R32_FLOAT, false) ||
        !MakeShared(FEED_MV, w, h, DXGI_FORMAT_R16G16_FLOAT, false))
    { ReleaseShared(); return false; }

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format              = g.output_fmt;
    sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(g.dev->CreateShaderResourceView(g.tex[FEED_OUTPUT], &sv, &g.output_srv)))
    { Log("[feed32] output SRV failed"); ReleaseShared(); return false; }
    if (!MakeBlitShaders()) { ReleaseShared(); return false; }

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

    Log("[feed32] shared set ready: %ux%u color fmt=%u output fmt=%u (host ngx 0x%08X, %s)",
        w, h, g.color_fmt, g.output_fmt, ack.ngx_result, g_cfg.mode == 1 ? "transport" : "DLSS");
    g.built      = true;
    g.need_reset = true;
    g.consecutive_fails = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Copy-back blit (verbatim from the 64-bit add-on)
// ---------------------------------------------------------------------------

static void BlitOutputToBackbuffer(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv)
{
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
    vp.Width    = static_cast<float>(g.width);
    vp.Height   = static_cast<float>(g.height);
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

static void FeedFrame(reshade::api::effect_runtime *rt, reshade::api::command_list *cl, reshade::api::resource_view rtv)
{

    if (!g_cfg.enabled || g.disabled || g_cfg.mode == 0) return;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    reshade::api::device *dev_api = rt->get_device();
    if (dev_api->get_api() != reshade::api::device_api::d3d11)
    { FeedDisable("only Direct3D 11 games are supported by the 32-bit add-on"); return; }

    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cl->get_native());
    if (ctx == nullptr || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;

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
    }

    if (ok && (!g.built || cd.Width != g.width || cd.Height != g.height || cd.Format != g.bb_fmt))
    {
        if (GetTickCount64() < g_retry_at)
            ok = false;                       // backing off after a failed build
        else
        {
            Log("[feed32] building: %ux%u backbuffer fmt=%u (depth reversed=%d, mode=%d)",
                cd.Width, cd.Height, cd.Format, g.depth_reversed ? 1 : 0, g_cfg.mode);
            ok = BuildShared(cd.Width, cd.Height, cd.Format);
            if (ok) g.consecutive_fails = 0;
            else if (!g.disabled) FeedFail("shared build");
        }
    }

    if (ok && g.built)
    {
        if (!HostAlive()) { HostLost("process died"); }
        else
        {
            Breadcrumb("copying inputs");
            ctx->CopyResource(g.tex[FEED_COLOR], color);
            ctx->CopyResource(g.tex[FEED_DEPTH], depth);
            ctx->CopyResource(g.tex[FEED_MV], mv);

            const UINT64 n = ++g.frame_n;
            const int reset = (g.need_reset || g_cfg.reset_every) ? 1 : 0;
            g.need_reset = false;

            g.ctx4->Signal(g.fence_in, n);
            ctx->Flush();

            BYTE tag = 'F';
            FeedFrameMsg fm = { n, static_cast<uint32_t>(reset) };
            if (!PipeWrite(&tag, 1) || !PipeWrite(&fm, sizeof(fm)))
                HostLost("frame message failed");
            else
            {
                Breadcrumb("waiting for the host's result");
                g.ctx4->Wait(g.fence_out, n);       // GPU-side; the host CPU-signals on failure
                BlitOutputToBackbuffer(ctx, rtv11);
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
        ReadHostNR(&g_host_nr);
        g_host_nr_loaded = true;
        Log("[feed32] host DLSS 5 settings loaded into the overlay page: uplift=%d intensity=%d style=%d structure=%.2f tone=%.2f automask=%d uicorr=%d",
            g_host_nr.uplift, g_host_nr.intensity, g_host_nr.style, g_host_nr.structure_, g_host_nr.tone,
            g_host_nr.automask, g_host_nr.uicorr);
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
    if (rt == g.runtime || g.runtime == nullptr) { g.runtime = rt; ResolveHandles(rt); }
}

static void OnRenderTechnique(reshade::api::effect_runtime *rt, reshade::api::effect_technique technique,
                              reshade::api::command_list *cl, reshade::api::resource_view rtv,
                              reshade::api::resource_view /*rtv_srgb*/)
{
    if (rt != g.runtime || g.technique.handle == 0 || technique.handle != g.technique.handle) return;
    // Do not let an explicit render request from another add-on bypass ReShade's Home-tab
    // technique checkbox or the global effects toggle.
    if (!rt->get_effects_state() || !rt->get_technique_state(g.technique)) return;
    FeedFrame(rt, cl, rtv);
}

static void OnDestroyDevice(reshade::api::device *dev)
{
    if (g.dev != nullptr && reinterpret_cast<ID3D11Device *>(dev->get_native()) == g.dev)
    {
        Log("[feed32] game device destroyed; shutting down");
        ReleaseShared();
        SafeRelease(g.fence_in);
        SafeRelease(g.fence_out);
        SafeRelease(g.ctx4);
        SafeRelease(g.blit_vs);
        SafeRelease(g.blit_ps);
        SafeRelease(g.blit_sampler);
        g.dev = nullptr;
        HostClose();
    }
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
    ImGui::Text("Feed: %s", g.disabled ? "disabled (see dlss5-feed.log)" : g.built ? "built" : "not built");
    ImGui::Text("Host process: %s", HostAlive() ? "running" : "not running");
    if (g.frames_done > 0) ImGui::Text("Frames delivered: %llu", static_cast<unsigned long long>(g.frames_done));
    ImGui::TextWrapped("Motion vectors: %s", g_mv_status);
    if (g_mv_problem[0])
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f), "%s", g_mv_problem);
    if (g.disabled && ImGui::Button("Re-enable"))
    {
        g.disabled = false;
        g.consecutive_fails = 0;
        g_retry_at = 0;
        Log("[feed32] re-enabled from the overlay");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("DLSS contract");
    static const char *kModes[] = { "Inert", "Transport test (no NGX, left half only)", "Full DLSS path" };
    if (ImGui::Combo("Mode", &g_cfg.mode, kModes, 3)) dirty = true;
    static const char *kTri[] = { "Auto", "Force off", "Force on" };
    int hdr_idx = g_cfg.hdr + 1, di_idx = g_cfg.depth_inverted + 1;
    if (ImGui::Combo("HDR", &hdr_idx, kTri, 3)) { g_cfg.hdr = hdr_idx - 1; dirty = true; }
    if (ImGui::Combo("Depth inverted", &di_idx, kTri, 3)) { g_cfg.depth_inverted = di_idx - 1; dirty = true; }
    bool reset_every = g_cfg.reset_every != 0;
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

    ImGui::Separator();
    ImGui::TextUnformatted("DLSS 5 neural-rendering settings (on the host)");
    bool uplift = g_host_nr.uplift != 0, automask = g_host_nr.automask != 0, uicorr = g_host_nr.uicorr != 0;
    ImGui::Checkbox("Neural uplift", &uplift);           g_host_nr.uplift   = uplift   ? 1 : 0;
    ImGui::SliderInt("NR intensity", &g_host_nr.intensity, 0, 5);
    ImGui::SliderInt("NR style", &g_host_nr.style, 0, 3);
    ImGui::SliderFloat("NR local structure", &g_host_nr.structure_, 0.0f, 1.0f);
    ImGui::SliderFloat("NR local tone", &g_host_nr.tone, 0.0f, 1.0f);
    ImGui::Checkbox("NR auto mask", &automask);          g_host_nr.automask = automask ? 1 : 0;
    ImGui::Checkbox("NR UI correction", &uicorr);        g_host_nr.uicorr   = uicorr   ? 1 : 0;
    if (ImGui::Button("Apply to the DLSS 5 host"))
        HostApplySettings();
    ImGui::SameLine();
    ImGui::TextDisabled("(restarts the helper process, ~2 s without DLSS)");

    if (dirty) CfgSave();
}

// ---------------------------------------------------------------------------

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
        Log("dlss5-feed32 %s (built %s %s) attached.", FEED_VERSION, __DATE__, __TIME__);
        {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            Log("  host game: %ls", exe);
        }
        CfgWriteDefault();
        CfgReload();

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
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
        reshade::unregister_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
        reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        HostClose();
        reshade::unregister_addon(module);
        Log("shut down cleanly.");
    }
    return TRUE;
}
