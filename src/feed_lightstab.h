// feed_lightstab.h -- post-DLSS low-frequency lighting flicker dampener.
//
// We cannot cache the game's shadow maps. After DLSS we only have the final color.
// For near-static pixels (tiny MV) with a *small* luma change (flicker band), blend
// toward the previous output's luma while keeping chroma/HF — damps monitor/emissive
// shimmer without freezing real light cuts.
//
// Include after Log() + D3D11. Call FeedLightStabApply on g.tex11[SLOT_OUTPUT] before blit.

#pragma once

#include <cmath>
#include <cstring>

struct FeedLightStabCfg
{
    int   enabled;     // light_stab=
    float strength;    // 0..1 blend toward previous LF when flicker detected
    float max_delta;   // relative luma delta above this = real change, do not damp
    float min_delta;   // below this = noise floor, ignore
    float mv_eps;      // |mv| in pixels below this counts as static
};

// Defaults: off until auto-profile / overlay enables. Strict mv_eps — only near-static pixels.
static FeedLightStabCfg g_feed_lightstab_cfg = { 0, 0.35f, 0.06f, 0.004f, 0.18f };

struct FeedLightStabState
{
    ID3D11Texture2D          *prev;
    ID3D11ShaderResourceView *prev_srv;
    ID3D11Texture2D          *tmp;
    ID3D11RenderTargetView   *tmp_rtv;
    ID3D11ShaderResourceView *tmp_srv;
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps;
    ID3D11Buffer             *cb;
    ID3D11SamplerState       *smp;
    UINT w, h;
    DXGI_FORMAT fmt;
    bool ready;
    bool have_prev;
    char status[96];
};

static FeedLightStabState g_feed_lightstab = {};

static void FeedLightStabRelease()
{
    if (g_feed_lightstab.prev_srv) { g_feed_lightstab.prev_srv->Release(); g_feed_lightstab.prev_srv = nullptr; }
    if (g_feed_lightstab.prev) { g_feed_lightstab.prev->Release(); g_feed_lightstab.prev = nullptr; }
    if (g_feed_lightstab.tmp_srv) { g_feed_lightstab.tmp_srv->Release(); g_feed_lightstab.tmp_srv = nullptr; }
    if (g_feed_lightstab.tmp_rtv) { g_feed_lightstab.tmp_rtv->Release(); g_feed_lightstab.tmp_rtv = nullptr; }
    if (g_feed_lightstab.tmp) { g_feed_lightstab.tmp->Release(); g_feed_lightstab.tmp = nullptr; }
    if (g_feed_lightstab.vs) { g_feed_lightstab.vs->Release(); g_feed_lightstab.vs = nullptr; }
    if (g_feed_lightstab.ps) { g_feed_lightstab.ps->Release(); g_feed_lightstab.ps = nullptr; }
    if (g_feed_lightstab.cb) { g_feed_lightstab.cb->Release(); g_feed_lightstab.cb = nullptr; }
    if (g_feed_lightstab.smp) { g_feed_lightstab.smp->Release(); g_feed_lightstab.smp = nullptr; }
    g_feed_lightstab.w = g_feed_lightstab.h = 0;
    g_feed_lightstab.ready = false;
    g_feed_lightstab.have_prev = false;
}

