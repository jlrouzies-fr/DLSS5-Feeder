// feed_early_color.h -- D3D11 SceneColor / HDR RT hunter + mid-frame snapshot for SLOT_COLOR.
//
// Include from dlss5-feed.cpp AFTER Log() is defined and <reshade.hpp> is available.
// Off by default (early_color=0). When enabled and a fresh snap exists, FeedFrame11 can
// feed NR from the snap instead of the Present backbuffer; otherwise Present fallback.
// MVP composite: existing BlitOutputToBackbuffer after NR (HUD may be missing — honest).

#pragma once

#include <cstring>
#include <cctype>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

struct FeedEarlyColorCfg
{
    int enabled; // early_color= 0|1
    int cand;    // early_color_cand= -1 auto | N
};

static FeedEarlyColorCfg g_feed_early_color_cfg = { 0, -1 };

enum { kFeedEarlyMaxCands = 32 };

struct FeedEarlyColorCand
{
    uint64_t handle;
    uint32_t width, height;
    reshade::api::format fmt;
    char     name[96];
    bool     name_hit;
    int      score;
    int      draws_frame;
    int      draws_total;
    bool     saw_with_depth;
};

struct FeedEarlyColorState
{
    FeedEarlyColorCand cands[kFeedEarlyMaxCands];
    int cand_count;
    int chosen_cand;

    uint64_t bound_rt[8];
    uint32_t bound_rt_count;
    bool     bound_has_dsv;

    UINT bb_w, bb_h;
    uint64_t frame_id;
    uint64_t snap_frame_id; // frame when snap was last filled
    bool     snap_valid;
    bool     using_early;   // last FeedFrame11 used snap
    char     overlay_line[256];
    char     status_line[96]; // "early" | "fallback Present"

    // Private snap (native cand format, full-res)
    ID3D11Texture2D *snap_tex;
    DXGI_FORMAT snap_fmt;
    UINT snap_w, snap_h;

    // Converted to backbuffer/work format for CopyOrResampleInputs
    ID3D11Texture2D *out_tex;
    DXGI_FORMAT out_fmt;
    UINT out_w, out_h;

    ID3D11VertexShader *blit_vs;
    ID3D11PixelShader  *blit_ps;
    ID3D11SamplerState *blit_smp;
    bool blit_ready;

    // Previous bind set — snapshot when a high-score cand leaves the RT set
    uint64_t prev_bound[8];
    uint32_t prev_bound_count;
};

static FeedEarlyColorState g_feed_early_color = {};
struct FeedEarlyColorInitOnce {
    FeedEarlyColorInitOnce()
    {
        g_feed_early_color.chosen_cand = -1;
        g_feed_early_color_cfg.cand = -1;
        _snprintf_s(g_feed_early_color.status_line, sizeof(g_feed_early_color.status_line),
                    _TRUNCATE, "fallback Present");
    }
};
static FeedEarlyColorInitOnce g_feed_early_color_init_once;

static const GUID kFeedEarlyDebugNameGuid =
    { 0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };

static bool FeedEarlyColorNameHit(const char *name)
{
    if (name == nullptr || name[0] == '\0') return false;
    char lower[256];
    size_t n = 0;
    for (; name[n] != '\0' && n + 1 < sizeof(lower); ++n)
        lower[n] = static_cast<char>(tolower(static_cast<unsigned char>(name[n])));
    lower[n] = '\0';
    if (strstr(lower, "dlss5") != nullptr) return false;
    if (strstr(lower, "lumenite") != nullptr) return false;
    if (strstr(lower, "velocity") != nullptr) return false;
    if (strstr(lower, "motion") != nullptr && strstr(lower, "vector") != nullptr) return false;
    return strstr(lower, "scenecolor") != nullptr ||
           strstr(lower, "scene_color") != nullptr ||
           strstr(lower, "scene colour") != nullptr ||
           strstr(lower, "hdrcolor") != nullptr ||
           strstr(lower, "hdr_color") != nullptr ||
           strstr(lower, "hdr") != nullptr ||
           strstr(lower, "lighting") != nullptr ||
           strstr(lower, "lightaccumulation") != nullptr ||
           strstr(lower, "diffuse") != nullptr ||
           (strstr(lower, "color") != nullptr && strstr(lower, "gbuffer") == nullptr) ||
           (strstr(lower, "colour") != nullptr && strstr(lower, "gbuffer") == nullptr);
}

