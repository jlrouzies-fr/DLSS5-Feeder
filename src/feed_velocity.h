// feed_velocity.h -- engine velocity RT hunter (PPOM-style) + bind into DLSS MV.
//
// Include from dlss5-feed.cpp AFTER Log() is defined and <reshade.hpp> is available.
// Tracks D3D11 float RTs during bind_render_targets + draw, scores candidates,
// copies the chosen one into a private RG16F for NGX (velocity > OFA > Lumenite).

#pragma once

#include <cstring>
#include <cctype>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

// Log() must already be defined by the including translation unit (dlss5-feed.cpp).

enum FeedVelocityDecode
{
    FeedVelDecode_Auto = 0,      // heuristic: RawPixels if magnitudes look like px, else DeltaUV
    FeedVelDecode_RawPixels = 1, // already pixel deltas (DLSS contract)
    FeedVelDecode_DeltaUV = 2,   // UV deltas → multiply by resolution
    FeedVelDecode_UEPacked = 3,  // UE-ish packed → scale * resolution
};

struct FeedVelocityCfg
{
    int   enabled;       // engine_velocity=
    int   cand;          // -1 = auto best score, else index
    int   decode;        // FeedVelocityDecode
    float scale;         // extra multiplier after decode
};

static FeedVelocityCfg g_feed_velocity_cfg = { 1, -1, FeedVelDecode_Auto, 1.0f };

enum { kFeedVelMaxCands = 32 };

struct FeedVelocityCand
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

struct FeedVelocityState
{
    reshade::api::effect_texture_variable var;
    char name[128];
    bool detected;
    bool bound_ok;
    int  last_w, last_h;
    char bound_src[32];
    char overlay_line[256];

    FeedVelocityCand cands[kFeedVelMaxCands];
    int cand_count;
    int chosen_cand = -1; // resolved index this frame, or -1

    // Currently bound RTs (updated on bind_render_targets)
    uint64_t bound_rt[8];
    uint32_t bound_rt_count;
    bool     bound_has_dsv;

    UINT bb_w, bb_h;
    uint64_t frame_id;

    // Private full-res RG16F we hand to DLSS (owned)
    ID3D11Texture2D *out_tex;
    ID3D11RenderTargetView *out_rtv;
    UINT out_w, out_h;

    // Lazy fullscreen blit (half-res upsample / RGBA→RG)
    ID3D11VertexShader *blit_vs;
    ID3D11PixelShader  *blit_ps;
    ID3D11SamplerState *blit_smp;
    ID3D11Buffer       *blit_cb;
    bool blit_ready;

    // Applied into evaluate mv_scale for this frame (decode)
    float apply_scale_x, apply_scale_y;
    bool  scale_override;
};

static FeedVelocityState g_feed_velocity = {};
// Ensure chosen_cand starts as -1 (aggregate init zeroes it otherwise).
struct FeedVelocityInitOnce { FeedVelocityInitOnce() { g_feed_velocity.chosen_cand = -1; g_feed_velocity_cfg.cand = -1; } };
static FeedVelocityInitOnce g_feed_velocity_init_once;

static const GUID kFeedVelDebugNameGuid =
    { 0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };

