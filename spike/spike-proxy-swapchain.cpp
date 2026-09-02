// Spike: a proxy swapchain. The "game" half of this program asks for a 960x540 swapchain
// on a 1920x1080 window and renders a test pattern into whatever GetBuffer() hands it.
// The proxy half wraps the real IDXGISwapChain: GetBuffer() returns a private 960x540
// texture, GetDesc() reports 960x540, and Present() upscales that texture into the real
// 1920x1080 backbuffer (FSR 1 EASU from src/feed_fsr1.h, or bilinear with --bilinear)
// before the real Present. ResizeBuffers() re-fits the real chain to the window and keeps
// the private one at the size the game asked for.
//
// It answers the contract question in PLAN-PROXY-SWAPCHAIN.md -- can an in-process COM
// wrapper make a D3D11 app render fewer pixels than it presents, with the app none the
// wiser -- without ReShade, NvPresent or a real game in the process. PASS = the frames
// reach the display through the wrapper, the readback sees the pattern, and a window
// resize survives. It proves nothing about the hook ordering those three fight over.
//
//   spike-proxy-swapchain.exe [--frames N] [--bilinear]
//   F toggles EASU/bilinear, Esc quits. --frames N quits after N presents (for scripts).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include "feed_fsr1.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

#define CHECK(hr, what) \
    if (FAILED(hr)) { printf("SPIKE FAIL: %s -> 0x%08lX\n", what, (unsigned long)(hr)); return 1; }

template <typename T> static void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------
// The proxy. Everything the game does not need to lie about is forwarded verbatim.
// ---------------------------------------------------------------------------
class ProxySwapChain final : public IDXGISwapChain
{
public:
    ProxySwapChain(IDXGISwapChain *real, ID3D11Device *dev, UINT game_w, UINT game_h, DXGI_FORMAT fmt, HWND hwnd)
        : m_real(real), m_dev(dev), m_game_w(game_w), m_game_h(game_h), m_fmt(fmt), m_hwnd(hwnd)
    {
        m_real->AddRef();
        m_dev->AddRef();
        m_dev->GetImmediateContext(&m_ctx);
        m_ok = MakeShaders() && MakePrivate() && FitReal();
    }
    ~ProxySwapChain()
    {
        ReleasePrivate();
        SafeRelease(m_easu_ps); SafeRelease(m_blit_ps); SafeRelease(m_vs); SafeRelease(m_cb); SafeRelease(m_smp);
        SafeRelease(m_ctx); SafeRelease(m_dev); SafeRelease(m_real);
    }
    bool ok() const { return m_ok; }
    void set_easu(bool on) { m_easu = on; }
    bool easu() const { return m_easu; }
    UINT presents() const { return m_presents; }
    IDXGISwapChain *real() const { return m_real; }