static bool FeedEarlyColorFmtCandidate(reshade::api::format f)
{
    using reshade::api::format;
    return f == format::r16g16b16a16_float ||
           f == format::r16g16b16a16_typeless ||
           f == format::r11g11b10_float ||
           f == format::r32g32b32a32_float ||
           f == format::r10g10b10a2_unorm ||
           f == format::r8g8b8a8_unorm ||
           f == format::r8g8b8a8_unorm_srgb ||
           f == format::b8g8r8a8_unorm ||
           f == format::b8g8r8a8_unorm_srgb;
}

static const char *FeedEarlyColorFmtLabel(reshade::api::format f)
{
    using reshade::api::format;
    switch (f)
    {
    case format::r16g16b16a16_float:
    case format::r16g16b16a16_typeless: return "RGBA16F";
    case format::r11g11b10_float: return "R11G11B10";
    case format::r32g32b32a32_float: return "RGBA32F";
    case format::r10g10b10a2_unorm: return "RGB10A2";
    case format::r8g8b8a8_unorm:
    case format::r8g8b8a8_unorm_srgb: return "RGBA8";
    case format::b8g8r8a8_unorm:
    case format::b8g8r8a8_unorm_srgb: return "BGRA8";
    default: return "?";
    }
}

static bool FeedEarlyColorFmtIsHdr(reshade::api::format f)
{
    using reshade::api::format;
    return f == format::r16g16b16a16_float || f == format::r16g16b16a16_typeless ||
           f == format::r11g11b10_float || f == format::r32g32b32a32_float;
}