static bool FeedVelocityNameHit(const char *name)
{
    if (name == nullptr || name[0] == '\0') return false;
    char lower[256];
    size_t n = 0;
    for (; name[n] != '\0' && n + 1 < sizeof(lower); ++n)
        lower[n] = static_cast<char>(tolower(static_cast<unsigned char>(name[n])));
    lower[n] = '\0';
    if (strstr(lower, "dlss5") != nullptr) return false;
    if (strstr(lower, "lumenite") != nullptr) return false;
    // Broad engine-agnostic name hits (UE / Unity / custom / console ports).
    return strstr(lower, "velocity") != nullptr ||
           strstr(lower, "motionblur") != nullptr ||
           strstr(lower, "motion_blur") != nullptr ||
           strstr(lower, "motion_vectors") != nullptr ||
           strstr(lower, "motionvectors") != nullptr ||
           strstr(lower, "motionvector") != nullptr ||
           strstr(lower, "velbuffer") != nullptr ||
           strstr(lower, "gvelocity") != nullptr ||
           strstr(lower, "scenevelocity") != nullptr ||
           strstr(lower, "worldvelocity") != nullptr ||
           strstr(lower, "screenvelocity") != nullptr ||
           strstr(lower, "objectmotion") != nullptr ||
           strstr(lower, "pixelvelocity") != nullptr ||
           strstr(lower, "cameramotion") != nullptr ||
           strstr(lower, "gbuffer_velocity") != nullptr ||
           strstr(lower, "gbuffervelocity") != nullptr ||
           strstr(lower, "mvbuffer") != nullptr ||
           strstr(lower, "vec_velocity") != nullptr ||
           strstr(lower, "rt_velocity") != nullptr ||
           strstr(lower, "velocityrt") != nullptr ||
           (strstr(lower, "vel") != nullptr && strstr(lower, "buffer") != nullptr) ||
           (strstr(lower, "motion") != nullptr && strstr(lower, "vector") != nullptr);
}

static bool FeedVelocityFmtCandidate(reshade::api::format f)
{
    using reshade::api::format;
    // Prefer true velocity layouts (RG*); RGBA16F is common for packed GBuffers but often wrong.
    return f == format::r16g16_float ||
           f == format::r16g16_typeless ||
           f == format::r16g16b16a16_float ||
           f == format::r16g16b16a16_typeless ||
           f == format::r32g32_float ||
           f == format::r32g32_typeless;
}

static DXGI_FORMAT FeedVelocityDxgiTyped(reshade::api::format f)
{
    using reshade::api::format;
    switch (f)
    {
    case format::r16g16_float:
    case format::r16g16_typeless: return DXGI_FORMAT_R16G16_FLOAT;
    case format::r32g32_float:
    case format::r32g32_typeless: return DXGI_FORMAT_R32G32_FLOAT;
    case format::r16g16b16a16_float:
    case format::r16g16b16a16_typeless: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static const char *FeedVelocityFmtLabel(reshade::api::format f)
{
    using reshade::api::format;
    switch (f)
    {
    case format::r16g16_float:
    case format::r16g16_typeless: return "RG16F";
    case format::r32g32_float:
    case format::r32g32_typeless: return "RG32F";
    case format::r16g16b16a16_float:
    case format::r16g16b16a16_typeless: return "RGBA16F";
    default: return "?";
    }
}

static bool FeedVelocityFmtIsRg(reshade::api::format f)
{
    using reshade::api::format;
    return f == format::r16g16_float || f == format::r16g16_typeless ||
           f == format::r32g32_float || f == format::r32g32_typeless;
}

static bool FeedVelocityFmtIsRgba16(reshade::api::format f)
{
    using reshade::api::format;
    return f == format::r16g16b16a16_float || f == format::r16g16b16a16_typeless;
}

static void FeedVelocityReadDebugName(ID3D11DeviceChild *obj, char *out, size_t out_n)
{
    out[0] = '\0';
    if (obj == nullptr || out_n < 2) return;
    UINT sz = 0;
    if (FAILED(obj->GetPrivateData(kFeedVelDebugNameGuid, &sz, nullptr)) || sz == 0 || sz > 512)
        return;
    char buf[512];
    if (sz > sizeof(buf)) sz = sizeof(buf);
    if (FAILED(obj->GetPrivateData(kFeedVelDebugNameGuid, &sz, buf))) return;
    if (sz > 0 && buf[sz - 1] == '\0')
        _snprintf_s(out, out_n, _TRUNCATE, "%s", buf);
    else
    {
        if (sz >= out_n) sz = (UINT)(out_n - 1);
        memcpy(out, buf, sz);
        out[sz] = '\0';
    }
}

static void FeedVelocityReleaseBlit()
{
    if (g_feed_velocity.blit_vs) { g_feed_velocity.blit_vs->Release(); g_feed_velocity.blit_vs = nullptr; }
    if (g_feed_velocity.blit_ps) { g_feed_velocity.blit_ps->Release(); g_feed_velocity.blit_ps = nullptr; }
    if (g_feed_velocity.blit_smp) { g_feed_velocity.blit_smp->Release(); g_feed_velocity.blit_smp = nullptr; }
    if (g_feed_velocity.blit_cb) { g_feed_velocity.blit_cb->Release(); g_feed_velocity.blit_cb = nullptr; }
    g_feed_velocity.blit_ready = false;
}

static void FeedVelocityReleaseOut()
{
    if (g_feed_velocity.out_rtv) { g_feed_velocity.out_rtv->Release(); g_feed_velocity.out_rtv = nullptr; }
    if (g_feed_velocity.out_tex) { g_feed_velocity.out_tex->Release(); g_feed_velocity.out_tex = nullptr; }
    g_feed_velocity.out_w = g_feed_velocity.out_h = 0;
}

static bool FeedVelocityEnsureBlit(ID3D11Device *dev)
{
    if (g_feed_velocity.blit_ready) return true;
    if (dev == nullptr) return false;

    typedef HRESULT (WINAPI *pD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                          ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (compile == nullptr) { Log("[feed] velocity blit: d3dcompiler_47.dll unavailable"); return false; }

    // Sample .xy from any float texture; uv_scale packs into CB (unused for sample; kept for future).
    static const char kSrc[] =
        "Texture2D src_tex : register(t0);\n"
        "SamplerState smp : register(s0);\n"
        "cbuffer CB : register(b0) { float2 uv_scale; float2 pad; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float2 ps(VSOut i) : SV_Target { return src_tex.SampleLevel(smp, i.uv, 0).xy; }\n";

    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "velblit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr))
    {
        Log("[feed] velocity blit VS failed 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : "");
        if (err) err->Release();
        return false;
    }
    if (err) { err->Release(); err = nullptr; }
    hr = compile(kSrc, sizeof(kSrc) - 1, "velblit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr))
    {
        Log("[feed] velocity blit PS failed 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : "");
        if (err) err->Release();
        if (vs) vs->Release();
        return false;
    }
    if (err) err->Release();

    hr = dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_feed_velocity.blit_vs);
    if (SUCCEEDED(hr))
        hr = dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_feed_velocity.blit_ps);
    vs->Release();
    ps->Release();
    if (FAILED(hr)) { Log("[feed] velocity blit shader create failed 0x%08X", hr); FeedVelocityReleaseBlit(); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &g_feed_velocity.blit_smp)))
    { FeedVelocityReleaseBlit(); return false; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, &g_feed_velocity.blit_cb)))
    { FeedVelocityReleaseBlit(); return false; }

    g_feed_velocity.blit_ready = true;
    Log("[feed] velocity blit shaders ready (upsample / RGBA→RG)");
    return true;
}

