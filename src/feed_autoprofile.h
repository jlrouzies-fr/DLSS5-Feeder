// feed_autoprofile.h -- smart one-shot / Optimize from observed game telemetry.
//
// NOT screenshot CV. After settle (or overlay "Optimize"), classify from:
 //   velocity bind/cands → reset / LightStab / OFA
 //   mask_avg + reset_rate → flicker / ghosting proxies
 //   recent frame interval → work% / OFA perf / evaluate_stride (cost pass)
 // Persists via auto_profile_applied=1 so the next launch skips the probe.
//
 // Include after g_cfg / g_feed_* / CfgSave() / Log() exist.

#pragma once

#ifndef FEED_AUTOPROFILE_SETTLE_FRAMES
#define FEED_AUTOPROFILE_SETTLE_FRAMES 180
#endif
#ifndef FEED_AUTOPROFILE_COST_FRAMES
#define FEED_AUTOPROFILE_COST_FRAMES 90
#endif
#ifndef FEED_AUTOPROFILE_METRIC_FRAMES
#define FEED_AUTOPROFILE_METRIC_FRAMES 60
#endif

enum FeedAutoProfileKind
{
    FeedAuto_None = 0,
    FeedAuto_EstimatedMv = 1, // no usable engine vectors
    FeedAuto_EngineMv = 2,    // engine velocity bound with a live cand score
};

enum FeedAutoPhase
{
    FeedAutoPhase_Idle = 0,
    FeedAutoPhase_Settle = 1,
    FeedAutoPhase_Metrics = 2, // accumulate mask/reset after MV knobs applied
    FeedAutoPhase_Cost = 3,    // FPS / frame-time budget pass
    FeedAutoPhase_Done = 4,
};

struct FeedAutoProfileState
{
    int  applied;          // auto_profile_applied= in cfg (1 = already done)
    int  kind;             // FeedAutoProfileKind last applied (0 if never)
    int  settle_frames;    // frames to wait after attach / re-run (default 180)
    int  force_rerun;      // overlay Optimize / Re-run
    int  phase;            // FeedAutoPhase
    int  phase_frames;     // frames spent in current phase
    int  cost_steps;       // how many work%/OFA cuts applied this run
    float sum_mask;
    float sum_reset_rate;
    float sum_luma;
    int   metric_n;
    float sum_frame_ms;
    int   fps_n;
    LONGLONG last_qpc;
    int   evaluate_stride; // 1 = every frame, 2 = half-rate (persisted)
    char name[48];         // "estimated_mv" / "engine_mv" / "…+cost"
    char status[160];      // overlay line
};

static FeedAutoProfileState g_feed_auto = {
    0, FeedAuto_None, FEED_AUTOPROFILE_SETTLE_FRAMES, 0,
    FeedAutoPhase_Idle, 0, 0,
    0.f, 0.f, 0.f, 0,
    0.f, 0, 0,
    1,
    "", ""
};

static const char *FeedAutoProfileName(int kind)
{
    switch (kind)
    {
    case FeedAuto_EstimatedMv: return "estimated_mv";
    case FeedAuto_EngineMv:    return "engine_mv";
    default:                   return "";
    }
}

static int FeedAutoProfileParseName(const char *s)
{
    if (s == nullptr || s[0] == '\0') return FeedAuto_None;
    // Accept "estimated_mv+cost" etc.
    if (_strnicmp(s, "estimated_mv", 12) == 0) return FeedAuto_EstimatedMv;
    if (_strnicmp(s, "engine_mv", 9) == 0) return FeedAuto_EngineMv;
    return FeedAuto_None;
}

static void FeedAutoProfileMarkCustomized()
{
    g_feed_quality_customized = true;
    g_feed_quality = FeedQuality_Custom;
}

