// feed_renodx_api.h -- RenoDX DLSS5's own control interface, for changing its settings while it runs.
//
// The RenoDX DLSS5 add-on exports, from v10 on (seen in 10.8.0; v6.1 exports nothing):
//
//     const RenoNrControlApi *RenoDX_NR_GetControlApi(int version);     // version 1, else null
//
// There is no public header for it. What follows was read off the 10.8.0 binary, function by
// function, and then confirmed in the lab by calling it:
//
//   describe(index, &info)   one of its controls (59 in 10.8.0): the setting's name as it is in
//                            ReShade.ini, its limits, and whether it is a number, a choice or a tick.
//                            Returns 0 past the last one.
//   get(name, &value)        the current value, as a float whatever the kind.
//   set(items, count)        up to 64 name/value pairs. Each is checked: the name must be one of
//                            its controls, the value finite and within the limits, and whole for a
//                            choice or a tick. Then the value is written into the running tool AND
//                            saved to its config, exactly as its own page would. One bad pair
//                            refuses that pair.
//   the last two entries register and remove "external controls": sliders another tool wants
//   shown on RenoDX's page. Not used here.
//
// Only the controls it lists can be set this way. In 10.8.0 that is the intensity, preset and
// style, the model tone / structure / skin controls, the mask and HUD switches and the whole
// "look" group -- NOT neural rendering on/off, the pass count, the working resolution or the
// composition strengths, which it still reads at start only.
//
// Everything is guarded: a build without the export, another version of the table, or a fault
// inside the tool all come back as "not available", never as a crash of the process.
//
// Requires <windows.h>. Header-only and static, like its siblings.

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>

#define RENO_API_EXPORT "RenoDX_NR_GetControlApi"   // also the text looked for in the add-on's file

struct RenoNrControlInfo    // 24 bytes
{
    const char *key;
    float       lo, hi;
    uint32_t    kind;       // 0 a number, 1 a choice (whole numbers), 2 a tick (0 or 1)
    uint32_t    reserved;
};
struct RenoNrControlValue   // 16 bytes
{
    const char *key;
    float       value;
    uint32_t    reserved;
};
struct RenoNrControlApi
{
    uint32_t size;          // 0x30 in version 1
    uint32_t version;       // 1
    int (*describe)(uint32_t index, RenoNrControlInfo *out);
    int (*get)(const char *key, float *out);
    int (*set)(const RenoNrControlValue *items, uint32_t count);
    void *register_external;
    void *unregister_external;
};
static_assert(sizeof(RenoNrControlInfo) == 24 && sizeof(RenoNrControlValue) == 16, "layouts read off the binary");

// The interface of whichever loaded module exports it, or nullptr. Looked for again on every
// call until found (the add-on is loaded by ReShade some time after this process starts).
static const RenoNrControlApi *FeedRenoApi(char *module_name = nullptr, size_t cb = 0)
{
    static const RenoNrControlApi *api = nullptr;
    static char                    name[64] = "";
    if (api == nullptr)
    {
        typedef BOOL (WINAPI *PFN_EnumMods)(HANDLE, HMODULE *, DWORD, LPDWORD);
        typedef const RenoNrControlApi *(*PFN_Get)(int);
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        PFN_EnumMods enum_mods = k32 != nullptr ? reinterpret_cast<PFN_EnumMods>(GetProcAddress(k32, "K32EnumProcessModules")) : nullptr;
        static HMODULE mods[1024];
        DWORD need = 0;
        if (enum_mods == nullptr || !enum_mods(GetCurrentProcess(), mods, sizeof(mods), &need)) return nullptr;
        for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024 && api == nullptr; ++i)
        {
            FARPROC f = GetProcAddress(mods[i], RENO_API_EXPORT);
            if (f == nullptr) continue;
            const RenoNrControlApi *a = nullptr;
            __try { a = reinterpret_cast<PFN_Get>(f)(1); }
            __except (EXCEPTION_EXECUTE_HANDLER) { a = nullptr; }
            // Version 1 exactly, and at least as far as `set`: anything else is a table this
            // code was not written against.
            bool ok = false;
            __try { ok = a != nullptr && a->version == 1 && a->size >= 0x20 && a->describe != nullptr && a->get != nullptr && a->set != nullptr; }
            __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
            if (!ok) continue;
            api = a;
            char path[MAX_PATH] = {};
            GetModuleFileNameA(mods[i], path, MAX_PATH);
            const char *s = strrchr(path, '\\');
            strcpy_s(name, s != nullptr ? s + 1 : path);
        }
    }
    if (module_name != nullptr && cb > 0) strcpy_s(module_name, cb, name);
    return api;
}

// 1 = taken, 0 = the tool refused it (unknown name, out of its limits), -1 = no interface or a fault.
static int FeedRenoApiSet(const char *key, float value)
{
    const RenoNrControlApi *api = FeedRenoApi();
    if (api == nullptr) return -1;
    RenoNrControlValue v = { key, value, 0 };
    int r = -1;
    __try { r = api->set(&v, 1) != 0 ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    return r;
}

static bool FeedRenoApiGet(const char *key, float *out)
{
    const RenoNrControlApi *api = FeedRenoApi();
    if (api == nullptr) return false;
    bool ok = false;
    __try { ok = api->get(key, out) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

// The names it lists, one per line, into `out`. Returns how many (0 = no interface).
static int FeedRenoApiKeys(char *out, size_t cb)
{
    if (cb > 0) out[0] = '\0';
    const RenoNrControlApi *api = FeedRenoApi();
    if (api == nullptr) return 0;
    int    n = 0;
    size_t used = 0;
    for (uint32_t i = 0; i < 512; ++i)
    {
        RenoNrControlInfo info = {};
        int ok = 0;
        __try { ok = api->describe(i, &info); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        if (ok == 0 || info.key == nullptr) break;
        char line[96];
        int  len = 0;
        __try { len = sprintf_s(line, "%.80s\n", info.key); }
        __except (EXCEPTION_EXECUTE_HANDLER) { len = 0; }
        if (len <= 0 || used + static_cast<size_t>(len) + 1 > cb) break;
        memcpy(out + used, line, static_cast<size_t>(len) + 1);
        used += static_cast<size_t>(len);
        ++n;
    }
    return n;
}