static bool FeedVelocityEnsureOut(ID3D11Device *dev, UINT w, UINT h)
{
    if (g_feed_velocity.out_tex && g_feed_velocity.out_w == w && g_feed_velocity.out_h == h &&
        g_feed_velocity.out_rtv)
        return true;
    FeedVelocityReleaseOut();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_feed_velocity.out_tex)) || !g_feed_velocity.out_tex)
        return false;
    if (FAILED(dev->CreateRenderTargetView(g_feed_velocity.out_tex, nullptr, &g_feed_velocity.out_rtv)) ||
        !g_feed_velocity.out_rtv)
    {
        FeedVelocityReleaseOut();
        return false;
    }
    g_feed_velocity.out_w = w;
    g_feed_velocity.out_h = h;
    return true;
}

// Blit src → private RG16F out (handles size mismatch + RGBA/RG32 → .xy).
static bool FeedVelocityBlitSrc(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src,
                                DXGI_FORMAT srv_fmt)
{
    if (!FeedVelocityEnsureBlit(dev) || g_feed_velocity.out_rtv == nullptr || src == nullptr)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = srv_fmt;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView *srv = nullptr;
    if (FAILED(dev->CreateShaderResourceView(src, &sv, &srv)) || !srv)
        return false;

    // Minimal state: save nothing — FeedFrame11 already owns the immediate context between
    // ReShade draws; restore RT/VS/PS/topology after.
    ID3D11RenderTargetView *prev_rtv = nullptr;
    ID3D11DepthStencilView *prev_dsv = nullptr;
    ctx->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)g_feed_velocity.out_w;
    vp.Height = (float)g_feed_velocity.out_h;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &g_feed_velocity.out_rtv, nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_feed_velocity.blit_vs, nullptr, 0);
    ctx->PSSetShader(g_feed_velocity.blit_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &g_feed_velocity.blit_smp);
    if (g_feed_velocity.blit_cb)
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(ctx->Map(g_feed_velocity.blit_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            float *f = (float *)mapped.pData;
            f[0] = 1.f; f[1] = 1.f; f[2] = 0.f; f[3] = 0.f;
            ctx->Unmap(g_feed_velocity.blit_cb, 0);
        }
        ctx->PSSetConstantBuffers(0, 1, &g_feed_velocity.blit_cb);
    }
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView *null_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ctx->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
    if (prev_rtv) prev_rtv->Release();
    if (prev_dsv) prev_dsv->Release();
    srv->Release();
    return true;
}