static DXGI_FORMAT FeedEarlyColorDxgiTyped(reshade::api::format f)
{
    using reshade::api::format;
    switch (f)
    {
    case format::r16g16b16a16_float:
    case format::r16g16b16a16_typeless: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case format::r11g11b10_float: return DXGI_FORMAT_R11G11B10_FLOAT;
    case format::r32g32b32a32_float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case format::r10g10b10a2_unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case format::r8g8b8a8_unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case format::r8g8b8a8_unorm_srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case format::b8g8r8a8_unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case format::b8g8r8a8_unorm_srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static void FeedEarlyColorReadDebugName(ID3D11DeviceChild *obj, char *out, size_t out_n)
{
    out[0] = '\0';
    if (obj == nullptr || out_n < 2) return;
    UINT sz = 0;
    if (FAILED(obj->GetPrivateData(kFeedEarlyDebugNameGuid, &sz, nullptr)) || sz == 0 || sz > 512)
        return;
    char buf[512];
    if (sz > sizeof(buf)) sz = sizeof(buf);
    if (FAILED(obj->GetPrivateData(kFeedEarlyDebugNameGuid, &sz, buf))) return;
    if (sz > 0 && buf[sz - 1] == '\0')
        _snprintf_s(out, out_n, _TRUNCATE, "%s", buf);
    else
    {
        if (sz >= out_n) sz = (UINT)(out_n - 1);
        memcpy(out, buf, sz);
        out[sz] = '\0';
    }
}

static void FeedEarlyColorReleaseBlit()
{
    if (g_feed_early_color.blit_vs) { g_feed_early_color.blit_vs->Release(); g_feed_early_color.blit_vs = nullptr; }
    if (g_feed_early_color.blit_ps) { g_feed_early_color.blit_ps->Release(); g_feed_early_color.blit_ps = nullptr; }
    if (g_feed_early_color.blit_smp) { g_feed_early_color.blit_smp->Release(); g_feed_early_color.blit_smp = nullptr; }
    g_feed_early_color.blit_ready = false;
}

static void FeedEarlyColorReleaseSnap()
{
    if (g_feed_early_color.snap_tex) { g_feed_early_color.snap_tex->Release(); g_feed_early_color.snap_tex = nullptr; }
    g_feed_early_color.snap_w = g_feed_early_color.snap_h = 0;
    g_feed_early_color.snap_fmt = DXGI_FORMAT_UNKNOWN;
    g_feed_early_color.snap_valid = false;
}

static void FeedEarlyColorReleaseOut()
{
    if (g_feed_early_color.out_tex) { g_feed_early_color.out_tex->Release(); g_feed_early_color.out_tex = nullptr; }
    g_feed_early_color.out_w = g_feed_early_color.out_h = 0;
    g_feed_early_color.out_fmt = DXGI_FORMAT_UNKNOWN;
}

static bool FeedEarlyColorEnsureBlit(ID3D11Device *dev)
{
    if (g_feed_early_color.blit_ready) return true;
    if (dev == nullptr) return false;

    typedef HRESULT (WINAPI *pD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                          ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (compile == nullptr) { Log("[feed] early_color blit: d3dcompiler_47.dll unavailable"); return false; }

    static const char kSrc[] =
        "Texture2D src_tex : register(t0);\n"
        "SamplerState smp : register(s0);\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(VSOut i) : SV_Target { return src_tex.SampleLevel(smp, i.uv, 0); }\n";

    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "earlyblit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr))
    {
        Log("[feed] early_color blit VS failed 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : "");
        if (err) err->Release();
        return false;
    }
    if (err) { err->Release(); err = nullptr; }
    hr = compile(kSrc, sizeof(kSrc) - 1, "earlyblit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr))
    {
        Log("[feed] early_color blit PS failed 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : "");
        if (err) err->Release();
        if (vs) vs->Release();
        return false;
    }
    if (err) err->Release();

    hr = dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_feed_early_color.blit_vs);
    if (SUCCEEDED(hr))
        hr = dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_feed_early_color.blit_ps);
    vs->Release();
    ps->Release();
    if (FAILED(hr)) { Log("[feed] early_color blit shader create failed 0x%08X", hr); FeedEarlyColorReleaseBlit(); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &g_feed_early_color.blit_smp)))
    { FeedEarlyColorReleaseBlit(); return false; }

    g_feed_early_color.blit_ready = true;
    Log("[feed] early_color blit shaders ready");
    return true;
}

static bool FeedEarlyColorEnsureSnap(ID3D11Device *dev, UINT w, UINT h, DXGI_FORMAT fmt)
{
    if (g_feed_early_color.snap_tex && g_feed_early_color.snap_w == w &&
        g_feed_early_color.snap_h == h && g_feed_early_color.snap_fmt == fmt)
        return true;
    FeedEarlyColorReleaseSnap();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_feed_early_color.snap_tex)) || !g_feed_early_color.snap_tex)
        return false;
    g_feed_early_color.snap_w = w;
    g_feed_early_color.snap_h = h;
    g_feed_early_color.snap_fmt = fmt;
    return true;
}

static bool FeedEarlyColorEnsureOut(ID3D11Device *dev, UINT w, UINT h, DXGI_FORMAT fmt)
{
    if (g_feed_early_color.out_tex && g_feed_early_color.out_w == w &&
        g_feed_early_color.out_h == h && g_feed_early_color.out_fmt == fmt)
        return true;
    FeedEarlyColorReleaseOut();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_feed_early_color.out_tex)) || !g_feed_early_color.out_tex)
        return false;
    g_feed_early_color.out_w = w;
    g_feed_early_color.out_h = h;
    g_feed_early_color.out_fmt = fmt;
    return true;
}

static bool FeedEarlyColorBlitTo(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                 ID3D11Texture2D *src, DXGI_FORMAT srv_fmt,
                                 ID3D11Texture2D *dst, UINT dst_w, UINT dst_h)
{
    if (!FeedEarlyColorEnsureBlit(dev) || src == nullptr || dst == nullptr) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = srv_fmt;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView *srv = nullptr;
    if (FAILED(dev->CreateShaderResourceView(src, &sv, &srv)) || !srv)
        return false;

    ID3D11RenderTargetView *rtv = nullptr;
    if (FAILED(dev->CreateRenderTargetView(dst, nullptr, &rtv)) || !rtv)
    {
        srv->Release();
        return false;
    }

    ID3D11RenderTargetView *prev_rtv = nullptr;
    ID3D11DepthStencilView *prev_dsv = nullptr;
    ctx->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)dst_w;
    vp.Height = (float)dst_h;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_feed_early_color.blit_vs, nullptr, 0);
    ctx->PSSetShader(g_feed_early_color.blit_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &g_feed_early_color.blit_smp);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView *null_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ctx->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
    if (prev_rtv) prev_rtv->Release();
    if (prev_dsv) prev_dsv->Release();
    rtv->Release();
    srv->Release();
    return true;
}