    // IUnknown. The game only ever asks for IDXGISwapChain here; a shipping wrapper has to
    // answer IDXGISwapChain1..4 too (games call GetDesc1 / SetColorSpace1 / Present1).
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (out == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(IDXGISwapChain))
        { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // IDXGIObject / IDXGIDeviceSubObject: forwarded.
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void *d) override { return m_real->SetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown *u) override { return m_real->SetPrivateDataInterface(n, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT *s, void *d) override { return m_real->GetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **p) override { return m_real->GetParent(riid, p); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **p) override { return m_real->GetDevice(riid, p); }

    // IDXGISwapChain: the three lies, and the rest forwarded.
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID riid, void **out) override
    {
        if (buffer != 0 || m_private == nullptr) return DXGI_ERROR_INVALID_CALL;
        return m_private->QueryInterface(riid, out);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *desc) override
    {
        const HRESULT hr = m_real->GetDesc(desc);
        if (SUCCEEDED(hr)) { desc->BufferDesc.Width = m_game_w; desc->BufferDesc.Height = m_game_h; }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) override
    {
        // The game keeps its resolution (0 = keep, like DXGI); the window may have moved on.
        if (w != 0) m_game_w = w;
        if (h != 0) m_game_h = h;
        if (fmt != DXGI_FORMAT_UNKNOWN) m_fmt = fmt;
        (void)count; (void)flags;
        ReleasePrivate();
        if (!MakePrivate()) return E_FAIL;
        return FitReal() ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override
    {
        if (!Upscale()) return E_FAIL;
        const HRESULT hr = m_real->Present(sync, flags);
        if (SUCCEEDED(hr)) ++m_presents;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fs, IDXGIOutput *o) override { return m_real->SetFullscreenState(fs, o); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *fs, IDXGIOutput **o) override { return m_real->GetFullscreenState(fs, o); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *m) override { return m_real->ResizeTarget(m); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **o) override { return m_real->GetContainingOutput(o); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *s) override { return m_real->GetFrameStatistics(s); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *c) override { return m_real->GetLastPresentCount(c); }

private:
    bool MakeShaders()
    {
        static const char kVs[] =
            "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
            "VSOut vs(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
            "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
            "Texture2D<float4> src : register(t0); SamplerState smp : register(s0);\n"
            "float4 ps(VSOut i) : SV_Target { return float4(src.Sample(smp, i.uv).rgb, 1.0); }\n";
        ID3DBlob *vs = nullptr, *ps = nullptr, *easu = nullptr, *err = nullptr;
        HRESULT hr = D3DCompile(kVs, sizeof(kVs) - 1, "proxyblit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
        if (SUCCEEDED(hr)) hr = D3DCompile(kVs, sizeof(kVs) - 1, "proxyblit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
        if (SUCCEEDED(hr)) hr = D3DCompile(kFsr1Src, sizeof(kFsr1Src) - 1, "feedfsr1", nullptr, nullptr, "ps_easu", "ps_5_0", 0, 0, &easu, &err);
        if (FAILED(hr)) { printf("SPIKE FAIL: shader compile 0x%08lX %s\n", (unsigned long)hr, err ? (const char *)err->GetBufferPointer() : ""); SafeRelease(err); return false; }
        hr = m_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs);
        if (SUCCEEDED(hr)) hr = m_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_blit_ps);
        if (SUCCEEDED(hr)) hr = m_dev->CreatePixelShader(easu->GetBufferPointer(), easu->GetBufferSize(), nullptr, &m_easu_ps);
        SafeRelease(vs); SafeRelease(ps); SafeRelease(easu);
        if (FAILED(hr)) { printf("SPIKE FAIL: shader objects 0x%08lX\n", (unsigned long)hr); return false; }
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(m_dev->CreateSamplerState(&sd, &m_smp))) return false;
        D3D11_BUFFER_DESC cbd = {};
        cbd.ByteWidth = sizeof(FsrConstants);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(m_dev->CreateBuffer(&cbd, nullptr, &m_cb));
    }
    bool MakePrivate()
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = m_game_w; td.Height = m_game_h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = m_fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_private))) { printf("SPIKE FAIL: private backbuffer\n"); return false; }
        if (FAILED(m_dev->CreateShaderResourceView(m_private, nullptr, &m_private_srv))) { printf("SPIKE FAIL: private SRV\n"); return false; }
        return true;
    }
    void ReleasePrivate() { SafeRelease(m_private_srv); SafeRelease(m_private); SafeRelease(m_real_rtv); }
    // The real chain follows the window; the game never learns its size.
    bool FitReal()
    {
        RECT rc = {};
        GetClientRect(m_hwnd, &rc);
        const UINT w = rc.right > 0 ? static_cast<UINT>(rc.right) : 1, h = rc.bottom > 0 ? static_cast<UINT>(rc.bottom) : 1;
        SafeRelease(m_real_rtv);
        m_ctx->ClearState();   // the game's RTV on the OLD real backbuffer would block the resize
        const HRESULT hr = m_real->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) { printf("SPIKE FAIL: real ResizeBuffers %ux%u -> 0x%08lX\n", w, h, (unsigned long)hr); return false; }
        ID3D11Texture2D *bb = nullptr;
        if (FAILED(m_real->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb)))) return false;
        const HRESULT r2 = m_dev->CreateRenderTargetView(bb, nullptr, &m_real_rtv);
        bb->Release();
        m_real_w = w; m_real_h = h;
        printf("proxy: game %ux%u -> display %ux%u\n", m_game_w, m_game_h, m_real_w, m_real_h);
        return SUCCEEDED(r2);
    }
    bool Upscale()
    {
        if (m_real_rtv == nullptr || m_private_srv == nullptr) return false;
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        FsrFillConstants(static_cast<FsrConstants *>(mapped.pData), m_game_w, m_game_h, m_real_w, m_real_h, 0.0f);
        m_ctx->Unmap(m_cb, 0);

        // The game's state is whatever it left; a shipping wrapper saves and restores it.
        // Here the game re-binds everything each frame, so a ClearState is enough.
        m_ctx->ClearState();
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(m_real_w); vp.Height = static_cast<float>(m_real_h); vp.MaxDepth = 1.0f;
        m_ctx->RSSetViewports(1, &vp);
        m_ctx->OMSetRenderTargets(1, &m_real_rtv, nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_vs, nullptr, 0);
        m_ctx->PSSetShader(m_easu ? m_easu_ps : m_blit_ps, nullptr, 0);
        m_ctx->PSSetShaderResources(0, 1, &m_private_srv);
        m_ctx->PSSetSamplers(0, 1, &m_smp);
        m_ctx->PSSetConstantBuffers(0, 1, &m_cb);
        m_ctx->Draw(3, 0);
        ID3D11ShaderResourceView *none = nullptr;
        m_ctx->PSSetShaderResources(0, 1, &none);
        return true;
    }

    LONG m_ref = 1;
    IDXGISwapChain *m_real = nullptr;
    ID3D11Device *m_dev = nullptr;
    ID3D11DeviceContext *m_ctx = nullptr;
    HWND m_hwnd = nullptr;
    UINT m_game_w, m_game_h, m_real_w = 0, m_real_h = 0;
    DXGI_FORMAT m_fmt;
    ID3D11Texture2D *m_private = nullptr;
    ID3D11ShaderResourceView *m_private_srv = nullptr;
    ID3D11RenderTargetView *m_real_rtv = nullptr;
    ID3D11VertexShader *m_vs = nullptr;
    ID3D11PixelShader *m_blit_ps = nullptr, *m_easu_ps = nullptr;
    ID3D11SamplerState *m_smp = nullptr;
    ID3D11Buffer *m_cb = nullptr;
    bool m_ok = false, m_easu = true;
    UINT m_presents = 0;
};