static int FeedVelocityFindCand(uint64_t handle)
{
    for (int i = 0; i < g_feed_velocity.cand_count; ++i)
        if (g_feed_velocity.cands[i].handle == handle) return i;
    return -1;
}

static void FeedVelocityUpsert(reshade::api::device *device, reshade::api::resource res)
{
    if (device == nullptr || res.handle == 0) return;
    const reshade::api::resource_desc desc = device->get_resource_desc(res);
    if (desc.type != reshade::api::resource_type::texture_2d) return;
    if (desc.texture.depth_or_layers != 1) return;
    if (!FeedVelocityFmtCandidate(desc.texture.format)) return;
    if (desc.texture.width < 64 || desc.texture.height < 64) return;
    if (g_feed_velocity.bb_w != 0 && desc.texture.width * 2 < g_feed_velocity.bb_w) return; // < ~50%

    char name[96] = {};
    if (device->get_api() == reshade::api::device_api::d3d11)
        FeedVelocityReadDebugName(reinterpret_cast<ID3D11DeviceChild *>(res.handle), name, sizeof(name));
    if (FeedVelocityNameHit(name) == false && name[0] &&
        (strstr(name, "DLSS5") != nullptr || strstr(name, "dlss5") != nullptr))
        return;

    const bool name_hit = FeedVelocityNameHit(name);
    int idx = FeedVelocityFindCand(res.handle);
    if (idx < 0)
    {
        if (g_feed_velocity.cand_count >= kFeedVelMaxCands)
        {
            // Drop lowest-score slot
            int worst = 0;
            for (int i = 1; i < g_feed_velocity.cand_count; ++i)
                if (g_feed_velocity.cands[i].score < g_feed_velocity.cands[worst].score) worst = i;
            if (g_feed_velocity.cands[worst].score > 5) return;
            idx = worst;
        }
        else
            idx = g_feed_velocity.cand_count++;
        FeedVelocityCand &c = g_feed_velocity.cands[idx];
        memset(&c, 0, sizeof(c));
        c.handle = res.handle;
        c.width = desc.texture.width;
        c.height = desc.texture.height;
        c.fmt = desc.texture.format;
        if (name[0]) _snprintf_s(c.name, sizeof(c.name), _TRUNCATE, "%s", name);
        else _snprintf_s(c.name, sizeof(c.name), _TRUNCATE, "RT %s %ux%u",
                         FeedVelocityFmtLabel(c.fmt), c.width, c.height);
        c.name_hit = name_hit;
    }
    else
    {
        FeedVelocityCand &c = g_feed_velocity.cands[idx];
        c.width = desc.texture.width;
        c.height = desc.texture.height;
        c.fmt = desc.texture.format;
        if (name_hit) { c.name_hit = true; if (name[0]) _snprintf_s(c.name, sizeof(c.name), _TRUNCATE, "%s", name); }
    }
}