static void FeedAutoProfileArmProbe(const char *why)
{
    g_feed_auto.force_rerun = 1;
    g_feed_auto.applied = 0;
    g_feed_auto.kind = FeedAuto_None;
    g_feed_auto.name[0] = '\0';
    g_feed_auto.phase = FeedAutoPhase_Settle;
    g_feed_auto.phase_frames = 0;
    g_feed_auto.cost_steps = 0;
    g_feed_auto.sum_mask = g_feed_auto.sum_reset_rate = g_feed_auto.sum_luma = 0.f;
    g_feed_auto.metric_n = 0;
    g_feed_auto.sum_frame_ms = 0.f;
    g_feed_auto.fps_n = 0;
    g_feed_auto.last_qpc = 0;
    g_feed_auto.evaluate_stride = 1;
    g_feed_quality_customized = false;
    g_feed_quality = FeedQuality_Auto;
    _snprintf_s(g_feed_auto.status, sizeof(g_feed_auto.status), _TRUNCATE,
                "Optimize: armed (%s) — settle %d frames",
                why ? why : "probe", g_feed_auto.settle_frames);
    Log("[diag] auto_profile: probe armed (%s) settle=%d",
        why ? why : "probe", g_feed_auto.settle_frames);
}

static void FeedAutoProfileRequestRerun()
{
    FeedAutoProfileArmProbe("re-run");
}

static void FeedAutoProfileRequestOptimize()
{
    FeedAutoProfileArmProbe("optimize");
}

// Best cand score among current velocity candidates (0 if none).
static int FeedAutoBestCandScore()
{
    int best = 0;
    for (int i = 0; i < g_feed_velocity.cand_count; ++i)
        if (g_feed_velocity.cands[i].score > best)
            best = g_feed_velocity.cands[i].score;
    return best;
}

// Branch A — MV quality → base knobs. Does not touch work_resolution (cost pass does).
static void FeedAutoProfileApplyKind(FeedAutoProfileKind kind, bool ofa_capable_d3d11)
{
    g_feed_auto.kind = kind;
    _snprintf_s(g_feed_auto.name, sizeof(g_feed_auto.name), _TRUNCATE, "%s", FeedAutoProfileName(kind));

    switch (kind)
    {
    case FeedAuto_EstimatedMv:
        // No trusted engine vectors: kill temporal history cache, damp flicker, keep OFA on.
        g_cfg.reset_mode = FeedReset_Every;
        g_cfg.reset_every = 1;
        g_feed_adapt_cfg.reset_mode = FeedReset_Every;
        g_feed_lightstab_cfg.enabled = 1;
        g_feed_lightstab_cfg.strength = 0.35f;
        g_feed_lightstab_cfg.max_delta = 0.06f;
        g_feed_lightstab_cfg.mv_eps = 0.18f;
        g_feed_velocity_cfg.enabled = 1; // keep hunting
        if (ofa_capable_d3d11)
        {
            g_feed_ofa_cfg.enabled = 1;
            if (g_feed_ofa_cfg.grid < 2) g_feed_ofa_cfg.grid = 2;
            if (g_feed_ofa_cfg.perf != 5 && g_feed_ofa_cfg.perf != 10 && g_feed_ofa_cfg.perf != 20)
                g_feed_ofa_cfg.perf = 10;
            if (g_feed_ofa_cfg.perf == 20) g_feed_ofa_cfg.perf = 10; // MEDIUM when OFA is sole MV
        }
        else
        {
            // DX12-only / no OFA: stay on Every + LightStab; cost pass may lower work%.
            g_feed_ofa_cfg.enabled = 0;
        }
        break;

    case FeedAuto_EngineMv:
        g_cfg.reset_mode = FeedReset_Adaptive;
        g_cfg.reset_every = 0;
        g_feed_adapt_cfg.reset_mode = FeedReset_Adaptive;
        g_feed_lightstab_cfg.enabled = 0;
        g_feed_lightstab_cfg.strength = 0.25f;
        g_feed_lightstab_cfg.max_delta = 0.05f;
        g_feed_lightstab_cfg.mv_eps = 0.15f;
        g_feed_velocity_cfg.enabled = 1;
        if (ofa_capable_d3d11)
        {
            g_feed_ofa_cfg.enabled = 1; // fallback if bind drops
            if (g_feed_ofa_cfg.grid == 0) g_feed_ofa_cfg.grid = 2;
        }
        break;

    default:
        break;
    }

    g_feed_quality_customized = false;
    g_feed_quality = FeedQuality_Auto;
    _snprintf_s(g_feed_auto.status, sizeof(g_feed_auto.status), _TRUNCATE,
                "Optimize: applied %s — measuring residuals…",
                g_feed_auto.name[0] ? g_feed_auto.name : "none");
}