// ---------------------------------------------------------------------------
// The "game": a fixed-resolution renderer that trusts GetBuffer/GetDesc.
// ---------------------------------------------------------------------------
static ProxySwapChain *g_proxy = nullptr;
static bool g_quit = false;
static bool g_resized = false;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_CLOSE: g_quit = true; return 0;
    case WM_SIZE: if (w != SIZE_MINIMIZED) g_resized = true; return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) g_quit = true;
        if (w == 'F' && g_proxy != nullptr) { g_proxy->set_easu(!g_proxy->easu()); printf("filter: %s\n", g_proxy->easu() ? "EASU" : "bilinear"); }
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
    UINT max_frames = 0;
    bool bilinear = false;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) max_frames = static_cast<UINT>(atoi(argv[++i]));
        else if (strcmp(argv[i], "--bilinear") == 0) bilinear = true;
    }
    const UINT kGameW = 960, kGameH = 540, kWinW = 1920, kWinH = 1080;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "dlss5-feed-proxy-spike";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT rc = { 0, 0, static_cast<LONG>(kWinW), static_cast<LONG>(kWinH) };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "proxy swapchain spike: 960x540 game, 1920x1080 display (F: filter, Esc: quit)",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) { printf("SPIKE FAIL: window\n"); return 1; }

    // The game asks for ITS resolution. The proxy quietly creates the real chain at the
    // window's size instead and keeps a private texture at the requested one.
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = kGameW;
    sd.BufferDesc.Height = kGameH;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    IDXGISwapChain *real = nullptr;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                                               &sd, &real, &dev, nullptr, &ctx);
    CHECK(hr, "D3D11CreateDeviceAndSwapChain");
    g_proxy = new ProxySwapChain(real, dev, kGameW, kGameH, sd.BufferDesc.Format, hwnd);
    real->Release();   // the proxy holds its own reference
    if (!g_proxy->ok()) { printf("SPIKE FAIL: proxy setup\n"); return 1; }
    g_proxy->set_easu(!bilinear);
    IDXGISwapChain *swapchain = g_proxy;   // from here on the game only knows this

    // Game-side shader: a checkerboard with thin diagonal lines and a moving disc --
    // exactly the content where bilinear vs EASU shows.
    static const char kGame[] =
        "cbuffer C : register(b0) { float t; float3 _p; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(VSOut i) : SV_Target {\n"
        "  float2 p = i.uv * float2(960, 540);\n"
        "  float chk = fmod(floor(p.x / 24) + floor(p.y / 24), 2);\n"
        "  float3 c = lerp(float3(0.15, 0.17, 0.2), float3(0.85, 0.85, 0.8), chk);\n"
        "  float d = abs(fmod(p.x + p.y + t * 60, 40) - 20);\n"
        "  c = lerp(c, float3(1, 0.3, 0.1), d < 1.0);\n"
        "  float2 q = p - float2(480 + 300 * sin(t), 270 + 150 * cos(t * 0.7));\n"
        "  c = lerp(c, float3(0.2, 0.9, 0.3), length(q) < 40);\n"
        "  return float4(c, 1); }\n";
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    hr = D3DCompile(kGame, sizeof(kGame) - 1, "game", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
    if (SUCCEEDED(hr)) hr = D3DCompile(kGame, sizeof(kGame) - 1, "game", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) { printf("SPIKE FAIL: game shader %s\n", err ? (const char *)err->GetBufferPointer() : ""); return 1; }
    ID3D11VertexShader *gvs = nullptr; ID3D11PixelShader *gps = nullptr;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &gvs);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &gps);
    vs->Release(); ps->Release();
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DYNAMIC; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *gcb = nullptr;
    CHECK(dev->CreateBuffer(&cbd, nullptr, &gcb), "game cb");

    // A readback of the REAL backbuffer's centre proves the pattern crossed the wrapper.
    D3D11_TEXTURE2D_DESC rbd = {};
    rbd.Width = 1; rbd.Height = 1; rbd.MipLevels = 1; rbd.ArraySize = 1;
    rbd.Format = sd.BufferDesc.Format; rbd.SampleDesc.Count = 1;
    rbd.Usage = D3D11_USAGE_STAGING; rbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *readback = nullptr;
    CHECK(dev->CreateTexture2D(&rbd, nullptr, &readback), "readback");

    LARGE_INTEGER qpf, t0; QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&t0);
    UINT frames = 0; bool seen_pattern = false; bool resized_ok = false;
    while (!g_quit)
    {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (g_quit) break;
        if (g_resized)
        {
            // A game does this on WM_SIZE: it keeps its own resolution, the proxy re-fits the display.
            g_resized = false;
            hr = swapchain->ResizeBuffers(0, kGameW, kGameH, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr)) { printf("SPIKE FAIL: ResizeBuffers through the proxy 0x%08lX\n", (unsigned long)hr); return 1; }
            resized_ok = true;
        }

        // The game renders into the buffer the (proxy) swapchain hands it, at the size GetDesc reports.
        DXGI_SWAP_CHAIN_DESC seen = {};
        swapchain->GetDesc(&seen);
        ID3D11Texture2D *bb = nullptr;
        CHECK(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb)), "GetBuffer through the proxy");
        ID3D11RenderTargetView *rtv = nullptr;
        CHECK(dev->CreateRenderTargetView(bb, nullptr, &rtv), "game RTV");
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const float t = static_cast<float>(now.QuadPart - t0.QuadPart) / static_cast<float>(qpf.QuadPart);
        D3D11_MAPPED_SUBRESOURCE m = {};
        ctx->Map(gcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, &t, sizeof(t));
        ctx->Unmap(gcb, 0);
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(seen.BufferDesc.Width); vp.Height = static_cast<float>(seen.BufferDesc.Height); vp.MaxDepth = 1.0f;
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(gvs, nullptr, 0);
        ctx->PSSetShader(gps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &gcb);
        ctx->Draw(3, 0);
        rtv->Release(); bb->Release();

        hr = swapchain->Present(1, 0);
        CHECK(hr, "Present through the proxy");
        ++frames;

        if (frames == 30 || frames == 60)
        {
            ID3D11Texture2D *realbb = nullptr;
            if (SUCCEEDED(g_proxy->real()->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&realbb))))
            {
                D3D11_TEXTURE2D_DESC d = {}; realbb->GetDesc(&d);
                D3D11_BOX box = { d.Width / 2, d.Height / 2, 0, d.Width / 2 + 1, d.Height / 2 + 1, 1 };
                ctx->CopySubresourceRegion(readback, 0, 0, 0, 0, realbb, 0, &box);
                realbb->Release();
                D3D11_MAPPED_SUBRESOURCE rm = {};
                if (SUCCEEDED(ctx->Map(readback, 0, D3D11_MAP_READ, 0, &rm)))
                {
                    const BYTE *px = static_cast<const BYTE *>(rm.pData);
                    printf("frame %u: real backbuffer %ux%u, centre pixel %u,%u,%u (game believes %ux%u)\n", frames, d.Width, d.Height,
                           px[0], px[1], px[2], seen.BufferDesc.Width, seen.BufferDesc.Height);
                    if (px[0] + px[1] + px[2] > 0) seen_pattern = true;
                    ctx->Unmap(readback, 0);
                }
            }
            if (frames == 30) { SetWindowPos(hwnd, nullptr, 0, 0, 1600, 900, SWP_NOMOVE | SWP_NOZORDER); }   // forces the resize path
        }
        if (max_frames != 0 && frames >= max_frames) break;
    }

    const bool pass = seen_pattern && (max_frames == 0 || max_frames < 31 || resized_ok);
    printf("%s: %u frames through the proxy (%u presented), filter %s, resize %s, pattern %s\n",
           pass ? "PASS" : "FAIL", frames, g_proxy->presents(), g_proxy->easu() ? "EASU" : "bilinear",
           resized_ok ? "ok" : "not exercised", seen_pattern ? "seen" : "NOT seen");

    ctx->ClearState();
    SafeRelease(readback); SafeRelease(gcb); SafeRelease(gvs); SafeRelease(gps);
    swapchain->Release(); g_proxy = nullptr;
    SafeRelease(ctx); SafeRelease(dev);
    DestroyWindow(hwnd);
    return pass ? 0 : 1;
}