static int FeedEarlyColorFindCand(uint64_t handle)
{
    for (int i = 0; i < g_feed_early_color.cand_count; ++i)
        if (g_feed_early_color.cands[i].handle == handle) return i;
    return -1;
}

static void FeedEarlyColorUpsert(reshade::api::device *device, reshade::api::resource res)
{
    if (device == nullptr || res.handle == 0) return;
    const reshade::api::resource_desc desc = device->get_resource_desc(res);
    if (desc.type != reshade::api::resource_type::texture_2d) return;
    if (desc.texture.depth_or_layers != 1) return;
    if (!FeedEarlyColorFmtCandidate(desc.texture.format)) return;
    if (desc.texture.width < 128 || desc.texture.height < 128) return;
    if (g_feed_early_color.bb_w != 0 && desc.texture.width * 2 < g_feed_early_color.bb_w) return;

    char name[96] = {};
    if (device->get_api() == reshade::api::device_api::d3d11)
        FeedEarlyColorReadDebugName(reinterpret_cast<ID3D11DeviceChild *>(res.handle), name, sizeof(name));

    const bool name_hit = FeedEarlyColorNameHit(name);
    int idx = FeedEarlyColorFindCand(res.handle);
    if (idx < 0)
    {
        if (g_feed_early_color.cand_count >= kFeedEarlyMaxCands)
        {
            int worst = 0;
            for (int i = 1; i < g_feed_early_color.cand_count; ++i)
                if (g_feed_early_color.cands[i].score < g_feed_early_color.cands[worst].score) worst = i;
            if (g_feed_early_color.cands[worst].score > 20) return;
            idx = worst;
        }
        else
            idx = g_feed_early_color.cand_count++;
        FeedEarlyColorCand &c = g_feed_early_color.cands[idx];
        memset(&c, 0, sizeof(c));
        c.handle = res.handle;
        c.width = desc.texture.width;
        c.height = desc.texture.height;
        c.fmt = desc.texture.format;
        if (name[0]) _snprintf_s(c.name, sizeof(c.name), _TRUNCATE, "%s", name);
        else _snprintf_s(c.name, sizeof(c.name), _TRUNCATE, "RT %s %ux%u",
                         FeedEarlyColorFmtLabel(c.fmt), c.width, c.height);
        c.name_hit = name_hit;
    }
    else
    {
        FeedEarlyColorCand &c = g_feed_early_color.cands[idx];
        c.width = desc.texture.width;
        c.height = desc.texture.height;
        c.fmt = desc.texture.format;
        if (name_hit) { c.name_hit = true; if (name[0]) _snprintf_s(c.name, sizeof(c.name), _TRUNCATE, "%s", name); }
    }
}

static void FeedEarlyColorRescore(FeedEarlyColorCand &c)
{
    int s = 0;
    if (c.name_hit) s += 200;
    if (FeedEarlyColorFmtIsHdr(c.fmt)) s += 100;
    else if (c.fmt == reshade::api::format::r10g10b10a2_unorm) s += 40;
    else s += 15;

    if (g_feed_early_color.bb_w != 0 && c.width == g_feed_early_color.bb_w && c.height == g_feed_early_color.bb_h)
        s += 120;
    else if (g_feed_early_color.bb_w != 0 && c.width >= g_feed_early_color.bb_w * 3 / 4)
        s += 40;

    s += c.draws_frame > 40 ? 40 : c.draws_frame;
    if (c.saw_with_depth) s += 25;
    // Prefer SceneColor-like names over bare "Color" UI targets with few draws.
    if (!c.name_hit && c.draws_total < 20) s -= 30;
    if (s < 0) s = 0;
    c.score = s;
}

