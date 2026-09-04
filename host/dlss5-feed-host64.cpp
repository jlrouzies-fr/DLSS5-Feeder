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
#include <string>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_defs_dlssd.h>   // SuperSamplingDenoising.Available (DLSS Ray Reconstruction, nvngx_dlssd.dll)

#include "../src/feed_ngx.h"  // NGX result names and DLL identity, shared with the add-on
#include "../src/feed_ipc.h"
#include "../src/feed_fmt.h"
#include "../src/feed_dfc.h"   // Deep Fried Chicken interop ABI 1 (producer side, HostMode=1 here)

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static char g_log_path[MAX_PATH];
static bool g_show_window = false;   // visible host window = the user's door to the DLSS 5 panel
// --behind: the window is shown (DWM needs a shown, non-minimized source to render the
// live thumbnail the 32-bit add-on casts into the game window, dlss5-feed32's "cast")
// but as a tool window -- no taskbar button, never activated -- parked at the bottom of
// the Z-order. The game's add-on positions it under the game window and forwards the
// user's clicks to it. Banner and auto-Home run as for a visible window.
static bool g_behind = false;
// Client area of the host window and its swapchain: TALL and narrow, not a 16:9 landscape
// shape. The cast panel (dlss5-feed32's CastLayout) is right-aligned and fit to the game
// window's height, so a tall source scales up to fill the game's vertical space and stays
// a modest fraction of its width -- a menu column down the right edge, not a large overlay
// eating the middle of the screen. Set once in InitDisguise; the banner and the swapchain
// both size themselves from it.
static int  g_win_w = 620;
static int  g_win_h = 1080;

// ReShade's overlay toggle key in the host's ReShade.ini ([INPUT] KeyOverlay, Home by
// default) -- pressed once for the user a few frames after the window is up, so the
// neural consumer's panel is already open when they look at this window. Posted through
// the message queue, which is where ReShade's WH_GETMESSAGE input hook reads keys.
static UINT g_overlay_key = VK_HOME;
static int  g_pump_count  = 0;

// Present accounting (issue #15). The neural consumer wants one Present per evaluate; when
// DWM holds every back buffer the per-evaluate Present cannot happen on the spot, and the
// deficit is what makes it decline passes. A raw skip counter could not be compared against
// anything, so count what it is a fraction OF, and how much is still owed.
static unsigned long long g_present_forced  = 0;   // per-evaluate PumpPresent(true) calls
static unsigned long long g_present_skipped = 0;   // ... that found no free back buffer
static unsigned long long g_present_owed    = 0;   // ... still owed; retired from the frame and idle paths
static unsigned long long g_present_debt_run = 0;  // consecutive skips, whatever the client contract

static void Log(const char *fmt, ...);

// Height goes to (almost) the full primary monitor work area by default -- a menu column
// benefits from vertical room more than the old 16:9 shape ever gave it, and this window is
// normally parked behind the game and never seen at OS size, only cast into it (see the
// comment on g_win_w/g_win_h above). Width stays fixed by default: a settings column
// doesn't get more useful past a few hundred px, and a fixed width keeps this from
// ballooning sideways on an ultrawide monitor.
//
// Either dimension can be overridden in the host's own ReShade.ini -- [DLSS5Host]
// WindowWidth / WindowHeight, pixels, 0 (height only) meaning the default above. This is
// a REAL resize: the swapchain and the panel texture the 32-bit add-on casts are both this
// size, so ReShade's own UI actually gets more room to lay out in, not just a bigger-drawn
// copy of the same pixels -- unlike the cast panel's own "Panel size" slider, which only
// scales the picture. WindowHeight is not clamped to the monitor's height: this window is
// normally hidden behind the game (never composited on screen at OS size), so taller than
// the screen is a legitimate way to ask for more vertical room without more scrolling.
// Keys are written back with their in-use values when unset, so they show up in the ini
// for a user to find and edit without needing to know this. Runs before PrepareHostOverlay,
// which sizes the overlay layout from the result -- pass it the ini path so both functions
// don't each resolve the module's own directory.
static void FitWindowToWorkArea(const char *ini)
{
    RECT wa = {};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0) && wa.bottom > wa.top)
    {
        // The height computed here becomes a client rect; CreateWindowExW's caller
        // (InitDisguise) runs it through AdjustWindowRect for WS_OVERLAPPEDWINDOW before
        // sizing the actual window, so the outer window stays on screen in the rare case
        // this window is shown directly (not parked behind a game) -- reserve that
        // caption+border overhead here rather than after the fact.
        RECT deco = {};
        AdjustWindowRect(&deco, WS_OVERLAPPEDWINDOW, FALSE);
        g_win_h = (wa.bottom - wa.top) - (deco.bottom - deco.top);
    }
    if (g_win_h < 540) g_win_h = 540;

    const int cfg_w = GetPrivateProfileIntA("DLSS5Host", "WindowWidth", 0, ini);
    const int cfg_h = GetPrivateProfileIntA("DLSS5Host", "WindowHeight", 0, ini);
    if (cfg_w > 0) g_win_w = cfg_w < 300 ? 300 : cfg_w > 4000 ? 4000 : cfg_w;
    if (cfg_h > 0) g_win_h = cfg_h < 300 ? 300 : cfg_h > 8000 ? 8000 : cfg_h;
    g_win_w &= ~1; g_win_h &= ~1;

    char buf[16];
    if (GetPrivateProfileStringA("DLSS5Host", "WindowWidth", "", buf, sizeof(buf), ini) == 0)
    {
        sprintf_s(buf, "%d", g_win_w);
        WritePrivateProfileStringA("DLSS5Host", "WindowWidth", buf, ini);
    }
    if (GetPrivateProfileStringA("DLSS5Host", "WindowHeight", "", buf, sizeof(buf), ini) == 0)
        WritePrivateProfileStringA("DLSS5Host", "WindowHeight", "0", ini);   // 0 = auto (fill the work area)
    Log("[host] window size: %dx%d (from [DLSS5Host] WindowWidth/WindowHeight in ReShade.ini; "
        "WindowHeight=0 there means auto -- fill the work area)", g_win_w, g_win_h);
}

// Once ReShade's overlay is docked, its tabs (Home / Add-ons -- where the neural
// consumer's panel is) sit in a 335 px column with the effect editor beside it. In this
// window nobody edits shaders, so the tab column gets almost the whole width.
static void PrepareHostOverlay()
{
    char dir[MAX_PATH], ini[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char *s = strrchr(dir, '\\')) *(s + 1) = '\0';
    sprintf_s(ini, "%sReShade.ini", dir);

    FitWindowToWorkArea(ini);

    char buf[64] = {};
    GetPrivateProfileStringA("INPUT", "KeyOverlay", "36", buf, sizeof(buf), ini);
    const int k = atoi(buf);   // "36,0,0,0" -> 36; modifiers are ignored, ReShade's default has none
    if (k > 0 && k < 256) g_overlay_key = static_cast<UINT>(k);

    // Only the stock ReShade split (335 | 623 for a 960-wide window) is rewritten; a
    // layout the user dragged into shape is left exactly as ReShade saved it.
    char dock[4096] = {};
    GetPrivateProfileStringA("OVERLAY", "Docking", "", dock, sizeof(dock), ini);
    const int tabs_w = g_win_w - 96;
    if (dock[0] == '\0')
    {
        char v[4096];
        sprintf_s(v, "[Docking][Data],DockSpace   ID=0xB0DF600F Window=0xCC18005E Pos=8,,8 Size=%d,,%d Split=X,  "
                     "DockNode  ID=0x00000001 Parent=0xB0DF600F SizeRef=%d,,%d Selected=0xCBCD3A85,  "
                     "DockNode  ID=0x00000002 Parent=0xB0DF600F SizeRef=96,,%d CentralNode=1",
                  g_win_w - 16, g_win_h - 16, tabs_w, g_win_h, g_win_h);
        WritePrivateProfileStringA("OVERLAY", "Docking", v, ini);
        sprintf_s(v, "[Window][###home],Pos=8,,8,Size=%d,,%d,Collapsed=0,DockId=0x00000001,,0,"
                     "[Window][###addons],Pos=8,,8,Size=%d,,%d,Collapsed=0,DockId=0x00000001,,1,"
                     "[Window][###settings],Pos=8,,8,Size=%d,,%d,Collapsed=0,DockId=0x00000001,,2,"
                     "[Window][###statistics],Pos=8,,8,Size=%d,,%d,Collapsed=0,DockId=0x00000001,,3,"
                     "[Window][###log],Pos=8,,8,Size=%d,,%d,Collapsed=0,DockId=0x00000001,,4,"
                     "[Window][###about],Pos=8,,8,Size=%d,,%d,Collapsed=0,DockId=0x00000001,,5,"
                     "[Window][###editor],Collapsed=0,DockId=0x00000002,"
                     "[Window][Viewport],Pos=0,,0,Size=%d,,%d,Collapsed=0",
                  tabs_w, g_win_h - 16, tabs_w, g_win_h - 16, tabs_w, g_win_h - 16, tabs_w, g_win_h - 16,
                  tabs_w, g_win_h - 16, tabs_w, g_win_h - 16, g_win_w, g_win_h);
        WritePrivateProfileStringA("OVERLAY", "Window", v, ini);
        Log("[host] ReShade.ini: wrote an overlay layout with the tab column %d px wide", tabs_w);
    }
    else if (strstr(dock, "SizeRef=335,,540") != nullptr && strstr(dock, "SizeRef=623,,540") != nullptr)
    {
        std::string d(dock);
        char a[64], b[64];
        sprintf_s(a, "SizeRef=%d,,%d", tabs_w, g_win_h);
        sprintf_s(b, "SizeRef=96,,%d", g_win_h);
        d.replace(d.find("SizeRef=335,,540"), 16, a);
        d.replace(d.find("SizeRef=623,,540"), 16, b);
        WritePrivateProfileStringA("OVERLAY", "Docking", d.c_str(), ini);
        Log("[host] ReShade.ini: widened the overlay's tab column from the stock 335 px to %d px", tabs_w);
    }
    else
    {
        // A layout this host wrote for an earlier window size (its signature: the second
        // node is our 96 px editor strip) is brought to the current size; the tab column
        // then fills the window again instead of the smaller window it was sized for. A
        // layout whose split the user changed is left exactly as ReShade saved it.
        std::string d(dock);
        const size_t n1 = d.find("SizeRef="), n2 = n1 == std::string::npos ? n1 : d.find("SizeRef=96,,", n1 + 8);
        int w1 = 0, h1 = 0, h2 = 0;
        if (n1 != std::string::npos && n2 != std::string::npos &&
            sscanf_s(d.c_str() + n1, "SizeRef=%d,,%d", &w1, &h1) == 2 && sscanf_s(d.c_str() + n2, "SizeRef=96,,%d", &h2) == 1 &&
            (w1 != tabs_w || h1 != g_win_h || h2 != g_win_h))
        {
            char a[64], b[64];
            sprintf_s(a, "SizeRef=%d,,%d", tabs_w, g_win_h);
            sprintf_s(b, "SizeRef=96,,%d", g_win_h);
            const size_t e1 = d.find(' ', n1), e2 = d.find(' ', n2);
            if (e2 != std::string::npos && e1 != std::string::npos)
            {
                d.replace(n2, e2 - n2, b);   // the later one first so n1 stays valid
                d.replace(n1, e1 - n1, a);
                WritePrivateProfileStringA("OVERLAY", "Docking", d.c_str(), ini);
                Log("[host] ReShade.ini: overlay layout was sized for a %dx%d window; resized to %dx%d", w1 + 96, h1, g_win_w, g_win_h);
                return;
            }
        }
        Log("[host] ReShade.ini: overlay layout is user-arranged; leaving it alone");
    }
}
static bool g_renodx_present = false;   // renodx-dlss5.addon64 sits next to this exe
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