static void FeedVelocityRescore(FeedVelocityCand &c)
{
    int s = 0;
    if (c.name_hit) s += 220;
    // True velocity buffers are almost always RG16F/RG32F. Unnamed RGBA16F full-res
    // is frequently a lighting/GBuffer RT that ME wrongly latched — score it down hard.
    if (FeedVelocityFmtIsRg(c.fmt))
        s += (c.fmt == reshade::api::format::r16g16_float ||
              c.fmt == reshade::api::format::r16g16_typeless) ? 120 : 90;
    else if (FeedVelocityFmtIsRgba16(c.fmt))
        s += c.name_hit ? 35 : 5;
    else
        s += 0;

    if (g_feed_velocity.bb_w != 0 && c.width == g_feed_velocity.bb_w && c.height == g_feed_velocity.bb_h)
        s += FeedVelocityFmtIsRg(c.fmt) ? 110 : (c.name_hit ? 60 : 15);
    else if (g_feed_velocity.bb_w != 0 && c.width * 2 == g_feed_velocity.bb_w && c.height * 2 == g_feed_velocity.bb_h)
        s += 45; // half-res common for UE velocity
    else if (g_feed_velocity.bb_w != 0 && c.width >= g_feed_velocity.bb_w / 2)
        s += 20;

    s += c.draws_frame > 50 ? 50 : c.draws_frame;
    if (c.saw_with_depth) s += 35;
    // Penalize unnamed RGBA full-res with little draw evidence (wrong ME pick pattern).
    if (FeedVelocityFmtIsRgba16(c.fmt) && !c.name_hit && c.draws_total < 30)
        s -= 40;
    if (s < 0) s = 0;
    c.score = s;
}

static void FeedVelocityBeginFrame(UINT bb_w, UINT bb_h)
{
    g_feed_velocity.bb_w = bb_w;
    g_feed_velocity.bb_h = bb_h;
    ++g_feed_velocity.frame_id;
    // draws_frame is cleared AFTER acquire — FeedFrame11 runs after the game's draws.
    g_feed_velocity.bound_ok = false;
    g_feed_velocity.scale_override = false;
    g_feed_velocity.apply_scale_x = g_feed_velocity.apply_scale_y = 1.0f;
}

static void FeedVelocityAfterAcquire()
{
    for (int i = 0; i < g_feed_velocity.cand_count; ++i)
        g_feed_velocity.cands[i].draws_frame = 0;
}

// --- ReShade event hooks ---

static void FeedVelocityOnBindRTs(reshade::api::command_list *cmd_list, uint32_t count,
                                  const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv)
{
    g_feed_velocity.bound_rt_count = 0;
    g_feed_velocity.bound_has_dsv = dsv.handle != 0;
    if (cmd_list == nullptr || g_feed_velocity_cfg.enabled == 0) return;
    reshade::api::device *dev = cmd_list->get_device();
    if (dev == nullptr || dev->get_api() != reshade::api::device_api::d3d11) return;
    const uint32_t n = count > 8 ? 8 : count;
    for (uint32_t i = 0; i < n; ++i)
    {
        if (rtvs[i].handle == 0) continue;
        const reshade::api::resource res = dev->get_resource_from_view(rtvs[i]);
        if (res.handle == 0) continue;
        g_feed_velocity.bound_rt[g_feed_velocity.bound_rt_count++] = res.handle;
        FeedVelocityUpsert(dev, res);
    }
}

static bool FeedVelocityOnDraw(reshade::api::command_list *cmd_list)
{
    if (g_feed_velocity_cfg.enabled == 0 || g_feed_velocity.bound_rt_count == 0) return false;
    if (cmd_list == nullptr) return false;
    reshade::api::device *dev = cmd_list->get_device();
    if (dev == nullptr || dev->get_api() != reshade::api::device_api::d3d11) return false;
    for (uint32_t i = 0; i < g_feed_velocity.bound_rt_count; ++i)
    {
        reshade::api::resource res = { g_feed_velocity.bound_rt[i] };
        FeedVelocityUpsert(dev, res);
        const int idx = FeedVelocityFindCand(res.handle);
        if (idx < 0) continue;
        FeedVelocityCand &c = g_feed_velocity.cands[idx];
        ++c.draws_frame;
        ++c.draws_total;
        if (g_feed_velocity.bound_has_dsv) c.saw_with_depth = true;
        FeedVelocityRescore(c);
    }
    return false; // never skip draws
}

