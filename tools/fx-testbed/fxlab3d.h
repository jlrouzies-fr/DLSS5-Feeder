// fxlab3d.h -- fxlab's 3D scene (--scene 3d). Included by fxlab.cpp, not a module of its own.
//
// What the 2D scene cannot test: a depth buffer, perspective, parallax, disocclusion, rotating
// and translating objects, thin geometry, a flickering light on a still wall -- and a neural
// consumer, which wants something that looks like a game. It is a small courtyard: tiled
// ground, brick pillars and a back wall, crates, a spinning cube, a sphere sliding from side to
// side, a row of thin fence bars close to the camera, a lamp that flickers, fog and a sky, with
// a game HUD drawn on top (bars, counters, a minimap frame, a crosshair, subtitles).
//
// The camera runs a fixed 720-frame loop: 120 frames still, a yaw pan (up to ~29 px a frame at
// 4K), a dolly in and out past the fence, a strafe. Same picture for a given frame number on
// every run, like the 2D scene.
//
// Two things make it a measuring instrument rather than a demo:
//   - the picture is supersampled (8 jittered passes averaged), so it is the finished,
//     anti-aliased frame a game hands ReShade -- not raw aliased geometry, which would give a
//     temporal model real work to do and hide what it does to a finished picture;
//   - a last, unjittered pass writes the TRUE motion vector of every pixel (from this frame's
//     and last frame's transforms) and leaves the depth buffer ReShade picks up. So the
//     shader's vectors are scored per pixel against the truth, and -- the part that matters
//     with a neural pass, where the right output is no longer the input -- what the chain
//     ADDED to the frame (output minus input) can be followed along the true motion from one
//     frame to the next: an edit that stays put on the surface it belongs to is stable, one
//     that swims, flickers or lags is not, whatever it looks like.

#include <DirectXMath.h>
#include <DirectXPackedVector.h>

using namespace DirectX;

static const char k3dHlsl[] = R"(
cbuffer D : register(b0)
{
    row_major float4x4 World, PrevWorld, ViewProjJ, ViewProj, PrevViewProj;
    float4 Tint;     // rgb, w = material
    float4 Size;     // object size in metres (the mesh is a unit box / unit sphere)
    float4 CamPos;   // xyz, w = frame
    float4 Light;    // lamp position, w = intensity this frame
    float4 Misc;     // x = unused, y = W, z = H, w = 1 for the truth pass (no jitter)
};
struct VO
{
    float4 pos : SV_Position;
    float3 wp : TEXCOORD0; float3 n : TEXCOORD1; float3 op : TEXCOORD2; float3 on : TEXCOORD3;
    float4 cur : TEXCOORD4; float4 prev : TEXCOORD5;
};
VO vs(float3 p : POSITION, float3 n : NORMAL)
{
    VO o;
    float4 w = mul(float4(p, 1.0), World);
    o.cur  = mul(w, ViewProj);
    o.prev = mul(mul(float4(p, 1.0), PrevWorld), PrevViewProj);
    o.pos  = Misc.w > 0.5 ? o.cur : mul(w, ViewProjJ);
    o.wp = w.xyz; o.n = mul(float4(n, 0.0), World).xyz; o.op = p * Size.xyz; o.on = n;
    return o;
}
float h2(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float vnoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(h2(i), h2(i + float2(1, 0)), f.x), lerp(h2(i + float2(0, 1)), h2(i + float2(1, 1)), f.x), f.y);
}
static const float3 kFog = float3(0.62, 0.72, 0.86);
float3 sky(float y01) { return lerp(float3(0.20, 0.38, 0.70), kFog, saturate(y01 * 1.9)); }

