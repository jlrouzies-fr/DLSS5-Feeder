// feed_opti.h -- OptiScaler DLSS-NR as a neural consumer, producer side.
//
// Dagherbou/OptiScaler_DLSSNR (branch dlss-neural-rendering) is an OptiScaler fork that runs the
// DLSS 5 neural-rendering model (NGX feature 18, nvngx_dlssnr.dll) over its upscaler's output.
// Unlike renodx-dlss5 and Deep Fried Chicken it is not a Detours hook over NVIDIA's _nvngx.dll:
// it IS the NGX implementation the process talks to. Installed the normal OptiScaler way --
// renamed to a DLL the process imports (winmm.dll or version.dll; ReShade owns dxgi.dll) -- it is
// loaded at process start, and its LoadLibrary hooks hand its own module to anything that asks
// for nvngx.dll or _nvngx.dll. The statically linked NGX SDK in this project asks for exactly
// that on its first call, so every NVSDK_NGX_D3D12_* call lands in OptiScaler with no indirection
// on this side: OptiScaler runs its upscaler (dlss, the backend we ask for) on the DLAA contract
// and then its neural pass, in place, on the Output texture.
//
// Measured 2026-09-07 with the host --test rig (OptiScaler-DLSSNR-v0.2.0 as winmm.dll, driver
// 616.64, RTX 5090): 300/300 evaluates; DLSS alone 0.25 ms/frame at 640x360, with the neural pass
// 3.4 ms (dlss), 3.1 ms (xess), 2.1 ms (fsr31) -- the pass runs whatever the upscaler is.
//
// So this header is not a protocol but a set of checks:
//   1. is an OptiScaler build loaded in this process, under which name, and is it the DLSS-NR
//      fork -- upstream OptiScaler would take the calls, upscale, and never run a neural pass,
//      which is a silent "does nothing" the log must name;
//   2. did the NGX probe really reach it: the ROUTING fingerprint. OptiScaler answers the
//      SuperSampling requirements query with MinHWArchitecture 0 and MinOSVersion
//      10.0.10240.16384 (inputs/NVNGX_DLSS_Dx12.cpp); the driver core answers a real
//      architecture id (0x160 on this machine);
//   3. after the first evaluate, whether the neural model is in the process: the BACKEND
//      fingerprint. OptiScaler returns Success from CreateFeature even when it silently fell back
//      to FSR 2.1.2 (nvngx_dlss.dll missing), and from Evaluate on frames it skipped, so "routed"
//      alone does not prove the neural model was created. The model (nvngx_dlssnr.dll) and its
//      forwarder are loaded only at the first neural dispatch, so their presence after the first
//      evaluate is the proof. Which UPSCALER ran is not observable from here -- OptiScaler
//      preloads every runtime it might use, nvngx_dlss.dll and libxess.dll included (measured) --
//      so the ini says what was asked for, and the feature-18 requirements probe says whether
//      OptiScaler's DLSS side is alive at all: it can only forward that query to the driver core
//      when nvngx_dlss.dll is beside it and the GPU is NVIDIA, the same two conditions under
//      which its dlss backend exists and its neural pass can get the core's capability block;
//   4. is OptiScaler.ini set up for a bare NGX client: [DlssNr] Enabled is off by default (its
//      release refuses to ship it on), and [DlssNr] ScanExposure hooks resource creation on our
//      device looking for an exposure buffer we never offer.
//
// Exactly one consumer, as always -- but here it is a hard rule rather than a quality note. With
// OptiScaler's redirect live, Deep Fried Chicken's own deep-fried-chicken-nvngx.dll (which ends in
// nvngx.dll) and renodx-dlss5's own _nvngx.dll load are handed OptiScaler too, and OptiScaler's
// dlss backend calls the real core, where their detours would fire a second neural pass.
//
// Nothing here writes to OptiScaler.ini except OptiIniDefault, which only ever replaces a key
// that is absent or "auto", and says so -- OptiScaler reads its ini in DllMain, before any of
// this runs, so a write is for the NEXT launch.
//
// Requires <windows.h>. With nvsdk_ngx.h and feed_ngx.h included first (the 64-bit add-on and the
// host) it also carries the version identity and the routing fingerprint; the 32-bit add-on has
// neither the NGX headers nor version.lib and includes it for the file scan and the ini reader
// only. Header-only and static: all three translation units are single-file builds.

#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define OPTI_LABEL     "OptiScaler DLSS-NR"
#define OPTI_INI       "OptiScaler.ini"
#define OPTI_FORWARDER "nvngx.dll_dlssnr.dll"   // the fork's caller-gate shim; its name is also the literal that marks the fork
#define OPTI_MIN_OS    "10.0.10240.16384"        // what OptiScaler answers as MinOSVersion for SuperSampling
#define OPTI_MAX_SCAN  (96u * 1024u * 1024u)     // OptiScaler.dll is ~25 MB