// Branch B — ghosting / flicker proxies from adapt metrics (no CV).
static void FeedAutoProfileApplyResidualHeuristics()
{
    if (g_feed_auto.metric_n <= 0) return;
    const float inv = 1.0f / (float)g_feed_auto.metric_n;
    const float avg_mask = g_feed_auto.sum_mask * inv;
    const float avg_rr = g_feed_auto.sum_reset_rate * inv;
    const float avg_luma = g_feed_auto.sum_luma * inv;

    if (g_feed_adapt_cfg.reset_mode == FeedReset_Adaptive && avg_rr > 0.50f)
    {
        g_feed_adapt_cfg.mask_thr = fminf(g_feed_adapt_cfg.mask_thr + 0.04f, 0.35f);
        g_feed_adapt_cfg.mask_thr_vel = fminf(g_feed_adapt_cfg.mask_thr_vel + 0.04f, 0.45f);
        g_feed_lightstab_cfg.enabled = 1;
        g_feed_lightstab_cfg.strength = fmaxf(g_feed_lightstab_cfg.strength, 0.30f);
        Log("[diag] auto_profile residual: high reset_rate=%.2f → raise mask thr + light_stab", avg_rr);
    }

    // High mask with modest luma swings ≈ lighting flicker (ME static lamps).
    if (avg_mask > 0.12f && avg_luma < 0.04f)
    {
        g_feed_lightstab_cfg.enabled = 1;
        g_feed_lightstab_cfg.strength = fmaxf(g_feed_lightstab_cfg.strength, 0.35f);
        g_feed_lightstab_cfg.max_delta = fmaxf(g_feed_lightstab_cfg.max_delta, 0.06f);
        Log("[diag] auto_profile residual: mask_avg=%.3f lumaΔ=%.3f → light_stab on",
            avg_mask, avg_luma);
    }
}

// Branch C — FPS / cost. Only moves work%, OFA perf, evaluate_stride.
// When OFA is off (typical DX12), prefer half-rate stride before work% — same-device
// DX12 does not currently rebuild at reduced work_resolution.
static bool FeedAutoProfileCostStep()
{
    if (g_feed_auto.fps_n < 30) return false;
    const float avg_ms = g_feed_auto.sum_frame_ms / (float)g_feed_auto.fps_n;
    const float fps = avg_ms > 0.1f ? (1000.0f / avg_ms) : 0.f;
    const float floor_fps = 45.0f;
    const float panic_fps = 30.0f;
    if (fps >= floor_fps)
    {
        Log("[diag] auto_profile cost: fps=%.1f ok (avg frame %.2f ms)", fps, avg_ms);
        return false;
    }

    bool changed = false;
    const bool ofa_on = g_feed_ofa_cfg.enabled != 0;
    if (!ofa_on && g_feed_auto.evaluate_stride < 2)
    {
        g_feed_auto.evaluate_stride = 2;
        changed = true;
        Log("[diag] auto_profile cost: fps=%.1f (no OFA) → evaluate_stride=2", fps);
    }
    else if (g_cfg.work_resolution > 70)
    {
        const int step = (fps < panic_fps) ? 10 : 5;
        g_cfg.work_resolution = (g_cfg.work_resolution - step) < 70 ? 70 : (g_cfg.work_resolution - step);
        changed = true;
        Log("[diag] auto_profile cost: fps=%.1f → work_resolution=%d%%", fps, g_cfg.work_resolution);
    }
    else if (ofa_on && g_feed_ofa_cfg.perf != 20)
    {
        g_feed_ofa_cfg.perf = 20;
        changed = true;
        Log("[diag] auto_profile cost: fps=%.1f → ofa_perf=FAST", fps);
    }
    else if (g_feed_auto.evaluate_stride < 2)
    {
        g_feed_auto.evaluate_stride = 2;
        changed = true;
        Log("[diag] auto_profile cost: fps=%.1f → evaluate_stride=2 (half-rate)", fps);
    }

    if (changed)
    {
        g_feed_auto.cost_steps++;
        g_feed_auto.sum_frame_ms = 0.f;
        g_feed_auto.fps_n = 0;
        g_feed_auto.last_qpc = 0;
    }
    return changed && g_feed_auto.cost_steps < 4 && fps < floor_fps;
}