// Every pattern fades to its average where it gets finer than a pixel: the frame is meant to
// be a clean one, and procedural texture that aliases in the distance is not.
float3 albedo(float mat, float3 wp, float3 op, float3 on)
{
    float3 a = abs(on);
    float2 uv = a.x > a.y && a.x > a.z ? op.zy : (a.y > a.z ? op.xz : op.xy);
    if (mat < 0.5)
    {
        float2 t = wp.xz / 2.0;
        float aa = max(fwidth(t.x), fwidth(t.y));
        float2 g = abs(frac(t) - 0.5);
        float d = (0.5 - max(g.x, g.y)) * 2.0;                       // metres to the nearest joint
        float joint = (1.0 - smoothstep(0.03, 0.03 + 3.0 * aa, d)) * saturate(1.0 - aa * 3.0);
        float tone = lerp(0.5, h2(floor(t)), saturate(1.0 - aa * 2.5));
        float grain = (vnoise(wp.xz * 5.0) - 0.5) * 0.16 * saturate(1.0 - aa * 12.0);
        return (0.42 + 0.20 * tone + grain) * (1.0 - 0.55 * joint);
    }
    if (mat < 1.5)
    {
        float row = floor(uv.y / 0.3);
        float2 b = float2(uv.x / 0.6 + 0.5 * fmod(abs(row), 2.0), uv.y / 0.3);
        float aa = max(fwidth(uv.x), fwidth(uv.y));
        float2 f = abs(frac(b) - 0.5);
        float d = min((0.5 - f.x) * 0.6, (0.5 - f.y) * 0.3);
        float fade = saturate(1.0 - aa * 8.0);
        float mortar = (1.0 - smoothstep(0.012, 0.012 + 1.5 * aa, d)) * fade;
        float tone = lerp(0.5, h2(floor(b)), fade);
        float grain = (vnoise(uv * 24.0) - 0.5) * 0.14 * saturate(1.0 - aa * 50.0);
        return lerp((0.75 + 0.45 * tone + grain).xxx, float3(1.25, 1.3, 1.35), mortar);
    }
    if (mat < 2.5)
    {
        float aa = fwidth(op.y) * 6.0;
        float s = 0.5 + 0.5 * sin(op.y * 6.0 * 6.2832) * saturate(1.0 - aa);
        return lerp(0.55, 1.15, s) + (vnoise(op.xz * 14.0) - 0.5) * 0.12;
    }
    if (mat < 3.5) return 1.0;
    {
        float aa = max(fwidth(uv.x), fwidth(uv.y));
        float2 e = 0.5 * Size.xy - abs(uv);                           // distance to the face's edge
        float frame = 1.0 - smoothstep(0.09, 0.09 + 1.5 * aa, min(e.x, e.y));
        float pl = abs(frac(uv.x / 0.19) - 0.5) * 0.19;
        float gap = (1.0 - smoothstep(0.006, 0.006 + 1.5 * aa, pl)) * saturate(1.0 - aa * 10.0);
        float grain = (vnoise(float2(uv.x * 40.0, uv.y * 4.0)) - 0.5) * 0.18 * saturate(1.0 - aa * 60.0);
        return (1.0 + grain) * (1.0 - 0.45 * gap) * lerp(1.0, 0.7, frame);
    }
}
float4 ps_color(VO i) : SV_Target
{
    float3 n = normalize(i.n);
    float3 base = Tint.rgb * albedo(Tint.w, i.wp, i.op, i.on);
    float3 L = normalize(float3(0.45, 0.8, -0.35));
    float3 l = Light.xyz - i.wp;
    float  dl = length(l);
    float3 lit = (0.34 + 0.14 * n.y) + 0.72 * saturate(dot(n, L))
               + float3(1.0, 0.72, 0.42) * Light.w * saturate(dot(n, l / dl)) / (1.0 + 0.10 * dl * dl) * 2.2;
    float3 v = normalize(CamPos.xyz - i.wp);
    float spec = pow(saturate(dot(n, normalize(L + v))), 40.0) * (Tint.w > 2.5 && Tint.w < 3.5 ? 0.6 : 0.08);
    float3 col = base * lit + spec;
    float fog = 1.0 - exp(-length(CamPos.xyz - i.wp) * 0.016);
    return float4(lerp(col, kFog, fog), 1.0);
}
// xy = where this pixel was a frame ago minus where it is, in pixels; z = its view depth now;
// w = the view depth it had then (to tell, with last frame's z, whether it was visible at all).
float4 ps_truth(VO i) : SV_Target
{
    float2 c = i.cur.xy / i.cur.w, p = i.prev.xy / i.prev.w;
    return float4((p - c) * float2(0.5 * Misc.y, -0.5 * Misc.z), i.cur.w, i.prev.w);
}
float4 ps_sky(float4 pos : SV_Position) : SV_Target { return float4(sky(pos.y / Misc.z), 1.0); }
)";

struct Cb3
{
    XMFLOAT4X4 world, prev_world, vpj, vp, prev_vp;
    float tint[4], size[4], cam[4], light[4], misc[4];
};

struct Obj3 { int mesh; float mat; float tint[3]; float size[3]; float pos[3]; float rot; int dyn; };

struct Scene3D
{
    ComPtr<ID3D11Texture2D> depth, sample, accum, truth, truth_staging;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11RenderTargetView> sample_rtv, accum_rtv, truth_rtv;
    ComPtr<ID3D11ShaderResourceView> sample_srv, accum_srv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_color, ps_truth, ps_sky;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> cb, vb[2], ib[2];
    UINT count[2] = {};
    ComPtr<ID3D11DepthStencilState> dss, dss_off;
    ComPtr<ID3D11BlendState> bs_accum;
    std::vector<Obj3> objs;
};
static Scene3D s3;

static const int kJitters = 8;
static const float kJitter[kJitters][2] = { { 1, -3 }, { -1, 3 }, { 5, 1 }, { -3, -5 }, { -5, 5 }, { -7, -1 }, { 3, 7 }, { 7, -7 } };   // sixteenths of a pixel