static void FeedEarlyColorBeginFrame(UINT bb_w, UINT bb_h)
{
    g_feed_early_color.bb_w = bb_w;
    g_feed_early_color.bb_h = bb_h;
    ++g_feed_early_color.frame_id;
    g_feed_early_color.using_early = false;
}

static void FeedEarlyColorAfterAcquire()
{
    for (int i = 0; i < g_feed_early_color.cand_count; ++i)
        g_feed_early_color.cands[i].draws_frame = 0;
}

static int FeedEarlyColorAutoPick(UINT expect_w, UINT expect_h)
{
    int best = -1, best_score = -1;
    for (int i = 0; i < g_feed_early_color.cand_count; ++i)
    {
        FeedEarlyColorCand &c = g_feed_early_color.cands[i];
        FeedEarlyColorRescore(c);
        const bool full = (c.width == expect_w && c.height == expect_h);
        const bool near_full = (c.width * 4 >= expect_w * 3 && c.height * 4 >= expect_h * 3);
        if (!full && !near_full && !c.name_hit) continue;
        if (!FeedEarlyColorFmtIsHdr(c.fmt) && !c.name_hit && c.draws_total < 30) continue;
        if (c.score > best_score) { best_score = c.score; best = i; }
    }
    return best;
}

static void FeedEarlyColorUpdateOverlay()
{
    if (g_feed_early_color.chosen_cand >= 0 &&
        g_feed_early_color.chosen_cand < g_feed_early_color.cand_count)
    {
        const FeedEarlyColorCand &c = g_feed_early_color.cands[g_feed_early_color.chosen_cand];
        _snprintf_s(g_feed_early_color.overlay_line, sizeof(g_feed_early_color.overlay_line), _TRUNCATE,
                    "pick #%d %s %ux%u score=%d %s | %s",
                    g_feed_early_color.chosen_cand, FeedEarlyColorFmtLabel(c.fmt),
                    c.width, c.height, c.score, c.name,
                    g_feed_early_color.snap_valid ? "snap fresh" : "snap stale");
    }
    else
        _snprintf_s(g_feed_early_color.overlay_line, sizeof(g_feed_early_color.overlay_line), _TRUNCATE,
                    "%d color RT candidate(s) — early_color off or no pick",
                    g_feed_early_color.cand_count);
}

static bool FeedEarlyColorSnapshotHandle(ID3D11Device *dev, ID3D11DeviceContext *ctx, uint64_t handle)
{
    if (dev == nullptr || ctx == nullptr || handle == 0) return false;
    auto *raw = reinterpret_cast<ID3D11Resource *>(handle);
    ID3D11Texture2D *src = nullptr;
    if (FAILED(raw->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&src))) || !src)
        return false;
    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);
    if (sd.SampleDesc.Count != 1)
    {
        src->Release();
        return false;
    }
    DXGI_FORMAT typed = sd.Format;
    // Typeless → typed for create/copy; prefer FLOAT HDR layouts.
    if (typed == DXGI_FORMAT_R16G16B16A16_TYPELESS) typed = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!FeedEarlyColorEnsureSnap(dev, sd.Width, sd.Height, typed))
    {
        src->Release();
        return false;
    }
    ctx->CopyResource(g_feed_early_color.snap_tex, src);
    src->Release();
    g_feed_early_color.snap_frame_id = g_feed_early_color.frame_id;
    g_feed_early_color.snap_valid = true;
    return true;
}

static void FeedEarlyColorTrySnapshotLeaving(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    if (g_feed_early_color_cfg.enabled == 0 || dev == nullptr || ctx == nullptr) return;
    int pick = g_feed_early_color_cfg.cand;
    if (pick < 0 || pick >= g_feed_early_color.cand_count)
        pick = FeedEarlyColorAutoPick(g_feed_early_color.bb_w, g_feed_early_color.bb_h);
    if (pick < 0 || pick >= g_feed_early_color.cand_count) return;
    g_feed_early_color.chosen_cand = pick;
    const uint64_t want = g_feed_early_color.cands[pick].handle;

    bool was = false, now = false;
    for (uint32_t i = 0; i < g_feed_early_color.prev_bound_count; ++i)
        if (g_feed_early_color.prev_bound[i] == want) was = true;
    for (uint32_t i = 0; i < g_feed_early_color.bound_rt_count; ++i)
        if (g_feed_early_color.bound_rt[i] == want) now = true;

    // Snapshot when the chosen SceneColor leaves the RT set (end of lighting / before post).
    if (was && !now)
        FeedEarlyColorSnapshotHandle(dev, ctx, want);
}