static void FeedVelocityOnInitResource(reshade::api::device *device,
                                       const reshade::api::resource_desc &desc,
                                       reshade::api::resource resource)
{
    if (device == nullptr || resource.handle == 0) return;
    if (device->get_api() != reshade::api::device_api::d3d11) return;
    if (desc.type != reshade::api::resource_type::texture_2d) return;
    if (!FeedVelocityFmtCandidate(desc.texture.format)) return;
    FeedVelocityUpsert(device, resource);
    const int idx = FeedVelocityFindCand(resource.handle);
    if (idx >= 0) FeedVelocityRescore(g_feed_velocity.cands[idx]);
}

static void FeedVelocityOnDestroyResource(reshade::api::resource resource)
{
    if (resource.handle == 0) return;
    for (int i = 0; i < g_feed_velocity.cand_count; ++i)
    {
        if (g_feed_velocity.cands[i].handle != resource.handle) continue;
        g_feed_velocity.cands[i] = g_feed_velocity.cands[g_feed_velocity.cand_count - 1];
        --g_feed_velocity.cand_count;
        if (g_feed_velocity.chosen_cand == i) g_feed_velocity.chosen_cand = -1;
        else if (g_feed_velocity.chosen_cand == g_feed_velocity.cand_count)
            g_feed_velocity.chosen_cand = i;
        return;
    }
}

static void FeedVelocityClearCands()
{
    g_feed_velocity.cand_count = 0;
    g_feed_velocity.chosen_cand = -1;
    g_feed_velocity.detected = false;
    g_feed_velocity.var = {};
    g_feed_velocity.overlay_line[0] = '\0';
    Log("[feed] velocity: candidate list cleared (rescan)");
}

static int FeedVelocityAutoPick(UINT expect_w, UINT expect_h)
{
    int best = -1, best_score = -1;
    for (int i = 0; i < g_feed_velocity.cand_count; ++i)
    {
        FeedVelocityCand &c = g_feed_velocity.cands[i];
        FeedVelocityRescore(c);
        // Prefer RG full-res with draws; allow half-res / RGBA only with name hit or strong evidence.
        const bool full = (c.width == expect_w && c.height == expect_h);
        const bool half = (c.width * 2 == expect_w && c.height * 2 == expect_h);
        const bool rg = FeedVelocityFmtIsRg(c.fmt);
        if (!full && !half && !c.name_hit) continue;
        if (!rg && !c.name_hit)
        {
            // Unnamed RGBA: require depth co-bind + enough draws, else skip (wrong ME latch).
            if (!c.saw_with_depth || c.draws_total < 40) continue;
        }
        if (c.score > best_score) { best_score = c.score; best = i; }
    }
    return best;
}

// Periodic effect-name scan (still useful when games expose MV via ReShade).
static void FeedVelocityScanEffects(reshade::api::effect_runtime *rt)
{
    if (rt == nullptr) return;
    rt->enumerate_texture_variables(nullptr,
        [](reshade::api::effect_runtime *runtime, reshade::api::effect_texture_variable var, void *)
        {
            char name[256] = {};
            runtime->get_texture_variable_name(var, name);
            if (!FeedVelocityNameHit(name)) return;
            g_feed_velocity.var = var;
            _snprintf_s(g_feed_velocity.name, sizeof(g_feed_velocity.name), _TRUNCATE, "%s", name);
            g_feed_velocity.detected = true;
            Log("[feed] velocity: effect texture \"%s\"", name);
        }, nullptr);
}