static void Camera3D(int frame, XMVECTOR *pos, float *yaw)
{
    const int f = ((frame % 720) + 720) % 720;
    float x = 0.0f, z = -8.0f, a = 0.0f;
    const float tau = 6.2831853f;
    if (f >= 120 && f < 300)      a = 0.45f * sinf(tau * (f - 120) / 180.0f);
    else if (f >= 300 && f < 480) z = -8.0f + 2.0f * (1.0f - cosf(tau * (f - 300) / 180.0f));
    else if (f >= 480)            { const float s = sinf(tau * (f - 480) / 240.0f); x = 3.0f * s; a = -0.3f * s; }
    *pos = XMVectorSet(x, 1.7f, z, 1.0f);
    *yaw = a;
}
static XMMATRIX View3D(int frame)
{
    XMVECTOR pos; float yaw;
    Camera3D(frame, &pos, &yaw);
    return XMMatrixLookToLH(pos, XMVectorSet(sinf(yaw), -0.06f, cosf(yaw), 0.0f), XMVectorSet(0, 1, 0, 0));
}
static XMMATRIX World3D(const Obj3 &o, int frame)
{
    float rot = o.rot, x = o.pos[0];
    if (o.dyn == 1) rot += 0.02f * frame;
    if (o.dyn == 2) x += 3.0f * sinf(0.03f * frame);
    return XMMatrixScaling(o.size[0], o.size[1], o.size[2]) * XMMatrixRotationY(rot) * XMMatrixTranslation(x, o.pos[1], o.pos[2]);
}

