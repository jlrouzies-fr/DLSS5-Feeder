// feed_hud.h -- lightweight in-game ImGui stats panel (no PresentMon).
//
// Include near TimingTick / DrawOverlay. Hotkey key_hud (default F9) toggles hud_stats.
// Cfg lives in feed_async.h (early). Register overlay title "DLSS 5 Feed · HUD".

#pragma once

enum { kFeedHudSpark = 64 };

struct FeedHudState
{
    float fps_ring[kFeedHudSpark];
    int   fps_i;
    int   fps_n;
    float last_fps;
    float last_frame_ms;
    float last_cpu_pct;
    float last_gpu_ms;
    float last_eval_ms;
    int sample_countdown;
};

static FeedHudState g_feed_hud = {};

static void FeedHudPushFps(float fps)
{
    if (fps < 1.f || fps > 1000.f)
        return;
    g_feed_hud.fps_ring[g_feed_hud.fps_i % kFeedHudSpark] = fps;
    g_feed_hud.fps_i = (g_feed_hud.fps_i + 1) % kFeedHudSpark;
    if (g_feed_hud.fps_n < kFeedHudSpark)
        ++g_feed_hud.fps_n;
    g_feed_hud.last_fps = fps;
}

// Call from TimingTick when interval known. Samples cheaply unless HUD is visible.
static void FeedHudOnInterval(double interval_ms)
{
    if (interval_ms < 1.0 || interval_ms > 250.0)
        return;
    g_feed_hud.last_frame_ms = (float)interval_ms;
    const float fps = 1000.f / (float)interval_ms;
    FeedHudPushFps(fps);

    if (g_feed_hud_cfg.enabled == 0)
        return;
    if (++g_feed_hud.sample_countdown < 8)
        return;
    g_feed_hud.sample_countdown = 0;
    const float cpu = FeedPerfCpuPercent();
    if (cpu >= 0.f)
        g_feed_hud.last_cpu_pct = cpu;
    if (g.ts_n > 0)
        g_feed_hud.last_gpu_ms = (float)(g.ts_sum_ms / (double)g.ts_n);
    if (g.qpf > 0 && g_last_eval_ticks > 0)
        g_feed_hud.last_eval_ms = (float)(1000.0 * (double)g_last_eval_ticks / (double)g.qpf);
}

static const char *FeedHudKeyName(int vk)
{
    switch (vk)
    {
    case 0x70: return "F1";  case 0x71: return "F2";  case 0x72: return "F3";
    case 0x73: return "F4";  case 0x74: return "F5";  case 0x75: return "F6";
    case 0x76: return "F7";  case 0x77: return "F8";  case 0x78: return "F9";
    case 0x79: return "F10"; case 0x7A: return "F11"; case 0x7B: return "F12";
    default: return "key";
    }
}

static void FeedPollConfigHotkeys()
{
    static bool prev_async = false;
    static bool prev_hud = false;
    const bool async_down = (GetAsyncKeyState(g_feed_async_cfg.key) & 0x8000) != 0;
    const bool hud_down = (GetAsyncKeyState(g_feed_hud_cfg.key) & 0x8000) != 0;
    if (async_down && !prev_async)
    {
        g_feed_async_cfg.enabled = g_feed_async_cfg.enabled ? 0 : 1;
        FeedAsyncUpdateLagMetric();
        CfgSave();
        Log("[feed] async_feed=%d (hotkey %s) — display lag %d frame(s)",
            g_feed_async_cfg.enabled, FeedHudKeyName(g_feed_async_cfg.key),
            g_display_lag_frames);
        FeedPerfMarkDirty();
    }
    if (hud_down && !prev_hud)
    {
        g_feed_hud_cfg.enabled = g_feed_hud_cfg.enabled ? 0 : 1;
        CfgSave();
        Log("[feed] hud_stats=%d (hotkey %s)", g_feed_hud_cfg.enabled,
            FeedHudKeyName(g_feed_hud_cfg.key));
    }
    prev_async = async_down;
    prev_hud = hud_down;
}

static void DrawFeedHudPanel(reshade::api::effect_runtime * /*rt*/)
{
    if (g_feed_hud_cfg.enabled == 0)
        return;

    const ImGuiIO &io = ImGui::GetIO();
    const float pad = 10.f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, pad), ImGuiCond_Always, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (!ImGui::Begin("##dlss5_feed_hud", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    if (g_feed_async_cfg.enabled)
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
                           "Async feed ON · display lag ~1 frame");
    else
        ImGui::TextDisabled("Async feed OFF · display lag 0");

    ImGui::Separator();
    ImGui::Text("FPS  %.1f", g_feed_hud.last_fps);
    ImGui::Text("frame  %.2f ms", g_feed_hud.last_frame_ms);
    ImGui::Text("CPU  %.0f%%", g_feed_hud.last_cpu_pct);
    ImGui::Text("GPU feed  %.2f ms", g_feed_hud.last_gpu_ms);
    ImGui::Text("NR (eval CPU)  %.2f ms", g_feed_hud.last_eval_ms);
    ImGui::Text("async  %s", g_feed_async_cfg.enabled ? "on" : "off");
    ImGui::Text("display_lag_frames  %d", g_display_lag_frames);
    {
        const int wr = g_cfg.work_resolution;
        const int stride = g_feed_auto.evaluate_stride > 0 ? g_feed_auto.evaluate_stride : 1;
        ImGui::Text("work %d%% · stride %d", wr, stride);
    }

    if (g_feed_hud.fps_n > 4)
    {
        ImGui::PlotLines("##fps_spark", g_feed_hud.fps_ring, g_feed_hud.fps_n,
                         g_feed_hud.fps_i % kFeedHudSpark, nullptr, 0.f, 0.f, ImVec2(160.f, 28.f));
    }

    ImGui::TextDisabled("%s HUD · %s async",
                        FeedHudKeyName(g_feed_hud_cfg.key),
                        FeedHudKeyName(g_feed_async_cfg.key));
    ImGui::End();
}
