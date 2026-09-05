// feed_ngx.h -- things the 64-bit add-on and the host64 helper must say identically about
// NGX. Both open an NGX session against a D3D12 device, and until now each carried its own
// copy of everything below.
//
// That duplication was not free. The NVSDK_NGX_Result name table existed twice, kept in
// step by a comment, and the single most-reported failure -- 0xBAD00001 out of Init -- read
// as "(?)" in the helper's log while the add-on named it, which cost a round trip in issue
// #47. The file-version reader existed twice too, welded inside each side's RenoDX add-on
// detection, so neither could say anything at all about the NGX runtimes themselves.
//
// Requires <windows.h>, nvsdk_ngx.h and version.lib. Header-only and static: both
// translation units are single-file builds.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------------------
// NVSDK_NGX_Result -> name.
//
// Every code in nvsdk_ngx_defs.h, including 0xBAD00000 (Result_Fail) -- which is what the
// add-on's SEH wrapper returns when Init raises, and which used to print as "?" on both
// sides, so a FAULT during init was indistinguishable from an unknown status code.
// ---------------------------------------------------------------------------------------
static const char *NgxResultName(NVSDK_NGX_Result r)
{
    switch (static_cast<unsigned>(r))
    {
    case 0x1:        return "Success";
    case 0xBAD00000: return "Fail";
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

// ---------------------------------------------------------------------------------------
// Who a DLL says it is.
//
// The version number alone does not identify an NGX runtime. NVIDIA's own nvngx_dlssnr.dll
// and ShortFuse's .SF repack of it both report file version 310.8.0.0, and issue #47 turns
// on telling them apart: three machines that fail NGX init are all loading the repack, and
// two that succeed are not.
//
// This used to read three fields and call OriginalFilename ("CL 38718415") the sharpest tell
// there is. Issue #50's reporter says that is backwards -- the changelist is identical on
// both builds, and what differs is the FileVersion STRING in the version resource: NVIDIA
// writes "310,8,0,0" there, the repack writes "310.8.SF.0". That string is a different thing
// from the VS_FIXEDFILEINFO quad below it, an author can put anything in it, and this code
// was reading only the quad -- so the one field that reportedly separates the two builds was
// the one field never looked at.
//
// So report both and assert neither. The measured NVIDIA runtime is
//   FileVersion string "310,8,0,0", quad 310.8.0.0, OriginalFilename "CL 38718415"
// and a log that carries all three settles the question for whoever reads it next, without
// this file having to be right about which one wins.
// ---------------------------------------------------------------------------------------
struct FeedFileIdent
{
    char version[48];   // "310.8.0.0" from VS_FIXEDFILEINFO, or ""
    char file_ver[64];  // the FileVersion STRING -- "310,8,0,0" / "310.8.SF.0" -- or ""
    char product[192];  // "ProductName | FileDescription", or ""
    char build[128];    // OriginalFilename -- NVIDIA's changelist lives here, or ""
};

// One string out of the version resource's string table, in whatever language it carries.
static bool FeedVerString(void *vdata, const char *name, char *out, size_t out_size)
{
    struct LangCp { WORD lang, cp; } *lc = nullptr;
    UINT lc_len = 0;
    if (!VerQueryValueA(vdata, "\\VarFileInfo\\Translation", reinterpret_cast<void **>(&lc), &lc_len) ||
        lc == nullptr || lc_len < sizeof(LangCp))
        return false;

    char key[128];
    sprintf_s(key, "\\StringFileInfo\\%04x%04x\\%s", lc->lang, lc->cp, name);
    char *val = nullptr;
    UINT  vlen = 0;
    if (!VerQueryValueA(vdata, key, reinterpret_cast<void **>(&val), &vlen) || val == nullptr || vlen == 0)
        return false;
    strncpy_s(out, out_size, val, _TRUNCATE);
    return out[0] != '\0';
}

// Fills every field it can and leaves the rest empty. Returns false only when the file has
// no version resource at all, which is itself worth reporting for an NGX runtime.
static bool FeedReadFileIdent(const char *path, FeedFileIdent *out)
{
    if (out == nullptr) return false;
    *out = FeedFileIdent{};
    if (path == nullptr || path[0] == '\0') return false;

    DWORD       dummy = 0;
    const DWORD vsize = GetFileVersionInfoSizeA(path, &dummy);
    if (vsize == 0) return false;
    void *vdata = malloc(vsize);
    if (vdata == nullptr) return false;

    bool any = false;
    if (GetFileVersionInfoA(path, 0, vsize, vdata))
    {
        VS_FIXEDFILEINFO *ffi = nullptr;
        UINT              flen = 0;
        if (VerQueryValueA(vdata, "\\", reinterpret_cast<void **>(&ffi), &flen) && ffi != nullptr)
        {
            sprintf_s(out->version, "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                      HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
            any = true;
        }

        // The string, not the quad -- see the note above. Kept only when it says something
        // the quad does not, because on every stock NVIDIA runtime it is just the quad with
        // commas and repeating it in the log would train people to skip the line.
        char file_ver[64] = {};
        if (FeedVerString(vdata, "FileVersion", file_ver, sizeof(file_ver)))
        {
            char   normalised[64];
            size_t n = 0;
            for (const char *s = file_ver; *s != '\0' && n + 1 < sizeof(normalised); ++s)
            {
                if (*s == ' ') continue;              // "310, 8, 0, 0"
                normalised[n++] = (*s == ',') ? '.' : *s;
            }
            normalised[n] = '\0';
            if (strcmp(normalised, out->version) != 0)
            {
                strncpy_s(out->file_ver, file_ver, _TRUNCATE);
                any = true;
            }
        }

        char product[96] = {}, desc[96] = {};
        FeedVerString(vdata, "ProductName", product, sizeof(product));
        FeedVerString(vdata, "FileDescription", desc, sizeof(desc));
        if (product[0] != '\0' && desc[0] != '\0') sprintf_s(out->product, "%s | %s", product, desc);
        else if (product[0] != '\0')               strncpy_s(out->product, product, _TRUNCATE);
        else if (desc[0] != '\0')                  strncpy_s(out->product, desc, _TRUNCATE);
        if (out->product[0] != '\0') any = true;

        if (FeedVerString(vdata, "OriginalFilename", out->build, sizeof(out->build))) any = true;
    }
    free(vdata);
    return any;
}

// One log line's worth: "310.8.0.0, NVIDIA DLSSNR | ... , build CL 38718415". The stated
// FileVersion only appears when it disagrees with the quad -- on a repack that is the whole
// point of the line, and on a stock runtime it would be noise. Always returns something
// printable, so callers never have to branch on emptiness.
static void FeedFormatFileIdent(const FeedFileIdent &id, char *out, size_t out_size)
{
    if (id.version[0] == '\0' && id.file_ver[0] == '\0' && id.product[0] == '\0' && id.build[0] == '\0')
    {
        strncpy_s(out, out_size, "no version resource", _TRUNCATE);
        return;
    }
    sprintf_s(out, out_size, "%s%s%s%s%s%s%s%s",
              id.version[0]  != '\0' ? id.version : "version ?",
              id.file_ver[0] != '\0' ? " (stated FileVersion \"" : "", id.file_ver,
              id.file_ver[0] != '\0' ? "\")" : "",
              id.product[0]  != '\0' ? ", " : "", id.product,
              id.build[0]    != '\0' ? ", build " : "", id.build);
}

// ---------------------------------------------------------------------------------------
// The NGX runtimes, by identity rather than by path.
//
// Both sides logged where nvngx_dlssnr.dll resolved from and nothing about WHAT it is, so
// no report has ever said which build of the neural model was loaded. Called on every
// session, success or failure -- a machine where NGX works is the control the failing ones
// need, and it is only two lines.
//
// `log` is the caller's own Log(), so each side keeps its [feed] / [host] prefix.
// `dir` is where the runtimes are expected (the add-on's folder, or host64\).
// ---------------------------------------------------------------------------------------
static void FeedLogNgxRuntimes(void (*log)(const char *, ...), const char *tag, const char *dir)
{
    static const char *kRuntimes[] = { "nvngx_dlssnr.dll", "nvngx_dlss.dll" };
    for (const char *name : kRuntimes)
    {
        char path[MAX_PATH];
        sprintf_s(path, "%s%s", dir, name);
        FeedFileIdent id = {};
        if (!FeedReadFileIdent(path, &id))
        {
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
                log("[%s] NGX runtime %s: not present in %s", tag, name, dir);
            else
                log("[%s] NGX runtime %s: present, but it carries no version resource", tag, name);
            continue;
        }
        char line[400];
        FeedFormatFileIdent(id, line, sizeof(line));
        log("[%s] NGX runtime %s: %s", tag, name, line);
    }
}

// ---------------------------------------------------------------------------------------
// Ask NGX, in so many words, whether it supports what we are about to ask for.
//
// NVSDK_NGX_D3D12_GetFeatureRequirements has been in the SDK all along and neither side has
// ever called it. It is the one API that answers the literal question in issue #47's error
// text: FeatureSupported comes back as a bitfield naming AdapterUnsupported (4),
// DriverVersionUnsupported (2) or OSVersionBelowMinimumSupported (8), and MinHWArchitecture
// is the NvAPI architecture id the feature needs. Where a whole investigation is trying to
// decide between "this GPU generation", "this driver" and "this model file", that is the
// difference between an answer and another round of hypotheses.
//
// Queried for BOTH ids that matter here: SuperSampling (1) is the feature this project
// creates, and 18 is the neural-rendering feature the consumer add-on creates on top of it,
// the one nvngx_dlssnr.dll backs. No capability PARAMETER reports on 18 at all, so this is
// the only way to ask about it -- and a machine where 1 is supported and 18 is not is a
// completely different report from one where neither is.
// ---------------------------------------------------------------------------------------
static void FeedFormatFeatureSupport(unsigned bits, char *out, size_t out_size)
{
    if (bits == 0) { strncpy_s(out, out_size, "supported", _TRUNCATE); return; }
    out[0] = 0;
    if (bits & 1)  strncat_s(out, out_size, "no check present, ", _TRUNCATE);
    if (bits & 2)  strncat_s(out, out_size, "DRIVER too old, ", _TRUNCATE);
    if (bits & 4)  strncat_s(out, out_size, "ADAPTER unsupported, ", _TRUNCATE);
    if (bits & 8)  strncat_s(out, out_size, "OS below minimum, ", _TRUNCATE);
    if (bits & 16) strncat_s(out, out_size, "not implemented, ", _TRUNCATE);
    const size_t n = strlen(out);
    if (n >= 2) out[n - 2] = 0;   // trim the trailing ", "
}

static void FeedLogNgxFeatureRequirements(void (*log)(const char *, ...), const char *tag,
                                          IDXGIAdapter *adapter, const wchar_t *data_path,
                                          const NVSDK_NGX_FeatureCommonInfo *info)
{
    if (adapter == nullptr) { log("[%s] NGX feature requirements: no adapter to ask about", tag); return; }

    struct Probe { NVSDK_NGX_Feature id; const char *name; };
    const Probe probes[] = {
        { NVSDK_NGX_Feature_SuperSampling,                                "SuperSampling (DLSS, what this project creates)" },
        { static_cast<NVSDK_NGX_Feature>(18), "feature 18 (neural rendering, what nvngx_dlssnr.dll backs)" },
    };

    for (const Probe &pr : probes)
    {
        NVSDK_NGX_FeatureDiscoveryInfo di = {};
        di.SDKVersion                   = NVSDK_NGX_Version_API;
        di.FeatureID                    = pr.id;
        di.Identifier.IdentifierType    = NVSDK_NGX_Application_Identifier_Type_Application_Id;
        di.Identifier.v.ApplicationId   = 0x1000000ULL;
        di.ApplicationDataPath          = data_path;
        di.FeatureInfo                  = info;

        NVSDK_NGX_FeatureRequirement req = {};
        const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_GetFeatureRequirements(adapter, &di, &req);
        if (NVSDK_NGX_FAILED(r))
        {
            log("[%s] NGX feature requirements: %s -> the query itself failed 0x%08X (%s)",
                tag, pr.name, r, NgxResultName(r));
            continue;
        }
        char why[192];
        FeedFormatFeatureSupport(static_cast<unsigned>(req.FeatureSupported), why, sizeof(why));
        log("[%s] NGX feature requirements: %s -> %s (min GPU architecture 0x%X, min OS %s)",
            tag, pr.name, why, req.MinHWArchitecture,
            req.MinOSVersion[0] != 0 ? req.MinOSVersion : "unstated");
    }
}
