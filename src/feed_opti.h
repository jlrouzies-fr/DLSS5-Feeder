// feed_opti.h -- OptiScaler DLSS-NR as a neural consumer, producer side.
//
// Two OptiScaler forks run the DLSS 5 neural-rendering model (NGX feature 18, nvngx_dlssnr.dll)
// over their upscaler's output: Dagherbou/OptiScaler_DLSSNR (branch dlss-neural-rendering, the
// original) and wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass (forked from it; issue #126). They differ
// in ONE thing this header can see -- how the model is reached. Dagherbou's build loads the model
// through its own shim, nvngx.dll_dlssnr.dll (the "forwarder": the model refuses a caller whose
// path lacks nvngx.dll, and the shim's name satisfies it); wilsjo2's v0.8.1+ has no shim and
// dispatches feature 18 through the driver's NGX core, aliasing the caller path for the one call.
// Both keep the ini section, the routing answer and the model's file name; both are "the
// DLSS-NR fork" to this header, told apart as the forwarder build and the direct-runtime build.
// Unlike renodx-dlss5 and Deep Fried Chicken neither is a Detours hook over NVIDIA's _nvngx.dll:
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
// Measured 2026-09-22, same rig, driver 616.92: wilsjo2 v0.8.8 300/300, 3.68 ms (dlss), model
// loaded by evaluate 2; Dagherbou v0.2.0 again 300/300, 3.90 ms. Same routing answer from both.
//
// So this header is not a protocol but a set of checks:
//   1. is an OptiScaler build loaded in this process, under which name, and is it a DLSS-NR
//      fork -- upstream OptiScaler would take the calls, upscale, and never run a neural pass,
//      which is a silent "does nothing" the log must name. The tell is the model's file name as
//      a byte string in the DLL: both forks carry "nvngx_dlssnr.dll", upstream carries nothing
//      with dlssnr in it (source checked at upstream master 2026-08-29, both release binaries
//      scanned 2026-09-22). The forwarder's name on top of that says which generation it is;
//   2. did the NGX probe really reach it: the ROUTING fingerprint. OptiScaler answers the
//      SuperSampling requirements query with MinHWArchitecture 0 and MinOSVersion
//      10.0.10240.16384 (inputs/NVNGX_DLSS_Dx12.cpp); the driver core answers a real
//      architecture id (0x160 on this machine);
//   3. after the first evaluates, whether the neural model is in the process: the BACKEND
//      fingerprint. OptiScaler returns Success from CreateFeature even when it silently fell back
//      to FSR 2.1.2 (nvngx_dlss.dll missing), and from Evaluate on frames it skipped, so "routed"
//      alone does not prove the neural model was created. The model (nvngx_dlssnr.dll) is loaded
//      only at the first neural dispatch, so its presence once the evaluates are flowing is the
//      proof. The forwarder build does that inside the first evaluate; the direct-runtime build
//      creates the model on its own schedule (it retires and rebuilds features behind GPU
//      completion), so the check is repeated for a while before it calls the model absent.
//      Which UPSCALER ran is not observable from here -- OptiScaler
//      preloads every runtime it might use, nvngx_dlss.dll and libxess.dll included (measured) --
//      so the ini says what was asked for, and the feature-18 requirements probe says whether
//      OptiScaler's DLSS side is alive at all: it can only forward that query to the driver core
//      when nvngx_dlss.dll is beside it and the GPU is NVIDIA, the same two conditions under
//      which its dlss backend exists and its neural pass can get the core's capability block;
//   4. is OptiScaler.ini set up for a bare NGX client: [DlssNr] Enabled is off by default (both
//      releases refuse to ship it on); in the forwarder build [DlssNr] ScanExposure hooks resource
//      creation on our device looking for an exposure buffer we never offer (the direct-runtime
//      build dropped the key and deletes it from the file on save); in the direct-runtime build
//      [DlssNr] FinishedPicture moves the pass from the evaluate to Present, which on the helper
//      path is a window the game never sees; and [ProcessFilter] TargetProcessName, when it
//      names another exe, puts OptiScaler into pass-through (no hooks, no menu) -- upstream
//      behaviour, and a copy of an ini configured for a game brings that name along.
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

#define OPTI_LABEL      "OptiScaler DLSS-NR"
#define OPTI_INI        "OptiScaler.ini"
#define OPTI_NR_LITERAL "nvngx_dlssnr.dll"       // the model's file name: in every DLSS-NR build, in no upstream OptiScaler
#define OPTI_FORWARDER  "nvngx.dll_dlssnr.dll"   // the forwarder build's caller-gate shim; a build that names it loads it
#define OPTI_MIN_OS     "10.0.10240.16384"       // what OptiScaler answers as MinOSVersion for SuperSampling
#define OPTI_MAX_SCAN   (96u * 1024u * 1024u)    // OptiScaler.dll is ~26 MB
#define OPTI_MODEL_WAIT 120u                     // evaluates to keep looking for the model before calling it never created
#define OPTI_FORKS      "wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass or Dagherbou/OptiScaler_DLSSNR"