// Versioned copies ("renodx-dlss5-4.7.addon64") are what the channel actually ships;
// ReShade loads any *.addon64, so an exact-name check reported a working add-on as
// missing and then treated it as the classic engine. Match the prefix instead.
static bool FindRenodxAddon(const char *dir, char *out, size_t out_size)
{
    char pattern[MAX_PATH];
    sprintf_s(pattern, "%srenodx-dlss5*.addon64", dir);
    WIN32_FIND_DATAA fd = {};
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return false;
    int matches = 0;
    char first[MAX_PATH] = "";
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (++matches == 1) strcpy_s(first, fd.cFileName);
        else Log("[host] %s is ALSO in host64\\ -- ReShade loads every *.addon64, so two copies of the add-on will "
                 "both hook NGX; keep one", fd.cFileName);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    if (matches == 0) return false;
    strcpy_s(out, out_size, first);
    return true;
}

static void DetectRenodxAddon()
{
    char dir[MAX_PATH], path[MAX_PATH], ini[MAX_PATH], name[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char *s = strrchr(dir, '\\')) *(s + 1) = '\0';
    sprintf_s(ini, "%sReShade.ini", dir);
    if (!FindRenodxAddon(dir, name, sizeof(name))) { Log("[host] renodx-dlss5*.addon64 not found next to the host"); return; }
    sprintf_s(path, "%s%s", dir, name);

    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log("[host] %s is here but could not be opened (error %lu)", name, GetLastError()); return; }
    Log("[host] DLSS 5 add-on file: %s", name);
    g_renodx_present = true;
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

    // The toolkit attaches by recognising a structural layout inside the DLSS 5 add-on. That
    // signature only matches the v4.55-era build; against v4.6/v4.7 its scan reports
    // "candidates=0 (expected exactly 1)", it leaves the IAT alone and stops retrying for the
    // process, so the cascade silently does nothing. Verified with --test in one folder,
    // swapping only the add-on: v4.55 arms, v4.6 and v4.7 are both rejected.
    if (passes >= 2 && (g_renodx_v46 || g_renodx_v47))
        Log("[host] WARNING: Alex's Toolkit %s cannot attach to this DLSS 5 add-on generation -- it only "
            "recognises the v4.55-era build, and alexs-toolkit.log will say \"Generic structural layout "
            "rejected\". THE CASCADE WILL DO NOTHING. Use the v4.55-era renodx-dlss5.addon64 for the "
            "cascade, or remove alexs-toolkit.addon64 from this folder.", ver);
}

// Deep Fried Chicken next to this exe -- the alternative neural consumer (mirrors the
// section in src/dlss5-feed.cpp; keep the two in step). A 32-bit game cannot load the
// x64 Chicken, so for the companion path it lives HERE, in host64, beside ReShade x64
// and the NVIDIA DLLs, and consumes the contract this host publishes on its private
// D3D12 device. The x86 game-side add-on does not negotiate with it at all; this host
// publishes the ABI-1 marker with HostMode=1 (see feed_dfc.h).
static char g_chicken_ver[64]  = "not found";
static bool g_chicken_present  = false;
static bool g_chicken_abi      = false;
static bool g_chicken_loaded   = false;
static LONG g_chicken_state    = DFC_STATE_UNKNOWN;
// True when the live feature was created while Chicken was not yet ARMED. Chicken arms
// its NGX detours several seconds after it claims ownership (6.4 s in Fable Anniversary),
// and a Create it did not see is never adopted at Evaluate -- the feed then runs plain
// DLAA for the whole session with Chicken silently idle. Serve() re-creates once when the
// state flips to ARMED (see the warm-up block there).
static bool g_chicken_created_unarmed = false;

static void DetectChickenAddon()
{
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char *s = strrchr(dir, '\\')) *(s + 1) = '\0';
    g_chicken_present = DfcScanFile(dir, g_chicken_ver, sizeof(g_chicken_ver));
    if (!g_chicken_present) { Log("[host] Deep Fried Chicken: not present next to the host"); return; }
    Log("[host] Deep Fried Chicken %s: present next to the host -- it is the neural consumer of the synthetic DLAA "
        "contract; the DFC.Feeder.* interop marker (ABI 1, HostMode=1) is published on every Create and Evaluate, "
        "and if the first create lands before Chicken has armed its NGX detours the feature is re-created once "
        "when it does", g_chicken_ver);
    if (g_renodx_present)
        Log("[host] WARNING: Deep Fried Chicken %s and renodx-dlss5.addon64 are BOTH next to this host. Chicken "
            "replaces the RenoDX neural provider and stays inert while both are loaded. Keep dlss5-feed-host64.exe, "
            "remove one of the two neural providers from host64, then restart the game.", g_chicken_ver);
}

// Polled before each feature build: reads the ABI exports once ReShade (in this process)
// has loaded Chicken and reports the first sighting and every state change.
static void ChickenPoll()
{
    if (!g_chicken_present) return;
    unsigned int abi = 0;
    LONG state = DFC_STATE_UNKNOWN;
    bool loaded = false;
    const bool have = DfcReadExports(&abi, &state, &loaded);
    if (loaded != g_chicken_loaded)
    {
        g_chicken_loaded = loaded;
        if (!have && loaded)
            Log("[host] Deep Fried Chicken %s: loaded, but pre-1.4.0 (no interop ABI) -- legacy exact-identity "
                "fallback only", g_chicken_ver);
    }
    if (!have) return;
    if (!g_chicken_abi)
    {
        g_chicken_abi = true;
        Log("[host] Deep Fried Chicken %s: interop ABI %u (this host speaks ABI %u), feature-1 interception state %ld (%s)",
            g_chicken_ver, abi, DFC_CONTRACT_VERSION, state, DfcStateName(state));
        if (abi != DFC_CONTRACT_VERSION)
            Log("[host] WARNING: Deep Fried Chicken %s publishes interop ABI %u, this host publishes ABI %u -- Chicken "
                "will reject the marker and skip its passes (the DLAA contract itself is unaffected).",
                g_chicken_ver, abi, DFC_CONTRACT_VERSION);
    }
    if (state == g_chicken_state) return;
    g_chicken_state = state;
    if (DfcStateAvailable(state))
        Log("[host] Deep Fried Chicken %s: %s -- consuming the synthetic contract", g_chicken_ver, DfcStateName(state));
    else
        Log("[host] WARNING: Deep Fried Chicken %s reports feature-1 interception state %s -- it will not run its "
            "passes in this host. %s The host keeps serving plain DLAA. See host64\\deep-fried-chicken.log.",
            g_chicken_ver, DfcStateName(state),
            state == DFC_STATE_DISARMED ? "Its cfg has arm=0 (a restart-only hard disarm), or it has not armed yet."
          : state == DFC_STATE_CONFLICT ? "Another feature-1 consumer already owns this process (RenoDX or a second Chicken?)."
          : state == DFC_STATE_FAILED   ? "It could not create its ownership marker or arm its resolver hook."
                                        : "Unknown state value; a newer Chicken ABI than this host knows.");
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
    // GPU time for the DLSS work this helper submits. Same shape as the add-on's: two
    // timestamps per ring slot, collected a full ring later when the fence says the slot
    // is finished, so nothing ever waits for it (issue #52).
    ID3D12QueryHeap           *ts_heap;
    ID3D12Resource            *ts_read;
    UINT64                     ts_freq;
    bool                       ts_failed;
    double                     ts_sum_ms;
    unsigned                   ts_n;
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
    ID3D12Resource *out_scratch;   // FEED_BUILD_OUTPUT_NO_UAV: NGX writes here; copied into the shared Output
    UINT            width, height;          // the work size: what DLSS renders from
    UINT            out_width, out_height;  // the Output slot: == work (DLAA) or the native size (SR, v6)
    int             sr_quality;             // NVSDK_NGX_PerfQuality_Value in use when out != work
    const char     *sr_quality_name;
    DXGI_FORMAT     color_fmt, output_fmt;

    HANDLE          latency_wait;  // the disguise swapchain's frame-latency waitable object.
                                   // Held, never waited on: see PumpPresent for why waiting
                                   // on it froze the window for good (issue #33).
    bool            async_home;    // FEED_BUILD_ASYNC_HOME: the client copies home the PREVIOUS frame's
                                   // result, so it is never blocked behind the evaluate running now. Lets
                                   // PumpPresent retire a skipped present instead of dropping it (#15).

    // v7: the panel texture -- the GAME's D3D11 texture (this driver refuses the other
    // direction), opened here, into which the frame this window just presented is copied
    // after every Present, ReShade x64's overlay (the neural consumer's tuning panel)
    // included. The game draws it where DWM cannot paint a thumbnail (exclusive
    // fullscreen), unfenced: a UI layer, tearing is the worst case.
    ID3D12Resource *panel;
    bool            panel_host_owned;   // GL / Vulkan client: created here (they cannot export one)
    HANDLE          panel_local;        // ... and its handle, duplicated into the game on every build
    uint64_t        panel_size;
};

static Host h;

// ---------------------------------------------------------------------------
// Command submission (allocator ring), same shape as the add-on
// ---------------------------------------------------------------------------