static void FeedEarlyColorOnBindRTs(reshade::api::command_list *cmd_list, uint32_t count,
                                   const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv)
{
    // Always remember previous binds when enabled so we can snap on unbind.
    memcpy(g_feed_early_color.prev_bound, g_feed_early_color.bound_rt,
           sizeof(uint64_t) * g_feed_early_color.bound_rt_count);
    g_feed_early_color.prev_bound_count = g_feed_early_color.bound_rt_count;

    g_feed_early_color.bound_rt_count = 0;
    g_feed_early_color.bound_has_dsv = dsv.handle != 0;
    if (cmd_list == nullptr || g_feed_early_color_cfg.enabled == 0) return;
    reshade::api::device *dev = cmd_list->get_device();
    if (dev == nullptr || dev->get_api() != reshade::api::device_api::d3d11) return;

    const uint32_t n = count > 8 ? 8 : count;
    for (uint32_t i = 0; i < n; ++i)
    {
        if (rtvs[i].handle == 0) continue;
        const reshade::api::resource res = dev->get_resource_from_view(rtvs[i]);
        if (res.handle == 0) continue;
        g_feed_early_color.bound_rt[g_feed_early_color.bound_rt_count++] = res.handle;
        FeedEarlyColorUpsert(dev, res);
    }

    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native());
    if (ctx != nullptr)
    {
        ID3D11Device *dev11 = nullptr;
        ctx->GetDevice(&dev11);
        if (dev11 != nullptr)
        {
            FeedEarlyColorTrySnapshotLeaving(dev11, ctx);
            dev11->Release();
        }
    }
}

static bool FeedEarlyColorOnDraw(reshade::api::command_list *cmd_list)
{
    if (g_feed_early_color_cfg.enabled == 0 || g_feed_early_color.bound_rt_count == 0) return false;
    if (cmd_list == nullptr) return false;
    reshade::api::device *dev = cmd_list->get_device();
    if (dev == nullptr || dev->get_api() != reshade::api::device_api::d3d11) return false;
    for (uint32_t i = 0; i < g_feed_early_color.bound_rt_count; ++i)
    {
        reshade::api::resource res = { g_feed_early_color.bound_rt[i] };
        FeedEarlyColorUpsert(dev, res);
        const int idx = FeedEarlyColorFindCand(res.handle);
        if (idx < 0) continue;
        FeedEarlyColorCand &c = g_feed_early_color.cands[idx];
        ++c.draws_frame;
        ++c.draws_total;
        if (g_feed_early_color.bound_has_dsv) c.saw_with_depth = true;
        FeedEarlyColorRescore(c);
    }
    return false;
}

static void FeedEarlyColorOnInitResource(reshade::api::device *device,
                                         const reshade::api::resource_desc &desc,
                                         reshade::api::resource resource)
{
    if (device == nullptr || resource.handle == 0) return;
    if (device->get_api() != reshade::api::device_api::d3d11) return;
    if (desc.type != reshade::api::resource_type::texture_2d) return;
    if (!FeedEarlyColorFmtCandidate(desc.texture.format)) return;
    FeedEarlyColorUpsert(device, resource);
    const int idx = FeedEarlyColorFindCand(resource.handle);
    if (idx >= 0) FeedEarlyColorRescore(g_feed_early_color.cands[idx]);
}