static void FeedVelocityTryBind(reshade::api::effect_runtime *rt, UINT expect_w, UINT expect_h)
{
    if (g_feed_velocity_cfg.enabled == 0) return;
    FeedVelocityScanEffects(rt);
    if (g_feed_velocity_cfg.cand >= 0 && g_feed_velocity_cfg.cand < g_feed_velocity.cand_count)
        g_feed_velocity.chosen_cand = g_feed_velocity_cfg.cand;
    else
        g_feed_velocity.chosen_cand = FeedVelocityAutoPick(expect_w, expect_h);

    g_feed_velocity.detected = g_feed_velocity.cand_count > 0 || g_feed_velocity.var.handle != 0;
    if (g_feed_velocity.chosen_cand >= 0)
    {
        const FeedVelocityCand &c = g_feed_velocity.cands[g_feed_velocity.chosen_cand];
        _snprintf_s(g_feed_velocity.overlay_line, sizeof(g_feed_velocity.overlay_line), _TRUNCATE,
                    "auto/pick #%d %s %ux%u score=%d draws=%d",
                    g_feed_velocity.chosen_cand, FeedVelocityFmtLabel(c.fmt),
                    c.width, c.height, c.score, c.draws_total);
    }
    else
        _snprintf_s(g_feed_velocity.overlay_line, sizeof(g_feed_velocity.overlay_line), _TRUNCATE,
                    "%d float RT candidate(s) — pick one or enable Motion Blur in-game",
                    g_feed_velocity.cand_count);
}

static void FeedVelocityComputeScale(FeedVelocityDecode mode, UINT src_w, UINT src_h,
                                     UINT dst_w, UINT dst_h, float user_scale,
                                     float *ox, float *oy)
{
    const float ux = user_scale > 0.f ? user_scale : 1.f;
    float sx = ux, sy = ux;
    switch (mode)
    {
    case FeedVelDecode_DeltaUV:
        sx = ux * (float)dst_w;
        sy = ux * (float)dst_h;
        break;
    case FeedVelDecode_UEPacked:
        // Heuristic: packed [-1,1]-ish → pixel deltas
        sx = ux * (float)dst_w * 0.5f;
        sy = ux * (float)dst_h * 0.5f;
        break;
    case FeedVelDecode_RawPixels:
        sx = ux; sy = ux;
        if (src_w * 2 == dst_w && src_h * 2 == dst_h)
        { sx *= 2.f; sy *= 2.f; } // half-res vectors in pixels of the half buffer
        break;
    case FeedVelDecode_Auto:
    default:
        // Prefer RawPixels; if half-res source, double.
        sx = ux; sy = ux;
        if (src_w * 2 == dst_w && src_h * 2 == dst_h) { sx *= 2.f; sy *= 2.f; }
        break;
    }
    *ox = sx; *oy = sy;
}