static bool FeedLightStabEnsureShaders(ID3D11Device *dev)
{
    if (g_feed_lightstab.vs && g_feed_lightstab.ps && g_feed_lightstab.cb && g_feed_lightstab.smp)
        return true;
    if (dev == nullptr) return false;

    typedef HRESULT (WINAPI *pD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                          ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile) { Log("[feed] lightstab: no d3dcompiler"); return false; }

    static const char kSrc[] =
        "Texture2D cur_tex : register(t0);\n"
        "Texture2D prev_tex : register(t1);\n"
        "Texture2D mv_tex : register(t2);\n"
        "SamplerState smp : register(s0);\n"
        "cbuffer CB : register(b0) {\n"
        "  float strength; float max_delta; float min_delta; float mv_eps;\n"
        "  float has_prev; float has_mv; float2 pad;\n"
        "};\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv; o.pos = float4(uv * float2(2,-2) + float2(-1,1), 0, 1); return o; }\n"
        "float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }\n"
        "float4 ps(VSOut i) : SV_Target {\n"
        "  float3 cur = cur_tex.SampleLevel(smp, i.uv, 0).rgb;\n"
        "  if (has_prev < 0.5) return float4(cur, 1);\n"
        "  // No MV => do not damp (avoids camera-pan throb when vectors are missing).\n"
        "  if (has_mv < 0.5) return float4(cur, 1);\n"
        "  float3 prev = prev_tex.SampleLevel(smp, i.uv, 0).rgb;\n"
        "  float lc = max(Luma(cur), 1e-4); float lp = max(Luma(prev), 1e-4);\n"
        "  float rel = abs(lc - lp) / max(lc, lp);\n"
        "  float2 mv = mv_tex.SampleLevel(smp, i.uv, 0).xy;\n"
        "  float mvlen = length(mv);\n"
        "  // Hard gate: only pixels with |MV| <= mv_eps (near-static). No soft bleed into pans.\n"
        "  if (mvlen > mv_eps) return float4(cur, 1);\n"
        "  float band = (rel >= min_delta && rel <= max_delta) ? 1.0 : 0.0;\n"
        "  float w = saturate(strength) * band;\n"
        "  // Keep chroma of current; pull luma toward previous (dampen flicker).\n"
        "  float new_l = lerp(lc, lp, w);\n"
        "  float3 outc = cur * (new_l / lc);\n"
        "  return float4(outc, 1);\n"
        "}\n";

    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "lightstab", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr))
    {
        Log("[feed] lightstab VS fail 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : "");
        if (err) err->Release();
        return false;
    }
    if (err) { err->Release(); err = nullptr; }
    hr = compile(kSrc, sizeof(kSrc) - 1, "lightstab", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr))
    {
        Log("[feed] lightstab PS fail 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : "");
        if (err) err->Release();
        if (vs) vs->Release();
        return false;
    }
    if (err) err->Release();

    hr = dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_feed_lightstab.vs);
    if (SUCCEEDED(hr))
        hr = dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_feed_lightstab.ps);
    vs->Release();
    ps->Release();
    if (FAILED(hr)) { FeedLightStabRelease(); return false; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 32;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, &g_feed_lightstab.cb)))
    { FeedLightStabRelease(); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &g_feed_lightstab.smp)))
    { FeedLightStabRelease(); return false; }

    return true;
}

static bool FeedLightStabEnsureTargets(ID3D11Device *dev, UINT w, UINT h, DXGI_FORMAT fmt)
{
    if (g_feed_lightstab.prev && g_feed_lightstab.tmp &&
        g_feed_lightstab.w == w && g_feed_lightstab.h == h && g_feed_lightstab.fmt == fmt)
        return true;

    if (g_feed_lightstab.prev_srv) { g_feed_lightstab.prev_srv->Release(); g_feed_lightstab.prev_srv = nullptr; }
    if (g_feed_lightstab.prev) { g_feed_lightstab.prev->Release(); g_feed_lightstab.prev = nullptr; }
    if (g_feed_lightstab.tmp_srv) { g_feed_lightstab.tmp_srv->Release(); g_feed_lightstab.tmp_srv = nullptr; }
    if (g_feed_lightstab.tmp_rtv) { g_feed_lightstab.tmp_rtv->Release(); g_feed_lightstab.tmp_rtv = nullptr; }
    if (g_feed_lightstab.tmp) { g_feed_lightstab.tmp->Release(); g_feed_lightstab.tmp = nullptr; }
    g_feed_lightstab.have_prev = false;

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_feed_lightstab.prev)) ||
        FAILED(dev->CreateShaderResourceView(g_feed_lightstab.prev, nullptr, &g_feed_lightstab.prev_srv)))
        return false;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_feed_lightstab.tmp)) ||
        FAILED(dev->CreateRenderTargetView(g_feed_lightstab.tmp, nullptr, &g_feed_lightstab.tmp_rtv)) ||
        FAILED(dev->CreateShaderResourceView(g_feed_lightstab.tmp, nullptr, &g_feed_lightstab.tmp_srv)))
        return false;

    g_feed_lightstab.w = w;
    g_feed_lightstab.h = h;
    g_feed_lightstab.fmt = fmt;
    g_feed_lightstab.ready = true;
    return true;
}

