// ovlab -- a 32-bit Direct3D 11 "old game" for trying the in-game panel without a game.
//
// It never shows a window. It renders a moving scene, runs a script of key presses, mouse
// moves and clicks against its own window, and saves what was PRESENTED (ReShade's drawing
// included) as .bmp files. ReShade x86 + dlss5-feed.addon32 load into it the usual way
// (dxgi.dll beside the exe), and the add-on starts host64\dlss5-feed-host64.exe by itself.
//
//   ovlab --size 1280x720 --fps 60 --frames 1500 --out <dir> --script <file>
//
// Script: one action per line, "<frame> <verb> [args]"; '#' starts a comment.
//   120 key 114          press and release a virtual key (down at 120, up two frames later)
//   200 mouse 900 300    put the pointer at client 900,300
//   210 glide 1000 420 30   move it there over 30 frames
//   260 down | 262 up    left button
//   300 wheel -2         wheel notches
//   320 shot name        save the picture presented at frame 320 as <out>\name.bmp
//
// The pointer. ReShade takes the mouse position of EVERY input message from MSG.pt -- the
// real cursor at the moment the message was posted -- and not from lParam, so a posted
// WM_MOUSEMOVE cannot place it. Moving the real cursor would take the user's mouse away.
// Instead this window, which nobody can see, is moved each frame so that the real cursor
// happens to be over the client point the script wants.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Action { int frame; std::string verb; std::string a[3]; };

static HWND                    g_hwnd;
static ID3D11Device           *g_dev;
static ID3D11DeviceContext    *g_ctx;
static IDXGISwapChain1        *g_swap;
static ID3D11VertexShader     *g_vs;
static ID3D11PixelShader      *g_ps;
static ID3D11Buffer           *g_cb;
static ID3D11Texture2D        *g_staging;
static int                     g_w = 1280, g_h = 720;
static FILE                   *g_log;

static void Say(const char *fmt, ...)
{
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    printf("%s\n", b);
    if (g_log) { fprintf(g_log, "%s\n", b); fflush(g_log); }
}

#include "ovlab_scene.h"   // kShader: the picture

static bool Fail(const char *what, HRESULT hr) { Say("ovlab: %s failed 0x%08lX", what, static_cast<unsigned long>(hr)); return false; }

static bool InitGraphics()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ovlab";
    RegisterClassW(&wc);
    // A popup with no frame, so the client area IS the window; a tool window that is never
    // activated and never shown.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"ovlab", L"ovlab", WS_POPUP, 0, 0, g_w, g_h,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (g_hwnd == nullptr) return Fail("CreateWindow", HRESULT_FROM_WIN32(GetLastError()));

    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &want, 1, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) return Fail("D3D11CreateDevice", hr);
    IDXGIDevice *dd = nullptr; IDXGIAdapter *ad = nullptr; IDXGIFactory2 *fa = nullptr;
    g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dd));
    dd->GetAdapter(&ad);
    ad->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&fa));
    DXGI_ADAPTER_DESC adesc = {}; ad->GetDesc(&adesc);
    Say("ovlab: adapter %ls", adesc.Description);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = g_w; sd.Height = g_h; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT; sd.BufferCount = 2;
    // FLIP_SEQUENTIAL keeps a presented buffer's contents until it comes round again, which is
    // what lets the picture -- ReShade's overlay drawing included -- be read back afterwards.
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.Scaling = DXGI_SCALING_STRETCH; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = fa->CreateSwapChainForHwnd(g_dev, g_hwnd, &sd, nullptr, nullptr, &g_swap);
    if (FAILED(hr)) return Fail("CreateSwapChainForHwnd", hr);
    fa->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    fa->Release(); ad->Release(); dd->Release();

    ID3DBlob *v = nullptr, *p = nullptr, *e = nullptr;
    hr = D3DCompile(kShader, strlen(kShader), "ovlab", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &v, &e);
    if (FAILED(hr)) { Say("%s", e ? static_cast<const char *>(e->GetBufferPointer()) : "?"); return Fail("vs", hr); }
    hr = D3DCompile(kShader, strlen(kShader), "ovlab", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &p, &e);
    if (FAILED(hr)) { Say("%s", e ? static_cast<const char *>(e->GetBufferPointer()) : "?"); return Fail("ps", hr); }
    g_dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &g_ps);
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_w; td.Height = g_h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_dev->CreateTexture2D(&td, nullptr, &g_staging);
    return g_vs && g_ps && g_cb && g_staging;
}