// Names OptiScaler can be installed under (its setup_windows.bat), plus its own. dxgi.dll is in
// the list for a game folder; a caller that knows dxgi.dll is ReShade skips it.
static const char *const kOptiProxyNames[] = {
    "winmm.dll", "version.dll", "dbghelp.dll", "winhttp.dll", "wininet.dll", "d3d12.dll", "dxgi.dll",
    "OptiScaler.dll", "OptiScaler.asi",
};

struct OptiInfo
{
    bool present;            // an OptiScaler build is loaded in this process
    bool nr_fork;            // its file names the neural model (or the forwarder): a DLSS-NR build
    bool direct;             // ... without the forwarder: feature 18 goes through the driver's NGX core (wilsjo2 v0.8.1+)
    bool routed;             // the NGX requirements probe answered with OptiScaler's fingerprint
    char module[64];         // the name it is loaded as ("winmm.dll")
    char path[MAX_PATH];     // its full path
    char dir[MAX_PATH];      // its folder, trailing backslash (where OptiScaler.ini lives)
#ifdef NVSDK_NGX_SUCCEED
    FeedFileIdent ident;     // its version resource (feed_ngx.h; not in the 32-bit add-on)
#endif
    // OptiScaler.ini as read: -1 = key absent or "auto" (the compiled default applies), else 0/1.
    int  nr_enabled;         // [DlssNr] Enabled              default false -- the neural pass itself
    int  scan_exposure;      // [DlssNr] ScanExposure         default false (forwarder build only)
    int  run_before_sr;      // [DlssNr] RunBeforeSR          default false (direct build only; same size either way under DLAA)
    int  finished_picture;   // [DlssNr] FinishedPicture      default false (direct build only; true moves the pass to Present)
    int  dlss_inputs;        // [Inputs] EnableDlssInputs     default true  -- the nvngx redirect itself
    int  hook_original_only; // [Hooks] HookOriginalNvngxOnly default false -- true exempts loads from the exe folder
    int  overlay_menu;       // [Menu] OverlayMenu            default true  -- must stay true
    char upscaler[32];       // [Upscalers] Dx12Upscaler      "auto" = DLSS on a capable GPU with nvngx_dlss.dll beside it
    char target_process[64]; // [ProcessFilter] TargetProcessName  "auto" = every process; another exe's name = pass-through here
};

struct OptiBackend
{
    bool     checked;     // the verdict below is final
    bool     forwarder;   // nvngx.dll_dlssnr.dll: the forwarder build's neural pass reached its caller gate
    bool     nr_created;  // nvngx_dlssnr.dll: feature 18 exists
    unsigned looks;       // how many evaluates the check has looked after
};

static inline const char *OptiTri(int v, const char *when_auto)
{
    return v == 1 ? "true" : v == 0 ? "false" : when_auto;
}