// Created on first use. A queue that refuses timestamps costs one log line and nothing
// else: the feed does not depend on this.
static void TimingEnsure()
{
    if (h.ts_heap != nullptr || h.ts_failed) return;
    if (h.dev == nullptr || h.queue == nullptr) return;
    if (FAILED(h.queue->GetTimestampFrequency(&h.ts_freq)) || h.ts_freq == 0)
    { Log("[host] GPU timing unavailable: this queue reports no timestamp frequency"); h.ts_failed = true; return; }

    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = Host::kFrames * 2;
    if (FAILED(h.dev->CreateQueryHeap(&qd, __uuidof(ID3D12QueryHeap),
                                      reinterpret_cast<void **>(&h.ts_heap))) || h.ts_heap == nullptr)
    { Log("[host] GPU timing unavailable: no timestamp query heap"); h.ts_failed = true; return; }

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = Host::kFrames * 2 * sizeof(UINT64);
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // A readback resource stays in COPY_DEST for its whole life.
    if (FAILED(h.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, __uuidof(ID3D12Resource),
                                              reinterpret_cast<void **>(&h.ts_read))) || h.ts_read == nullptr)
    {
        Log("[host] GPU timing unavailable: no timestamp readback buffer");
        if (h.ts_heap != nullptr) { h.ts_heap->Release(); h.ts_heap = nullptr; }
        h.ts_failed = true;
        return;
    }
    Log("[host] GPU timing on (queue timestamp frequency %llu Hz)", (unsigned long long)h.ts_freq);
}

// Only called once the slot's fence has retired, so the map cannot block.
static void TimingCollect(int slot)
{
    if (h.ts_read == nullptr) return;
    const size_t base = static_cast<size_t>(slot) * 2;
    D3D12_RANGE  want = { base * sizeof(UINT64), (base + 2) * sizeof(UINT64) };
    void        *p    = nullptr;
    if (FAILED(h.ts_read->Map(0, &want, &p)) || p == nullptr) return;
    const UINT64 *t = static_cast<const UINT64 *>(p);
    if (t[base + 1] > t[base])
    {
        h.ts_sum_ms += 1000.0 * double(t[base + 1] - t[base]) / double(h.ts_freq);
        ++h.ts_n;
    }
    const D3D12_RANGE wrote = { 0, 0 };
    h.ts_read->Unmap(0, &wrote);
}

