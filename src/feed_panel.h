// feed_panel.h -- the feeder's own in-game panel, the same in every build of the add-on.
//
// PROTOTYPE. One panel, drawn by the GAME's ReShade with plain Dear ImGui, in front of
// whichever neural tool does the work: the "RenoDX DLSS5 Generic" add-on or the OptiScaler
// DLSS-NR fork. It slides in from an edge of the picture on a
// shortcut (Alt+C unless dlss5-feed.cfg says otherwise), leaves the rest of the picture as it is, and takes
// the mouse and keyboard while it is up.
//
// It is included by three builds and knows nothing about how any of them works:
//
//   dlss5-feed.addon32          a 32-bit game; the tool runs in host64's helper process
//   dlss5-feed-helper.addon64   a 64-bit game in helper mode; the same
//   dlss5-feed.addon64          a 64-bit game; the tool runs in the game's own process
//
// Each fills in a FeedPanelEnv: where the tool's files are, whether it is running, and how to
// reach it. Everything else -- the vocabulary of settings, where each one lives in each tool's
// file and how it is spelled there, the drawing -- is here, once.
//
// How a change gets to the tool, by tool:
//   OptiScaler reads OptiScaler.ini. A build of the fork that re-reads [DlssNr] while it runs
//     (it carries OPTI_LIVE_LITERAL) takes the change live; an older build at its next start.
//   The RenoDX add-on reads its ini at start only: the helper is restarted when there is one,
//     and otherwise the change waits for the next start of the game. From v10 it also has a
//     control interface (feed_renodx_api.h) that takes some of its settings while it runs:
//     those rows apply at once, the others keep the Apply button and are marked with a star.
//   ShortFuse's renodx-dlss.addon64 is NOT one of the tools (user's decision, 2026-11-12): it is
//     built to run without the feeder, and in the feeder's own process its neural pass either
//     faults or does not start. The panel takes no notice of it.
//
// Requires <windows.h>, <imgui.h> + <reshade.hpp> (the add-on's own includes) before it.
// Header-only and static, like its siblings: every add-on is a single-file build.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>

struct FeedPanelEnv
{
    bool helper;                 // the tool runs in host64's helper process, not in this one
    char tool_dir[MAX_PATH];     // the folder the tool's files are in, with its trailing backslash
    char reshade_ini[MAX_PATH];  // helper only: the ReShade.ini of the helper's ReShade (the RenoDX add-ons save there).
                                 // In-process the tool shares this ReShade, and its config is reached through the API.
    bool opti_nr;                // an OptiScaler DLSS-NR build is the tool
    bool opti_live;              // ... one that re-reads its ini while it runs
    char opti_ini[MAX_PATH];
    char cfg_path[MAX_PATH];     // dlss5-feed.cfg, where the panel keeps its own few keys (its size)

    void (*refresh)();                                   // fill in the fields above (called when the panel first opens)
    bool (*running)();                                   // the tool can be handed a setting right now
    bool (*starting)();                                  // a helper is on its way up
    void (*schedule_apply)();                            // have FeedPanelApply() called where the helper may be stopped
                                                         // (nullptr: it is called on the spot)
    void (*stop_tool)();                                 // helper only: stop it, so its files can be written under it
    void (*restart_tool)();                              // helper only: let it start again
    bool  (*send_setting)(const char *key, float value);  // hand the tool one setting while it runs (RenoDX's control interface)
    int   (*live_keys)(char *out, size_t cb);             // tool in THIS process: the names it takes that way, one per line
    float (*gpu_ms)();                                    // GPU time of the feed's work on one frame (DLSS + neural pass); <= 0 unknown
    void (*log)(const char *fmt, ...);
};
static FeedPanelEnv g_panel_env;

// The shortcut and the edge: parse-only keys of dlss5-feed.cfg (CfgSave carries unknown keys
// over, so a value set by hand survives). panel_mods: 1 Alt, 2 Ctrl, 4 Shift.
static int g_panel_key  = 'C';     // Alt+C: one hand, free in Windows, ReShade and the NVIDIA overlay's defaults,
static int g_panel_mods = 1;       // and in the same place on QWERTY and AZERTY (user's choice, 2026-11-12)
static int g_panel_side = 1;      // 0 left, 1 right
// The size of everything on the panel, in percent of its design size (13 px text, drawn for a
// 1080p picture on a 100% desktop). 0 = automatic, the larger of two things, never below 100%:
//   the picture's height over 1080, so the panel covers the same share of a 4K picture as of a
//     1080p one;
//   the desktop scaling of the game's window (200% on a 4K television, say), for a DPI-aware
//     game in a small window there -- Fable Anniversary through dgVoodoo is one: 1920x1080 real
//     pixels in a window, where 13 px text is half the size of everything else on the screen.
// A game that is NOT DPI-aware reports 100% and renders a picture Windows then enlarges, so
// it needs nothing.
static int g_panel_scale = 0;

static bool FeedPanelCfg(const char *key, int value)
{
    if      (_stricmp(key, "panel_key") == 0)  g_panel_key  = (value >= 0 && value < 256) ? value : 'C';
    else if (_stricmp(key, "panel_mods") == 0) g_panel_mods = value & 7;
    else if (_stricmp(key, "panel_side") == 0) g_panel_side = value != 0 ? 1 : 0;
    else if (_stricmp(key, "panel_scale") == 0) g_panel_scale = value <= 0 ? 0 : value < 50 ? 50 : value > 400 ? 400 : value;
    else return false;
    return true;
}

// The desktop scaling the game's window is drawn at, as this process sees it (1.0 = 100%).
// GetDpiForWindow is Windows 10 1607 and later; without it, or without a window, 100%.
static float FeedPanelDpiScale(HWND hwnd)
{
    static HWND      last_hwnd = nullptr;
    static ULONGLONG last_at = 0;
    static float     last = 1.0f;
    const ULONGLONG now = GetTickCount64();
    if (hwnd == last_hwnd && now - last_at < 1000) return last;   // a window can change monitor; once a second is plenty
    last_hwnd = hwnd;
    last_at = now;
    last = 1.0f;
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow fn = nullptr;
    static bool looked = false;
    if (!looked)
    {
        looked = true;
        if (HMODULE u = GetModuleHandleA("user32.dll")) fn = reinterpret_cast<PFN_GetDpiForWindow>(GetProcAddress(u, "GetDpiForWindow"));
    }
    if (fn != nullptr && hwnd != nullptr)
    {
        const UINT dpi = fn(hwnd);
        if (dpi >= 96 && dpi <= 960) last = static_cast<float>(dpi) / 96.0f;
    }
    return last;
}

static float FeedPanelScale(float picture_h, float dpi_scale)
{
    if (g_panel_scale > 0) return static_cast<float>(g_panel_scale) / 100.0f;
    float s = picture_h / 1080.0f;
    if (s < dpi_scale) s = dpi_scale;
    return s < 1.0f ? 1.0f : s > 4.0f ? 4.0f : s;
}

