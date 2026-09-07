// feed_quality.h -- quality presets (Auto/Low/Medium/High) mirrored from oneclick.
// Include after g_cfg / g_feed_ofa_cfg / g_feed_velocity_cfg and CfgSave() exist.

#pragma once

enum FeedQualityChoice
{
    FeedQuality_Auto = 0,
    FeedQuality_Low = 1,
    FeedQuality_Medium = 2,
    FeedQuality_High = 3,
    FeedQuality_Custom = 4,
};

static FeedQualityChoice g_feed_quality = FeedQuality_Auto;
static bool g_feed_quality_customized = false;

static const char *FeedQualityName(FeedQualityChoice q)
{
    switch (q)
    {
    case FeedQuality_Auto:   return "Auto";
    case FeedQuality_Low:    return "Low";
    case FeedQuality_Medium: return "Medium";
    case FeedQuality_High:   return "High";
    default:                 return "Custom";
    }
}

static const char *FeedQualityCfgName(FeedQualityChoice q)
{
    switch (q)
    {
    case FeedQuality_Auto:   return "auto";
    case FeedQuality_Low:    return "low";
    case FeedQuality_Medium: return "medium";
    case FeedQuality_High:   return "high";
    default:                 return "custom";
    }
}

// Apply preset to live cfg (OFA + work resolution). FX uniforms are set at Install.
static void FeedQualityApply(FeedQualityChoice q, bool ofa_capable_d3d11)
{
    g_feed_quality_customized = false;
    g_feed_quality = q;
    switch (q)
    {
    case FeedQuality_Low:
        g_feed_ofa_cfg.enabled = 0;
        g_feed_ofa_cfg.grid = 2;
        g_feed_ofa_cfg.perf = 10;
        g_cfg.work_resolution = 70;
        g_cfg.work_upscale = 1;
        g_cfg.work_sharpness = 0.35f;
        g_feed_velocity_cfg.enabled = 0;
        g_cfg.reset_mode = FeedReset_Adaptive;
        g_cfg.reset_every = 0;
        g_feed_adapt_cfg.reset_mode = FeedReset_Adaptive;
        g_feed_lightstab_cfg.enabled = 1;
        g_feed_lightstab_cfg.strength = 0.35f;
        g_feed_lightstab_cfg.max_delta = 0.06f;
        break;
    case FeedQuality_Medium:
        g_feed_ofa_cfg.enabled = ofa_capable_d3d11 ? 1 : 0;
        g_feed_ofa_cfg.grid = 2;
        g_feed_ofa_cfg.perf = 10;
        g_cfg.work_resolution = 85;
        g_cfg.work_upscale = 1;
        g_cfg.work_sharpness = 0.30f;
        g_feed_velocity_cfg.enabled = 1; // hunt on; bind when found
        g_cfg.reset_mode = FeedReset_Adaptive;
        g_cfg.reset_every = 0;
        g_feed_adapt_cfg.reset_mode = FeedReset_Adaptive;
        g_feed_lightstab_cfg.enabled = 0;
        break;
    case FeedQuality_High:
        g_feed_ofa_cfg.enabled = ofa_capable_d3d11 ? 1 : 0;
        g_feed_ofa_cfg.grid = 1;
        g_feed_ofa_cfg.perf = 10;
        g_cfg.work_resolution = 100;
        g_cfg.work_upscale = 0;
        g_cfg.work_sharpness = 0.30f;
        g_feed_velocity_cfg.enabled = 1;
        g_cfg.reset_mode = FeedReset_Adaptive;
        g_cfg.reset_every = 0;
        g_feed_adapt_cfg.reset_mode = FeedReset_Adaptive;
        g_feed_lightstab_cfg.enabled = 0;
        break;
    case FeedQuality_Auto:
    default:
        if (ofa_capable_d3d11)
        {
            g_feed_ofa_cfg.enabled = 1;
            g_feed_ofa_cfg.grid = 2;
            g_feed_ofa_cfg.perf = 10;
            g_cfg.work_resolution = 100;
            g_cfg.work_upscale = 0;
            g_cfg.work_sharpness = 0.30f;
        }
        else
        {
            g_feed_ofa_cfg.enabled = 0;
            g_feed_ofa_cfg.grid = 2;
            g_feed_ofa_cfg.perf = 10;
            g_cfg.work_resolution = 85;
            g_cfg.work_upscale = 1;
            g_cfg.work_sharpness = 0.30f;
        }
        g_feed_velocity_cfg.enabled = 1;
        g_cfg.reset_mode = FeedReset_Adaptive;
        g_cfg.reset_every = 0;
        g_feed_adapt_cfg.reset_mode = FeedReset_Adaptive;
        g_feed_lightstab_cfg.enabled = 0;
        g_feed_quality = FeedQuality_Auto;
        break;
    }
    g_work_resolution_ui = g_cfg.work_resolution;
}