static bool BeginCommands()
{
    // AbortCommands releases the list and tries to make a new one; if that create failed --
    // which is most likely exactly when things are already going wrong, a removed device --
    // there is no list to reset and the h.list->Reset() below would fault. Fail the frame
    // instead: the caller CPU-signals fence_out, so the game never waits on us for it.
    if (h.list == nullptr)
    {
        static bool said = false;
        if (!said) { said = true; Log("[host] no command list (a previous NGX fault could not be recovered from)"); }
        return false;
    }
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
    // Past the fence wait: this slot's previous submission is finished, so its timestamps
    // are readable and reading them costs no synchronisation.
    if (h.alloc_fence[slot] != 0) TimingCollect(slot);
    if (FAILED(h.alloc[slot]->Reset())) return false;
    if (FAILED(h.list->Reset(h.alloc[slot], nullptr))) return false;
    TimingEnsure();
    if (h.ts_heap != nullptr) h.list->EndQuery(h.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    return true;
}

static UINT64 EndCommands()
{
    if (h.ts_heap != nullptr)
    {
        h.list->EndQuery(h.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, h.frame_slot * 2 + 1);
        h.list->ResolveQueryData(h.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, h.frame_slot * 2, 2, h.ts_read,
                                 static_cast<UINT64>(h.frame_slot) * 2 * sizeof(UINT64));
    }
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

// A removed D3D12 device never comes back: every later OpenSharedHandle fails with
// DXGI_ERROR_DEVICE_REMOVED, every create stalls, and the window (a swapchain on that
// device) stops answering. The add-on already respawns a host that exits, so the right
// answer is to say why and leave. Seen on a work-resolution change with Deep Fried
// Chicken (Fable Anniversary, 2026-09-02): DEVICE_HUNG, then ten dead rebuilds.
static bool g_device_removed = false;
static bool DeviceRemoved(const char *where)
{
    if (h.dev == nullptr) return false;
    const HRESULT reason = h.dev->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) return false;
    if (!g_device_removed)
        Log("[host] the D3D12 device was removed (0x%08X%s) during %s; exiting so the game can respawn a fresh host",
            reason, reason == DXGI_ERROR_DEVICE_HUNG ? " DEVICE_HUNG" : reason == DXGI_ERROR_DEVICE_RESET ? " DEVICE_RESET" : "", where);
    g_device_removed = true;
    return true;
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

// Deep Fried Chicken interop marker (feed_dfc.h): the complete tuple must be on the
// parameter object immediately before Create AND before every Evaluate. Ordinary
// application parameters to the driver and to RenoDX, so it is set unconditionally.
static void PublishDfcInterop()
{
    if (h.params == nullptr) return;
    h.params->Set(DFC_KEY_CONTRACT_VERSION, DFC_CONTRACT_VERSION);
    h.params->Set(DFC_KEY_PROVIDER_ID,      DFC_PROVIDER_ID_DL5F);
    h.params->Set(DFC_KEY_HOST_MODE,        DFC_HOST_MODE_COMPANION);
    h.params->Set(DFC_KEY_EVALUATE_CADENCE, DFC_EVALUATE_CADENCE);
}

static NVSDK_NGX_Result SafeCreateDLSS(NVSDK_NGX_DLSS_Create_Params *cp, DWORD *code)
{
    *code = 0;
    ChickenPoll();
    g_chicken_created_unarmed = g_chicken_present && g_chicken_state != DFC_STATE_ARMED;
    PublishDfcInterop();
    __try { return NGX_D3D12_CREATE_DLSS_EXT(h.list, 1, 1, &h.feature, h.params, cp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return static_cast<NVSDK_NGX_Result>(0x7FFFFFFF); }
}

static NVSDK_NGX_Result SafeEvaluateDLSS(NVSDK_NGX_D3D12_DLSS_Eval_Params *ep, DWORD *code)
{
    *code = 0;
    PublishDfcInterop();
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
// A second allocator/list pair for the panel copy after Present: the banner pair is still
// in flight on the GPU at that point, and neither copy may wait for the other.
static ID3D12CommandAllocator     *g_panel_alloc;
static ID3D12GraphicsCommandList  *g_panel_list;
static ID3D12Fence                *g_panel_fence;
static UINT64                      g_panel_val;
static bool                        g_panel_ready;   // the pair above exists: a panel can be served

static bool BeginCommands();
static UINT64 EndCommands();
static bool WaitFenceValue(ID3D12Fence *f, UINT64 v, DWORD ms);

static void InitBanner()
{
    const int   W = g_win_w, H = g_win_h;
    const float s = static_cast<float>(W) / 960.0f;   // the layout below was drawn for 960x540
    const auto  S = [s](int v) { return static_cast<int>(v * s + 0.5f); };

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

    HFONT fnt_big   = CreateFontW(S(64), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT fnt_small = CreateFontW(S(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, fnt_big);
    SetTextColor(dc, RGB(118, 185, 0));
    RECT r1 = { 0, S(150), W, S(240) };
    DrawTextW(dc, L"32-bit DLSS 5 Feeder", -1, &r1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, fnt_small);
    SetTextColor(dc, RGB(200, 200, 205));
    RECT r2 = { 0, S(260), W, S(300) };
    DrawTextW(dc, L"DLSS 5 neural rendering runs here for your 32-bit game.", -1, &r2,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT r3 = { 0, S(305), W, S(345) };
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

    // 4. The panel copy's own submission pair (v7); the texture itself is the game's.
    h.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                  reinterpret_cast<void **>(&g_panel_alloc));
    if (g_panel_alloc != nullptr)
        h.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_panel_alloc, nullptr,
                                 __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g_panel_list));
    if (g_panel_list != nullptr) g_panel_list->Close();
    h.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g_panel_fence));
    g_panel_ready = g_panel_list != nullptr && g_panel_fence != nullptr;
    if (g_panel_ready)
        Log("[host] panel copy ready: a D3D11 game may hand over a %dx%d texture to receive every presented frame", W, H);
    else
        Log("[host] panel copy unavailable; the game can only cast this window through the compositor");
}

// After Present: copy the buffer that was just presented -- banner plus whatever ReShade
// drew on it inside its Present hook -- into the shared panel texture. FLIP_SEQUENTIAL
// keeps that buffer's contents intact until it is handed back to us.
//
// The index arithmetic has to follow BufferCount, which is why it is read from the
// swapchain rather than written here: this used to be "(current + 1) % 2" from when the
// chain had two buffers, and it survived the move to three (issue #15's third buffer).
// With three it picks 0 or 1 arbitrarily and never 2, so the panel the 32-bit game shows
// in its cast was fed a stale buffer roughly a third of the time -- a picture that stops
// tracking the helper while the helper is perfectly healthy (issue #33).
static void CopyPanel()
{
    if (h.panel == nullptr || g_panel_list == nullptr || g_swap3 == nullptr) return;
    if (g_panel_fence->GetCompletedValue() < g_panel_val) return;   // the last copy is still running
    DXGI_SWAP_CHAIN_DESC sd = {};
    if (FAILED(g_swap3->GetDesc(&sd)) || sd.BufferCount == 0) return;
    const UINT presented = (g_swap3->GetCurrentBackBufferIndex() + sd.BufferCount - 1) % sd.BufferCount;
    ID3D12Resource *bb = nullptr;
    if (FAILED(g_swap3->GetBuffer(presented, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&bb))) || bb == nullptr)
        return;
    if (SUCCEEDED(g_panel_alloc->Reset()) && SUCCEEDED(g_panel_list->Reset(g_panel_alloc, nullptr)))
    {
        D3D12_RESOURCE_BARRIER b[2] = {};
        for (auto &x : b) { x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
        b[0].Transition.pResource = bb;      b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[1].Transition.pResource = h.panel; b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;  b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_panel_list->ResourceBarrier(2, b);
        g_panel_list->CopyResource(h.panel, bb);
        std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
        std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
        g_panel_list->ResourceBarrier(2, b);
        g_panel_list->Close();
        ID3D12CommandList *lists[] = { g_panel_list };
        h.pump_queue->ExecuteCommandLists(1, lists);
        h.pump_queue->Signal(g_panel_fence, ++g_panel_val);
    }
    bb->Release();
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
// Returns true when a Present actually went through, which is what PumpRetireOwedPresents
// needs in order to pay the debt down one at a time.
static bool PumpPresent(bool force = false)
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (h.swap == nullptr) return false;

    // Open ReShade's overlay for the user once the window is up: key down on one pump, key
    // up three pumps later, so ReShade sees a held key across a frame boundary (down and
    // up inside the same frame reads as never pressed). Only when the window is visible.
    if (g_show_window && h.hwnd != nullptr)
    {
        ++g_pump_count;
        const UINT scan = MapVirtualKeyW(g_overlay_key, MAPVK_VK_TO_VSC);
        if (g_pump_count == 90)
            PostMessageW(h.hwnd, WM_KEYDOWN, g_overlay_key, 1 | (scan << 16));
        else if (g_pump_count == 93)
        {
            PostMessageW(h.hwnd, WM_KEYUP, g_overlay_key, 1 | (scan << 16) | (1u << 30) | (1u << 31));
            Log("[host] opened ReShade's overlay (key %u) so the neural consumer's panel is in view", g_overlay_key);
        }
    }

    // Idle throttle: with no frames arriving there is nothing to show, so 30 Hz is plenty.
    // An owed present is not idle work, though -- it is a frame the consumer is missing --
    // so it goes as soon as a back buffer is free.
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (!force && g_present_owed == 0 && now - last < 33) return false;
    last = now;

    // Never wait for DWM here -- this runs between "evaluate n is done" and "read the next
    // frame message", so a wait would delay evaluate n+1 and pace the game at the
    // compositor's rhythm all over again (the 33.5 ms plateau).
    //
    // This used to ask the frame-latency waitable object, with a zero timeout, whether a
    // back buffer was free, and return early when it was not. That was a permanent-freeze
    // bug (issue #33). The object is a SEMAPHORE: a successful wait takes a count, and the
    // only thing that ever puts one back is a PRESENTED frame retiring. Both failure exits
    // below returned after the count had already been taken, so each Present that did not
    // go through spent a count that never came back -- and SetMaximumFrameLatency(2) means
    // there are only two. Two failed presents over the life of the process drained it to
    // zero for good, after which every check here failed, PumpRetireOwedPresents failed the
    // same check, and the window never repainted again while the feed carried on evaluating.
    // That is exactly the report: responsive window, frozen picture, healthy feed, forever.
    //
    // DXGI_PRESENT_DO_NOT_WAIT already gives us the non-blocking guarantee this needs, and
    // answers DXGI_ERROR_WAS_STILL_DRAWING when there is no free buffer -- the same
    // information, from the call itself, with nothing to leak. So we ask Present, not the
    // semaphore. The waitable flag stays on the swapchain for its latency behaviour; the
    // handle is simply never waited on.
    if (force) ++g_present_forced;   // counted here so "skipped of forced" is always consistent

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
    const HRESULT hr = h.swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (FAILED(hr))
    {
        // One accounting for every reason a present did not go through. WAS_STILL_DRAWING
        // is the ordinary one (DWM still holds every back buffer); anything else used to
        // return here in complete silence, so a window frozen by a repeating INVALID_CALL
        // or a device reset left nothing whatsoever in the log to read (issue #33).
        if (force) ++g_present_skipped;
        // A dropped present is not free: the neural consumer keys its per-frame state to
        // Present, so two evaluates then share one frame of it and it declines a pass
        // (issue #15's "toggling on and off"). It becomes a DEBT rather than a drop.
        if (force && h.async_home && g_present_owed < 4) ++g_present_owed;

        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            if (g_present_skipped == 1 || (g_present_skipped % 1800) == 0)
                Log("[host] present skipped: DWM still holds the back buffer (%llu of %llu per-evaluate "
                    "presents, %llu owed; the game is never made to wait for it)",
                    (unsigned long long)g_present_skipped, (unsigned long long)g_present_forced,
                    (unsigned long long)g_present_owed);
        }
        else
        {
            static unsigned long long hard    = 0;
            static HRESULT            last_hr = S_OK;
            const bool                fresh   = (hr != last_hr);
            last_hr = hr;
            if (++hard == 1 || fresh || (hard % 1800) == 0)
                Log("[host] Present failed 0x%08X (%llu so far). This window holds its last picture until "
                    "one succeeds; the feed runs on a separate queue and is unaffected.",
                    hr, (unsigned long long)hard);
        }

        // A rising skip count on its own reads as benign, because it usually is. A long
        // UNBROKEN run is the state that is not: from there the window has stopped
        // repainting and the consumer is missing frames. Counted for every client, not
        // only the pipelined ones -- a same-frame client owes no debt, and so used to
        // reach this state with fewer clues in its log rather than more (issue #33).
        if (++g_present_debt_run == 120)
            Log("[host] 120 presents in a row could not go through: this window has stopped repainting and "
                "the consumer is missing frames. The feed itself is unaffected; a window that looks frozen "
                "from here is this, not a hang.");
        return false;
    }
    // DXGI_STATUS_OCCLUDED is a SUCCESS code, so it falls through the test above: the
    // present was accepted, but nothing of it reaches the screen. That is the normal state
    // for a helper parked at HWND_BOTTOM under a fullscreen game, and worth naming once
    // rather than leaving a reader to infer it from a picture that never changes.
    if (hr == DXGI_STATUS_OCCLUDED)
    {
        static bool said_occluded = false;
        if (!said_occluded)
        {
            said_occluded = true;
            Log("[host] this window is occluded (the game is in front of it): presents are accepted but "
                "nothing is drawn until it is raised. The feed is unaffected.");
        }
    }
    g_present_debt_run = 0;   // a present went through; the window is repainting again
    CopyPanel();
    return true;
}

// Retire presents the per-evaluate call could not make, from the serve loop's idle path.
// The game is not waiting on us there -- we are blocked on its next frame message -- so a
// present here is the one that costs nothing, and it is what keeps the neural consumer's
// per-Present state at one frame per evaluate (issue #15). The debt is only paid down here:
// a per-evaluate present is the frame THAT evaluate is entitled to, not a repayment.
// Bounded by g_present_owed, so this can never present more often than evaluates asked for.
static void PumpRetireOwedPresents()
{
    // No frame-latency wait here either, for the reason spelled out in PumpPresent: a wait
    // that is not followed by a present spends a semaphore count nothing gives back, and
    // this loop's whole job is to keep trying when presents are failing (issue #33).
    // PumpPresent already returns false without blocking when there is no free buffer.
    while (g_present_owed > 0 && h.swap != nullptr)
    {
        if (!PumpPresent(false)) break;   // no buffer after all, or DXGI busy: try again later
        --g_present_owed;
    }
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

    // g_win_w/h were fitted to the work area in PrepareHostOverlay; size the OUTER window so
    // the client area -- and the swapchain -- is exactly that.
    RECT frame = { 0, 0, g_win_w, g_win_h };
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    h.hwnd = CreateWindowExW(g_behind ? (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) : 0, wc.lpszClassName,
                             L"DLSS 5 Feed host - press Home HERE to tune DLSS 5 neural rendering",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             frame.right - frame.left, frame.bottom - frame.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    Log("[host] window: %dx%d client (tall and narrow, fit to the work area height)%s", g_win_w, g_win_h,
        g_behind ? ", behind the game (tool window, no taskbar button)" : "");
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
    sd.Width            = static_cast<UINT>(g_win_w);
    sd.Height           = static_cast<UINT>(g_win_h);
    sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Three buffers, latency 2. Two-and-one frees the waitable object at most once per
    // vblank, so above the desktop's refresh rate one present per evaluate is not merely
    // unlikely, it is impossible -- a 72 fps game on a 60 Hz desktop must drop roughly one
    // present in six no matter what. Each drop makes two evaluates share one frame of the
    // neural consumer's per-Present state, and the consumer answers by declining a pass:
    // the frame-by-frame "neural rendering toggling on and off" of issue #15. The third
    // buffer is what makes one-per-evaluate reachable; it costs one window-sized surface.
    sd.BufferCount      = 3;
    // SEQUENTIAL, not DISCARD: the buffer just presented must keep its contents so
    // CopyPanel can lift ReShade's overlay off it afterwards (the v7 panel texture).
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // Waitable: Present on this chain must never block. It is called once per evaluate, and
    // the game's next frame waits on fence_out behind that evaluate -- so a Present that DWM
    // holds until the next vblank (the window is occluded by a fullscreen game, or the
    // present queue is full) paced the GAME at the compositor's rhythm: the rigid 33.5 ms /
    // 30 fps plateaus of issue #15 on 32-bit DXVK, with the feed's own CPU cost at 0.1 ms.
    // The flag is kept for that latency behaviour. PumpPresent does NOT wait on the object
    // it produces, and no longer polls it either: DXGI_PRESENT_DO_NOT_WAIT is what actually
    // guarantees the non-blocking present, and a zero-timeout poll spent semaphore counts
    // that only a presented frame gives back -- which froze the window for good (issue #33).
    sd.Flags            = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    hr = factory->CreateSwapChainForHwnd(h.pump_queue, h.hwnd, &sd, nullptr, nullptr, &h.swap);
    if (SUCCEEDED(hr))
    {
        IDXGISwapChain2 *swap2 = nullptr;
        if (SUCCEEDED(h.swap->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&swap2))) && swap2 != nullptr)
        {
            swap2->SetMaximumFrameLatency(2);
            h.latency_wait = swap2->GetFrameLatencyWaitableObject();
            swap2->Release();
        }
        if (h.latency_wait == nullptr)
            Log("[host] no frame-latency waitable object; this chain queues presents the ordinary way "
                "(DXGI_PRESENT_DO_NOT_WAIT still keeps them off the game's thread)");
    }
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

// Which GPU this helper actually landed on. It creates its device on DXGI's DEFAULT adapter
// while the 64-bit add-on creates its private device on the GAME's adapter, and neither side
// logged an adapter identity -- so a report where the helper and the game are on different
// GPUs was indistinguishable from one where they are not (issue #47).
static void LogHostAdapter()
{
    if (h.dev == nullptr) return;
    const LUID luid = h.dev->GetAdapterLuid();
    wchar_t desc[128] = L"(unnamed)";
    UINT vendor = 0, device = 0;
    char driver[32] = "?";
    IDXGIFactory1 *f = nullptr;
    // GetProcAddress like InitDisguise does: this exe carries no dxgi import, so that the
    // app-directory dxgi.dll (which is ReShade) is loaded in the order a real game gives it.
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    auto create_factory = dxgi != nullptr
        ? reinterpret_cast<PFN_CreateDXGIFactory1_>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (create_factory != nullptr &&
        SUCCEEDED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&f))) && f != nullptr)
    {
        IDXGIAdapter1 *a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 ad = {};
            a->GetDesc1(&ad);
            if (ad.AdapterLuid.LowPart == luid.LowPart && ad.AdapterLuid.HighPart == luid.HighPart)
            {
                wcscpy_s(desc, ad.Description);
                vendor = ad.VendorId;
                device = ad.DeviceId;
                // Same decode as the add-on's LogAdapterIdentity: the last five digits of
                // the quad's last two components are NVIDIA's branded version.
                LARGE_INTEGER umd = {};
                if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                {
                    const unsigned sub_v = HIWORD(umd.LowPart), bld = LOWORD(umd.LowPart);
                    const unsigned n     = (sub_v * 10000u + bld) % 100000u;
                    sprintf_s(driver, "%u.%02u", n / 100u, n % 100u);
                }
                a->Release();
                break;
            }
            a->Release();
        }
        f->Release();
    }
    Log("[host] device adapter: %ls  LUID %08lX:%08lX  PCI %04X:%04X  driver %s (DXGI's default adapter)",
        desc, (unsigned long)luid.HighPart, (unsigned long)luid.LowPart, vendor, device, driver);
}