// Stabilizes DLSS output in-place via tmp ping; updates prev history.
// mv may be null. output must be SRV+RT capable (SLOT_OUTPUT is).
static void FeedLightStabApply(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                               ID3D11Texture2D *output, ID3D11ShaderResourceView *output_srv,
                               ID3D11Texture2D *mv, ID3D11ShaderResourceView *mv_srv,
                               UINT w, UINT h)
{
    _snprintf_s(g_feed_lightstab.status, sizeof(g_feed_lightstab.status), _TRUNCATE, "LightStab: off");
    if (g_feed_lightstab_cfg.enabled == 0 || dev == nullptr || ctx == nullptr ||
        output == nullptr || output_srv == nullptr || w == 0 || h == 0)
        return;

    D3D11_TEXTURE2D_DESC od = {};
    output->GetDesc(&od);
    if (!FeedLightStabEnsureShaders(dev) || !FeedLightStabEnsureTargets(dev, w, h, od.Format))
    {
        _snprintf_s(g_feed_lightstab.status, sizeof(g_feed_lightstab.status), _TRUNCATE, "LightStab: init fail");
        return;
    }

    // First frame: just seed history.
    if (!g_feed_lightstab.have_prev)
    {
        ctx->CopyResource(g_feed_lightstab.prev, output);
        g_feed_lightstab.have_prev = true;
        _snprintf_s(g_feed_lightstab.status, sizeof(g_feed_lightstab.status), _TRUNCATE, "LightStab: seeded");
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(g_feed_lightstab.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    float *f = (float *)mapped.pData;
    f[0] = g_feed_lightstab_cfg.strength;
    f[1] = g_feed_lightstab_cfg.max_delta;
    f[2] = g_feed_lightstab_cfg.min_delta;
    f[3] = g_feed_lightstab_cfg.mv_eps;
    f[4] = 1.0f;
    f[5] = (mv_srv != nullptr) ? 1.0f : 0.0f;
    f[6] = 0.0f; f[7] = 0.0f;
    ctx->Unmap(g_feed_lightstab.cb, 0);

    ID3D11RenderTargetView *prev_rtv = nullptr;
    ID3D11DepthStencilView *prev_dsv = nullptr;
    ctx->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)w; vp.Height = (float)h; vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &g_feed_lightstab.tmp_rtv, nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_feed_lightstab.vs, nullptr, 0);
    ctx->PSSetShader(g_feed_lightstab.ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_feed_lightstab.cb);
    ctx->PSSetSamplers(0, 1, &g_feed_lightstab.smp);

    ID3D11ShaderResourceView *srvs[3] = { output_srv, g_feed_lightstab.prev_srv, mv_srv };
    ctx->PSSetShaderResources(0, 3, srvs);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView *nulls[3] = {};
    ctx->PSSetShaderResources(0, 3, nulls);
    ctx->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
    if (prev_rtv) prev_rtv->Release();
    if (prev_dsv) prev_dsv->Release();

    // Write stabilized result back to OUTPUT and refresh history.
    ctx->CopyResource(output, g_feed_lightstab.tmp);
    ctx->CopyResource(g_feed_lightstab.prev, g_feed_lightstab.tmp);

    _snprintf_s(g_feed_lightstab.status, sizeof(g_feed_lightstab.status), _TRUNCATE,
                "LightStab: on str=%.2f maxΔ=%.3f", g_feed_lightstab_cfg.strength,
                g_feed_lightstab_cfg.max_delta);
}