static void FeedEarlyColorOnDestroyResource(reshade::api::resource resource)
{
    if (resource.handle == 0) return;
    for (int i = 0; i < g_feed_early_color.cand_count; ++i)
    {
        if (g_feed_early_color.cands[i].handle != resource.handle) continue;
        g_feed_early_color.cands[i] = g_feed_early_color.cands[g_feed_early_color.cand_count - 1];
        --g_feed_early_color.cand_count;
        if (g_feed_early_color.chosen_cand == i) g_feed_early_color.chosen_cand = -1;
        else if (g_feed_early_color.chosen_cand == g_feed_early_color.cand_count)
            g_feed_early_color.chosen_cand = i;
        g_feed_early_color.snap_valid = false;
        return;
    }
}

static void FeedEarlyColorClearCands()
{
    g_feed_early_color.cand_count = 0;
    g_feed_early_color.chosen_cand = -1;
    g_feed_early_color.snap_valid = false;
    g_feed_early_color.overlay_line[0] = '\0';
    Log("[feed] early_color: candidate list cleared (rescan)");
}

static bool FeedEarlyColorSnapFresh()
{
    if (!g_feed_early_color.snap_valid || g_feed_early_color.snap_tex == nullptr)
        return false;
    // Accept snap from this frame or the immediately previous hunt frame.
    const uint64_t age = g_feed_early_color.frame_id - g_feed_early_color.snap_frame_id;
    return age <= 1;
}

// Returns AddRef'd texture in want_fmt / expect size for SLOT_COLOR, or nullptr → Present.
static ID3D11Texture2D *FeedEarlyColorAcquire(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                              UINT expect_w, UINT expect_h, DXGI_FORMAT want_fmt)
{
    g_feed_early_color.using_early = false;
    _snprintf_s(g_feed_early_color.status_line, sizeof(g_feed_early_color.status_line),
                _TRUNCATE, "fallback Present");
    if (g_feed_early_color_cfg.enabled == 0 || dev == nullptr || ctx == nullptr)
    {
        FeedEarlyColorUpdateOverlay();
        return nullptr;
    }

    if (g_feed_early_color_cfg.cand >= 0 && g_feed_early_color_cfg.cand < g_feed_early_color.cand_count)
        g_feed_early_color.chosen_cand = g_feed_early_color_cfg.cand;
    else if (g_feed_early_color.chosen_cand < 0)
        g_feed_early_color.chosen_cand = FeedEarlyColorAutoPick(expect_w, expect_h);

    // Last-chance snap if still bound at Present (some engines keep SceneColor live).
    if (!FeedEarlyColorSnapFresh() && g_feed_early_color.chosen_cand >= 0 &&
        g_feed_early_color.chosen_cand < g_feed_early_color.cand_count)
    {
        FeedEarlyColorSnapshotHandle(dev, ctx, g_feed_early_color.cands[g_feed_early_color.chosen_cand].handle);
    }

    FeedEarlyColorUpdateOverlay();
    if (!FeedEarlyColorSnapFresh()) return nullptr;

    if (!FeedEarlyColorEnsureOut(dev, expect_w, expect_h, want_fmt))
        return nullptr;

    const bool same =
        g_feed_early_color.snap_w == expect_w && g_feed_early_color.snap_h == expect_h &&
        g_feed_early_color.snap_fmt == want_fmt;
    if (same)
        ctx->CopyResource(g_feed_early_color.out_tex, g_feed_early_color.snap_tex);
    else if (!FeedEarlyColorBlitTo(dev, ctx, g_feed_early_color.snap_tex, g_feed_early_color.snap_fmt,
                                   g_feed_early_color.out_tex, expect_w, expect_h))
        return nullptr;

    g_feed_early_color.using_early = true;
    _snprintf_s(g_feed_early_color.status_line, sizeof(g_feed_early_color.status_line),
                _TRUNCATE, "early");
    static bool said = false;
    if (!said)
    {
        said = true;
        Log("[feed] early_color: feeding SLOT_COLOR from SceneColor snap (%ux%u %s → %s) — "
            "NR output still blits over backbuffer (HUD may be missing)",
            g_feed_early_color.snap_w, g_feed_early_color.snap_h,
            FeedEarlyColorFmtLabel(g_feed_early_color.cands[
                g_feed_early_color.chosen_cand >= 0 ? g_feed_early_color.chosen_cand : 0].fmt),
            "BB");
    }
    g_feed_early_color.out_tex->AddRef();
    return g_feed_early_color.out_tex;
}