static void FeedAutoProfileFinish(bool ofa_capable_d3d11)
{
    (void)ofa_capable_d3d11;
    if (g_feed_auto.cost_steps > 0 && g_feed_auto.name[0])
    {
        char base[40];
        _snprintf_s(base, sizeof(base), _TRUNCATE, "%s", g_feed_auto.name);
        _snprintf_s(g_feed_auto.name, sizeof(g_feed_auto.name), _TRUNCATE, "%s+cost", base);
    }
    g_feed_auto.applied = 1;
    g_feed_auto.force_rerun = 0;
    g_feed_auto.phase = FeedAutoPhase_Done;
    g_feed_quality_customized = false;
    g_feed_quality = FeedQuality_Auto;
    _snprintf_s(g_feed_auto.status, sizeof(g_feed_auto.status), _TRUNCATE,
                "Optimize: %s (work %d%%, ofa g%d/p%d, stride %d)",
                g_feed_auto.name[0] ? g_feed_auto.name : "done",
                g_cfg.work_resolution,
                g_feed_ofa_cfg.grid, g_feed_ofa_cfg.perf,
                g_feed_auto.evaluate_stride);
    CfgSave();
    Log("[diag] auto_profile done name=%s reset_mode=%d light_stab=%d ofa=%d grid=%d work=%d stride=%d "
        "mask_thr=%.3f/%.3f",
        g_feed_auto.name, g_cfg.reset_mode, g_feed_lightstab_cfg.enabled,
        g_feed_ofa_cfg.enabled, g_feed_ofa_cfg.grid, g_cfg.work_resolution,
        g_feed_auto.evaluate_stride,
        g_feed_adapt_cfg.mask_thr, g_feed_adapt_cfg.mask_thr_vel);
}

static void FeedAutoProfileSampleFps()
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (g_feed_auto.last_qpc != 0 && freq.QuadPart > 0)
    {
        const double ms = 1000.0 * double(now.QuadPart - g_feed_auto.last_qpc) / double(freq.QuadPart);
        if (ms > 1.0 && ms < 250.0)
        {
            g_feed_auto.sum_frame_ms += (float)ms;
            g_feed_auto.fps_n++;
        }
    }
    g_feed_auto.last_qpc = now.QuadPart;
}

