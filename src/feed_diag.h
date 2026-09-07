// feed_diag.h -- extended diagnostics for ME / ghosting / adapt tests.
// Dump-friendly lines tagged [diag] so a user can paste dlss5-feed.log back.
//
// Include AFTER feed_lightstab.h, feed_quality.h / g_cfg / g_feed_* are available.

#pragma once

struct FeedDiagCfg
{
    int  enabled;     // log_detail=1
    int  every_n;     // periodic dump interval in delivered frames (default 60)
};

static FeedDiagCfg g_feed_diag_cfg = { 1, 60 };

static void FeedDiagDump(const char *why, int reset_flag, UINT64 frame_n,
                         bool mask_ok, UINT bb_w, UINT bb_h, UINT work_w, UINT work_h)
{
    if (g_feed_diag_cfg.enabled == 0) return;

    const char *mv = "lumenite/reshade";
    if (g_feed_velocity.bound_ok) mv = "engine_velocity";
    else if (g_feed_ofa_cfg.enabled && FeedOfaReady()) mv = "ofa";
    else if (g_feed_ofa_cfg.enabled) mv = "ofa_pending";

    Log("[diag] ---- dump (%s) frame=%llu ----", why ? why : "?", frame_n);
#if defined(_M_X64) || defined(__x86_64__)
    Log("[diag] arch=x64 (in-process NGX). 32-bit games: use host64 helper — no in-process DLSS5 in a 32-bit PE.");
#else
    Log("[diag] arch=x86 — NGX cannot run here; host64 bridge is required.");
#endif
    Log("[diag] reset_mode=%d reset_every=%d InReset=%d",
        g_feed_adapt_cfg.reset_mode, g_cfg.reset_every, reset_flag);
    Log("[diag] adapt: %s | mask=%.4f lumaΔ=%.4f thr_mask=%.3f/%.3f thr_luma=%.3f rate=%.2f sampled=%d",
        g_feed_adapt.status[0] ? g_feed_adapt.status : "-",
        g_feed_adapt.mask_avg, g_feed_adapt.luma_delta,
        g_feed_adapt_cfg.mask_thr, g_feed_adapt_cfg.mask_thr_vel, g_feed_adapt_cfg.luma_thr,
        g_feed_adapt.reset_rate, g_feed_adapt.sampled_ok ? 1 : 0);
    Log("[diag] mv_source=%s vel_en=%d bound=%d cands=%d chosen=%d cand_cfg=%d decode=%d scale=%.2f",
        mv,
        g_feed_velocity_cfg.enabled,
        g_feed_velocity.bound_ok ? 1 : 0,
        g_feed_velocity.cand_count,
        g_feed_velocity.chosen_cand,
        g_feed_velocity_cfg.cand,
        g_feed_velocity_cfg.decode,
        g_feed_velocity_cfg.scale);
    if (g_feed_velocity.bound_ok)
        Log("[diag] vel_bound: src=%s name=\"%s\" %dx%d",
            g_feed_velocity.bound_src,
            g_feed_velocity.name[0] ? g_feed_velocity.name : "?",
            g_feed_velocity.last_w, g_feed_velocity.last_h);
    if (g_feed_velocity.cand_count > 0)
    {
        const int nshow = g_feed_velocity.cand_count < 8 ? g_feed_velocity.cand_count : 8;
        for (int i = 0; i < nshow; ++i)
        {
            const FeedVelocityCand &c = g_feed_velocity.cands[i];
            Log("[diag] cand#%d %s %ux%u score=%d draws=%d depth=%d name=\"%s\"",
                i, FeedVelocityFmtLabel(c.fmt), c.width, c.height, c.score,
                c.draws_total, c.saw_with_depth ? 1 : 0,
                c.name[0] ? c.name : "?");
        }
    }
    Log("[diag] ofa_en=%d ofa_ready=%d grid=%d perf=%d | work=%d%% upscale=%d sharp=%.2f preset=%d mask_ok=%d",
        g_feed_ofa_cfg.enabled, FeedOfaReady() ? 1 : 0, g_feed_ofa_cfg.grid, g_feed_ofa_cfg.perf,
        g_cfg.work_resolution, g_cfg.work_upscale, g_cfg.work_sharpness, g_cfg.preset,
        mask_ok ? 1 : 0);
    Log("[diag] quality=%s custom=%d auto=%s applied=%d | bb=%ux%u work_tex=%ux%u lightstab=%d str=%.2f",
        g_feed_quality_customized ? "custom" : FeedQualityCfgName(g_feed_quality),
        g_feed_quality_customized ? 1 : 0,
        g_feed_auto.name[0] ? g_feed_auto.name : "-",
        g_feed_auto.applied,
        bb_w, bb_h, work_w, work_h,
        g_feed_lightstab_cfg.enabled, g_feed_lightstab_cfg.strength);
    if (g_feed_lightstab.status[0])
        Log("[diag] %s", g_feed_lightstab.status);
}

static void FeedDiagOnFrame(UINT64 n, int reset_flag,
                            bool mask_ok, UINT bb_w, UINT bb_h, UINT work_w, UINT work_h)
{
    if (g_feed_diag_cfg.enabled == 0) return;
    const int every = g_feed_diag_cfg.every_n > 0 ? g_feed_diag_cfg.every_n : 60;
    static int prev_reset = 0;
    const bool reset_edge = reset_flag != 0 && prev_reset == 0;
    prev_reset = reset_flag;
    if (n <= (UINT64)g_cfg.log_frames || (n % (UINT64)every) == 0 || reset_edge)
        FeedDiagDump(reset_edge ? "reset_edge" : "periodic", reset_flag, n,
                     mask_ok, bb_w, bb_h, work_w, work_h);
}