// Which generation of the fork, for the log. Both are OPTI_LABEL; the difference is the model's
// route, and it decides which ini keys exist and which module the backend check may expect.
static inline const char *OptiFlavour(bool direct)
{
    return direct ? "direct-runtime build (wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass v0.8.1+): feature 18 through the "
                    "driver's NGX core, no forwarder"
                  : "forwarder build (Dagherbou/OptiScaler_DLSSNR, or wilsjo2 before v0.8.1): feature 18 through its "
                    "nvngx.dll_dlssnr.dll shim";
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
// (OptiScaler.ini, the model's and the forwarder's file names) exist nowhere but in an
// OptiScaler build, and the last two nowhere but in a DLSS-NR build of it.
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

// Is this file an OptiScaler build at all (any fork, upstream included)?
static inline bool OptiIsBuild(const char *path)
{
    return OptiFileHasLiteral(path, OPTI_INI);
}

// Which kind: a DLSS-NR build names the model; the forwarder build also names its shim. Reads
// the file twice at most (~26 MB each); called once per candidate, at load.
static inline void OptiClassify(const char *path, bool *nr_fork, bool *direct)
{
    const bool forwarder = OptiFileHasLiteral(path, OPTI_FORWARDER);
    *nr_fork = forwarder || OptiFileHasLiteral(path, OPTI_NR_LITERAL);
    *direct  = *nr_fork && !forwarder;
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
    o->run_before_sr      = OptiIniBool(ini, "DlssNr", "RunBeforeSR");
    o->finished_picture   = OptiIniBool(ini, "DlssNr", "FinishedPicture");
    o->dlss_inputs        = OptiIniBool(ini, "Inputs", "EnableDlssInputs");
    o->hook_original_only = OptiIniBool(ini, "Hooks", "HookOriginalNvngxOnly");
    o->overlay_menu       = OptiIniBool(ini, "Menu", "OverlayMenu");
    GetPrivateProfileStringA("Upscalers", "Dx12Upscaler", "auto", o->upscaler, sizeof(o->upscaler), ini);
    if (o->upscaler[0] == '\0') strcpy_s(o->upscaler, "auto");
    GetPrivateProfileStringA("ProcessFilter", "TargetProcessName", "auto", o->target_process, sizeof(o->target_process), ini);
    if (o->target_process[0] == '\0') strcpy_s(o->target_process, "auto");
}

// The [DlssNr] keys that exist in this build, for the one log line that shows the ini.
static inline void OptiFormatNrKeys(const OptiInfo *o, char *out, size_t n)
{
    if (o->direct)
        _snprintf_s(out, n, _TRUNCATE, "RunBeforeSR=%s FinishedPicture=%s",
                    OptiTri(o->run_before_sr, "auto (= false)"), OptiTri(o->finished_picture, "auto (= false)"));
    else
        _snprintf_s(out, n, _TRUNCATE, "ScanExposure=%s", OptiTri(o->scan_exposure, "auto (= false)"));
}

// [ProcessFilter] TargetProcessName set to some other exe: OptiScaler's DllMain compares it with
// the process name (lower-cased on both sides) and goes pass-through on a mismatch -- loaded,
// exporting everything, hooking nothing. Upstream behaviour; an ini copied from a game brings
// the game's name along, and on the helper path the process is the helper.
static inline bool OptiProcessFilterMismatch(const OptiInfo *o, const char *exe_name)
{
    if (_stricmp(o->target_process, "auto") == 0) return false;
    return _stricmp(o->target_process, exe_name) != 0;
}

// The obsolete forwarder beside a direct-runtime build: nothing loads it, and the fork's install
// notes say to remove it on upgrade. Reported, not removed -- it is the user's file.
// FindFirstFile, not GetFileAttributes: OptiScaler hooks GetFileAttributesW and answers "exists"
// for any path containing nvngx.dll while its spoofing is on (so a game believes DLSS is
// installed), and this name contains it. Measured on the rig: the direct build reported a
// forwarder that was not there. Its FindFirstFile is not hooked.
static inline bool OptiStrayForwarder(const OptiInfo *o)
{
    if (!o->direct) return false;
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s" OPTI_FORWARDER, o->dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(path, &fd);
    if (f == INVALID_HANDLE_VALUE) return false;
    FindClose(f);
    return true;
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

// The backend fingerprint (point 3). Call after each evaluate that RETURNED Success, from the
// second one on, until it says it is done (`checked`): the forwarder build has the model loaded
// inside the first evaluate; the direct-runtime build may take a few more, so a miss is retried
// for OPTI_MODEL_WAIT evaluates before it counts. Logs once, at the verdict. The wording of the
// "neural model (feature 18) loaded / NOT loaded" clause is read by Verify-DLSS5Feeder.ps1.
static inline void OptiBackendCheck(void (*log)(const char *, ...), const char *tag, const char *upscaler, bool direct,
                                    OptiBackend *b)
{
    b->forwarder  = GetModuleHandleA(OPTI_FORWARDER) != nullptr;
    b->nr_created = GetModuleHandleW(L"nvngx_dlssnr.dll") != nullptr;
    ++b->looks;
    if (!b->nr_created && b->looks < OPTI_MODEL_WAIT) return;
    b->checked = true;
    log("[%s] %s after evaluate %u: %s, neural model (feature 18) %s; upscaler asked for in OptiScaler.ini: %s "
        "(which one actually ran is in OptiScaler.log, not observable from here)",
        tag, OPTI_LABEL, b->looks + 1,
        direct ? "no forwarder (none in a direct-runtime build)"
               : b->forwarder ? "neural forwarder loaded" : "neural forwarder NOT loaded",
        b->nr_created ? "loaded" : "NOT loaded", upscaler);
    if (!b->nr_created)
        log("[%s] WARNING: the neural model was never created in %u evaluates -- OptiScaler is upscaling and nothing "
            "more. Needs [DlssNr] Enabled=true in OptiScaler.ini and nvngx_dlssnr.dll beside it%s; OptiScaler.log says why",
            tag, b->looks + 1, direct ? "" : " (plus " OPTI_FORWARDER ")");
}