// Ask NGX which of the adapter, the driver or the OS it is objecting to. Same question the
// add-on asks through its own NgxAskWhy; kept symmetrical on purpose, because issue #47 is
// built on comparing what the two sides report.
static void NgxAskWhy(const wchar_t *data_path, const NVSDK_NGX_FeatureCommonInfo *info)
{
    if (h.dev == nullptr) return;
    IDXGIAdapter  *ad = nullptr;
    IDXGIFactory4 *f4 = nullptr;
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    auto make_factory = dxgi != nullptr
        ? reinterpret_cast<PFN_CreateDXGIFactory1_>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (make_factory != nullptr &&
        SUCCEEDED(make_factory(__uuidof(IDXGIFactory4), reinterpret_cast<void **>(&f4))) && f4 != nullptr)
    {
        f4->EnumAdapterByLuid(h.dev->GetAdapterLuid(), __uuidof(IDXGIAdapter),
                              reinterpret_cast<void **>(&ad));
        f4->Release();
    }
    FeedLogNgxFeatureRequirements(&Log, "host", ad, data_path, info);
    if (ad != nullptr) ad->Release();
}

static bool InitNgx()
{
    wchar_t data_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, data_path, MAX_PATH);
    if (wchar_t *s = wcsrchr(data_path, L'\\')) *(s + 1) = L'\0';

    LogHostAdapter();
    // Say where NGX is being pointed, and whether it can write there. NGX puts its own logs
    // in this folder, and an install under Program Files is not writable without elevation
    // -- which nothing checked or reported (issue #47).
    {
        wchar_t probe[MAX_PATH];
        _snwprintf_s(probe, _TRUNCATE, L"%sdlss5-feed-ngx-probe.tmp", data_path);
        HANDLE t = CreateFileW(probe, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        const bool writable = t != INVALID_HANDLE_VALUE;
        if (writable) CloseHandle(t);
        Log("[host] NGX application data path: %ls (%s)", data_path, writable ? "writable" : "NOT WRITABLE");
    }

    {
        char dir8[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, data_path, -1, dir8, MAX_PATH, nullptr, nullptr);
        FeedLogNgxRuntimes(&Log, "host", dir8);
    }

    // A PathListInfo, which this side passed as nullptr while the add-on passed one. That
    // was the last structural difference between the two call sites, and issue #47 is built
    // entirely on comparing them -- so it should not be a difference at all.
    const wchar_t *const search[1] = { data_path };
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path   = search;
    info.PathListInfo.Length = 1;

    // Before the attempt, not only after a failure: a machine where NGX works records what
    // it answers here too, and the working cases are the control the failing ones need.
    NgxAskWhy(data_path, &info);

    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, data_path, h.dev, &info, NVSDK_NGX_Version_API);
    Log("[host] NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", r, NgxResultName(r));
    if (NVSDK_NGX_FAILED(r))
    {
        r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                "1.0", data_path, h.dev, &info, NVSDK_NGX_Version_API);
        Log("[host] Init_with_ProjectID -> 0x%08X (%s)", r, NgxResultName(r));
    }
    if (NVSDK_NGX_FAILED(r))
    {
        // The helper failing here is the case the add-on's data-path theory does NOT explain
        // (issue #47, case C: same files, same driver, the helper itself returns
        // FeatureNotSupported). Leave a reader everything needed to tell the two apart.
        static const wchar_t *kMods[] = { L"_nvngx.dll", L"nvngx.dll", L"nvngx_dlss.dll", L"nvngx_dlssnr.dll" };
        for (const wchar_t *m : kMods)
        {
            HMODULE mh = GetModuleHandleW(m);
            if (mh == nullptr) continue;
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(mh, path, MAX_PATH);
            Log("[host] NGX module loaded: %ls -> %ls", m, path);
        }
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0,
                          KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS)
        {
            DWORD installed = 0, cb = sizeof(installed), type = 0;
            if (RegQueryValueExW(k, L"NGXCoreInstalled", nullptr, &type, reinterpret_cast<BYTE *>(&installed), &cb) != ERROR_SUCCESS)
            { cb = sizeof(installed); RegQueryValueExW(k, L"Installed", nullptr, &type, reinterpret_cast<BYTE *>(&installed), &cb); }
            wchar_t full[MAX_PATH] = {};
            cb = sizeof(full);
            RegQueryValueExW(k, L"FullPath", nullptr, &type, reinterpret_cast<BYTE *>(full), &cb);
            Log("[host] NGX Core: Installed=%lu FullPath=%ls", (unsigned long)installed,
                full[0] != L'\0' ? full : L"(unset)");
            RegCloseKey(k);
        }
        else
            Log("[host] NGX Core: the HKLM NGXCore key could not be opened -- the driver's NGX runtime may not be installed");

        return false;
    }
    h.ngx_inited = true;

    NVSDK_NGX_Parameter *caps = nullptr;
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_SUCCEED(r) && caps != nullptr)
    {
        int avail = 0, denoise = 0, needs_driver = 0, maj = 0, min_v = 0;
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
        caps->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &denoise);
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &maj);
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_v);
        // SuperSamplingDenoising is DLSS Ray Reconstruction (nvngx_dlssd.dll), NOT the
        // DLSS 5 neural rendering this project feeds -- that is NGX feature 18, backed by
        // nvngx_dlssnr.dll, and no capability parameter reports on it. Kept as the cheapest
        // way to see how much of the DLSS family NGX has here; feature 18 is asked about
        // properly by FeedLogNgxFeatureRequirements on the failure path.
        Log("[host] NGX capabilities: SuperSampling.Available=%d SuperSamplingDenoising.Available=%d "
            "NeedsUpdatedDriver=%d MinDriver=%d.%d", avail, denoise, needs_driver, maj, min_v);
        if (!avail) return false;
    }
    r = NVSDK_NGX_D3D12_AllocateParameters(&h.params);
    if (NVSDK_NGX_FAILED(r) || h.params == nullptr) { Log("[host] AllocateParameters failed 0x%08X", r); return false; }
    return true;
}

// work_upscale=2 (v6): find the DLSS quality preset whose dynamic render range contains
// the work size for this output size. DLSS accepts any render size inside [min, max] of
// the chosen preset. Fills h.sr_quality(_name); false when no preset covers the ratio.
static bool PickSrQuality(UINT w, UINT h_, UINT out_w, UINT out_h)
{
    static const struct { NVSDK_NGX_PerfQuality_Value q; const char *name; } kOrder[] = {
        { NVSDK_NGX_PerfQuality_Value_UltraQuality,     "Ultra Quality" },
        { NVSDK_NGX_PerfQuality_Value_MaxQuality,       "Quality" },
        { NVSDK_NGX_PerfQuality_Value_Balanced,         "Balanced" },
        { NVSDK_NGX_PerfQuality_Value_MaxPerf,          "Performance" },
        { NVSDK_NGX_PerfQuality_Value_UltraPerformance, "Ultra Performance" },
    };
    // The optimal-settings callback lives on the CAPABILITY parameter object only; an
    // AllocateParameters object answers every preset with "no callback" (the first Fable
    // run: every query failed and SR silently fell back to DLAA).
    NVSDK_NGX_Parameter *caps = nullptr;
    const NVSDK_NGX_Result rc = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_FAILED(rc) || caps == nullptr) { Log("[host] GetCapabilityParameters failed 0x%08X; cannot pick an SR preset", rc); return false; }
    for (const auto &o : kOrder)
    {
        unsigned opt_w = 0, opt_h = 0, max_w = 0, max_h = 0, min_w = 0, min_h = 0;
        float sharp = 0.0f;
        const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(caps, out_w, out_h, o.q,
                                                                 &opt_w, &opt_h, &max_w, &max_h, &min_w, &min_h, &sharp);
        if (NVSDK_NGX_FAILED(r) || opt_w == 0 || opt_h == 0)
        { Log("[host] DLSS %s at %ux%u: not offered (0x%08X, optimal %ux%u)", o.name, out_w, out_h, r, opt_w, opt_h); continue; }
        Log("[host] DLSS %s at %ux%u: optimal %ux%u, render range %ux%u .. %ux%u",
            o.name, out_w, out_h, opt_w, opt_h, min_w, min_h, max_w, max_h);
        if (w >= min_w && w <= max_w && h_ >= min_h && h_ <= max_h)
        {
            h.sr_quality      = static_cast<int>(o.q);
            h.sr_quality_name = o.name;
            return true;
        }
    }
    return false;
}

// target 0 = DLAA (render == output). Otherwise DLSS Super Resolution at the preset
// PickSrQuality chose; the caller made sure it did.
static bool CreateFeature(UINT w, UINT h_, int flags, NVSDK_NGX_Result *out_r, UINT target_w = 0, UINT target_h = 0)
{
    const bool sr = target_w != 0 && target_h != 0 && (target_w != w || target_h != h_);
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth            = w;
    cp.Feature.InHeight           = h_;
    cp.Feature.InTargetWidth      = sr ? target_w : w;
    cp.Feature.InTargetHeight     = sr ? target_h : h_;
    cp.Feature.InPerfQualityValue = sr ? static_cast<NVSDK_NGX_PerfQuality_Value>(h.sr_quality) : NVSDK_NGX_PerfQuality_Value_DLAA;
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
    if (sr) Log("[host] feature ready: %ux%u -> %ux%u DLSS %s (synthetic jitter) flags=%d", w, h_, target_w, target_h, h.sr_quality_name, flags);
    else    Log("[host] feature ready: %ux%u DLAA flags=%d", w, h_, flags);
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
                     UINT w, UINT h_, int reset, float mvsx, float mvsy, float jitter_x = 0.0f, float jitter_y = 0.0f)
{
    if (!BeginCommands()) return false;

    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor  = color;
    ep.Feature.pInOutput = output;
    ep.pInDepth          = depth;
    ep.pInMotionVectors  = mv;
    ep.InJitterOffsetX   = jitter_x;   // render-pixel units; the game's synthetic grid shift (v6), 0 under DLAA
    ep.InJitterOffsetY   = jitter_y;
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
    if (NVSDK_NGX_SUCCEED(re) && output == h.out_scratch && h.out_scratch != nullptr)
    {
        // The game's device cannot open a UAV texture: NGX wrote the private scratch,
        // and the shared Output (no UAV flag) receives a copy on the same list. The
        // scratch was promoted to UNORDERED_ACCESS by the evaluate; the shared target is
        // promoted to COPY_DEST implicitly (SIMULTANEOUS_ACCESS), and both decay to
        // COMMON when this submission completes.
        D3D12_RESOURCE_BARRIER bar = {};
        bar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource   = h.out_scratch;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        h.list->ResourceBarrier(1, &bar);
        h.list->CopyResource(h.tex[FEED_OUTPUT], h.out_scratch);
    }
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
                                         HANDLE *out_handle, uint64_t *out_size, bool render_target = false)
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
    // ALLOW_RENDER_TARGET is what a D3D11 opener needs for its work-resolution resample
    // RTVs (D3D11 derives its bind flags from these); GL/Vulkan importers do not care.
    // It also has to be set on any slot a D3D11 opener sees that carries no UAV either --
    // see the FEED_SLOTS loop in Serve() and issue #43.
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                          (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE) |
                          (render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
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
    // One line per slot with the exact flags. Issue #43 came down to which of four
    // otherwise identical textures was born without a bind flag, and no log said.
    Log("[host]   shared %ux%u %s d3d12 flags=0x%X (%s%ssimultaneous)", w, h_, FeedFmtName(fmt),
        static_cast<unsigned>(rd.Flags), uav ? "UAV " : "", render_target ? "RT " : "");
    return t;
}