// One of the panel's own keys, written into dlss5-feed.cfg in place: every other line of the
// file is kept as it is. The add-on reads the file again by itself and hands the key back to
// FeedPanelCfg.
static void FeedPanelSaveCfg(const char *key, int value)
{
    if (g_panel_env.cfg_path[0] == '\0') return;
    std::string kept;
    const size_t n = strlen(key);
    FILE *r = nullptr;
    if (fopen_s(&r, g_panel_env.cfg_path, "r") == 0 && r != nullptr)
    {
        char line[512];
        while (fgets(line, sizeof(line), r) != nullptr)
        {
            const char *k = line;
            while (*k == ' ' || *k == '\t') ++k;
            if (_strnicmp(k, key, n) == 0)
            {
                const char *e = k + n;
                while (*e == ' ' || *e == '\t') ++e;
                if (*e == '=') continue;   // the old value
            }
            kept += line;
            if (kept.back() != '\n') kept += '\n';
        }
        fclose(r);
    }
    FILE *f = nullptr;
    if (fopen_s(&f, g_panel_env.cfg_path, "w") != 0 || f == nullptr) return;
    fputs(kept.c_str(), f);
    fprintf(f, "%s=%d\n", key, value);
    fclose(f);
    if (g_panel_env.log != nullptr) g_panel_env.log("[panel] %s=%d saved to dlss5-feed.cfg", key, value);
}

static void FeedPanelKeyName(char *out, size_t n)
{
    char key[64] = "";
    UINT scan = MapVirtualKeyA(static_cast<UINT>(g_panel_key), MAPVK_VK_TO_VSC);
    if (g_panel_key <= 0 || GetKeyNameTextA(static_cast<LONG>(scan << 16), key, sizeof(key)) == 0) sprintf_s(key, "key %d", g_panel_key);
    sprintf_s(out, n, "%s%s%s%s", (g_panel_mods & 1) != 0 ? "Alt+" : "", (g_panel_mods & 2) != 0 ? "Ctrl+" : "",
              (g_panel_mods & 4) != 0 ? "Shift+" : "", key);
}

// ---------------------------------------------------------------------------
// The settings. One row is one idea; each tool's side of it says where it lives.
// ---------------------------------------------------------------------------
enum FeedPanelKind { BR_BOOL, BR_COMBO, BR_FLOAT };
enum { BR_BACKEND_NONE = 0, BR_BACKEND_RENODX, BR_BACKEND_OPTI };

// key == nullptr: this tool has no such setting, and the row is not shown.
struct BridgeSide
{
    const char        *key;
    float              def, lo, hi;   // in the panel's units
    float              scale;         // the tool's number = the panel's times this (0 = the same number)
    float              on;            // a tick box: the tool's number for "ticked" (0 = 1)
    const char *const *items;         // a combo whose choices this tool names differently (nullptr = the row's)
    int                item_count;
};

struct BridgeItem
{
    const char        *group;
    const char        *label;
    FeedPanelKind      kind;
    const char        *format;     // sliders
    bool               whole;      // a count: whole steps
    bool               log;        // a range of several decades: logarithmic slider
    const char *const *items;      // combos
    int                item_count;
    const int         *values;     // combos: the number written for each item; nullptr = the item's index
    int                need_row;   // shown only while combo row `need_row` has item `need_item` selected; -1 = always
    int                need_item;
    BridgeSide         reno;       // "RenoDX DLSS5 Generic": [RenoDX.DLSS5] in its ReShade.ini
    BridgeSide         opti;       // OptiScaler DLSS-NR: [DlssNr] in OptiScaler.ini
    BridgeSide         reno8;      // RenoDX from v8 on (it writes ConfigVersion), where it differs from `reno`:
                                   // another range, or a setting older builds do not have (then reno is BR_NONE)
};

static const char *const kBrPresets[]   = { "Default", "Preset #1", "Preset #2", "Preset #3" };
static const char *const kBrStyles[]    = { "Default", "Natural", "Cinematic" };
static const char *const kBrWhiteSource[]  = { "Manual", "Game exposure", "Automatic HDR exposure" };
static const int         kBrWhiteSourceV[] = { 0, 1, 3 };   // OptiScaler's WhitePointSource values for those three
enum { BR_ROW_WHITE_SOURCE = 16 };   // index of "White point source" below
enum { BR_ROW_RENO_LOOK = 29 };     // index of "Adjust the model's result" below
// RenoDX 10.8.0's choices, in the order of the numbers it takes. Detail stability really is
// Auto / Off / Always: its log says "(Auto)" for 0, nothing for 1 and "(Always)" for 2.
static const char *const kBrRenoStabilize[] = { "Off", "Static", "Motion" };
static const char *const kBrRenoDetail[]    = { "Auto", "Off", "Always" };
#define BR_NONE { nullptr, 0.0f, 0.0f, 0.0f }