// Names OptiScaler can be installed under (its setup_windows.bat), plus its own. dxgi.dll is in
// the list for a game folder; a caller that knows dxgi.dll is ReShade skips it.
static const char *const kOptiProxyNames[] = {
    "winmm.dll", "version.dll", "dbghelp.dll", "winhttp.dll", "wininet.dll", "d3d12.dll", "dxgi.dll",
    "OptiScaler.dll", "OptiScaler.asi",
};

struct OptiInfo
{
    bool present;            // an OptiScaler build is loaded in this process
    bool nr_fork;            // its file carries the DLSS-NR forwarder literal
    bool routed;             // the NGX requirements probe answered with OptiScaler's fingerprint
    char module[64];         // the name it is loaded as ("winmm.dll")
    char path[MAX_PATH];     // its full path
    char dir[MAX_PATH];      // its folder, trailing backslash (where OptiScaler.ini lives)
#ifdef NVSDK_NGX_SUCCEED
    FeedFileIdent ident;     // its version resource (feed_ngx.h; not in the 32-bit add-on)
#endif
    // OptiScaler.ini as read: -1 = key absent or "auto" (the compiled default applies), else 0/1.
    int  nr_enabled;         // [DlssNr] Enabled              default false -- the neural pass itself
    int  scan_exposure;      // [DlssNr] ScanExposure         default false
    int  dlss_inputs;        // [Inputs] EnableDlssInputs     default true  -- the nvngx redirect itself
    int  hook_original_only; // [Hooks] HookOriginalNvngxOnly default false -- true exempts loads from the exe folder
    int  overlay_menu;       // [Menu] OverlayMenu            default true  -- must stay true
    char upscaler[32];       // [Upscalers] Dx12Upscaler      "auto" = DLSS on a capable GPU with nvngx_dlss.dll beside it
};

struct OptiBackend
{
    bool checked;
    bool forwarder;   // nvngx.dll_dlssnr.dll: the neural pass reached its caller gate
    bool nr_created;  // nvngx_dlssnr.dll: feature 18 exists
};

static inline const char *OptiTri(int v, const char *when_auto)
{
    return v == 1 ? "true" : v == 0 ? "false" : when_auto;
}

// The loaded module, if any. OptiScaler exports the NGX entry points AND the DXGI factory
// functions from one module whatever it is named; NVIDIA's core exports no DXGI, and no DXGI
// (ReShade's included) exports NGX. Fills module/path/dir.
static inline bool OptiFindModule(OptiInfo *o)
{
    typedef BOOL (WINAPI *PFN_EnumMods)(HANDLE, HMODULE *, DWORD, LPDWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_EnumMods enum_mods = k32 != nullptr
        ? reinterpret_cast<PFN_EnumMods>(GetProcAddress(k32, "K32EnumProcessModules")) : nullptr;
    if (enum_mods == nullptr) return false;
    HMODULE mods[1024];
    DWORD   need = 0;
    if (!enum_mods(GetCurrentProcess(), mods, sizeof(mods), &need)) return false;
    DWORD n = need / sizeof(HMODULE);
    if (n > 1024) n = 1024;
    for (DWORD i = 0; i < n; ++i)
    {
        HMODULE m = mods[i];
        if (GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature") == nullptr) continue;
        if (GetProcAddress(m, "CreateDXGIFactory2") == nullptr) continue;   // the driver core, not OptiScaler
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(m, path, MAX_PATH) == 0) continue;
        strcpy_s(o->path, path);
        const char *slash = strrchr(path, '\\');
        strcpy_s(o->module, slash != nullptr ? slash + 1 : path);
        strcpy_s(o->dir, path);
        if (char *s = strrchr(o->dir, '\\')) s[1] = '\0';
        return true;
    }
    return false;
}

// Does the FILE contain this byte string? A plain substring: the names this is used for
// (the forwarder's file name, OptiScaler.ini) exist nowhere but in an OptiScaler build.
static inline bool OptiFileHasLiteral(const char *path, const char *needle)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    const DWORD size = GetFileSize(f, nullptr);
    char *buf = (size > 0 && size < OPTI_MAX_SCAN) ? static_cast<char *>(malloc(size)) : nullptr;
    DWORD got = 0;
    if (buf != nullptr && ReadFile(f, buf, size, &got, nullptr) && got == size)
    {
        const DWORD n = static_cast<DWORD>(strlen(needle));
        for (DWORD i = 0; n > 0 && i + n <= size && !found; ++i)
            if (buf[i] == needle[0] && memcmp(buf + i, needle, n) == 0) found = true;
    }
    free(buf);
    CloseHandle(f);
    return found;
}