// Buffer 0 right now is the one presented two presents ago, untouched since.
static void SaveShot(const std::string &path)
{
    ID3D11Texture2D *bb = nullptr;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb)))) return;
    g_ctx->CopyResource(g_staging, bb);
    bb->Release();
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(g_ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &m))) return;
    const int stride = (g_w * 3 + 3) & ~3;
    std::vector<BYTE> img(static_cast<size_t>(stride) * g_h);
    for (int y = 0; y < g_h; ++y)
    {
        const BYTE *s = static_cast<const BYTE *>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        BYTE *d = img.data() + static_cast<size_t>(g_h - 1 - y) * stride;   // .bmp rows run bottom-up
        for (int x = 0; x < g_w; ++x) { d[x * 3] = s[x * 4 + 2]; d[x * 3 + 1] = s[x * 4 + 1]; d[x * 3 + 2] = s[x * 4]; }
    }
    g_ctx->Unmap(g_staging, 0);
    BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + static_cast<DWORD>(img.size());
    ih.biSize = sizeof(ih); ih.biWidth = g_w; ih.biHeight = g_h; ih.biPlanes = 1; ih.biBitCount = 24;
    FILE *f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr) { Say("ovlab: cannot write %s", path.c_str()); return; }
    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(img.data(), 1, img.size(), f);
    fclose(f);
    Say("ovlab: saved %s", path.c_str());
}

