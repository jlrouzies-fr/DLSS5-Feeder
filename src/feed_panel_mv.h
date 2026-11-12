// feed_panel_mv.h -- what the menu needs to know, and can do, about motion vectors.
//
// The menu (feed_panel.h) says so at its top when DLSS5_Feed.fx is not using its own built-in
// estimator (DLSS5_MV_PROVIDER = 5), offers to switch to it, and afterwards names the other
// motion-vector shaders that are still switched on. The panel itself knows nothing of ReShade's
// effects; this is the add-on's side of that, the same in both add-ons.
//
// Included by dlss5-feed.cpp and dlss5-feed32.cpp just before their PanelSetup(), because it uses
// what each of them already has by the same names: g.runtime, kEffectFile, kMvProviders,
// kMvModeName, kMvModeBuiltIn, ReadMvProviderMode() and Log().

#pragma once

// The provider DLSS5_Feed.fx is compiled for; -1 when there is no runtime to ask yet.
static int PanelMvMode()
{
    return g.runtime != nullptr ? ReadMvProviderMode(g.runtime) : -1;
}
static const char *PanelMvName()
{
    const int m = PanelMvMode();
    return m >= 0 ? kMvModeName[m] : "unknown";
}

// Every known motion-vector shader that is switched on right now, by the name the user sees in
// ReShade's list, comma separated. Returns how many.
static int PanelMvEnabled(char *out, size_t cb)
{
    if (cb > 0) out[0] = '\0';
    if (g.runtime == nullptr) return 0;
    int    n = 0;
    size_t used = 0;
    for (const auto &p : kMvProviders)
    {
        const reshade::api::effect_technique t = g.runtime->find_technique(p.file, p.tech);
        if (t.handle == 0 || !g.runtime->get_technique_state(t)) continue;
        char label[96] = "";
        if (!g.runtime->get_annotation_string_from_technique(t, "ui_label", label) || label[0] == '\0') strcpy_s(label, p.tech);
        char item[200];
        const int len = sprintf_s(item, "%s%s (%s)", n > 0 ? ", " : "", label, p.file);
        if (len <= 0 || used + static_cast<size_t>(len) + 1 > cb) break;
        memcpy(out + used, item, static_cast<size_t>(len) + 1);
        used += static_cast<size_t>(len);
        ++n;
    }
    return n;
}

// DLSS5_MV_PROVIDER = 5 on DLSS5_Feed.fx alone (the effect's own definition wins over a global
// one, and no other effect is touched). ReShade recompiles the effect and saves the preset.
static void PanelMvUseBuiltIn()
{
    if (g.runtime == nullptr) return;
    char v[8];
    sprintf_s(v, "%d", kMvModeBuiltIn);
    Log("[panel] motion vectors: DLSS5_MV_PROVIDER set to %d (built in) on %s, from %d (%s); ReShade recompiles it", kMvModeBuiltIn, kEffectFile,
        PanelMvMode(), PanelMvName());
    g.runtime->set_preprocessor_definition_for_effect(kEffectFile, "DLSS5_MV_PROVIDER", v);
}

// Switch the motion-vector shaders off. Only on the user's click: another effect of theirs may
// read one of them (LumeniteFX's own effects read its Kernel), which the notice says.
static void PanelMvDisableOthers()
{
    if (g.runtime == nullptr) return;
    for (const auto &p : kMvProviders)
    {
        const reshade::api::effect_technique t = g.runtime->find_technique(p.file, p.tech);
        if (t.handle == 0 || !g.runtime->get_technique_state(t)) continue;
        g.runtime->set_technique_state(t, false);
        Log("[panel] motion vectors: switched off %s (%s)", p.tech, p.file);
    }
}

static void PanelMvSetup()
{
    g_panel_env.mv_mode           = PanelMvMode;
    g_panel_env.mv_name           = PanelMvName;
    g_panel_env.mv_enabled        = PanelMvEnabled;
    g_panel_env.mv_use_builtin    = PanelMvUseBuiltIn;
    g_panel_env.mv_disable_others = PanelMvDisableOthers;
}