static int RunTest()
{
    const UINT W = 640, H = 360;
    Log("[host] --test: %ux%u synthetic DLAA", W, H);
    // Also prove the SR preset query answers (work_upscale=2 depends on it): 50% of 1080p.
    Log("[host] --test: SR preset probe for 960x540 -> 1920x1080: %s",
        PickSrQuality(960, 540, 1920, 1080) ? h.sr_quality_name : "none (the optimal-settings query failed)");

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
    // What those evaluates actually cost on the GPU. The rig is the one place this can be
    // checked against a known workload before anyone reads it in a bug report (issue #52).
    if (h.ts_n > 0)
        Log("[host] --test: DLSS GPU %.2f ms/frame over %u timed frames at %dx%d",
            h.ts_sum_ms / double(h.ts_n), h.ts_n, W, H);
    else
        Log("[host] --test: no GPU timing was collected");
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
//
// This runs on the window thread -- the host has exactly one thread, and it owns the
// window, the pipe and the D3D12 queue alike. So the wait cannot be the plain
// GetOverlappedResult(..., TRUE) it used to be: the pipe buffer is 1024 bytes, a
// FeedBuild is written in pieces, and a game that stalls between them froze the window
// outright with no upper bound (issue #33). Pump the message queue while waiting, and
// give up after kTransferStallMs so a wedged client costs us a reconnect rather than a
// hung window. Not presenting here is deliberate: this is called during the hello, before
// there is a swapchain to present on.
static const DWORD kTransferStallMs = 30000;

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

        const ULONGLONG deadline = GetTickCount64() + kTransferStallMs;
        bool timed_out = false;
        for (;;)
        {
            const ULONGLONG now  = GetTickCount64();
            const DWORD     wait = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
            const DWORD     r    = MsgWaitForMultipleObjects(1, &ev, FALSE, wait, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_OBJECT_0 + 1)
            {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
                continue;
            }
            timed_out = true;   // WAIT_TIMEOUT, or the wait itself failed
            break;
        }
        if (timed_out)
        {
            Log("[host] the game stalled mid-message: %lu of %lu bytes %s after %lu ms; dropping the connection",
                (unsigned long)(len - left), (unsigned long)len, write ? "written" : "read",
                (unsigned long)kTransferStallMs);
            CancelIoEx(pipe, &ov);
            GetOverlappedResult(pipe, &ov, &moved, TRUE);   // the cancel completes it; do not leak the OVERLAPPED
            ok = false;
            break;
        }
        if (!GetOverlappedResult(pipe, &ov, &moved, FALSE) || moved == 0) { ok = false; break; }
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
    if (g_panel_ready) { ack.panel_width = static_cast<uint32_t>(g_win_w); ack.panel_height = static_cast<uint32_t>(g_win_h); }
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

    // These two are the whole synchronisation contract. Ignoring the result meant a failure
    // (a process handle without PROCESS_DUP_HANDLE, say) still sent the game an ack saying
    // ok=1 with two null fence handles, and it went looking for the fault everywhere except
    // here. Fail the session instead; the add-on respawns a host.
    HANDLE game_in = nullptr, game_out = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), hin, hgame, &game_in, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), hout, hgame, &game_out, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        Log("[host] could not duplicate the shared fences into the game (error %lu); nothing could be "
            "synchronised, so this host exits", GetLastError());
        return 1;
    }

    int flags_active = 0;
    bool transport_only = false;
    float mvsx = 1.0f, mvsy = 1.0f;
    // The DLSS 5 add-on arms its NGX hooks ~150 ms after NGX init; the first create must
    // not race that (a 15 ms miss latched STANDBY in Blacklist), so hold it briefly.
    UINT64 hold_until = GetTickCount64() + 800;
    UINT64 evaluated  = 0;
    bool   warm_done  = g_renodx_lazy;   // v45+ adopts missed creates on its own; Chicken: see the build below
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
            // Idle: the game has not sent the next frame yet, so it is not waiting on us.
            // The one moment a Present costs it nothing -- pay off anything the
            // per-evaluate call could not present (issue #15).
            if (r == WAIT_OBJECT_0 + 1 || r == WAIT_TIMEOUT) { PumpRetireOwedPresents(); PumpPresent(); continue; }
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

            // Tear down the old set -- after the GPU is done with it. The last evaluate may
            // still be in flight, and a neural consumer (Deep Fried Chicken) keeps a graph
            // alive on the feature until its own fence; releasing the feature and the
            // textures under that work hung the GPU on a work-resolution change (Fable
            // Anniversary, 2026-09-02: the next create never completed, DEVICE_HUNG). The
            // warm-up re-create below has always drained first; this path now does too.
            //
            // Unconditionally, not just when a feature exists: in transport mode there is no
            // feature but the last frame's CopyTextureRegion out of h.tex[] can still be in
            // flight, and the release below would pull the source out from under it. The wait
            // returns at once on an idle queue, so it costs nothing when there is nothing to
            // wait for.
            if (!WaitFenceValue(h.fence, h.fence_value, 2000))
                Log("[host] rebuild: the previous frame's GPU work did not retire within 2 s");
            SafeReleaseFeature(h.feature);
            h.feature = nullptr;
            for (int i = 0; i < FEED_SLOTS; ++i)
                if (h.tex[i] != nullptr) { h.tex[i]->Release(); h.tex[i] = nullptr; }
            if (h.out_scratch != nullptr) { h.out_scratch->Release(); h.out_scratch = nullptr; }

            bool ok = true;
            uint64_t game_tex[FEED_SLOTS] = {}, tex_size[FEED_SLOTS] = {}, game_panel = 0;
            // A D3D11 client whose device refused the shared set asks for the GL/Vulkan
            // route per build (v5, issue #33); one that cannot bind UAVs at all gets an
            // Output without one, and NGX writes a private scratch that is copied over.
            const bool host_creates_b = host_creates || (b.client_flags & FEED_BUILD_HOST_CREATES) != 0;
            const bool no_uav         = host_creates_b && (b.client_flags & FEED_BUILD_OUTPUT_NO_UAV) != 0;
            const bool d3d11_opener   = host_creates_b && !host_creates;
            // Whether the client copies home the previous frame's result. Decides whether a
            // present the per-evaluate call could not make becomes a debt or a drop (#15).
            const bool was_async = h.async_home;
            h.async_home = (b.client_flags & FEED_BUILD_ASYNC_HOME) != 0;
            static bool said_handoff = false;
            if (h.async_home != was_async || !said_handoff)
            {
                said_handoff = true;
                Log("[host] client handoff: %s", h.async_home
                    ? "pipelined (async_home=1): a present DWM defers is retired from the idle path"
                    : "same frame (async_home=0): a present DWM defers is dropped");
            }
            if (d3d11_opener)
                Log("[host] the game's D3D11 device could not create the shared set; creating it here%s",
                    no_uav ? " with the DLSS output's UAV kept on this side" : "");
            // The Output format is the host's call whenever the host creates the
            // textures -- only this process has the D3D12 device that can be asked
            // about typed UAV stores. In transport mode there is no UAV at all and
            // the copy is Color -> Output on this side, so the two must stay equal.
            DXGI_FORMAT out_fmt = static_cast<DXGI_FORMAT>(b.output_fmt);
            if (host_creates_b && b.transport == 0) out_fmt = ResolveOutputFormatHost(out_fmt);
            // v6: an Output larger than the work size means DLSS Super Resolution. Settle the
            // preset before any texture exists, so a ratio no preset covers costs nothing.
            const bool want_sr = b.target_width != 0 && b.target_height != 0 &&
                                 (b.target_width != b.width || b.target_height != b.height) && b.transport == 0;
            const UINT out_w = want_sr ? b.target_width : b.width, out_h = want_sr ? b.target_height : b.height;
            bool sr_unavailable = false;
            if (want_sr && !PickSrQuality(b.width, b.height, out_w, out_h))
            {
                Log("[host] work_upscale=2: no DLSS preset covers %ux%u -> %ux%u; telling the game to rebuild as DLAA", b.width, b.height, out_w, out_h);
                sr_unavailable = true;
                ok = false;
            }
            if (host_creates_b)
            {
                // OpenGL / Vulkan client: WE create, and duplicate the handles into the
                // game, which imports them (PLAN-OPENGL §5 design A; PLAN-VULKAN32 §2).
                const DXGI_FORMAT fmt[FEED_SLOTS] = {
                    static_cast<DXGI_FORMAT>(b.color_fmt), out_fmt,
                    DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT };
                for (int i = 0; i < FEED_SLOTS && ok; ++i)
                {
                    HANDLE local = nullptr;
                    // ALLOW_RENDER_TARGET on every slot a D3D11 opener will see -- the Output
                    // included when its UAV has moved to the private scratch below (no_uav).
                    // The old `i != FEED_OUTPUT` assumed the Output always carried the UAV
                    // flag; with no_uav it carried neither, so it was the only resource in the
                    // set born with SIMULTANEOUS_ACCESS and nothing else, and the only one a
                    // feature-level 10_0 device refused to open (issue #43: Color opened, the
                    // Output came back E_INVALIDARG at identical size and format). The game
                    // never makes an RTV on the Output -- only the SRV it reads home from -- so
                    // the extra bind is unused there, and free.
                    h.tex[i] = MakeSharedTexHost(i == FEED_OUTPUT ? out_w : b.width, i == FEED_OUTPUT ? out_h : b.height,
                                                 fmt[i], i == FEED_OUTPUT && !no_uav, &local, &tex_size[i],
                                                 d3d11_opener && (i != FEED_OUTPUT || no_uav));
                    if (h.tex[i] == nullptr) { ok = false; break; }
                    HANDLE remote = nullptr;
                    if (!DuplicateHandle(GetCurrentProcess(), local, hgame, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
                    { Log("[host] DuplicateHandle(tex %d into the game) failed %lu", i, GetLastError()); ok = false; }
                    CloseHandle(local);   // the duplicate stands on its own; the resource keeps the memory
                    game_tex[i] = reinterpret_cast<uint64_t>(remote);
                }
                // v7: the panel texture for a client that cannot export one -- created once
                // per session, a fresh duplicate of its handle on every build. Never fatal.
                if (ok && g_panel_ready)
                {
                    if (h.panel == nullptr)
                    {
                        // Same rule as the slot loop: a D3D11 opener needs a bind flag on it,
                        // or CastAdoptHostPanel11 would hit the issue-#43 wall the moment the
                        // build got this far.
                        h.panel = MakeSharedTexHost(static_cast<UINT>(g_win_w), static_cast<UINT>(g_win_h), DXGI_FORMAT_R8G8B8A8_UNORM,
                                                    false, &h.panel_local, &h.panel_size, d3d11_opener);
                        h.panel_host_owned = h.panel != nullptr;
                        if (h.panel != nullptr) Log("[host] panel texture created for the game (%dx%d): every presented frame is copied into it", g_win_w, g_win_h);
                    }
                    HANDLE remote = nullptr;
                    if (h.panel != nullptr && h.panel_local != nullptr &&
                        DuplicateHandle(GetCurrentProcess(), h.panel_local, hgame, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
                        game_panel = reinterpret_cast<uint64_t>(remote);
                }
                if (ok)
                    Log("[host] created and handed over %d shared textures (%ux%u, color %s, output %s)",
                        FEED_SLOTS, b.width, b.height, FeedFmtName(static_cast<DXGI_FORMAT>(b.color_fmt)),
                        FeedFmtName(out_fmt));
                if (ok && no_uav && b.transport == 0)
                {
                    // Private, UAV-capable, same format: NGX evaluates into this and Evaluate()
                    // copies it into the shared Output the game can open. SIMULTANEOUS_ACCESS so
                    // its state decays to COMMON after every submission, like the shared set.
                    D3D12_HEAP_PROPERTIES hp = {};
                    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                    D3D12_RESOURCE_DESC rd = {};
                    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    rd.Width            = out_w;
                    rd.Height           = out_h;
                    rd.DepthOrArraySize = 1;
                    rd.MipLevels        = 1;
                    rd.Format           = out_fmt;
                    rd.SampleDesc.Count = 1;
                    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
                    const HRESULT hr = h.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                                      nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&h.out_scratch));
                    if (FAILED(hr)) { Log("[host] output scratch texture failed 0x%08X", hr); ok = false; }
                }
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
                    if (FAILED(hr)) { Log("[host] OpenSharedHandle(tex %d) failed 0x%08X", i, hr); ok = false; continue; }

                    // Check what actually arrived against what the message claims. Everything
                    // downstream -- the NGX create, InRenderSubrectDimensions, the transport
                    // copy box -- trusts b.width/height/formats, so a texture that does not
                    // match them is a device-removed or a corrupt frame several steps later,
                    // with nothing pointing back here. The panel has always been checked this
                    // way; the four slots that matter were not.
                    const D3D12_RESOURCE_DESC td = h.tex[i]->GetDesc();
                    const UINT want_w = (i == FEED_OUTPUT) ? out_w : b.width;
                    const UINT want_h = (i == FEED_OUTPUT) ? out_h : b.height;
                    const DXGI_FORMAT want_fmt =
                        i == FEED_COLOR  ? static_cast<DXGI_FORMAT>(b.color_fmt) :
                        i == FEED_OUTPUT ? out_fmt :
                        i == FEED_DEPTH  ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
                    const bool needs_uav = (i == FEED_OUTPUT) && !no_uav && b.transport == 0;
                    if (td.Width != static_cast<UINT64>(want_w) || td.Height != want_h || td.Format != want_fmt ||
                        (needs_uav && (td.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0))
                    {
                        Log("[host] the game's %s texture is %ux%u fmt=%u flags=0x%X, not %ux%u fmt=%u%s -- refusing the build",
                            i == FEED_COLOR ? "Color" : i == FEED_OUTPUT ? "Output" : i == FEED_DEPTH ? "Depth" : "MV",
                            static_cast<unsigned>(td.Width), td.Height, td.Format, td.Flags,
                            want_w, want_h, want_fmt, needs_uav ? " with a UAV" : "");
                        ok = false;
                    }
                }
            }

            // v7: a D3D11 game's panel texture, opened the same way. Never fatal for the build.
            // CopyPanel runs on pump_queue and tracks its own fence, which the drain at the top
            // of this rebuild does not cover -- so wait for it here or the release below can
            // pull the destination out from under a copy that is still running.
            if (h.panel != nullptr && !h.panel_host_owned && g_panel_fence != nullptr)
                WaitFenceValue(g_panel_fence, g_panel_val, 500);
            if (h.panel != nullptr && !h.panel_host_owned) { h.panel->Release(); h.panel = nullptr; }
            if (b.panel_tex != 0 && g_panel_ready && !h.panel_host_owned)
            {
                HANDLE local = nullptr;
                if (!DuplicateHandle(hgame, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(b.panel_tex)),
                                     GetCurrentProcess(), &local, 0, FALSE, DUPLICATE_SAME_ACCESS))
                    Log("[host] DuplicateHandle(panel) failed %lu; no in-game panel texture", GetLastError());
                else
                {
                    const HRESULT hr = h.dev->OpenSharedHandle(local, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&h.panel));
                    CloseHandle(local);
                    if (FAILED(hr)) { Log("[host] OpenSharedHandle(panel) failed 0x%08X; no in-game panel texture", hr); h.panel = nullptr; }
                    else
                    {
                        const D3D12_RESOURCE_DESC pd = h.panel->GetDesc();
                        if (pd.Width != static_cast<UINT64>(g_win_w) || pd.Height != static_cast<UINT>(g_win_h) ||
                            pd.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
                        {
                            Log("[host] the game's panel texture is %ux%u fmt=%u, not %dx%d RGBA8; ignoring it",
                                static_cast<unsigned>(pd.Width), pd.Height, pd.Format, g_win_w, g_win_h);
                            h.panel->Release(); h.panel = nullptr;
                        }
                        else
                            Log("[host] panel texture opened: every presented frame is copied into it");
                    }
                }
            }

            NVSDK_NGX_Result rf = NVSDK_NGX_Result_Fail;
            if (ok)
            {
                h.width = b.width; h.height = b.height;
                h.out_width = out_w; h.out_height = out_h;
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
                    ok = CreateFeature(b.width, b.height, flags_active, &rf, out_w, out_h);
                    hold_until = GetTickCount64() + 1000;   // next create not before +1 s

                    if (ok) build_fails = 0;
                    else if (++build_fails >= 2 && ReinitNgx())
                    {
                        Log("[host] retrying the create after an NGX reinit");
                        ok = CreateFeature(b.width, b.height, flags_active, &rf, out_w, out_h);
                        if (ok) build_fails = 0;
                    }
                }
            }

            evaluated = 0;
            // No warm-up without NGX, with v45+, or when Chicken already had its detours ARMED
            // at this create (then it saw it). Otherwise the block below waits for ARMED.
            warm_done = transport_only || g_renodx_lazy || (g_chicken_present && !g_chicken_created_unarmed);

            FeedBuildAck back = {};
            back.ok         = ok ? 1 : 0;
            back.ngx_result = static_cast<uint32_t>(rf);
            if (sr_unavailable)   back.flags |= FEED_ACK_SR_UNAVAILABLE;
            else if (ok && want_sr) { back.flags |= FEED_ACK_SR_ACTIVE; back.sr_quality = static_cast<uint32_t>(h.sr_quality); }
            back.fence_in   = reinterpret_cast<uint64_t>(game_in);
            back.fence_out  = reinterpret_cast<uint64_t>(game_out);
            back.output_fmt = static_cast<uint32_t>(out_fmt);
            for (int i = 0; i < FEED_SLOTS; ++i) { back.tex[i] = game_tex[i]; back.tex_size[i] = tex_size[i]; }
            back.panel_tex  = game_panel;
            back.panel_size = game_panel != 0 ? h.panel_size : 0;
            WriteFull(pipe, &back, sizeof(back));
            if (!ok && DeviceRemoved("a rebuild")) break;   // the ack went out; retrying here is pointless
        }
        else if (tag == 'F')
        {
            FeedFrameMsg fm = {};
            if (!ReadFull(pipe, &fm, sizeof(fm))) break;
            // No feature yet: release the game's wait and take the next frame. The pump
            // still has to run here. This loop's only other pumps are the per-evaluate
            // PumpPresent below and the idle branch of the tag wait, and with frames
            // arriving at game rate the tag wait never goes idle -- so a bare `continue`
            // left the window unpumped for as long as the feature was missing, and
            // Windows ghosts it as "Not Responding" within seconds while the game runs on.
            if (h.feature == nullptr && !transport_only) { h.fence_out->Signal(fm.n); PumpPresent(); continue; }

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
                done = Evaluate(h.tex[FEED_COLOR], h.out_scratch != nullptr ? h.out_scratch : h.tex[FEED_OUTPUT],
                                h.tex[FEED_DEPTH], h.tex[FEED_MV],
                                h.width, h.height, fm.reset ? 1 : 0, mvsx, mvsy, fm.jitter_x, fm.jitter_y);

            if (done)
            {
                h.queue->Signal(h.fence_out, fm.n);
                // One warm-up re-create per build. RenoDX: it misses the very first create
                // (STANDBY latch) when its hooks armed a moment too late, so re-create at a
                // fixed frame count. Chicken: it arms its detours seconds after claiming, and
                // never adopts a create it did not see -- so poll its exported state every
                // frame and re-create the moment it reads ARMED (900 frames as a backstop).
                bool warm_fire = false;
                const char *warm_why = "the DLSS 5 add-on misses the very first create";
                if (!warm_done)
                {
                    ++evaluated;
                    if (g_chicken_present)
                    {
                        ChickenPoll();
                        if (g_chicken_state == DFC_STATE_ARMED)
                        { warm_fire = true; warm_why = "Deep Fried Chicken armed its NGX detours after our first create"; }
                        else if (evaluated >= 900)
                        { warm_fire = true; warm_why = "Deep Fried Chicken still not ARMED after 900 frames -- re-creating anyway; "
                                                       "if it stays idle, see host64\\deep-fried-chicken.log"; }
                    }
                    else warm_fire = evaluated >= 180;
                }
                if (warm_fire)
                {
                    warm_done = true;
                    Log("[host] warm-up: re-creating the feature once (%s)", warm_why);
                    WaitFenceValue(h.fence, h.fence_value, 2000);
                    NVSDK_NGX_Handle *old = h.feature;
                    h.feature = nullptr;
                    NVSDK_NGX_Result rr = NVSDK_NGX_Result_Fail;
                    if (CreateFeature(h.width, h.height, flags_active, &rr, h.out_width, h.out_height)) SafeReleaseFeature(old);
                    else { h.feature = old; Log("[host] keeping the previous feature"); }
                }
            }
            else
            {
                h.fence_out->Signal(fm.n);     // CPU-signal so the game never hangs on us
                if (DeviceRemoved("an evaluate")) break;
            }

            if (fm.n <= 3 || (fm.n % 1800) == 0)
            {
                // The GPU cost of the DLSS work, which is the number a "performance loss"
                // report actually needs and which nothing here used to measure (issue #52).
                char gpu_part[64] = "";
                if (h.ts_n > 0)
                {
                    sprintf_s(gpu_part, ", DLSS GPU %.2f ms/frame", h.ts_sum_ms / double(h.ts_n));
                    h.ts_sum_ms = 0.0;
                    h.ts_n = 0;
                }
                Log("[host] frame %llu evaluated (%llu presents skipped so far, %llu owed%s)",
                    (unsigned long long)fm.n, (unsigned long long)g_present_skipped,
                    (unsigned long long)g_present_owed, gpu_part);
            }
            // Pay off what earlier evaluates could not present BEFORE taking this one's own
            // present. The idle branch of the tag wait above is the other repayment point,
            // but it only runs when MsgWaitForMultipleObjects times out after 100 ms -- so
            // in a game delivering frames every ~20 ms it never runs at all, and the debt
            // sat at its cap for the whole session (issue #33). Here the game is already
            // behind fence_out and is not waiting on us, which is the same argument.
            PumpRetireOwedPresents();
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
    if (!g_device_removed) WaitFenceValue(h.fence, h.fence_value, 2000);   // nothing to wait for on a dead device
    if (h.fence_out != nullptr) h.fence_out->Signal(UINT64_MAX);
    Log("[host] pending game fence waits released; exiting%s", g_device_removed ? " (device removed)" : "");
    return g_device_removed ? 3 : 0;
}

