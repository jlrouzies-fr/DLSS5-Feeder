// feed_dfc.h -- Deep Fried Chicken interop ABI 1, producer side.
//
// Deep Fried Chicken (deep-fried-chicken.addon64, 1.4.0+) is a neural consumer that
// detours the NGX feature-1 entry points, like the DLSS 5 add-on does. Its author
// implemented the compatibility surface asked for in FEEDBACK-DFC.md; the producer
// contract is external/deepfried/FEEDER-INTEROP-v1.md. It is a protocol, not a
// dependency: neither side loads or embeds the other.
//
// What a producer (this feeder, in-process or in the x64 companion host) does:
//   1. read the two exported data symbols to learn whether Chicken is available;
//   2. set the four unsigned NGX parameters below on the shared parameter object
//      immediately before feature-1 Create and before EVERY feature-1 Evaluate.
// Chicken binds the complete tuple to the created handle; a partial, unknown, or
// mismatched tuple fails closed (it skips its own work, the genuine NGX call and its
// output stay intact). Unknown NGX keys are ordinary application parameters, so the
// driver and the RenoDX add-on ignore them -- publishing is unconditional and harmless.
//
// A producer must NOT acquire Chicken's consumer mutex
// (Local\DeepFriedChicken.Feature1Consumer.v1.<pid>) and must NOT write either export.
//
// The package that shipped the docs did not include dfc-feeder-interop.h, so the state
// values below were recovered from deep-fried-chicken.addon64 (the export's initialiser is
// 0 and the arming code stores 1 before claiming, 2 once armed, 3 on a duplicate-consumer
// conflict, 4 when the marker or resolver hook cannot be created). Confirmed unchanged in
// both 1.4.0-alpha and 1.4.4-alpha, and corroborated at runtime: Chicken logs CLAIMING then
// ARMED while these exports read 1 then 2. Re-check when the author ships the header.
//
// FEEDER-INTEROP-v1.md is byte-identical between 1.4.0, 1.4.4 and 1.4.8, so ABI 1 is stable
// across all three (the exports and the state enum are unchanged too -- re-checked against each
// binary). Their differences are consumer-side only: 1.4.4 added a continuous 10-150% neural
// work scale in place of the Full/Half selector and automatic early-load registration (it
// appends itself to [ADDON] LoadFromDllMain in the sibling ReShade.ini, backing the file up
// first); 1.4.8 replaced a full process-module rescan every 300 presents with an OS loader
// callback, which is what cured a periodic stutter in module-heavy hosts.

#pragma once

#include <windows.h>

#define DFC_ADDON_FILENAME       "deep-fried-chicken.addon64"
#define DFC_NAME_MARK            "Deep Fried Chicken "      // prefix of its NAME export

#define DFC_KEY_CONTRACT_VERSION "DFC.Feeder.ContractVersion"
#define DFC_KEY_PROVIDER_ID      "DFC.Feeder.ProviderId"
#define DFC_KEY_HOST_MODE        "DFC.Feeder.HostMode"
#define DFC_KEY_EVALUATE_CADENCE "DFC.Feeder.EvaluateCadence"

#define DFC_CONTRACT_VERSION     1u
#define DFC_PROVIDER_ID_DL5F     0x444C3546u   // 'DL5F'
#define DFC_HOST_MODE_IN_PROCESS 0u
#define DFC_HOST_MODE_COMPANION  1u
#define DFC_EVALUATE_CADENCE     1u            // one Evaluate per Present

enum DfcFeature1State : LONG
{
    DFC_STATE_UNKNOWN  = -1,   // ours: exports not read yet (module not loaded, or pre-1.4.0 build)
    DFC_STATE_DISARMED = 0,    // arm=0 (restart-only hard disarm) or not armed yet
    DFC_STATE_CLAIMING = 1,
    DFC_STATE_ARMED    = 2,
    DFC_STATE_CONFLICT = 3,    // another feature-1 consumer already owns the process marker
    DFC_STATE_FAILED   = 4,    // could not create its marker or arm its resolver hook
};

static inline const char *DfcStateName(LONG s)
{
    switch (s)
    {
    case DFC_STATE_DISARMED: return "DISARMED";
    case DFC_STATE_CLAIMING: return "CLAIMING";
    case DFC_STATE_ARMED:    return "ARMED";
    case DFC_STATE_CONFLICT: return "CONFLICT";
    case DFC_STATE_FAILED:   return "FAILED";
    default:                 return "unknown";
    }
}

static inline bool DfcStateAvailable(LONG s) { return s == DFC_STATE_CLAIMING || s == DFC_STATE_ARMED; }

// Reads the ABI-1 exports from the loaded Chicken module. Returns false when the module is
// not loaded in this process (yet) or is a pre-1.4.0 build without the exports; then *abi
// is 0 and *state is DFC_STATE_UNKNOWN. Reads only; never writes either export.
// Cheap enough to call every frame: once the two exports have been resolved they are
// cached (an add-on never unloads mid-run), so a poll is two pointer reads.
static inline bool DfcReadExports(unsigned int *abi, LONG *state, bool *loaded)
{
    static HMODULE               s_mod     = nullptr;
    static const unsigned int   *s_abi_p   = nullptr;
    static const volatile LONG  *s_state_p = nullptr;
    *abi = 0;
    *state = DFC_STATE_UNKNOWN;
    if (s_abi_p == nullptr || s_state_p == nullptr)
    {
        s_mod = GetModuleHandleA(DFC_ADDON_FILENAME);
        *loaded = s_mod != nullptr;
        if (s_mod == nullptr) return false;
        s_abi_p   = reinterpret_cast<const unsigned int *>(GetProcAddress(s_mod, "DFC_FeederInteropAbi"));
        s_state_p = reinterpret_cast<const volatile LONG *>(GetProcAddress(s_mod, "DFC_Feature1InterceptionState"));
        if (s_abi_p == nullptr || s_state_p == nullptr) { s_abi_p = nullptr; s_state_p = nullptr; return false; }
    }
    *loaded = true;
    *abi = *s_abi_p;
    *state = *s_state_p;
    return true;
}

// Scans the add-on FILE (not the loaded module: ReShade may not have loaded it yet) for
// its NAME export string and copies the version that follows "Deep Fried Chicken ".
// Returns false when the file is not next to 'dir' (which must end in a backslash).
static inline bool DfcScanFile(const char *dir, char *ver, size_t ver_size)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s" DFC_ADDON_FILENAME, dir);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    strcpy_s(ver, ver_size, "?");
    const DWORD size = GetFileSize(f, nullptr);
    DWORD got = 0;
    char *buf = (size > 0 && size < 16u * 1024 * 1024) ? static_cast<char *>(malloc(size)) : nullptr;
    if (buf != nullptr && ReadFile(f, buf, size, &got, nullptr) && got == size)
    {
        static const char kMark[] = DFC_NAME_MARK;
        const DWORD mlen = sizeof(kMark) - 1;
        for (DWORD i = 0; i + mlen < size; ++i)
            if (memcmp(buf + i, kMark, mlen) == 0)
            {
                const char *v = buf + i + mlen;
                size_t n = 0;
                while (n + 1 < ver_size && i + mlen + n < size && v[n] >= 32 && v[n] < 127 && v[n] != '%')
                    ++n;
                if (n > 0) { memcpy(ver, v, n); ver[n] = '\0'; }
                break;
            }
    }
    free(buf);
    CloseHandle(f);
    return true;
}
