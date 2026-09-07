// feed_adapt.h -- scene-adaptive DLSS history reset (engine-agnostic).
//
// Goal: keep the "no ghosting" win of reset_every without always nuking temporal
// history (which makes lighting flicker). Samples bias-mask coverage + mean luma
// from the feed's D3D11 work textures and sets InReset when the scene looks unsafe.
//
// Include from dlss5-feed.cpp after Log() and D3D11 headers.

#pragma once

#include <cmath>
#include <cstring>
#include <cstdint>

enum FeedResetMode
{
    FeedReset_Off = 0,       // never (except g.need_reset)
    FeedReset_Every = 1,     // diagnostic: every frame
    FeedReset_Adaptive = 2,  // auto by scene metrics (default)
};

struct FeedAdaptCfg
{
    int   reset_mode;      // FeedResetMode; default Adaptive
    float mask_thr;        // fraction of masked pixels that forces reset
    float luma_thr;        // relative |Δ mean luma| that forces reset
    float mask_thr_vel;    // softer when engine velocity is bound
};

static FeedAdaptCfg g_feed_adapt_cfg = { FeedReset_Adaptive, 0.10f, 0.035f, 0.18f };

struct FeedAdaptState
{
    ID3D11Texture2D *stage_mask;
    ID3D11Texture2D *stage_color;
    UINT stage_w, stage_h;
    DXGI_FORMAT stage_color_fmt;

    float mask_avg;
    float luma_avg;
    float luma_prev;
    float luma_delta;
    bool  have_luma_prev;
    bool  want_reset;
    bool  sampled_ok;

    // Rolling: how often we reset (for overlay + soft auto-tune)
    int   window_n;
    int   window_resets;
    float reset_rate;

    char  status[160];
};

static FeedAdaptState g_feed_adapt = {};

static void FeedAdaptRelease()
{
    if (g_feed_adapt.stage_mask) { g_feed_adapt.stage_mask->Release(); g_feed_adapt.stage_mask = nullptr; }
    if (g_feed_adapt.stage_color) { g_feed_adapt.stage_color->Release(); g_feed_adapt.stage_color = nullptr; }
    g_feed_adapt.stage_w = g_feed_adapt.stage_h = 0;
}

static bool FeedAdaptEnsureStaging(ID3D11Device *dev, UINT w, UINT h, DXGI_FORMAT color_fmt)
{
    if (dev == nullptr || w == 0 || h == 0) return false;
    if (g_feed_adapt.stage_mask && g_feed_adapt.stage_color &&
        g_feed_adapt.stage_w == w && g_feed_adapt.stage_h == h &&
        g_feed_adapt.stage_color_fmt == color_fmt)
        return true;

    FeedAdaptRelease();

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    d.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_feed_adapt.stage_mask)))
        return false;

    d.Format = color_fmt;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_feed_adapt.stage_color)))
    {
        FeedAdaptRelease();
        return false;
    }

    g_feed_adapt.stage_w = w;
    g_feed_adapt.stage_h = h;
    g_feed_adapt.stage_color_fmt = color_fmt;
    return true;
}

static float FeedAdaptLumaFromPixel(const uint8_t *p, DXGI_FORMAT fmt, UINT bpp_guess)
{
    // Best-effort for common feed color formats.
    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT || fmt == DXGI_FORMAT_R16G16B16A16_TYPELESS)
    {
        // Binary16 approximate: reinterpret high bits roughly — staging of FP16 is 8 bytes/pixel.
        // Use simple decode: IEEE754 half via bit cast helpers.
        const uint16_t *h = reinterpret_cast<const uint16_t *>(p);
        auto half = [](uint16_t v) -> float {
            const uint32_t s = (v >> 15) & 1u;
            const uint32_t e = (v >> 10) & 0x1fu;
            const uint32_t m = v & 0x3ffu;
            uint32_t o;
            if (e == 0) o = (s << 31);
            else if (e == 31) o = (s << 31) | 0x7f800000u | (m << 13);
            else o = (s << 31) | ((e + 112u) << 23) | (m << 13);
            float f;
            memcpy(&f, &o, 4);
            return f;
        };
        const float r = half(h[0]), g = half(h[1]), b = half(h[2]);
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }
    if (fmt == DXGI_FORMAT_R11G11B10_FLOAT)
    {
        // Skip precise decode; treat raw as rough energy.
        const uint32_t v = *reinterpret_cast<const uint32_t *>(p);
        return (float)((v >> 2) & 0x1fffu) * (1.0f / 8191.0f);
    }
    // UNORM 8-bit family (R8G8B8A8, B8G8R8A8, …)
    (void)bpp_guess;
    const float r = p[0] * (1.0f / 255.0f);
    const float g = p[1] * (1.0f / 255.0f);
    const float b = p[2] * (1.0f / 255.0f);
    if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        return 0.2126f * b + 0.7152f * g + 0.0722f * r;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// Call after work textures are filled (g.tex11 COLOR/MASK). Cheap sparse CPU sample.