static void Mesh3D(int slot, const std::vector<float> &v, const std::vector<uint16_t> &idx)
{
    D3D11_BUFFER_DESC bd = { static_cast<UINT>(v.size() * sizeof(float)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA sd = { v.data(), 0, 0 };
    CHECK(g.dev->CreateBuffer(&bd, &sd, &s3.vb[slot]), "vertex buffer");
    bd.ByteWidth = static_cast<UINT>(idx.size() * sizeof(uint16_t)); bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = idx.data();
    CHECK(g.dev->CreateBuffer(&bd, &sd, &s3.ib[slot]), "index buffer");
    s3.count[slot] = static_cast<UINT>(idx.size());
}

static void Init3D()
{
    const UINT W = g.W, H = g.H;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    CHECK(g.dev->CreateTexture2D(&td, nullptr, &s3.depth), "depth");
    CHECK(g.dev->CreateDepthStencilView(s3.depth.Get(), nullptr, &s3.dsv), "DSV");
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    CHECK(g.dev->CreateTexture2D(&td, nullptr, &s3.sample), "sample");
    CHECK(g.dev->CreateTexture2D(&td, nullptr, &s3.accum), "accum");
    CHECK(g.dev->CreateTexture2D(&td, nullptr, &s3.truth), "truth");
    CHECK(g.dev->CreateRenderTargetView(s3.sample.Get(), nullptr, &s3.sample_rtv), "RTV");
    CHECK(g.dev->CreateRenderTargetView(s3.accum.Get(), nullptr, &s3.accum_rtv), "RTV");
    CHECK(g.dev->CreateRenderTargetView(s3.truth.Get(), nullptr, &s3.truth_rtv), "RTV");
    CHECK(g.dev->CreateShaderResourceView(s3.sample.Get(), nullptr, &s3.sample_srv), "SRV");
    CHECK(g.dev->CreateShaderResourceView(s3.accum.Get(), nullptr, &s3.accum_srv), "SRV");
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(g.dev->CreateTexture2D(&td, nullptr, &s3.truth_staging), "truth staging");

    auto v = Compile(k3dHlsl, "vs", "vs_5_0");
    auto c = Compile(k3dHlsl, "ps_color", "ps_5_0");
    auto t = Compile(k3dHlsl, "ps_truth", "ps_5_0");
    auto k = Compile(k3dHlsl, "ps_sky", "ps_5_0");
    CHECK(g.dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &s3.vs), "3D VS");
    CHECK(g.dev->CreatePixelShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &s3.ps_color), "3D PS");
    CHECK(g.dev->CreatePixelShader(t->GetBufferPointer(), t->GetBufferSize(), nullptr, &s3.ps_truth), "truth PS");
    CHECK(g.dev->CreatePixelShader(k->GetBufferPointer(), k->GetBufferSize(), nullptr, &s3.ps_sky), "sky PS");
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    CHECK(g.dev->CreateInputLayout(il, 2, v->GetBufferPointer(), v->GetBufferSize(), &s3.layout), "input layout");
    D3D11_BUFFER_DESC bd = { sizeof(Cb3), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    CHECK(g.dev->CreateBuffer(&bd, nullptr, &s3.cb), "3D constants");

    // Reversed depth (near = 1, far = 0): what ReShade.fxh assumes unless told otherwise.
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_GREATER;
    CHECK(g.dev->CreateDepthStencilState(&dd, &s3.dss), "depth state");
    dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    CHECK(g.dev->CreateDepthStencilState(&dd, &s3.dss_off), "depth state");
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE; bl.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR; bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    CHECK(g.dev->CreateBlendState(&bl, &s3.bs_accum), "accumulate blend");

    // Unit box, flat-shaded.
    {
        std::vector<float> vb; std::vector<uint16_t> ib;
        const float n[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        for (int f = 0; f < 6; ++f)
        {
            const XMVECTOR nn = XMVectorSet(n[f][0], n[f][1], n[f][2], 0);
            const XMVECTOR u = f < 2 ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
            const XMVECTOR w = XMVector3Cross(nn, u);
            const uint16_t base = static_cast<uint16_t>(vb.size() / 6);
            for (int k2 = 0; k2 < 4; ++k2)
            {
                const float su = (k2 & 1) ? 0.5f : -0.5f, sw = (k2 & 2) ? 0.5f : -0.5f;
                XMFLOAT3 p; XMStoreFloat3(&p, nn * 0.5f + u * su + w * sw);
                vb.insert(vb.end(), { p.x, p.y, p.z, n[f][0], n[f][1], n[f][2] });
            }
            ib.insert(ib.end(), { base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 3) });
        }
        Mesh3D(0, vb, ib);
    }
    // Unit sphere (diameter 1).
    {
        std::vector<float> vb; std::vector<uint16_t> ib;
        const int R = 24, C = 48;
        for (int r = 0; r <= R; ++r)
            for (int c2 = 0; c2 <= C; ++c2)
            {
                const float th = 3.14159265f * r / R, ph = 6.2831853f * c2 / C;
                const float x = sinf(th) * cosf(ph), y = cosf(th), z = sinf(th) * sinf(ph);
                vb.insert(vb.end(), { 0.5f * x, 0.5f * y, 0.5f * z, x, y, z });
            }
        for (int r = 0; r < R; ++r)
            for (int c2 = 0; c2 < C; ++c2)
            {
                const uint16_t a = static_cast<uint16_t>(r * (C + 1) + c2), b = static_cast<uint16_t>(a + C + 1);
                ib.insert(ib.end(), { a, b, static_cast<uint16_t>(a + 1), static_cast<uint16_t>(a + 1), b, static_cast<uint16_t>(b + 1) });
            }
        Mesh3D(1, vb, ib);
    }

    auto add = [](int mesh, float mat, float r, float gg, float b, float sx, float sy, float sz, float px, float py, float pz, float rot = 0.0f, int dyn = 0)
    {
        s3.objs.push_back({ mesh, mat, { r, gg, b }, { sx, sy, sz }, { px, py, pz }, rot, dyn });
    };
    add(0, 0, 0.78f, 0.74f, 0.68f, 240, 0.2f, 240, 0, -0.1f, 40);                       // ground
    for (int i = 0; i < 3; ++i)
    {
        add(0, 1, 0.72f, 0.42f, 0.33f, 1.0f, 4.5f, 1.0f, -3.5f, 2.25f, 2.0f + 7.0f * i);   // pillars
        add(0, 1, 0.72f, 0.42f, 0.33f, 1.0f, 4.5f, 1.0f,  3.5f, 2.25f, 2.0f + 7.0f * i);
    }
    add(0, 1, 0.66f, 0.50f, 0.42f, 34, 6, 1, 0, 3, 24);                                  // back wall, under the lamp
    add(0, 4, 0.75f, 0.58f, 0.36f, 1, 1, 1, -1.8f, 0.5f, 4.0f);                          // crates
    add(0, 4, 0.75f, 0.58f, 0.36f, 1, 1, 1, -0.6f, 0.5f, 4.4f, 0.4f);
    add(0, 4, 0.70f, 0.54f, 0.34f, 1, 1, 1, -1.3f, 1.5f, 4.1f, 0.2f);
    add(0, 4, 0.50f, 0.65f, 0.82f, 1.2f, 1.2f, 1.2f, 1.6f, 1.3f, 5.0f, 0.0f, 1);         // spinning cube
    add(1, 2, 0.92f, 0.30f, 0.25f, 1.6f, 1.6f, 1.6f, 0.0f, 0.8f, 9.0f, 0.0f, 2);         // sliding sphere
    add(1, 2, 0.30f, 0.70f, 0.42f, 2.0f, 2.0f, 2.0f, -5.5f, 1.0f, 11.0f);                // still sphere
    for (int i = 0; i < 12; ++i)                                                          // fence: thin bars, close
        add(0, 3, 0.13f, 0.13f, 0.15f, 0.05f, 1.4f, 0.05f, 1.4f + 0.35f * i, 0.7f, -3.6f);
    add(0, 3, 0.13f, 0.13f, 0.15f, 4.2f, 0.05f, 0.05f, 3.33f, 1.4f, -3.6f);
}

// The picture of frame `frame` into g.scene, its true motion into s3.truth, its depth left in
// s3.depth for ReShade.
// `before` is the scene frame that was presented last -- the same one again when a 30 fps game
// is fed at 60 -- and the true motion written is the motion since THAT picture.
static void Render3D(int frame, int before)
{
    const float W = static_cast<float>(g.W), H = static_cast<float>(g.H);
    const D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    const int fp = before >= 0 ? before : 0;
    const XMMATRIX P   = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), W / H, 400.0f, 0.1f);   // far and near swapped: reversed depth
    const XMMATRIX VP  = View3D(frame) * P, PVP = View3D(fp) * P;
    XMVECTOR cam; float yaw;
    Camera3D(frame, &cam, &yaw);
    const uint32_t hsh = static_cast<uint32_t>(frame / 2) * 2654435761u;
    const float lamp = 0.9f + 0.7f * static_cast<float>((hsh >> 8) & 0xFFFF) / 65535.0f;   // flickers every other frame

    Cb3 cb = {};
    XMStoreFloat3(reinterpret_cast<XMFLOAT3 *>(cb.cam), cam); cb.cam[3] = static_cast<float>(frame);
    cb.light[0] = 0.0f; cb.light[1] = 2.6f; cb.light[2] = 21.5f; cb.light[3] = lamp;
    cb.misc[1] = W; cb.misc[2] = H;
    XMStoreFloat4x4(&cb.vp, VP); XMStoreFloat4x4(&cb.prev_vp, PVP);

    auto draw_objects = [&](ID3D11PixelShader *ps)
    {
        g.ctx->IASetInputLayout(s3.layout.Get());
        g.ctx->VSSetShader(s3.vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(ps, nullptr, 0);
        g.ctx->OMSetDepthStencilState(s3.dss.Get(), 0);
        for (const Obj3 &o : s3.objs)
        {
            XMStoreFloat4x4(&cb.world, World3D(o, frame));
            XMStoreFloat4x4(&cb.prev_world, World3D(o, fp));
            memcpy(cb.tint, o.tint, 12); cb.tint[3] = o.mat;
            memcpy(cb.size, o.size, 12);
            g.ctx->UpdateSubresource(s3.cb.Get(), 0, nullptr, &cb, 0, 0);
            const UINT stride = 24, offset = 0;
            g.ctx->IASetVertexBuffers(0, 1, s3.vb[o.mesh].GetAddressOf(), &stride, &offset);
            g.ctx->IASetIndexBuffer(s3.ib[o.mesh].Get(), DXGI_FORMAT_R16_UINT, 0);
            g.ctx->DrawIndexed(s3.count[o.mesh], 0, 0);
        }
    };

    ID3D11RenderTargetView *null_rtv = nullptr;
    ID3D11ShaderResourceView *null_srv = nullptr;
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->RSSetState(g.rs.Get());
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->VSSetConstantBuffers(0, 1, s3.cb.GetAddressOf());
    g.ctx->PSSetConstantBuffers(0, 1, s3.cb.GetAddressOf());
    const float zero[4] = { 0, 0, 0, 0 };
    g.ctx->ClearRenderTargetView(s3.accum_rtv.Get(), zero);

    for (int j = 0; j < kJitters; ++j)
    {
        const XMMATRIX J = XMMatrixTranslation(2.0f * kJitter[j][0] / 16.0f / W, -2.0f * kJitter[j][1] / 16.0f / H, 0.0f);
        XMStoreFloat4x4(&cb.vpj, VP * J);
        cb.misc[3] = 0.0f;
        g.ctx->UpdateSubresource(s3.cb.Get(), 0, nullptr, &cb, 0, 0);
        g.ctx->ClearDepthStencilView(s3.dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        g.ctx->OMSetRenderTargets(1, s3.sample_rtv.GetAddressOf(), s3.dsv.Get());
        g.ctx->OMSetDepthStencilState(s3.dss_off.Get(), 0);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(s3.ps_sky.Get(), nullptr, 0);
        g.ctx->Draw(3, 0);
        draw_objects(s3.ps_color.Get());

        const float w8[4] = { 1.0f / kJitters, 1.0f / kJitters, 1.0f / kJitters, 1.0f / kJitters };
        g.ctx->OMSetRenderTargets(1, s3.accum_rtv.GetAddressOf(), nullptr);
        g.ctx->OMSetBlendState(s3.bs_accum.Get(), w8, 0xFFFFFFFF);
        g.ctx->OMSetDepthStencilState(s3.dss_off.Get(), 0);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(g.ps_blit.Get(), nullptr, 0);
        g.ctx->PSSetShaderResources(0, 1, s3.sample_srv.GetAddressOf());
        g.ctx->Draw(3, 0);
        g.ctx->PSSetShaderResources(0, 1, &null_srv);
    }

    // Truth: no jitter, and the depth buffer this leaves behind is the one ReShade reads.
    cb.misc[3] = 1.0f;
    XMStoreFloat4x4(&cb.vpj, VP);
    g.ctx->ClearDepthStencilView(s3.dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
    g.ctx->ClearRenderTargetView(s3.truth_rtv.Get(), zero);
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g.ctx->OMSetRenderTargets(1, s3.truth_rtv.GetAddressOf(), s3.dsv.Get());
    draw_objects(s3.ps_truth.Get());

    g.ctx->OMSetRenderTargets(1, g.scene_rtv.GetAddressOf(), nullptr);
    g.ctx->OMSetDepthStencilState(s3.dss_off.Get(), 0);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.ps_blit.Get(), nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, s3.accum_srv.GetAddressOf());
    g.ctx->Draw(3, 0);
    g.ctx->PSSetShaderResources(0, 1, &null_srv);
    g.ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
    g.ctx->OMSetDepthStencilState(nullptr, 0);
    g.ctx->PSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
    g.ctx->Flush();
}

// The HUD: everything on it is still, whatever the world behind it does.
static void DrawHud3D()
{
    auto dc = g.dc.Get();
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Scale(g.S, g.S));
    g.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
    dc->FillRectangle(D2D1::RectF(100, 90, 700, 150), g.brush.Get());
    g.brush->SetColor(D2D1::ColorF(0.25f, 0.85f, 0.35f, 1.0f));
    dc->FillRectangle(D2D1::RectF(110, 100, 110 + 580 * 0.87f, 140), g.brush.Get());
    Text(L"HP 87 / 100", 34, 110, 160, 600, 1.0f, true);
    Text(L"Objective: reach the lighthouse  -  240 m", 22, 110, 216, 900, 0.95f);
    Text(L"Press [E] to open the gate.  Checkpoint saved 02:14 ago.", 16, 110, 252, 900, 0.9f);

    g.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.35f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(3340, 90, 3740, 490), 16, 16), g.brush.Get());
    g.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.9f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(3340.5f, 90.5f, 3740.5f, 490.5f), 16, 16), g.brush.Get(), 2.0f);
    for (int i = 1; i < 4; ++i)
    {
        dc->DrawLine(D2D1::Point2F(3340.0f + 100.0f * i, 100), D2D1::Point2F(3340.0f + 100.0f * i, 480), g.brush.Get(), 1.0f / g.S);
        dc->DrawLine(D2D1::Point2F(3350, 90.0f + 100.0f * i), D2D1::Point2F(3730, 90.0f + 100.0f * i), g.brush.Get(), 1.0f / g.S);
    }
    Text(L"N", 26, 3528, 96, 60, 1.0f, true);

    g.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.9f));
    dc->DrawLine(D2D1::Point2F(1900, 1080), D2D1::Point2F(1914, 1080), g.brush.Get(), 2.0f);
    dc->DrawLine(D2D1::Point2F(1926, 1080), D2D1::Point2F(1940, 1080), g.brush.Get(), 2.0f);
    dc->DrawLine(D2D1::Point2F(1920, 1060), D2D1::Point2F(1920, 1074), g.brush.Get(), 2.0f);
    dc->DrawLine(D2D1::Point2F(1920, 1086), D2D1::Point2F(1920, 1100), g.brush.Get(), 2.0f);

    Text(L"AMMO", 22, 3400, 1930, 340, 0.9f);
    Text(L"24 | 120", 56, 3400, 1955, 340, 1.0f, true);

    g.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.5f));
    dc->FillRectangle(D2D1::RectF(1220, 1868, 2620, 1992), g.brush.Get());
    Text(L"- The old lamp by the wall has been flickering all night.", 30, 1250, 1878, 1340, 1.0f);
    Text(L"- Then let's be quick about it. Stay behind the pillars.", 30, 1250, 1932, 1340, 1.0f);
    CHECK(dc->EndDraw(), "D2D EndDraw");
}