// Returns AddRef'd RG16F full-res texture for DLSS, or nullptr.
static ID3D11Texture2D *FeedVelocityAcquireMv(reshade::api::effect_runtime *rt,
                                               reshade::api::device *dev_api,
                                               ID3D11Device *dev11,
                                               ID3D11DeviceContext *ctx,
                                               UINT expect_w, UINT expect_h)
{
    g_feed_velocity.bound_ok = false;
    g_feed_velocity.bound_src[0] = '\0';
    g_feed_velocity.scale_override = false;
    if (g_feed_velocity_cfg.enabled == 0 || dev11 == nullptr || ctx == nullptr)
        return nullptr;

    ID3D11Texture2D *src = nullptr;
    UINT src_w = 0, src_h = 0;
    const char *label = "?";
    const char *src_tag = "?";

    // 1) ReShade effect texture
    if (rt != nullptr && dev_api != nullptr && g_feed_velocity.var.handle != 0)
    {
        reshade::api::resource_view srv = {}, srgb = {};
        rt->get_texture_binding(g_feed_velocity.var, &srv, &srgb);
        if (srv.handle != 0)
        {
            const reshade::api::resource res = dev_api->get_resource_from_view(srv);
            if (res.handle != 0)
            {
                auto *raw = reinterpret_cast<ID3D11Resource *>(res.handle);
                if (SUCCEEDED(raw->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&src))) && src)
                {
                    D3D11_TEXTURE2D_DESC d = {};
                    src->GetDesc(&d);
                    src_w = d.Width; src_h = d.Height;
                    label = g_feed_velocity.name;
                    src_tag = "effect";
                }
            }
        }
    }

    // 2) Hunted RT candidate
    if (src == nullptr)
    {
        int pick = g_feed_velocity_cfg.cand;
        if (pick < 0 || pick >= g_feed_velocity.cand_count)
            pick = g_feed_velocity.chosen_cand;
        if (pick < 0 || pick >= g_feed_velocity.cand_count)
            pick = FeedVelocityAutoPick(expect_w, expect_h);
        if (pick >= 0 && pick < g_feed_velocity.cand_count)
        {
            g_feed_velocity.chosen_cand = pick;
            const FeedVelocityCand &c = g_feed_velocity.cands[pick];
            auto *raw = reinterpret_cast<ID3D11Resource *>(c.handle);
            if (raw && SUCCEEDED(raw->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&src))) && src)
            {
                src_w = c.width; src_h = c.height;
                label = c.name;
                src_tag = "hunt";
            }
        }
    }

    if (src == nullptr) return nullptr;

    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);
    const bool size_ok = (sd.Width == expect_w && sd.Height == expect_h) ||
                         (sd.Width * 2 == expect_w && sd.Height * 2 == expect_h);
    const bool fmt_ok = sd.Format == DXGI_FORMAT_R16G16_FLOAT ||
                        sd.Format == DXGI_FORMAT_R16G16_TYPELESS ||
                        sd.Format == DXGI_FORMAT_R32G32_FLOAT ||
                        sd.Format == DXGI_FORMAT_R32G32_TYPELESS ||
                        sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                        sd.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS;
    if (!size_ok || !fmt_ok)
    {
        src->Release();
        return nullptr;
    }

    if (!FeedVelocityEnsureOut(dev11, expect_w, expect_h))
    {
        src->Release();
        return nullptr;
    }

    DXGI_FORMAT srv_fmt = DXGI_FORMAT_R16G16_FLOAT;
    if (sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || sd.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS)
        srv_fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    else if (sd.Format == DXGI_FORMAT_R32G32_FLOAT || sd.Format == DXGI_FORMAT_R32G32_TYPELESS)
        srv_fmt = DXGI_FORMAT_R32G32_FLOAT;
    else if (sd.Format == DXGI_FORMAT_R16G16_FLOAT || sd.Format == DXGI_FORMAT_R16G16_TYPELESS)
        srv_fmt = DXGI_FORMAT_R16G16_FLOAT;
    else
    {
        src->Release();
        return nullptr;
    }

    const bool same_rg16 =
        sd.Width == expect_w && sd.Height == expect_h &&
        (sd.Format == DXGI_FORMAT_R16G16_FLOAT || sd.Format == DXGI_FORMAT_R16G16_TYPELESS);

    if (same_rg16)
        ctx->CopyResource(g_feed_velocity.out_tex, src);
    else if (!FeedVelocityBlitSrc(dev11, ctx, src, srv_fmt))
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[feed] velocity: blit failed for %ux%u fmt=%d — falling back to OFA/Lumenite",
                sd.Width, sd.Height, (int)sd.Format);
        }
        src->Release();
        return nullptr;
    }

    src->Release();

    FeedVelocityDecode mode = (FeedVelocityDecode)g_feed_velocity_cfg.decode;
    FeedVelocityComputeScale(mode, sd.Width, sd.Height, expect_w, expect_h,
                             g_feed_velocity_cfg.scale,
                             &g_feed_velocity.apply_scale_x, &g_feed_velocity.apply_scale_y);
    g_feed_velocity.scale_override = true;

    g_feed_velocity.bound_ok = true;
    g_feed_velocity.last_w = (int)expect_w;
    g_feed_velocity.last_h = (int)expect_h;
    _snprintf_s(g_feed_velocity.bound_src, sizeof(g_feed_velocity.bound_src), _TRUNCATE, "%s", src_tag);
    _snprintf_s(g_feed_velocity.name, sizeof(g_feed_velocity.name), _TRUNCATE, "%s", label);
    static bool said_ok = false;
    if (!said_ok)
    {
        said_ok = true;
        Log("[feed] motion vectors: ENGINE VELOCITY \"%s\" via %s (%ux%u←%ux%u) decode=%d scale=%.2f,%.2f",
            g_feed_velocity.name, src_tag, expect_w, expect_h, sd.Width, sd.Height,
            g_feed_velocity_cfg.decode, g_feed_velocity.apply_scale_x, g_feed_velocity.apply_scale_y);
    }

    g_feed_velocity.out_tex->AddRef();
    return g_feed_velocity.out_tex;
}