int main(int argc, char **argv)
{
    int frames = 1200; double fps = 60.0;
    std::string out = ".", script;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const char *n = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--size") { sscanf_s(n, "%dx%d", &g_w, &g_h); ++i; }
        else if (a == "--frames") { frames = atoi(n); ++i; }
        else if (a == "--fps") { fps = atof(n); ++i; }
        else if (a == "--out") { out = n; ++i; }
        else if (a == "--script") { script = n; ++i; }
    }
    fopen_s(&g_log, (out + "\\ovlab.log").c_str(), "w");

    std::vector<Action> acts;
    if (!script.empty())
    {
        FILE *f = nullptr;
        if (fopen_s(&f, script.c_str(), "r") == 0 && f != nullptr)
        {
            char line[256];
            while (fgets(line, sizeof(line), f))
            {
                if (char *c = strchr(line, '#')) *c = 0;
                Action ac = {}; char verb[32] = "", a0[64] = "", a1[64] = "", a2[64] = "";
                const int n = sscanf_s(line, "%d %31s %63s %63s %63s", &ac.frame, verb, 32u, a0, 64u, a1, 64u, a2, 64u);
                if (n < 2) continue;
                ac.verb = verb; ac.a[0] = a0; ac.a[1] = a1; ac.a[2] = a2;
                acts.push_back(ac);
            }
            fclose(f);
        }
    }
    Say("ovlab: %dx%d, %d frames at %.0f fps, %zu scripted actions", g_w, g_h, frames, fps, acts.size());
    if (!InitGraphics()) return 2;
    timeBeginPeriod(1);

    bool   have_mouse = false, lbutton = false, frozen = false;
    double frozen_t = 0;
    double mx = 0, my = 0, gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0; int glide_from = 0, glide_len = 0;
    std::vector<std::pair<int, UINT>> keyups;       // frame, vk
    std::vector<std::pair<int, std::string>> shots; // frame the picture was presented at, name
    LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);

    for (int frame = 0; frame < frames; ++frame)
    {
        // The picture presented at frame-2 is in buffer 0 now.
        for (auto &s : shots) if (s.first == frame - 2) SaveShot(out + "\\" + s.second + ".bmp");

        for (const Action &ac : acts)
        {
            if (ac.frame != frame) continue;
            if (ac.verb == "key")
            {
                const UINT vk = static_cast<UINT>(atoi(ac.a[0].c_str())), sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
                PostMessageW(g_hwnd, WM_KEYDOWN, vk, 1 | (sc << 16));
                keyups.emplace_back(frame + 2, vk);
                Say("ovlab: frame %d key %u", frame, vk);
            }
            else if (ac.verb == "mouse") { mx = atof(ac.a[0].c_str()); my = atof(ac.a[1].c_str()); have_mouse = true; glide_len = 0; Say("ovlab: frame %d mouse %.0f,%.0f", frame, mx, my); }
            else if (ac.verb == "glide") { gx0 = mx; gy0 = my; gx1 = atof(ac.a[0].c_str()); gy1 = atof(ac.a[1].c_str()); glide_from = frame; glide_len = atoi(ac.a[2].c_str()); have_mouse = true; }
            else if (ac.verb == "down") { lbutton = true;  PostMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<int>(mx), static_cast<int>(my))); Say("ovlab: frame %d button down at %.0f,%.0f", frame, mx, my); }
            else if (ac.verb == "up")   { lbutton = false; PostMessageW(g_hwnd, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<int>(mx), static_cast<int>(my))); Say("ovlab: frame %d button up", frame); }
            else if (ac.verb == "wheel") { POINT c; GetCursorPos(&c); PostMessageW(g_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, atoi(ac.a[0].c_str()) * WHEEL_DELTA), MAKELPARAM(c.x, c.y)); }
            else if (ac.verb == "shot") shots.emplace_back(frame, ac.a[0]);
            else if (ac.verb == "freeze") { frozen = true; frozen_t = frame / fps; Say("ovlab: frame %d scene frozen", frame); }   // the camera stops: later shots show the same view
        }
        for (auto &k : keyups)
            if (k.first == frame) PostMessageW(g_hwnd, WM_KEYUP, k.second, 1 | (MapVirtualKeyW(k.second, MAPVK_VK_TO_VSC) << 16) | (1u << 30) | (1u << 31));
        if (glide_len > 0)
        {
            double u = static_cast<double>(frame - glide_from) / glide_len; if (u >= 1.0) { u = 1.0; glide_len = 0; }
            mx = gx0 + (gx1 - gx0) * u; my = gy0 + (gy1 - gy0) * u;
        }
        if (have_mouse)
        {
            // Put this (unseen) window where the real cursor is over client mx,my, then post a
            // move so ReShade takes the position: it reads MSG.pt, which is the real cursor.
            POINT c; GetCursorPos(&c);
            SetWindowPos(g_hwnd, nullptr, c.x - static_cast<int>(mx), c.y - static_cast<int>(my), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            PostMessageW(g_hwnd, WM_MOUSEMOVE, lbutton ? MK_LBUTTON : 0, MAKELPARAM(static_cast<int>(mx), static_cast<int>(my)));
            // ReShade drops a posted WM_MOUSEMOVE while an overlay blocks the mouse ("no mouse
            // movement simulation", its PostMessage hook), which is exactly when the panel is
            // up. A wheel message of zero notches carries the same MSG.pt and is let through.
            PostMessageW(g_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(lbutton ? MK_LBUTTON : 0, 0), MAKELPARAM(c.x, c.y));
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            static int said = 0;
            if ((msg.message == WM_MOUSEMOVE || msg.message == WM_NULL) && said < 6)
            {
                ++said;
                POINT c; GetCursorPos(&c); RECT wr; GetWindowRect(g_hwnd, &wr);
                Say("ovlab: frame %d got message 0x%X pt=%ld,%ld lParam=%d,%d; real cursor %ld,%ld; window at %ld,%ld",
                    frame, msg.message, msg.pt.x, msg.pt.y, static_cast<short>(LOWORD(msg.lParam)), static_cast<short>(HIWORD(msg.lParam)),
                    c.x, c.y, wr.left, wr.top);
            }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }

        ID3D11Texture2D *bb = nullptr; ID3D11RenderTargetView *rtv = nullptr;
        g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb));
        g_dev->CreateRenderTargetView(bb, nullptr, &rtv);
        const float c[4] = { static_cast<float>(frozen ? frozen_t : frame / fps), static_cast<float>(g_w), static_cast<float>(g_h), 0.0f };
        g_ctx->UpdateSubresource(g_cb, 0, nullptr, c, 0, 0);
        D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetShader(g_ps, nullptr, 0);
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
        g_ctx->Draw(3, 0);
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        rtv->Release(); bb->Release();
        const HRESULT hr = g_swap->Present(0, 0);
        if (FAILED(hr)) { Fail("Present", hr); break; }

        if (fps > 0)
        {
            const double due = (frame + 1) / fps;
            for (;;)
            {
                LARGE_INTEGER q; QueryPerformanceCounter(&q);
                const double now = static_cast<double>(q.QuadPart - q0.QuadPart) / qf.QuadPart;
                if (now >= due) break;
                if (due - now > 0.002) Sleep(1);
            }
        }
        if (frame % 300 == 0) Say("ovlab: frame %d", frame);
    }
    Say("ovlab: done");
    return 0;
}