// ---------------------------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------------------------

struct Cap3
{
    std::vector<uint8_t>  scene, nohud; // RGBA8: the frame as drawn, and the same before the HUD went on (3D only)
    std::vector<float>    pre;          // RGB 0..1: the frame as fed -- the swap chain's own code values
    std::vector<uint16_t> truth;        // RGBA16F: see ps_truth
    std::vector<float>    post;         // RGB 0..1: what came back
    bool report = false;                // false: only kept as the previous frame of one that is
};

static float Bilin(const std::vector<float> &img, int W, int H, float x, float y)
{
    x = std::clamp(x, 0.0f, W - 1.001f); y = std::clamp(y, 0.0f, H - 1.001f);
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const float fx = x - x0, fy = y - y0;
    const float *p = img.data() + static_cast<size_t>(y0) * W + x0;
    return (p[0] * (1 - fx) + p[1] * fx) * (1 - fy) + (p[W] * (1 - fx) + p[W + 1] * fx) * fy;
}

static void Crop3(const char *name, int frame, const Cap3 &c, float bx0, float by0, float bx1, float by1);

static void Analyse3D(int frame, const Cap3 &c, const Cap3 *prev)
{
    using PackedVector::XMConvertHalfToFloat;
    const int W = g.W, H = g.H;
    const size_t N = static_cast<size_t>(W) * H;
    std::vector<float> lin(N), lout(N);
    std::vector<uint8_t> cls(N);   // 0 sky, 1 scene, 2 HUD
    for (size_t i = 0; i < N; ++i)
    {
        lin[i]  = 0.299f * c.pre[i * 3] + 0.587f * c.pre[i * 3 + 1] + 0.114f * c.pre[i * 3 + 2];
        lout[i] = 0.299f * c.post[i * 3] + 0.587f * c.post[i * 3 + 1] + 0.114f * c.post[i * 3 + 2];
        const bool hud = memcmp(&c.scene[i * 4], &c.nohud[i * 4], 3) != 0;
        cls[i] = hud ? 2 : (XMConvertHalfToFloat(c.truth[i * 4 + 2]) > 0.0f ? 1 : 0);
    }
    auto emit = [&](const char *probe, const char *kind, double a, double b, double cc, double d)
    {
        if (g.csv) fprintf(g.csv, "%s,%d,%s,%s,%.4f,%.3f,%.4f,%.2f\n", g.tag.c_str(), frame, probe, kind, a, b, cc, d);
    };

    if (g.mvdump)
    {
        // Vectors against the truth. "Textured" pixels have a motion one can see; "flat" ones
        // do not, but a consumer that tracks surfaces still wants them to move with the rest.
        struct Acc { double n = 0, err = 0, q = 0, o = 0, z = 0, sx = 0, sy = 0, tx = 0, ty = 0; };
        Acc acc[5];   // hud, still textured, moving textured, moving flat, moving thin (near a depth edge)
        static const char *const names[5] = { "3d.hud", "3d.still", "3d.moving", "3d.moving.flat", "3d.moving.edge" };
        for (int y = 2; y < H - 2; ++y)
            for (int x = 2; x < W - 2; ++x)
            {
                const size_t i = static_cast<size_t>(y) * W + x;
                if (cls[i] == 0) continue;
                const float grad = fabsf(lin[i + 1] - lin[i - 1]) + fabsf(lin[i + W] - lin[i - W]);
                float tx = 0.0f, ty = 0.0f;
                int k;
                if (cls[i] == 2) { if (grad < 0.30f) continue; k = 0; }
                else
                {
                    tx = XMConvertHalfToFloat(c.truth[i * 4]); ty = XMConvertHalfToFloat(c.truth[i * 4 + 1]);
                    const float z = XMConvertHalfToFloat(c.truth[i * 4 + 2]);
                    const bool moving = fabsf(tx) + fabsf(ty) > 0.05f;
                    // a depth step within two pixels: an outline, or something thin
                    bool edge = false;
                    for (int d2 = 0; d2 < 4 && !edge; ++d2)
                    {
                        const size_t j = i + (d2 == 0 ? 2 : d2 == 1 ? -2 : d2 == 2 ? 2 * W : -2 * W);
                        const float zj = XMConvertHalfToFloat(c.truth[j * 4 + 2]);
                        edge = zj <= 0.0f || fabsf(zj - z) > 0.08f * z + 0.05f;
                    }
                    if (!moving) { if (grad < 0.06f) continue; k = 1; }
                    else k = edge ? 4 : (grad >= 0.06f ? 2 : 3);
                }
                const float mx = (c.post[i * 3] * 1023.0f - 512.0f) / 8.0f, my = (c.post[i * 3 + 1] * 1023.0f - 512.0f) / 8.0f;
                const float e = hypotf(mx - tx, my - ty);
                Acc &a = acc[k];
                a.n += 1; a.err += e; a.sx += mx; a.sy += my; a.tx += tx; a.ty += ty;
                if (e < 0.25f) a.q += 1;
                if (e < 1.0f) a.o += 1;
                if (fabsf(mx) < 0.01f && fabsf(my) < 0.01f) a.z += 1;
            }
        for (int k = 0; k < 5; ++k)
        {
            const Acc &a = acc[k];
            const double n = a.n > 0 ? a.n : 1;
            printf("  f%-4d %-15s %9.0f px  truth (%6.2f,%6.2f) got (%6.2f,%6.2f)  err %5.2f px  <0.25: %5.1f%%  <1: %5.1f%%  zero %5.1f%%\n",
                   frame, names[k], a.n, a.tx / n, a.ty / n, a.sx / n, a.sy / n, a.err / n, 100 * a.q / n, 100 * a.o / n, 100 * a.z / n);
            emit(names[k], "vec", a.err / n, 100 * a.q / n, 100 * a.o / n, 100 * a.z / n);
        }
        return;
    }

    // 1. The picture against what went in, HUD and scene apart. Without a neural pass the
    //    right answer is the input; with one, the HUD still is.
    for (int k = 2; k >= 1; --k)
    {
        double sae = 0, sse = 0, gp = 0, gq = 0, n = 0;
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x)
            {
                const size_t i = static_cast<size_t>(y) * W + x;
                if (cls[i] != k) continue;
                const double d = fabs(static_cast<double>(lout[i]) - lin[i]);
                sae += d; sse += d * d; n += 1;
                gp += fabs(lin[i + 1] - lin[i]) + fabs(lin[i + W] - lin[i]);
                gq += fabs(lout[i + 1] - lout[i]) + fabs(lout[i + W] - lout[i]);
            }
        if (n < 1) n = 1;
        const double mae = 255 * sae / n, mse = sse / n, psnr = mse > 1e-12 ? 10 * log10(1 / mse) : 99, sharp = gp > 1e-9 ? gq / gp : 1;
        const char *name = k == 2 ? "3d.hud" : "3d.scene";
        printf("  f%-4d %-15s mae %6.2f  psnr %5.1f  sharp %5.3f\n", frame, name, mae, psnr, sharp);
        emit(name, k == 2 ? "static" : "moving", mae, psnr, sharp, 0);
    }

    // 2. Stability along the true motion. For every scene pixel that was visible a frame ago:
    //    how much did the picture change against itself, carried along its own motion? For the
    //    input that is what the scene really did (lighting, the lamp, resampling). For the
    //    output it is that plus whatever the chain adds on top; for the EDIT (output - input)
    //    it is the chain alone.
    if (prev != nullptr && !prev->post.empty() && !prev->truth.empty())
    {
        std::vector<float> pin(N), pout(N);
        for (size_t i = 0; i < N; ++i)
        {
            pin[i]  = 0.299f * prev->pre[i * 3] + 0.587f * prev->pre[i * 3 + 1] + 0.114f * prev->pre[i * 3 + 2];
            pout[i] = 0.299f * prev->post[i * 3] + 0.587f * prev->post[i * 3 + 1] + 0.114f * prev->post[i * 3 + 2];
        }
        double n = 0, fin = 0, fout = 0, fed = 0, ed = 0;
        for (int y = 2; y < H - 2; ++y)
            for (int x = 2; x < W - 2; ++x)
            {
                const size_t i = static_cast<size_t>(y) * W + x;
                if (cls[i] != 1) continue;
                const float tx = XMConvertHalfToFloat(c.truth[i * 4]), ty = XMConvertHalfToFloat(c.truth[i * 4 + 1]);
                const float px = x + tx, py = y + ty;
                if (px < 2 || py < 2 || px > W - 3 || py > H - 3) continue;
                const size_t j = static_cast<size_t>(py + 0.5f) * W + static_cast<size_t>(px + 0.5f);
                if (memcmp(&prev->scene[j * 4], &prev->nohud[j * 4], 3) != 0) continue;      // was under the HUD
                const float zw = XMConvertHalfToFloat(c.truth[i * 4 + 3]), zp = XMConvertHalfToFloat(prev->truth[j * 4 + 2]);
                if (zp <= 0.0f || fabsf(zp - zw) > 0.03f * zw + 0.03f) continue;             // was hidden: nothing to compare with
                const float a = Bilin(pin, W, H, px, py), b = Bilin(pout, W, H, px, py);
                const float e = lout[i] - lin[i];
                fin += fabsf(lin[i] - a); fout += fabsf(lout[i] - b); fed += fabsf(e - (b - a)); ed += fabsf(e); n += 1;
            }
        if (n < 1) n = 1;
        printf("  f%-4d 3d.stability    input %6.3f  output %6.3f (x%4.2f)   edit size %6.3f  edit change %6.3f (%3.0f%% of it)   [8-bit steps, %0.0f%% of the frame]\n",
               frame, 255 * fin / n, 255 * fout / n, fin > 0 ? fout / fin : 0.0, 255 * ed / n, 255 * fed / n, ed > 0 ? 100 * fed / ed : 0.0, 100.0 * n / N);
        emit("3d.change.in", "stab", 255 * fin / n, 0, 1, 0);
        emit("3d.change.out", "stab", 255 * fout / n, 0, fin > 0 ? fout / fin : 0, 0);
        emit("3d.edit.size", "stab", 255 * ed / n, 0, 1, 0);
        emit("3d.edit.change", "stab", 255 * fed / n, 0, ed > 0 ? fed / ed : 0, 0);
    }

    Crop3("3d.hud", frame, c, 80, 70, 1100, 290);
    Crop3("3d.subs", frame, c, 1200, 1850, 2640, 2010);
    Crop3("3d.centre", frame, c, 1380, 780, 2460, 1380);
    Crop3("3d.fence", frame, c, 2300, 900, 3380, 1500);
    Crop3("3d.left", frame, c, 300, 700, 1380, 1300);
}