// ReShade x64 writes its ini -- the overlay layout the user arranged, the neural
// consumer's own settings -- when its effect runtime is destroyed, which happens when the
// swapchain's last reference goes. It never does so for a process that simply exits
// around a live swapchain (its log then says "Add-ons are still loaded!"), which is why
// the host's layout was forgotten on every restart. So take the disguise down properly.
static void ShutdownDisguise()
{
    if (h.swap == nullptr) return;
    if (!g_device_removed)
    {
        if (g_pump_fence  != nullptr) WaitFenceValue(g_pump_fence,  g_pump_val,  500);
        if (g_panel_fence != nullptr) WaitFenceValue(g_panel_fence, g_panel_val, 500);
    }
    auto rel = [](IUnknown *&p) { if (p != nullptr) { p->Release(); p = nullptr; } };
    rel(reinterpret_cast<IUnknown *&>(g_panel_list));  rel(reinterpret_cast<IUnknown *&>(g_panel_alloc));
    rel(reinterpret_cast<IUnknown *&>(g_panel_fence)); rel(reinterpret_cast<IUnknown *&>(g_pump_list));
    rel(reinterpret_cast<IUnknown *&>(g_pump_alloc));  rel(reinterpret_cast<IUnknown *&>(g_pump_fence));
    rel(reinterpret_cast<IUnknown *&>(g_banner));      rel(reinterpret_cast<IUnknown *&>(h.panel));
    rel(reinterpret_cast<IUnknown *&>(g_swap3));
    rel(reinterpret_cast<IUnknown *&>(h.swap));   // ReShade's runtime goes with it, and saves
    MSG msg;
    for (int i = 0; i < 20; ++i)
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (h.hwnd != nullptr) { DestroyWindow(h.hwnd); h.hwnd = nullptr; }
    Log("[host] disguise released (ReShade saves its ini here)");
}