// Call once per delivered frame from the D3D11 feed path (after velocity try-bind).
static void FeedAutoProfileTick(UINT64 frames_done, bool ofa_capable_d3d11)
{
    const int settle = g_feed_auto.settle_frames > 30 ? g_feed_auto.settle_frames : FEED_AUTOPROFILE_SETTLE_FRAMES;

    if (g_feed_auto.applied && !g_feed_auto.force_rerun)
    {
        if (g_feed_auto.status[0] == '\0' && g_feed_auto.name[0])
            _snprintf_s(g_feed_auto.status, sizeof(g_feed_auto.status), _TRUNCATE,
                        "Optimize: %s (cached)", g_feed_auto.name);
        return;
    }

    // Do not fight a user who already customized before settle finished.
    if (!g_feed_auto.force_rerun &&
        (g_feed_quality_customized || g_feed_quality == FeedQuality_Custom))
    {
        g_feed_auto.applied = 1;
        g_feed_auto.phase = FeedAutoPhase_Done;
        _snprintf_s(g_feed_auto.status, sizeof(g_feed_auto.status), _TRUNCATE,
                    "Optimize: skipped (customized)");
        return;
    }

    if (g_feed_auto.phase == FeedAutoPhase_Idle || g_feed_auto.force_rerun)
    {
        if (g_feed_auto.force_rerun || g_feed_auto.phase == FeedAutoPhase_Idle)
        {
            if (g_feed_auto.phase == FeedAutoPhase_Idle && !g_feed_auto.force_rerun)
                g_feed_auto.phase = FeedAutoPhase_Settle;
        }
    }

    // ---- Settle: wait for velocity hunt ----
    // Absolute frame count on first attach; relative phase_frames on Optimize/re-run
    // (frames_done may already be huge mid-session).
    if (g_feed_auto.phase <= FeedAutoPhase_Settle)
    {
        g_feed_auto.phase = FeedAutoPhase_Settle;
        const bool rerun = g_feed_auto.force_rerun != 0 || frames_done >= (UINT64)settle;
        const int need = rerun ? (settle > 90 ? 90 : settle) : settle;
        if (rerun)
            g_feed_auto.phase_frames++;
        const int progress = rerun ? g_feed_auto.phase_frames : (int)frames_done;
        if (progress < need)
        {
            _snprintf_s(g_feed_auto.status, sizeof(g_feed_auto.status), _TRUNCATE,
                        "Optimize: settling… %d / %d", progress, need);
            return;
        }

        const bool bound = g_feed_velocity.bound_ok != 0;
        const int cands = g_feed_velocity.cand_count;
        const int best_score = FeedAutoBestCandScore();
        // Live engine MV: bound + at least one candidate with a meaningful score.
        const bool engine_ok = bound && g_feed_velocity_cfg.enabled && cands > 0 && best_score >= 8;
        const FeedAutoProfileKind kind = engine_ok ? FeedAuto_EngineMv : FeedAuto_EstimatedMv;

        FeedAutoProfileApplyKind(kind, ofa_capable_d3d11);
        Log("[diag] auto_profile MV kind=%s cands=%d bound=%d best_score=%d ofa_cap=%d",
            g_feed_auto.name, cands, bound ? 1 : 0, best_score, ofa_capable_d3d11 ? 1 : 0);

        g_feed_auto.phase = FeedAutoPhase_Metrics;
        g_feed_auto.phase_frames = 0;
        g_feed_auto.sum_mask = g_feed_auto.sum_reset_rate = g_feed_auto.sum_luma = 0.f;
        g_feed_auto.metric_n = 0;
        CfgSave(); // early persist of MV knobs
        return;
    }

    // ---- Metrics window for residual heuristics ----
    if (g_feed_auto.phase == FeedAutoPhase_Metrics)
    {
        if (g_feed_adapt.sampled_ok)
        {
            g_feed_auto.sum_mask += g_feed_adapt.mask_avg;
            g_feed_auto.sum_reset_rate += g_feed_adapt.reset_rate;
            g_feed_auto.sum_luma += g_feed_adapt.luma_delta;
            g_feed_auto.metric_n++;
        }
        g_feed_auto.phase_frames++;
        _snprintf_s(g_feed_auto.status, sizeof(g_feed_auto.status), _TRUNCATE,
                    "Optimize: residuals… %d / %d",
                    g_feed_auto.phase_frames, FEED_AUTOPROFILE_METRIC_FRAMES);
        if (g_feed_auto.phase_frames < FEED_AUTOPROFILE_METRIC_FRAMES)
            return;

        FeedAutoProfileApplyResidualHeuristics();
        g_feed_auto.phase = FeedAutoPhase_Cost;
        g_feed_auto.phase_frames = 0;
        g_feed_auto.sum_frame_ms = 0.f;
        g_feed_auto.fps_n = 0;
        g_feed_auto.last_qpc = 0;
        g_feed_auto.cost_steps = 0;
        CfgSave();
        return;
    }

    // ---- Cost / FPS pass ----
    if (g_feed_auto.phase == FeedAutoPhase_Cost)
    {
        FeedAutoProfileSampleFps();
        g_feed_auto.phase_frames++;
        _snprintf_s(g_feed_auto.status, sizeof(g_feed_auto.status), _TRUNCATE,
                    "Optimize: cost pass… %d / %d (steps %d)",
                    g_feed_auto.phase_frames, FEED_AUTOPROFILE_COST_FRAMES, g_feed_auto.cost_steps);

        if (g_feed_auto.phase_frames >= FEED_AUTOPROFILE_COST_FRAMES)
        {
            if (FeedAutoProfileCostStep())
            {
                // Need another measurement window after a cut.
                g_feed_auto.phase_frames = 0;
                CfgSave();
                return;
            }
            FeedAutoProfileFinish(ofa_capable_d3d11);
        }
        return;
    }
}

// True when this frame should run the full NGX evaluate (half-rate duty support).
static bool FeedAutoProfileShouldEvaluate(UINT64 frames_done)
{
    const int stride = g_feed_auto.evaluate_stride > 0 ? g_feed_auto.evaluate_stride : 1;
    if (stride <= 1) return true;
    // Always evaluate on reset / first frames so history does not go stale forever.
    if (g_feed_adapt.want_reset || g_cfg.reset_mode == FeedReset_Every) return true;
    if (frames_done < 30) return true;
    return (frames_done % (UINT64)stride) == 0;
}