// One boolean key: -1 when absent or "auto".
static inline int OptiIniBool(const char *ini, const char *section, const char *key)
{
    char v[32] = {};
    GetPrivateProfileStringA(section, key, "", v, sizeof(v), ini);
    if (v[0] == '\0' || _stricmp(v, "auto") == 0) return -1;
    if (_stricmp(v, "true") == 0 || strcmp(v, "1") == 0) return 1;
    if (_stricmp(v, "false") == 0 || strcmp(v, "0") == 0) return 0;
    return -1;
}

// The keys that decide whether a bare NGX client gets a neural pass out of OptiScaler at all.
static inline void OptiReadIni(OptiInfo *o)
{
    char ini[MAX_PATH];
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s" OPTI_INI, o->dir);
    o->nr_enabled         = OptiIniBool(ini, "DlssNr", "Enabled");
    o->scan_exposure      = OptiIniBool(ini, "DlssNr", "ScanExposure");
    o->dlss_inputs        = OptiIniBool(ini, "Inputs", "EnableDlssInputs");
    o->hook_original_only = OptiIniBool(ini, "Hooks", "HookOriginalNvngxOnly");
    o->overlay_menu       = OptiIniBool(ini, "Menu", "OverlayMenu");
    GetPrivateProfileStringA("Upscalers", "Dx12Upscaler", "auto", o->upscaler, sizeof(o->upscaler), ini);
    if (o->upscaler[0] == '\0') strcpy_s(o->upscaler, "auto");
}

// Write a key into OptiScaler.ini only when it is absent or "auto" -- a value the user (or
// OptiScaler's own menu, which saves the whole file) set explicitly always wins. Returns true
// when something was written. `log` is the caller's own Log(), so each side keeps its prefix.
static inline bool OptiIniDefault(const OptiInfo *o, const char *section, const char *key, const char *value,
                                  const char *why, void (*log)(const char *, ...), const char *tag)
{
    char ini[MAX_PATH], v[32] = {};
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s" OPTI_INI, o->dir);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES)
    {
        log("[%s] OptiScaler.ini is not beside %s -- nothing to set [%s] %s in; OptiScaler will write its own at exit",
            tag, o->module, section, key);
        return false;
    }
    GetPrivateProfileStringA(section, key, "", v, sizeof(v), ini);
    if (v[0] != '\0' && _stricmp(v, "auto") != 0)
    {
        log("[%s] OptiScaler.ini [%s] %s=%s (user-set; leaving it alone)", tag, section, key, v);
        return false;
    }
    if (!WritePrivateProfileStringA(section, key, value, ini))
    {
        log("[%s] OptiScaler.ini [%s] %s could not be written (error %lu) -- set %s=%s by hand",
            tag, section, key, GetLastError(), key, value);
        return false;
    }
    log("[%s] OptiScaler.ini [%s] %s was %s; wrote %s=%s (%s). OptiScaler reads its ini when it loads, so this "
        "takes effect at the NEXT launch", tag, section, key, v[0] != '\0' ? "auto" : "unset", key, value, why);
    return true;
}

// The routing fingerprint (see the header comment, point 2). Needs the SuperSampling probe to have
// answered, which FeedLogNgxFeatureRequirements records in the verdict.
#ifdef NVSDK_NGX_SUCCEED
static inline bool OptiRouted(const FeedNgxVerdict &v)
{
    return v.asked && NVSDK_NGX_SUCCEED(v.ss_query) && v.ss_min_arch == 0 && strcmp(v.ss_min_os, OPTI_MIN_OS) == 0;
}
#endif

// The backend fingerprint (point 3). Call once, after the first evaluate has RETURNED: the
// neural pass loads its forwarder and creates feature 18 on the CPU inside that first evaluate,
// so both modules are in the process by then if they ever will be.
static inline void OptiBackendCheck(void (*log)(const char *, ...), const char *tag, const char *upscaler,
                                    OptiBackend *b)
{
    b->checked    = true;
    b->forwarder  = GetModuleHandleA(OPTI_FORWARDER) != nullptr;
    b->nr_created = GetModuleHandleW(L"nvngx_dlssnr.dll") != nullptr;
    log("[%s] %s after the first evaluate: neural forwarder %s, neural model (feature 18) %s; upscaler asked for in "
        "OptiScaler.ini: %s (which one actually ran is in OptiScaler.log, not observable from here)",
        tag, OPTI_LABEL, b->forwarder ? "loaded" : "NOT loaded", b->nr_created ? "loaded" : "NOT loaded", upscaler);
    if (!b->nr_created)
        log("[%s] WARNING: the neural model was never created -- OptiScaler is upscaling and nothing more. Needs "
            "[DlssNr] Enabled=true in OptiScaler.ini, and nvngx_dlssnr.dll plus %s beside it; OptiScaler.log says why",
            tag, OPTI_FORWARDER);
}