// ---------------------------------------------------------------------------

// Same shape as the add-ons' filter: the crash goes in the log with the faulting
// module, and a minidump lands next to it (dbghelp loaded on demand).
typedef BOOL (WINAPI *PFN_MiniDumpWriteDump_)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
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
    Log("### CRASH RECORDED ###  exception 0x%08X at %p in %ls%s", code, addr, owner,
        mod == GetModuleHandleW(nullptr) ? " (inside this host)" : "");

    char path[MAX_PATH];
    strcpy_s(path, g_log_path);
    if (char *s = strrchr(path, '\\')) strcpy_s(s + 1, MAX_PATH - (s + 1 - path), "dlss5-feed-host-crash.dmp");
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    auto write = dbghelp ? reinterpret_cast<PFN_MiniDumpWriteDump_>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    HANDLE f = write != nullptr ? CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
                                : INVALID_HANDLE_VALUE;
    if (f != INVALID_HANDLE_VALUE)
    {
        struct { DWORD tid; EXCEPTION_POINTERS *ep; BOOL client; } info = { GetCurrentThreadId(), ep, FALSE };
        const int type = 0x0040 | 0x0001 | 0x0004;   // IndirectlyReferencedMemory | DataSegs | HandleData
        const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep != nullptr ? &info : nullptr, nullptr, nullptr);
        CloseHandle(f);
        Log(ok ? "[host] crash dump written: %s -- attach it to the issue with this log"
               : "[host] crash dump FAILED (%s, error %lu)", path, GetLastError());
    }
    else
        Log("[host] no crash dump written (dbghelp %s, error %lu)", write ? "present" : "missing", GetLastError());
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char **argv)
{
    // A run with no arguments is somebody double-clicking this exe to find out what it is --
    // and it used to answer by TRUNCATING the log of the run they were trying to explain,
    // because the log is opened "w" as the first thing main does. Say what this is and
    // leave, touching nothing (issue #46: the attached host log turned out to be a log of
    // the investigation rather than of the fault).
    if (argc < 2)
    {
        MessageBoxA(nullptr,
                    "This is the 64-bit helper for the DLSS 5 Feed ReShade add-on.\r\n\r\n"
                    "It is started by the add-on from inside the game -- there is nothing to run here.\r\n"
                    "Its log is dlss5-feed-host.log, next to this file; this run has left it alone.",
                    "DLSS 5 Feed helper", MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    GetModuleFileNameA(nullptr, g_log_path, MAX_PATH);
    if (char *s = strrchr(g_log_path, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - g_log_path), "dlss5-feed-host.log");
    // An install under Program Files is not writable without elevation, and both fopen_s
    // calls used to fail in silence -- CREATE_NO_WINDOW leaves no console for the duplicate
    // printf either, so the helper ran completely mute. Fall back to LocalAppData, and log
    // the path we settled on so nobody reads a stale file.
    {
        FILE *f = nullptr;
        if (fopen_s(&f, g_log_path, "w") == 0 && f) fclose(f);
        else
        {
            char fallback[MAX_PATH] = {};
            size_t n = 0;
            if (getenv_s(&n, fallback, MAX_PATH, "LOCALAPPDATA") == 0 && n > 1)
            {
                strcat_s(fallback, MAX_PATH, "\\DLSS5-Feeder");
                CreateDirectoryA(fallback, nullptr);
                strcat_s(fallback, MAX_PATH, "\\dlss5-feed-host.log");
                FILE *g = nullptr;
                if (fopen_s(&g, fallback, "w") == 0 && g) { fclose(g); strcpy_s(g_log_path, MAX_PATH, fallback); }
            }
        }
    }
    SetUnhandledExceptionFilter(&CrashFilter);

    Log("dlss5-feed-host64 (built %s %s)", __DATE__, __TIME__);
    // Logged before it is parsed, so a log that ends at the usage line says WHY: "no
    // arguments" and "an argument zeroed the pid" used to be indistinguishable (issue #46).
    Log("[host] command line (argc=%d): %s", argc, GetCommandLineA());
    Log("[host] log file: %s", g_log_path);

    // Every Sleep in this process is a frame-pacing decision: at the default 15.6 ms
    // timer tick a Sleep(1) lands at 15.6 ms, and the serve loop's poll alone used to
    // cap the game near 35 fps (issue #15). No timeEndPeriod -- this is a dedicated
    // helper, and process exit restores the resolution anyway.
    timeBeginPeriod(1);

    bool  test = false, hide = false, behind = false;
    DWORD pid = 0;
    for (int i = 1; i < argc; ++i)
    {
        if      (strcmp(argv[i], "--test") == 0) test = true;
        else if (strcmp(argv[i], "--hide") == 0) hide = true;
        else if (strcmp(argv[i], "--behind") == 0) behind = true;
        // First numeric token wins. This used to be a bare assignment, so ANY later token
        // the parser did not recognise ran through strtoul, came back 0, and silently
        // overwrote an already-parsed pid -- turning a good command line into the usage
        // exit with nothing in the log to say which argument did it.
        else if (pid == 0 && (pid = static_cast<DWORD>(strtoul(argv[i], nullptr, 10))) != 0) {}
        else Log("[host] ignoring an argument I do not understand: %s", argv[i]);
    }
    if (!test && pid == 0)
    {
        Log("usage: dlss5-feed-host64 --test | dlss5-feed-host64 <game pid> [--hide | --behind]");
        return 1;
    }
    g_show_window = !test && !hide;   // the visible window carries the DLSS 5 add-on's tuning panel
    g_behind      = g_show_window && behind;

    DetectRenodxAddon();   // must run BEFORE ReShade loads, so an EnableHooks write is read
    DetectToolkitAddon();
    DetectChickenAddon();   // after DetectRenodxAddon: it needs g_renodx_present
    DetectStaleD3DCompiler();
    PrepareHostOverlay();   // edits ReShade.ini, so also BEFORE ReShade loads (InitDisguise)

    // Both failures used to `return 1` straight out, skipping the tail below -- and a failed
    // NGX init is exactly the case where ReShade is already loaded and its teardown is the
    // thing that hangs. Everything leaves through one door now.
    int rc = 1;
    if (!InitDisguise())
        Log("[host] the disguise swapchain could not be created");
    else if (!InitNgx())
        Log("[host] NGX unavailable");
    else
        rc = test ? RunTest() : Serve(pid);

    ShutdownDisguise();
    // Everything that had to happen has happened: ReShade wrote its ini when its runtime
    // went with the swapchain above. What is left is ReShade's own DLL teardown (unhooking
    // every module it patched), which has been seen to hang after the window was gone --
    // WormsXHD, 2026-09-02: "disguise released" logged, the process still alive a minute
    // later, and the game, stuck in its own exit-time crash loop, never came to kill it.
    // A helper that outlives its game is worse than skipped unhooking at exit.
    Log("[host] exit %d", rc);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));
    return rc;
}