// Ranges. OptiScaler's are its own: Config.h defaults and the slider limits in
// DlssNr_MenuControls.cpp (fork bb484163). The Generic add-on publishes none: its keys are the
// ones v6.1 writes into ReShade.ini, its ranges the ones the 32-bit add-on's settings mirror
// uses.
static const BridgeItem kBridge[] = {
    { "LOOK", "Neural rendering", BR_BOOL, nullptr, false, false, nullptr, 0, nullptr, -1, 0,
      { "NeuralUplift", 1.0f, 0.0f, 1.0f },        { "Enabled", 0.0f, 0.0f, 1.0f } },
    { "LOOK", "Preset", BR_COMBO, nullptr, false, false, kBrPresets, 4, nullptr, -1, 0,
      { "NRPreset", 0.0f, 0.0f, 3.0f },            { "Preset", 0.0f, 0.0f, 3.0f } },
    { "LOOK", "Style", BR_COMBO, nullptr, false, false, kBrStyles, 3, nullptr, -1, 0,
      { "NRStyle", 0.0f, 0.0f, 2.0f },             { "Style", 0.0f, 0.0f, 2.0f } },
    { "LOOK", "Intensity", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      { "NRIntensity", 1.0f, 0.0f, 2.0f },         { "Intensity", 1.0f, 0.0f, 2.0f },
      { "NRIntensity", 1.0f, 0.0f, 1.0f } },                                          // 10.8.0 stops at 1

    // What the model is asked for.
    { "MODEL", "Tone", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      { "NRLocalTone", 1.0f, 0.0f, 2.0f },         { "LocalTone", 1.0f, 0.0f, 2.0f } },
    { "MODEL", "Global tone", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      { "NRGlobalTone", 1.0f, 0.0f, 2.0f },        BR_NONE },
    { "MODEL", "Structure", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      { "NRLocalStructure", 1.0f, 0.0f, 2.0f },    { "LocalStructure", 1.0f, 0.0f, 2.0f } },
    { "MODEL", "Skin", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      { "NRSkinStructure", -1.0f, -1.0f, 1.0f },   { "SkinStructure", -1.0f, -1.0f, 2.0f } },
    { "MODEL", "Character mask", BR_BOOL, nullptr, false, false, nullptr, 0, nullptr, -1, 0,
      { "NRAutoMask", 1.0f, 0.0f, 1.0f },          { "AutoMask", 1.0f, 0.0f, 1.0f } },
    { "MODEL", "Keep the HUD as it is", BR_BOOL, nullptr, false, false, nullptr, 0, nullptr, -1, 0,
      { "NRUICorrection", 1.0f, 0.0f, 1.0f },      BR_NONE },

    // How often, and on how many pixels.
    { "PASSES AND COST", "Passes", BR_FLOAT, nullptr, true, false, nullptr, 0, nullptr, -1, 0,
      { "NRPasses", 1.0f, 1.0f, 4.0f },            { "Passes", 1.0f, 1.0f, 2.0f } },   // 2 = OptiScaler's limit without its "unlock up to 10"
    { "PASSES AND COST", "Working resolution", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      { "NRResolutionScale", 1.0f, 0.33f, 1.0f },  { "WorkingScale", 1.0f, 0.25f, 1.0f } },   // OptiScaler itself goes to 2.0

    // How much of the model's picture is kept when it is put back over the game's.
    { "FINAL PICTURE", "Detail strength", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      { "NRTransferStrength", 1.0f, 0.0f, 1.0f },  { "TransferStrength", 1.0f, 0.0f, 2.0f } },
    { "FINAL PICTURE", "Colour strength", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      { "NRColorStrength", 1.0f, 0.0f, 1.0f },     { "ColourStrength", 1.0f, 0.0f, 4.0f } },
    { "FINAL PICTURE", "Highlight guard", BR_FLOAT, "%.1fx", false, false, nullptr, 0, nullptr, -1, 0,
      BR_NONE,                                     { "MaxRatio", 2.0f, 1.0f, 8.0f } },

    // What the model is told white is.
    { "WHITE POINT", "Paper white", BR_FLOAT, "%.2fx", false, true, nullptr, 0, nullptr, -1, 0,
      { "NRPaperWhiteScale", 1.0f, 0.1f, 10.0f },  { "WhitePointScale", 1.0f, 0.25f, 2000.0f } },
    { "WHITE POINT", "White point source", BR_COMBO, nullptr, false, false, kBrWhiteSource, 3, kBrWhiteSourceV, -1, 0,
      BR_NONE,                                     { "WhitePointSource", 0.0f, 0.0f, 2.0f } },
    { "WHITE POINT", "Exposure trim", BR_FLOAT, "%.3fx", false, true, nullptr, 0, nullptr, BR_ROW_WHITE_SOURCE, 1,
      BR_NONE,                                     { "WhitePointTrim", 1.0f, 0.001f, 1000.0f } },
    { "WHITE POINT", "Exposure trim", BR_FLOAT, "%.3fx", false, true, nullptr, 0, nullptr, BR_ROW_WHITE_SOURCE, 2,
      BR_NONE,                                     { "AutoExposureTrim", 5.0f, 0.001f, 1000.0f } },
    { "WHITE POINT", "Highlight protection (%)", BR_FLOAT, "%.0f", false, false, nullptr, 0, nullptr, BR_ROW_WHITE_SOURCE, 2,
      BR_NONE,                                     { "AutoExposureHighlightProtection", 0.0f, 0.0f, 100.0f } },
    { "WHITE POINT", "Diffuse white (nits)", BR_FLOAT, "%.0f", false, false, nullptr, 0, nullptr, -1, 0,
      { "NRDiffuseWhiteNits", 203.0f, 80.0f, 1000.0f }, BR_NONE },
    // ---- From here on: settings only ONE tool has. They are shown on the panel's second tab,
    // named after the tool (a row is "common" when both tools have it). Appended, never
    // inserted: BR_ROW_WHITE_SOURCE above is an index into this table.

    // OptiScaler DLSS-NR (its menu: DlssNr_MenuControls.cpp).
    { "PASSES", "Unlock up to 10 passes", BR_BOOL, nullptr, false, false, nullptr, 0, nullptr, -1, 0,
      BR_NONE, { "UnlockPasses", 0.0f, 0.0f, 1.0f } },
    { "SKIN AND ENVIRONMENT (FINAL EDIT)", "Separate skin / environment controls", BR_BOOL, nullptr, false, false, nullptr, 0, nullptr, -1, 0,
      BR_NONE, { "SkinProtection", 0.0f, 0.0f, 1.0f } },
    { "SKIN AND ENVIRONMENT (FINAL EDIT)", "Skin detail / lighting", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      BR_NONE, { "SkinDetail", 1.0f, 0.0f, 1.0f } },
    { "SKIN AND ENVIRONMENT (FINAL EDIT)", "Skin colour", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      BR_NONE, { "SkinColour", 1.0f, 0.0f, 1.0f } },
    { "SKIN AND ENVIRONMENT (FINAL EDIT)", "Environment detail / lighting", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      BR_NONE, { "EnvironmentDetail", 1.0f, 0.0f, 1.0f } },
    { "SKIN AND ENVIRONMENT (FINAL EDIT)", "Environment colour", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 0,
      BR_NONE, { "EnvironmentColour", 1.0f, 0.0f, 1.0f } },

    // RenoDX DLSS5 Generic (keys as v6.1 saves them; labels from its own page).
    { "STACKING", "Denoise before upscaling (pre-SR)", BR_BOOL, nullptr, false, false, nullptr, 0, nullptr, -1, 0,
      { "NRPreUpscale", 0.0f, 0.0f, 1.0f } },
    { "STACKING", "Chained temporal history", BR_BOOL, nullptr, false, false, nullptr, 0, nullptr, -1, 0,
      { "NRChainedHistory", 1.0f, 0.0f, 1.0f } },

    // RenoDX DLSS5 from v8 on (read off 10.8.0: names from the ini it saves, limits from what it
    // clamps an out-of-range value to, the order of each choice from what its log says it did).
    // "Adjust NR's look" reshapes what the model returned; the rows under it apply while it is on.
    { "LOOK OF THE RESULT", "Adjust the model's result", BR_BOOL, nullptr, false, false, nullptr, 0, nullptr, -1, 0,
      BR_NONE, BR_NONE, { "NRLookMode", 0.0f, 0.0f, 1.0f } },
    { "LOOK OF THE RESULT", "Edit strength", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookStrength", 1.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Brightening", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookBrighten", 1.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Darkening", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookDarken", 1.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Colour change", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookColour", 1.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Hue shift", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookHue", 1.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Shadows", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookShadows", 1.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Midtones", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookMidtones", 1.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Highlights", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookHighlights", 1.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Large-scale tone", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookTone", 1.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Fine detail", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookDetail", 1.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Detail radius", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookDetailRadius", 1.0f, 0.25f, 4.0f } },
    { "LOOK OF THE RESULT", "Max brightening (stops, 0 = none)", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookMaxBrighten", 0.0f, 0.0f, 4.0f } },
    { "LOOK OF THE RESULT", "Max darkening (stops, 0 = none)", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookMaxDarken", 0.0f, 0.0f, 4.0f } },
    { "LOOK OF THE RESULT", "Max colour change (0 = none)", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookMaxColour", 0.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Halo suppression", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookHalo", 0.0f, 0.0f, 1.0f } },
    { "LOOK OF THE RESULT", "Halo next to dark objects", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookHaloBrightSide", 0.0f, 0.0f, 1.0f } },
    { "LOOK OF THE RESULT", "Stabilize", BR_COMBO, nullptr, false, false, kBrRenoStabilize, 3, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookStabilize", 0.0f, 0.0f, 2.0f } },
    { "LOOK OF THE RESULT", "Stabilize time (ms)", BR_FLOAT, "%.0f", false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookStabilizeMs", 60.0f, 10.0f, 250.0f } },
    { "LOOK OF THE RESULT", "Stabilize fine detail too", BR_BOOL, nullptr, false, false, nullptr, 0, nullptr, BR_ROW_RENO_LOOK, 1,
      BR_NONE, BR_NONE, { "NRLookStabilizeDetail", 0.0f, 0.0f, 1.0f } },
    { "STABILITY", "Detail stability", BR_COMBO, nullptr, false, false, kBrRenoDetail, 3, nullptr, -1, 0,
      BR_NONE, BR_NONE, { "NRDetailStability", 0.0f, 0.0f, 2.0f } },
    { "HIGHLIGHTS AND COLOUR", "Style highlight guard (stops)", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 1,
      BR_NONE, BR_NONE, { "NRStyleHighlightGuard", 0.0f, 0.0f, 2.0f } },
    { "HIGHLIGHTS AND COLOUR", "Chroma clamp (stops)", BR_FLOAT, "%.2f", false, false, nullptr, 0, nullptr, -1, 1,
      BR_NONE, BR_NONE, { "NRChromaClamp", 1.0f, 0.25f, 2.0f } },
};
static const int kBridgeCount = static_cast<int>(sizeof(kBridge) / sizeof(kBridge[0]));
static_assert(sizeof(kBridge) / sizeof(kBridge[0]) <= 64, "g_br and its companions hold 64 rows");

static int       g_br_backend;
static float     g_br[64], g_br_saved[64];
static bool      g_br_reno8;          // the RenoDX build behind the panel is v8 or newer (see BridgeLoad)
static bool      g_br_rowlive[64];    // the tool takes this row's setting while it runs (see BridgeLoad)
static bool      g_br_apply_full;     // the Apply button was pressed: rows that need a restart go too
static char      g_br_livekeys[8192]; // RenoDX: the names its control interface lists, each between line ends
static bool      g_br_absent[64];     // this build of the tool has no such setting (see BridgeLoad)
static bool      g_br_loaded;
static int       g_br_tab;            // 0 = the settings tools share, 1 = the ones only this tool has
static char      g_br_note[160];      // the last thing that happened, shown at the foot of the panel
static bool      g_br_cursor_forced;  // we turned ReShade's software cursor on
static bool      g_panel_wanted;      // the user's intent
static float     g_panel_t;           // the slide: 0 = out of sight, 1 = fully in
static ULONGLONG g_panel_tick;        // when g_panel_t was last stepped; 0 = not animating

static bool FeedPanelVisible() { return g_panel_wanted || g_panel_t > 0.0f; }
static bool BridgeDirty();
// Opening reads the tool's settings again (the tool may have loaded since, or its own page may
// have changed them) -- unless there are changes here that were not applied yet.
static void FeedPanelShow(bool show)
{
    if (show && !g_panel_wanted && !(g_br_loaded && BridgeDirty())) g_br_loaded = false;
    g_panel_wanted = show;
}

static const char *BridgeBackendName()
{
    return g_br_backend == BR_BACKEND_RENODX ? "RenoDX DLSS5 Generic" : g_br_backend == BR_BACKEND_OPTI ? "OptiScaler DLSS-NR"
         : "no supported tool";
}
static const char *BridgeBackendShort()
{
    return g_br_backend == BR_BACKEND_OPTI ? "OptiScaler" : "Generic";   // short: it labels half-width tabs
}
static const BridgeSide &BridgeSideOf(int i)
{
    if (g_br_backend == BR_BACKEND_OPTI) return kBridge[i].opti;
    return g_br_reno8 && kBridge[i].reno8.key != nullptr ? kBridge[i].reno8 : kBridge[i].reno;
}
static const char *BridgeKey(int i) { return g_br_absent[i] ? nullptr : BridgeSideOf(i).key; }
static const char *BridgeSection()  { return g_br_backend == BR_BACKEND_OPTI ? "DlssNr" : "RenoDX.DLSS5"; }

// A setting only one of the tools has: it goes on the tool's own tab.
static bool BridgeSpecific(int i)
{
    const BridgeItem &it = kBridge[i];
    return (it.reno.key != nullptr || it.reno8.key != nullptr) + (it.opti.key != nullptr) < 2;
}
// A row is on show when the tool has the setting, it belongs to the tab in front, and the
// choice it depends on is the one made.
static bool BridgeShown(int i)
{
    const BridgeItem &it = kBridge[i];
    return BridgeKey(i) != nullptr && BridgeSpecific(i) == (g_br_tab == 1) &&
           (it.need_row < 0 || static_cast<int>(g_br[it.need_row]) == it.need_item);
}
// The number the tool is given for a row: its spelling of a tick, its combo value, its units.
static float BridgeOut(int i)
{
    const BridgeItem &it = kBridge[i];
    const BridgeSide &s = BridgeSideOf(i);
    if (it.kind == BR_BOOL)  return g_br[i] != 0.0f ? (s.on != 0.0f ? s.on : 1.0f) : 0.0f;
    if (it.kind == BR_COMBO) return it.values != nullptr ? static_cast<float>(it.values[static_cast<int>(g_br[i])]) : g_br[i];
    return s.scale != 0.0f ? g_br[i] * s.scale : g_br[i];
}

// One value of the tool's saved settings. OptiScaler's are a plain ini; the RenoDX add-on
// saves through its ReShade -- the helper's ini when the tool is over there, this process's
// own ReShade (asked through the add-on API, so nothing is written behind its back) when not.
static bool BridgeRead(const char *section, const char *key, char *out, size_t cb)
{
    out[0] = '\0';
    if (g_br_backend == BR_BACKEND_OPTI || g_panel_env.helper)
    {
        const char *file = g_br_backend == BR_BACKEND_OPTI ? g_panel_env.opti_ini : g_panel_env.reshade_ini;
        GetPrivateProfileStringA(section, key, "\x01", out, static_cast<DWORD>(cb), file);
        if (out[0] == '\x01') { out[0] = '\0'; return false; }
        return true;
    }
    size_t size = cb;
    return reshade::get_config_value(nullptr, section, key, out, &size) && out[0] != '\0';
}
static void BridgeWrite(const char *section, const char *key, const char *value)
{
    if (g_br_backend == BR_BACKEND_OPTI || g_panel_env.helper)
        WritePrivateProfileStringA(section, key, value, g_br_backend == BR_BACKEND_OPTI ? g_panel_env.opti_ini : g_panel_env.reshade_ini);
    else
        reshade::set_config_value(nullptr, section, key, value);
}

static void BridgeLoad()
{
    if (g_panel_env.refresh != nullptr) g_panel_env.refresh();
    // OptiScaler's neural fork if it is there; else the Generic RenoDX add-on (any name that
    // starts renodx-dlss5 -- ShortFuse's renodx-dlss.addon64 does not, and is not a tool here).
    // Deep Fried Chicken keeps its own file and its own page, and is not bridged by this prototype.
    char path[MAX_PATH];
    sprintf_s(path, "%srenodx-dlss5*.addon64", g_panel_env.tool_dir);
    WIN32_FIND_DATAA fd;
    const HANDLE fh = FindFirstFileA(path, &fd);
    const bool have_reno = fh != INVALID_HANDLE_VALUE;
    if (have_reno) FindClose(fh);
    g_br_backend = g_panel_env.opti_nr ? BR_BACKEND_OPTI : have_reno ? BR_BACKEND_RENODX : BR_BACKEND_NONE;

    char buf[64];
    // A RenoDX build from v8 on writes ConfigVersion and its whole set of settings into its
    // section. Once it has, a name that is not there is one this build does not have (10.8.0
    // has neither NRGlobalTone nor NRPreUpscale): the row is left out, not shown doing nothing.
    const bool full_set = g_br_backend == BR_BACKEND_RENODX && BridgeRead(BridgeSection(), "ConfigVersion", buf, sizeof(buf));
    g_br_reno8 = full_set;
    for (int i = 0; i < kBridgeCount; ++i)
    {
        const BridgeItem &it = kBridge[i];
        const BridgeSide &side = BridgeSideOf(i);
        g_br[i] = side.def;
        g_br_absent[i] = false;
        if (g_br_backend == BR_BACKEND_NONE || side.key == nullptr) { g_br_saved[i] = g_br[i]; continue; }
        const bool have = BridgeRead(BridgeSection(), side.key, buf, sizeof(buf));
        if (!have && full_set) { g_br_absent[i] = true; g_br_saved[i] = g_br[i]; continue; }
        // OptiScaler writes "auto" for "my own default", and booleans as words.
        if (have && _stricmp(buf, "auto") != 0 && buf[0] != '\0')
        {
            float v = _stricmp(buf, "true") == 0 ? 1.0f : _stricmp(buf, "false") == 0 ? 0.0f : static_cast<float>(atof(buf));
            if (it.kind == BR_COMBO && it.values != nullptr)   // the number in the file -> which item that is
            {
                int item = 0;
                for (int k = 0; k < it.item_count; ++k) if (it.values[k] == static_cast<int>(v)) item = k;
                v = static_cast<float>(item);
            }
            else if (it.kind == BR_BOOL) v = v != 0.0f ? 1.0f : 0.0f;      // any "on" the tool has is a tick
            else if (it.kind == BR_FLOAT && side.scale != 0.0f) v /= side.scale;
            g_br[i] = v < side.lo ? side.lo : v > side.hi ? side.hi : v;
        }
        g_br_saved[i] = g_br[i];
    }
    // Which rows the tool takes while it runs. OptiScaler with live settings: all of them.
    // RenoDX: the names its control interface lists -- asked directly when it is in this
    // process, read from the file the helper writes at start when it is over there.
    g_br_livekeys[0] = '\n';
    g_br_livekeys[1] = '\0';
    if (g_br_backend == BR_BACKEND_RENODX && g_panel_env.send_setting != nullptr)
    {
        if (!g_panel_env.helper && g_panel_env.live_keys != nullptr)
            g_panel_env.live_keys(g_br_livekeys + 1, sizeof(g_br_livekeys) - 1);
        else if (g_panel_env.helper)
        {
            char lp[MAX_PATH];
            sprintf_s(lp, "%sdlss5-feed-live.txt", g_panel_env.tool_dir);
            FILE *lf = nullptr;
            if (fopen_s(&lf, lp, "r") == 0 && lf != nullptr)
            {
                const size_t got = fread(g_br_livekeys + 1, 1, sizeof(g_br_livekeys) - 2, lf);
                g_br_livekeys[1 + got] = '\0';
                fclose(lf);
            }
        }
    }
    int live_rows = 0;
    for (int i = 0; i < kBridgeCount; ++i)
    {
        g_br_rowlive[i] = false;
        const char *k = BridgeKey(i);
        if (k == nullptr) continue;
        if (g_br_backend == BR_BACKEND_OPTI) g_br_rowlive[i] = g_panel_env.opti_live;
        else if (g_br_backend == BR_BACKEND_RENODX)
        {
            char pat[96];
            sprintf_s(pat, "\n%s\n", k);
            g_br_rowlive[i] = strstr(g_br_livekeys, pat) != nullptr;
        }
        if (g_br_rowlive[i]) ++live_rows;
    }
    g_br_loaded = true;
    if (g_br_backend == BR_BACKEND_RENODX && g_panel_env.log != nullptr)
        g_panel_env.log("[panel] RenoDX control interface: %d of this panel's settings are taken while it runs%s", live_rows,
                        live_rows == 0 ? " (none: an older build, or the helper has not listed them yet)" : "");
    if (g_panel_env.log != nullptr)
        g_panel_env.log("[panel] tool behind the panel: %s (%s); %s", BridgeBackendName(),
            g_panel_env.helper ? "in the helper process" : "in this process",
            g_br_backend == BR_BACKEND_OPTI && g_panel_env.opti_live ? "this build re-reads its file while running: changes apply live"
            : g_panel_env.helper ? "it reads its file at start only: Apply restarts the helper"
                                 : "it reads its file at start only: changes wait for the next start of the game");
}

// Every row is taken while the tool runs, so nothing ever has to restart (OptiScaler with live settings).
static bool BridgeLive()
{
    return g_br_backend == BR_BACKEND_OPTI && g_panel_env.opti_live;
}
// Some rows are (RenoDX from v10): those apply at once, the others keep the Apply button.
static bool BridgeAnyLive()
{
    for (int i = 0; i < kBridgeCount; ++i) if (g_br_rowlive[i] && BridgeKey(i) != nullptr) return true;
    return false;
}
static bool BridgeDirty()
{
    for (int i = 0; i < kBridgeCount; ++i) if (g_br[i] != g_br_saved[i]) return true;
    return false;
}
static bool BridgeDirtyLive()
{
    for (int i = 0; i < kBridgeCount; ++i) if (g_br_rowlive[i] && g_br[i] != g_br_saved[i]) return true;
    return false;
}
static bool BridgeDirtyRest()
{
    for (int i = 0; i < kBridgeCount; ++i) if (!g_br_rowlive[i] && g_br[i] != g_br_saved[i]) return true;
    return false;
}

static void BridgeFormat(int i, char *buf, size_t cb)
{
    const BridgeItem &it = kBridge[i];
    const float out = BridgeOut(i);
    if (it.kind == BR_FLOAT && !it.whole) sprintf_s(buf, cb, "%g", out);
    else if (it.kind == BR_BOOL && g_br_backend == BR_BACKEND_OPTI) strcpy_s(buf, cb, out != 0.0f ? "true" : "false");
    else sprintf_s(buf, cb, "%d", static_cast<int>(out));
}

// Make what changed count, in the tool's own spelling.
//   Rows the tool takes while it runs go to it at once: OptiScaler through its ini, which it
//     re-reads; RenoDX through its control interface, which also saves the value itself.
//   The others are written to the tool's file -- only when the Apply button asked for it
//     (g_br_apply_full): the helper is stopped first (its ReShade saves its ini on exit and
//     would write over ours) and started again after; with no helper they wait for a restart.
static void FeedPanelApply()
{
    const bool running = g_panel_env.running == nullptr || g_panel_env.running();
    const bool full = g_br_apply_full;
    g_br_apply_full = false;
    char buf[64], line[512];

    int n = sprintf_s(line, "[panel] applied LIVE to %s:", BridgeBackendName()), sent = 0;
    if (running)
        for (int i = 0; i < kBridgeCount; ++i)
        {
            const char *key = BridgeKey(i);
            if (key == nullptr || !g_br_rowlive[i] || g_br[i] == g_br_saved[i]) continue;
            BridgeFormat(i, buf, sizeof(buf));
            if (g_br_backend == BR_BACKEND_OPTI) BridgeWrite(BridgeSection(), key, buf);
            else if (g_panel_env.send_setting == nullptr || !g_panel_env.send_setting(key, BridgeOut(i))) continue;   // stays changed: Apply can still take it
            g_br_saved[i] = g_br[i];
            ++sent;
            if (n > 0 && n < static_cast<int>(sizeof(line)) - 80) n += sprintf_s(line + n, sizeof(line) - n, " %s=%s", key, buf);
        }
    if (sent > 0 && g_panel_env.log != nullptr) g_panel_env.log("%s (%d setting(s), no restart)", line, sent);
    if (!BridgeDirty()) { if (sent > 0) g_br_note[0] = '\0'; return; }
    if (!full) return;   // what is left needs the Apply button

    if (g_panel_env.stop_tool != nullptr) g_panel_env.stop_tool();
    n = sprintf_s(line, "[panel] applied to %s:", BridgeBackendName());
    int wrote = 0;
    for (int i = 0; i < kBridgeCount; ++i)
    {
        const char *key = BridgeKey(i);
        if (key == nullptr || g_br[i] == g_br_saved[i]) continue;   // only what the user moved
        BridgeFormat(i, buf, sizeof(buf));
        BridgeWrite(BridgeSection(), key, buf);
        g_br_saved[i] = g_br[i];
        ++wrote;
        if (n > 0 && n < static_cast<int>(sizeof(line)) - 80) n += sprintf_s(line + n, sizeof(line) - n, " %s=%s", key, buf);
    }
    if (g_panel_env.log != nullptr) g_panel_env.log("%s (%d key(s) in [%s])", line, wrote, BridgeSection());
    if (g_panel_env.restart_tool != nullptr)
    {
        g_panel_env.restart_tool();
        sprintf_s(g_br_note, "Applied %d setting%s. The helper is restarting.", wrote, wrote == 1 ? "" : "s");
    }
    else
        sprintf_s(g_br_note, "Saved %d setting%s. %s reads them when the game is next started.", wrote, wrote == 1 ? "" : "s", BridgeBackendShort());
}

static void FeedPanelStep(bool in)
{
    const ULONGLONG now = GetTickCount64();
    float dt = g_panel_tick == 0 ? 0.0f : static_cast<float>(now - g_panel_tick) / 180.0f;   // 180 ms end to end
    if (dt > 0.34f) dt = 0.34f;   // a hitch must not swallow the whole slide
    g_panel_tick = now;
    g_panel_t += in ? dt : -dt;
    if (g_panel_t < 0.0f) g_panel_t = 0.0f; else if (g_panel_t > 1.0f) g_panel_t = 1.0f;
}

// Once a frame, from reshade_present: the shortcut, the slide, and the input that goes with it.
// The modifier match is exact both ways, so Alt+C does not also answer to Ctrl+Alt+C (AltGr+C).
static void FeedPanelTick(reshade::api::effect_runtime *rt)
{
    if (rt == nullptr) return;
    const int held = (rt->is_key_down(VK_MENU) ? 1 : 0) | (rt->is_key_down(VK_CONTROL) ? 2 : 0) | (rt->is_key_down(VK_SHIFT) ? 4 : 0);
    if (g_panel_key > 0 && rt->is_key_pressed(static_cast<uint32_t>(g_panel_key)) && held == (g_panel_mods & 7))
    {
        FeedPanelShow(!g_panel_wanted);
        if (g_panel_env.log != nullptr) g_panel_env.log("[panel] %s (shortcut)", g_panel_wanted ? "shown" : "hidden");
    }
    else if (g_panel_wanted && rt->is_key_pressed(VK_ESCAPE))
    {
        g_panel_wanted = false;
        if (g_panel_env.log != nullptr) g_panel_env.log("[panel] hidden (Escape)");
    }
    if (!g_panel_wanted && g_panel_t <= 0.0f) { g_panel_tick = 0; return; }
    if (!g_br_loaded) BridgeLoad();
    FeedPanelStep(g_panel_wanted);
    if (g_panel_wanted) rt->block_input_next_frame();   // the panel owns mouse and keyboard while it is up
}

// From reshade_overlay: the panel itself.
static void FeedPanelDraw(reshade::api::effect_runtime *rt)
{
    ImGuiIO &io = ImGui::GetIO();
    if (g_panel_t <= 0.0f)
    {
        if (g_br_cursor_forced) { io.MouseDrawCursor = false; g_br_cursor_forced = false; }
        return;
    }
    // ReShade draws its software cursor only while its own overlay is open; the game has
    // usually hidden the system one.
    if (g_panel_wanted && !io.MouseDrawCursor) { io.MouseDrawCursor = true; g_br_cursor_forced = true; }

    const ImVec2 disp = io.DisplaySize;
    const bool   right = g_panel_side != 0;
    // Every length below is in design pixels times `s` (see g_panel_scale); the text is pushed
    // at 13 design pixels times `s`, whatever size ReShade's own overlay font has.
    const float s = FeedPanelScale(disp.y, FeedPanelDpiScale(rt != nullptr ? static_cast<HWND>(rt->get_hwnd()) : nullptr));
    const auto  S = [s](float v) { return floorf(v * s + 0.5f); };
    float w = disp.x * 0.30f;
    if (w < S(380.0f)) w = S(380.0f); else if (w > S(520.0f)) w = S(520.0f);
    if (w > disp.x * 0.9f) w = disp.x * 0.9f;
    {
        const ImGuiStyle &st = ImGui::GetStyle();
        const float mult = st.FontSizeBase > 0.0f ? ImGui::GetFontSize() / st.FontSizeBase : 1.0f;   // ReShade's own scale factors
        ImGui::PushFont(nullptr, 13.0f * s / (mult > 0.0f ? mult : 1.0f));
    }
    const float u = 1.0f - g_panel_t, ease = 1.0f - u * u * u;
    const float x = right ? disp.x - w * ease : -w * (1.0f - ease);

    const ImU32 accent = IM_COL32(118, 185, 0, 255);
    // The picture beside the panel is left exactly as it is: the panel is there to judge what
    // the settings do to it, and a dim over it hid that (user, 2026-11-12).

    ImGui::SetNextWindowPos(ImVec2(x, 0.0f), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(w, disp.y), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22.0f), S(18.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10.0f), S(9.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10.0f), S(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, S(5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, S(14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(S(6.0f), S(4.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,         IM_COL32(16, 17, 21, 246));
    ImGui::PushStyleColor(ImGuiCol_Text,             IM_COL32(232, 233, 238, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,          IM_COL32(38, 40, 48, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   IM_COL32(50, 53, 63, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    IM_COL32(58, 62, 74, 255));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(150, 220, 20, 255));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,        accent);
    ImGui::PushStyleColor(ImGuiCol_Button,           IM_COL32(46, 49, 58, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,    IM_COL32(62, 66, 78, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,     IM_COL32(74, 79, 94, 255));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,          IM_COL32(24, 25, 31, 252));
    ImGui::PushStyleColor(ImGuiCol_Header,           IM_COL32(60, 96, 0, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,    IM_COL32(80, 128, 0, 255));
    const ImVec4 muted(0.56f, 0.57f, 0.62f, 1.0f);
    const float  cw = w - S(44.0f);   // the width the content has

    if (ImGui::Begin("##dlss5_feed_panel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        // The inner edge: an accent line the height of the window.
        const float ex = right ? x : x + w - S(2.0f);
        dl->AddRectFilled(ImVec2(ex, 0.0f), ImVec2(ex + S(2.0f), disp.y), accent);

        {
            // The title, in the overlay font at twice its size (the add-on API has no font
            // scale for a window; the draw list takes a size per call).
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  fs = ImGui::GetFontSize();
            dl->AddText(nullptr, fs * 2.0f, p, IM_COL32(240, 241, 245, 255), "DLSS 5 Feeder", nullptr, 0.0f, nullptr);
            dl->AddText(ImVec2(p.x + S(1.0f), p.y + fs * 2.0f + S(2.0f)), IM_COL32(143, 145, 158, 255), "Author: winterelfeas");
            ImGui::Dummy(ImVec2(cw, fs * 3.0f + S(10.0f)));
        }

        // Which tool is behind the panel, where it runs, and whether it is up.
        const bool running  = g_panel_env.running == nullptr || g_panel_env.running();
        const bool starting = g_panel_env.starting != nullptr && g_panel_env.starting();
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  h = ImGui::GetFrameHeight();
            dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + h), IM_COL32(30, 32, 39, 255), S(5.0f));
            dl->AddCircleFilled(ImVec2(p.x + S(14.0f), p.y + h * 0.5f), 4.5f * s, running ? accent : IM_COL32(230, 160, 40, 255));
            char chip[128];
            sprintf_s(chip, "%s  -  %s", BridgeBackendName(),
                      !g_panel_env.helper ? "in the game" : running ? "running" : starting ? "starting" : "restarting");
            dl->AddText(ImVec2(p.x + S(28.0f), p.y + (h - ImGui::GetFontSize()) * 0.5f), IM_COL32(210, 212, 220, 255), chip);
            ImGui::Dummy(ImVec2(cw, h));
            // What it costs: the GPU time of the feed's own work on one frame, which is DLSS and
            // the neural pass together (the tool runs inside that work).
            const float gpu = g_panel_env.gpu_ms != nullptr ? g_panel_env.gpu_ms() : 0.0f;
            if (gpu > 0.0f) ImGui::TextColored(muted, "GPU cost  %.1f ms per frame  (DLSS + neural pass)", gpu);
            else            ImGui::TextColored(muted, "GPU cost  not measured yet");
        }

        // Two tabs: what every tool has under one name, and what only the tool behind has.
        {
            int specific = 0;
            for (int i = 0; i < kBridgeCount; ++i) if (BridgeKey(i) != nullptr && BridgeSpecific(i)) ++specific;
            if (specific == 0) g_br_tab = 0;
            else
            {
                ImGui::Dummy(ImVec2(1.0f, S(2.0f)));
                const float tw = (cw - S(6.0f)) * 0.5f;
                char label[64];
                for (int t = 0; t < 2; ++t)
                {
                    const bool on = g_br_tab == t;
                    ImGui::PushStyleColor(ImGuiCol_Button, on ? IM_COL32(92, 146, 0, 255) : IM_COL32(34, 36, 44, 255));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? IM_COL32(104, 164, 0, 255) : IM_COL32(50, 53, 63, 255));
                    if (t == 0) strcpy_s(label, "Common##brtab0"); else sprintf_s(label, "%s only (%d)##brtab1", BridgeBackendShort(), specific);
                    if (ImGui::Button(label, ImVec2(tw, S(28.0f)))) g_br_tab = t;
                    ImGui::PopStyleColor(2);
                    if (t == 0) ImGui::SameLine(0.0f, S(6.0f));
                }
            }
        }

        // The rows scroll in a region of their own when the window is too short for them (a
        // tool with more settings, a 720p game); the foot of the panel stays in sight.
        const float footer_h = S(34.0f) + ImGui::GetFontSize() * 5.0f + S(40.0f) + ImGui::GetFrameHeight() + S(9.0f);   // + the size row
        ImGui::BeginChild("##br_rows", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - footer_h), 0, 0);
        ImDrawList *rl = ImGui::GetWindowDrawList();
        const float row_w = ImGui::GetContentRegionAvail().x;
        const bool  opti = g_br_backend == BR_BACKEND_OPTI;
        // With a tool that takes changes live there is no Apply: a tick or a choice counts at
        // once, a slider when it is let go (a change can rebuild the model, a fifth of a second).
        bool        commit = false;
        const bool  mixed = !BridgeLive() && BridgeAnyLive();   // some rows apply at once, some need Apply
        const char *group = nullptr;
        for (int i = 0; i < kBridgeCount; ++i)
        {
            const BridgeItem &it = kBridge[i];
            if (!BridgeShown(i)) continue;   // the tool behind has no such setting, or it does not apply right now
            float lo = BridgeSideOf(i).lo, hi = BridgeSideOf(i).hi;
            // OptiScaler's pass limit follows its own unlock switch (2, or 10).
            if (opti && it.whole)
                for (int k = 0; k < kBridgeCount; ++k)
                    if (kBridge[k].opti.key != nullptr && strcmp(kBridge[k].opti.key, "UnlockPasses") == 0 && g_br[k] != 0.0f) hi = 10.0f;
            if (group == nullptr || strcmp(group, it.group) != 0)
            {
                group = it.group;
                ImGui::Dummy(ImVec2(1.0f, S(6.0f)));
                ImGui::TextColored(ImVec4(0.46f, 0.73f, 0.0f, 1.0f), "%s", group);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                rl->AddLine(ImVec2(p.x, p.y - S(3.0f)), ImVec2(p.x + row_w, p.y - S(3.0f)), IM_COL32(52, 55, 66, 255), S(1.0f));
            }
            char id[96];
            sprintf_s(id, "##br%d", i);
            if (it.kind == BR_BOOL)
            {
                bool v = g_br[i] != 0.0f;
                sprintf_s(id, "%s%s##br%d", it.label, (mixed && !g_br_rowlive[i]) ? " *" : "", i);
                if (ImGui::Checkbox(id, &v)) { g_br[i] = v ? 1.0f : 0.0f; commit = true; }
                continue;
            }
            ImGui::TextColored(muted, "%s%s", it.label, (mixed && !g_br_rowlive[i]) ? " *" : "");
            ImGui::SetNextItemWidth(row_w);
            if (it.kind == BR_COMBO)
            {
                const BridgeSide &side = BridgeSideOf(i);
                const char *const *names = side.items != nullptr ? side.items : it.items;
                const int count = side.items != nullptr ? side.item_count : it.item_count;
                int v = static_cast<int>(g_br[i]);
                if (v < 0 || v >= count) v = 0;
                if (ImGui::Combo(id, &v, names, count)) { g_br[i] = static_cast<float>(v); commit = true; }
            }
            else if (it.whole)
            {
                int v = static_cast<int>(g_br[i]);
                if (ImGui::SliderInt(id, &v, static_cast<int>(lo), static_cast<int>(hi))) g_br[i] = static_cast<float>(v);
                if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
            }
            else
            {
                ImGui::SliderFloat(id, &g_br[i], lo, hi, it.format != nullptr ? it.format : "%.2f",
                                   it.log ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None);
                if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
            }
        }
        ImGui::EndChild();

        ImGui::Dummy(ImVec2(1.0f, S(4.0f)));
        const bool dirty = BridgeDirtyRest(), can = dirty && g_br_backend != BR_BACKEND_NONE && !starting;
        const bool some_live = !BridgeLive() && BridgeAnyLive();
        bool apply = false;
        char key[96];
        FeedPanelKeyName(key, sizeof(key));
        if (BridgeLive())
        {
            apply = commit && BridgeDirty() && !starting;   // every row is live here: `dirty` (rows that need Apply) is never set
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, ImVec2(p.x + S(44.0f), p.y + ImGui::GetFrameHeight()), IM_COL32(60, 96, 0, 255), S(5.0f));
            dl->AddText(ImVec2(p.x + S(9.0f), p.y + (ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f), IM_COL32(236, 248, 210, 255), "LIVE");
            ImGui::Dummy(ImVec2(S(44.0f), ImGui::GetFrameHeight()));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
            ImGui::TextColored(muted, "%s", running ? "Changes take effect as you make them; a slider counts when you let it go. Nothing restarts."
                                                    : "The helper is not running. Changes are saved and used when it starts.");
        }
        else
        {
            const float bw = (cw - S(10.0f)) * 0.5f;
            if (!can) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(92, 146, 0, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(110, 172, 0, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(128, 200, 0, 255));
            if (ImGui::Button("Apply", ImVec2(bw, S(34.0f)))) { strcpy_s(g_br_note, "Applying."); apply = true; g_br_apply_full = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            if (ImGui::Button("Revert", ImVec2(bw, S(34.0f)))) { for (int i = 0; i < kBridgeCount; ++i) g_br[i] = g_br_saved[i]; g_br_note[0] = '\0'; }
            if (!can) ImGui::EndDisabled();
            // The rows the tool takes while it runs go at once, without the button.
            if (some_live && commit && BridgeDirtyLive() && !starting) apply = true;

            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
            const bool restarts = g_panel_env.restart_tool != nullptr;
            if (g_br_backend == BR_BACKEND_NONE)
                ImGui::TextColored(muted, "No RenoDX DLSS5 Generic add-on or OptiScaler DLSS-NR was found beside the feeder.");
            else if (g_br_note[0] != '\0' && !dirty)
                ImGui::TextColored(muted, "%s", running && strstr(g_br_note, "restarting") != nullptr ? "Applied. The helper is running with the new settings." : g_br_note);
            else if (dirty)
                ImGui::TextColored(muted, "%s", some_live ? (restarts ? "A row marked * changed. Apply restarts the helper for it (a few seconds; the game keeps running)."
                                                                   : "A row marked * changed. Apply saves it; it is used when the game starts.")
                                             : restarts ? "Changed. Apply restarts the helper, which takes a few seconds; the game keeps running."
                                                         : "Changed. Apply saves it; this tool reads its settings when the game starts.");
            else
                ImGui::TextColored(muted, "%s", some_live ? (restarts ? "Changes apply at once, with no restart. Rows marked * need Apply, which restarts the helper."
                                                                   : "Changes apply at once. Rows marked * need Apply and are used when the game starts.")
                                             : restarts ? "These settings are stored in the tool's own file and take effect when the helper starts."
                                                         : "These settings are stored in the tool's own file and take effect when the game starts.");
        }
        ImGui::TextColored(muted, g_panel_key > 0 ? "%s or Esc closes this panel." : "Esc closes this panel.", key);
        ImGui::PopTextWrapPos();

        // The panel's own size: steps of 10%, or automatic (the picture's height and the desktop scaling).
        // Buttons, not a slider -- a slider would move under the pointer as the panel grew.
        {
            const int   now = static_cast<int>(s * 100.0f + 0.5f);
            int         want = -1;
            const float fh = ImGui::GetFrameHeight();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(muted, "Panel size");
            ImGui::SameLine();
            if (ImGui::Button("-##brsz", ImVec2(fh, fh))) want = ((now + 5) / 10) * 10 - 10;
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d%%", now);
            ImGui::SameLine();
            if (ImGui::Button("+##brsz", ImVec2(fh, fh))) want = ((now + 5) / 10) * 10 + 10;
            ImGui::SameLine();
            const bool is_auto = g_panel_scale <= 0;
            if (is_auto) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 96, 0, 255));
            if (ImGui::Button("Auto##brsz", ImVec2(0.0f, fh))) want = 0;
            if (is_auto) ImGui::PopStyleColor(1);
            if (want >= 0)
            {
                g_panel_scale = want == 0 ? 0 : want < 50 ? 50 : want > 400 ? 400 : want;
                FeedPanelSaveCfg("panel_scale", g_panel_scale);
            }
        }

        if (apply)
        {
            if (g_panel_env.schedule_apply != nullptr) g_panel_env.schedule_apply();
            else FeedPanelApply();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(14);
    ImGui::PopStyleVar(10);
    ImGui::PopFont();
}