static void FeedAdaptSample(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                            ID3D11Texture2D *color, ID3D11Texture2D *mask,
                            bool mask_ok, UINT w, UINT h, bool velocity_bound)
{
    g_feed_adapt.sampled_ok = false;
    g_feed_adapt.want_reset = false;
    g_feed_adapt.mask_avg = 0.0f;
    g_feed_adapt.luma_delta = 0.0f;

    if (g_feed_adapt_cfg.reset_mode == FeedReset_Off)
    {
        _snprintf_s(g_feed_adapt.status, sizeof(g_feed_adapt.status), _TRUNCATE,
                    "Reset: off");
        return;
    }
    if (g_feed_adapt_cfg.reset_mode == FeedReset_Every)
    {
        g_feed_adapt.want_reset = true;
        _snprintf_s(g_feed_adapt.status, sizeof(g_feed_adapt.status), _TRUNCATE,
                    "Reset: every frame (diagnostic)");
        return;
    }

    // Adaptive
    if (dev == nullptr || ctx == nullptr || color == nullptr || w < 8 || h < 8)
    {
        _snprintf_s(g_feed_adapt.status, sizeof(g_feed_adapt.status), _TRUNCATE,
                    "Reset: adaptive (no sample)");
        return;
    }

    D3D11_TEXTURE2D_DESC cd = {};
    color->GetDesc(&cd);
    if (!FeedAdaptEnsureStaging(dev, w, h, cd.Format))
    {
        _snprintf_s(g_feed_adapt.status, sizeof(g_feed_adapt.status), _TRUNCATE,
                    "Reset: adaptive (staging failed)");
        return;
    }

    ctx->CopyResource(g_feed_adapt.stage_color, color);
    if (mask_ok && mask != nullptr)
        ctx->CopyResource(g_feed_adapt.stage_mask, mask);

    D3D11_MAPPED_SUBRESOURCE mm = {}, mc = {};
    const bool mask_mapped = mask_ok && mask != nullptr &&
        SUCCEEDED(ctx->Map(g_feed_adapt.stage_mask, 0, D3D11_MAP_READ, 0, &mm));
    const bool color_mapped = SUCCEEDED(ctx->Map(g_feed_adapt.stage_color, 0, D3D11_MAP_READ, 0, &mc));
    if (!color_mapped)
    {
        if (mask_mapped) ctx->Unmap(g_feed_adapt.stage_mask, 0);
        _snprintf_s(g_feed_adapt.status, sizeof(g_feed_adapt.status), _TRUNCATE,
                    "Reset: adaptive (map failed)");
        return;
    }

    const UINT stride = 24; // ~ (w/24)*(h/24) samples
    double mask_sum = 0.0, luma_sum = 0.0;
    int n = 0;
    const UINT bpp = (cd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8u :
                     (cd.Format == DXGI_FORMAT_R11G11B10_FLOAT) ? 4u : 4u;

    for (UINT y = 0; y < h; y += stride)
    {
        const uint8_t *rowc = reinterpret_cast<const uint8_t *>(mc.pData) + y * mc.RowPitch;
        const uint8_t *rowm = mask_mapped
            ? reinterpret_cast<const uint8_t *>(mm.pData) + y * mm.RowPitch
            : nullptr;
        for (UINT x = 0; x < w; x += stride)
        {
            luma_sum += FeedAdaptLumaFromPixel(rowc + x * bpp, cd.Format, bpp);
            if (rowm) mask_sum += rowm[x] * (1.0 / 255.0);
            ++n;
        }
    }

    if (mask_mapped) ctx->Unmap(g_feed_adapt.stage_mask, 0);
    ctx->Unmap(g_feed_adapt.stage_color, 0);

    if (n <= 0) return;

    g_feed_adapt.mask_avg = mask_mapped ? (float)(mask_sum / n) : 0.0f;
    g_feed_adapt.luma_avg = (float)(luma_sum / n);
    const bool had_luma = g_feed_adapt.have_luma_prev;
    if (had_luma)
    {
        const float base = fmaxf(g_feed_adapt.luma_prev, 1e-3f);
        g_feed_adapt.luma_delta = fabsf(g_feed_adapt.luma_avg - g_feed_adapt.luma_prev) / base;
    }
    else
        g_feed_adapt.luma_delta = 0.0f;
    g_feed_adapt.luma_prev = g_feed_adapt.luma_avg;
    g_feed_adapt.have_luma_prev = true;
    g_feed_adapt.sampled_ok = true;

    const float mthr = velocity_bound ? g_feed_adapt_cfg.mask_thr_vel : g_feed_adapt_cfg.mask_thr;
    const bool mask_hot = g_feed_adapt.mask_avg >= mthr;
    const bool luma_hot = had_luma && g_feed_adapt.luma_delta >= g_feed_adapt_cfg.luma_thr;
    g_feed_adapt.want_reset = mask_hot || luma_hot;

    // Soft auto-tune: if we reset almost every frame, raise mask thr a bit (prefer lighting
    // stability); if we almost never reset but mask sits mid, lower thr (prefer anti-ghost).
    g_feed_adapt.window_n++;
    if (g_feed_adapt.want_reset) g_feed_adapt.window_resets++;
    if (g_feed_adapt.window_n >= 90)
    {
        g_feed_adapt.reset_rate = (float)g_feed_adapt.window_resets / (float)g_feed_adapt.window_n;
        if (g_feed_adapt.reset_rate > 0.85f)
        {
            g_feed_adapt_cfg.mask_thr = fminf(g_feed_adapt_cfg.mask_thr + 0.01f, 0.35f);
            g_feed_adapt_cfg.mask_thr_vel = fminf(g_feed_adapt_cfg.mask_thr_vel + 0.01f, 0.45f);
        }
        else if (g_feed_adapt.reset_rate < 0.05f && g_feed_adapt.mask_avg > 0.04f)
        {
            g_feed_adapt_cfg.mask_thr = fmaxf(g_feed_adapt_cfg.mask_thr - 0.005f, 0.04f);
            g_feed_adapt_cfg.mask_thr_vel = fmaxf(g_feed_adapt_cfg.mask_thr_vel - 0.005f, 0.08f);
        }
        g_feed_adapt.window_n = 0;
        g_feed_adapt.window_resets = 0;
    }

    _snprintf_s(g_feed_adapt.status, sizeof(g_feed_adapt.status), _TRUNCATE,
                "Reset: %s | mask %.0f%% lumaΔ %.1f%% thr %.0f%% %s",
                g_feed_adapt.want_reset ? "YES" : "calm",
                g_feed_adapt.mask_avg * 100.0f,
                g_feed_adapt.luma_delta * 100.0f,
                mthr * 100.0f,
                velocity_bound ? "vel" : (mask_ok ? "ofa/lum" : "no-mask"));
}

static int FeedAdaptResolveReset(bool need_reset)
{
    if (need_reset) return 1;
    if (g_feed_adapt_cfg.reset_mode == FeedReset_Every) return 1;
    if (g_feed_adapt_cfg.reset_mode == FeedReset_Adaptive && g_feed_adapt.want_reset) return 1;
    return 0;
}

static const char *FeedAdaptResetModeName(int m)
{
    switch (m)
    {
    case FeedReset_Off: return "Off";
    case FeedReset_Every: return "Every frame";
    default: return "Adaptive";
    }
}
