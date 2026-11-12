// fxlab -- a synthetic "capture feed" for working on DLSS5_Feed.fx without a game or a capture card.
//
// A D3D11 window that draws a console-style UI (sharp static text over slowly moving translucent
// waves, a sliding tile row, a scrolling list, tickers) next to hard test patterns (zone plates,
// 1 px checkers, moving textures), exactly the same picture for a given frame number on every
// run. Put ReShade's dxgi.dll, dlss5-feed.addon64, nvngx_dlss.dll and the shaders next to it
// (tools\fx-testbed\Make-Rig.ps1 does that) and the whole real chain runs on it: provider ->
// DLSS5_Feed.fx -> the add-on's DLAA evaluate -> back into the frame.
//
// What it measures. The feed is handed a FINISHED picture with no jitter, so there is nothing
// for temporal accumulation to add: the best possible DLAA output is the input. fxlab keeps
// the frame it drew, reads the swap chain back after ReShade and the add-on are done with it,
// and reports per probe rectangle how far the two are apart (mean absolute error, PSNR) and
// how much edge energy survived ("sharp": 1.00 = as crisp as the input, 0.70 = smeared).
// Side-by-side crops (input | output | difference x8) are written as PNG to look at.
//
//   fxlab.exe [--scene ui|3d] [--size WxH] [--frames N] [--capture a,b,c] [--out dir] [--tag name]
//             [--show] [--loop] [--lat N] [--full] [--fmt 8|10] [--preroll seconds] [--fps N]
//             [--noise N] [--mv] [--hdr] [--hold A,B] [--cadence N] [--oracle]
//
// --cadence 2 is a 30 fps game through a 60 Hz capture: every picture is presented twice, and
// what moves moves twice as far when it does. Frame numbers on the command line and in the
// results stay PRESENT numbers; an odd one is then the second showing of a picture, for which
// the only right output is the same picture again.
// --oracle stamps the scene frame number being presented, and the previous one, into the first
// two pixels; fxlab_Oracle.fx turns them into exact motion vectors for the 3D scene.
//
// --hold A,B makes the scene move for A presents and stand still for B, over and over: a game
// that is paused and resumed, a menu that opens and closes. Every change between the two is
// written with its wall-clock time to <out>\<tag>.events.txt, to be laid next to the add-on's
// log (some failures in the chain happen exactly there).
//
// --hdr presents HDR10: the same picture converted to PQ / BT.2020 at a paper white of 203 nits
// with highlights up to ~600, in a 10-bit swap chain tagged ST.2084. That is what a capture
// preview hands ReShade on an HDR desktop, and the add-on treats it differently from SDR (its
// PQ bridge decodes it to linear light for DLSS). All comparisons are then made on PQ code
// values, which are perceptually spaced like SDR ones.
//
// --scene 3d is a small 3D courtyard with a depth buffer, a game HUD and exact per-pixel motion
// vectors to score against (fxlab3d.h). --loop --show runs either scene in a visible window
// until Esc, for looking at it: Space freezes the scene (a still frame is the easiest thing to
// judge a temporal chain on), and ReShade's overlay works as in a game.
//
// The read-back relies on DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL keeping buffer contents: with two
// buffers, the buffer about to be drawn for frame f still holds the finished frame f-2.
// Run it once with no ReShade next to it: every probe must read mae 0.00.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static void Die(const char *what, HRESULT hr = 0)
{
    fprintf(stderr, "fxlab: %s (0x%08lX)\n", what, static_cast<unsigned long>(hr));
    ExitProcess(2);
}
#define CHECK(expr, what) do { HRESULT hr_ = (expr); if (FAILED(hr_)) Die(what, hr_); } while (0)

// ---------------------------------------------------------------------------------------------
// Layout, in base units of a 3840x2160 frame (everything scales with the window).
// ---------------------------------------------------------------------------------------------

// truth: what the motion vector of a TEXTURED pixel in the probe should be (pixels, pointing from
// the pixel to where it was a frame ago): 0 unknown, 1 zero, 2 (vx, vy) in base units, 3 (vx, vy)
// in real pixels, 4 the carousel's step this frame.
struct Probe { const char *name; float x0, y0, x1, y1; const char *kind; int truth; float vx, vy; };

// kind: "static" = these pixels are identical every frame; "moving" = they move; "mixed" = a
// static element drawn over something that moves.
static const Probe kProbes[] = {
    { "menu.title",     100,   60, 1500,  270, "mixed", 1, 0.0f, 0.0f },   // 64 px + 36 px text over the waves
    { "menu.body",      100,  300, 1180,  600, "mixed", 1, 0.0f, 0.0f },   // 28 px paragraph over the waves
    { "menu.small",     100,  610, 1180,  880, "mixed", 1, 0.0f, 0.0f },   // 20 px paragraph over the waves
    { "menu.tiny",      100,  890, 1180, 1100, "mixed", 1, 0.0f, 0.0f },   // 14 px paragraph over the waves
    { "menu.list",     1280,  300, 2300,  720, "mixed", 1, 0.0f, 0.0f },   // 30 px list + 1 px frame
    { "menu.waves",    1300,  800, 2300, 1250, "moving", 0, 0.0f, 0.0f },   // the translucent background alone
    { "tiles.caption",  100, 1320, 1300, 1400, "mixed", 1, 0.0f, 0.0f },
    { "tiles.row",        0, 1410, 2400, 1910, "moving", 4, 0.0f, 0.0f },   // slides ~27 px/frame, then rests
    { "tiles.help",     100, 2040, 1500, 2110, "mixed", 1, 0.0f, 0.0f },
    { "scroll.header", 2420,   10, 3100,  110, "static", 1, 0.0f, 0.0f },
    { "scroll.body",   2420,  160, 3100,  900, "moving", 2, 0.0f, 2.5f },   // 2.5 px/frame up
    { "scroll.overlay",2420,  925, 3100, 1005, "mixed", 0, 0.0f, 0.0f },   // static text over the scrolling text
    { "ticker.fast",   3130,   90, 3830,  200, "moving", 2, 4.0f, 0.0f },   // 4 px/frame
    { "ticker.slow",   3130,  270, 3830,  360, "moving", 2, -0.75f, 0.0f },   // 0.75 px/frame
    { "ticker.static", 3130,  410, 3830,  490, "static", 1, 0.0f, 0.0f },
    { "zone.static",   3120,  540, 3840, 1080, "static", 1, 0.0f, 0.0f },   // zone plate, 1 px checker, 1 px lines
    { "zone.moving",   2400, 1080, 3120, 1620, "moving", 3, -1.3f, -0.7f },   // zone plate at (1.3, 0.7) px/frame
    { "tex.noise",     2400, 1620, 2760, 2160, "moving", 3, -2.0f, -1.0f },   // smooth noise at (2, 1) px/frame
    { "tex.checker",   2760, 1620, 3120, 2160, "moving", 3, 1.0f, 0.0f },     // 4 px checker at (-1, 0), hard edges
    { "text.flat",     3120, 1080, 3840, 1620, "static", 1, 0.0f, 0.0f },   // dense text, nothing moves anywhere near
    { "slow.blobs",    3120, 1620, 3840, 2160, "moving", 0, 0.0f, 0.0f },   // 0.2-0.6 px/frame translucent shapes
};

static const char kBackgroundHlsl[] = R"(
cbuffer C : register(b0) { float T; float S; float W; float H; };
static const float PI = 3.14159265;
// One draw adds 0..2a of per-pixel, per-frame noise, a second subtracts a: +-a on the picture.
float4 noise_add(float4 pos : SV_Position) : SV_Target { float n = frac(sin(dot(pos.xy + T * float2(17.13, 31.71), float2(12.9898, 78.233))) * 43758.5453); return float4((2.0 * n * H / 255.0).xxx, 1.0); }
float4 noise_sub(float4 pos : SV_Position) : SV_Target { return float4((H / 255.0).xxx, 1.0); }
float h1(float n) { return frac(sin(n * 127.1) * 43758.5453); }
float h2(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float vnoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(h2(i), h2(i + float2(1, 0)), f.x), lerp(h2(i + float2(0, 1)), h2(i + float2(1, 1)), f.x), f.y);
}
// The console home screen: a deep gradient, soft ribbons with a thin bright crest that sway
// slowly, and small drifting motes. Everything is additive and low contrast.
float3 waves(float2 p, float t)
{
    float3 col = lerp(float3(0.015, 0.04, 0.13), float3(0.05, 0.12, 0.32), saturate(p.y / 2160.0));
    for (int k = 0; k < 6; ++k)
    {
        float fk = k;
        float yc = 250.0 + 330.0 * fk
                 + 150.0 * sin(p.x * 0.0017 * (1.0 + 0.21 * fk) + t * 0.006 * (1.0 + 0.35 * fk) + fk * 1.7)
                 +  70.0 * sin(p.x * 0.0043 - t * 0.004 * (fk + 1.0));
        float d  = p.y - yc;
        float sg = 70.0 + 22.0 * fk;
        col += float3(0.07, 0.11, 0.22) * exp(-d * d / (2.0 * sg * sg));
        col += float3(0.16, 0.20, 0.30) * saturate(1.0 - abs(d) / 2.5);
    }
    for (int i = 0; i < 48; ++i)
    {
        float fi = i;
        float2 v = float2(0.3 + 1.2 * h1(fi + 3.0), -0.15 - 0.5 * h1(fi + 7.0));
        float2 c = frac(float2(h1(fi), h1(fi + 11.0)) + v * t / float2(2400.0, 2160.0)) * float2(2400.0, 2160.0);
        float r  = 3.0 + 9.0 * h1(fi + 5.0);
        float2 dd = p - c;
        col += float3(0.25, 0.30, 0.40) * h1(fi + 13.0) * exp(-dot(dd, dd) / (r * r));
    }
    return col;
}
// Zone plate in REAL pixels: spatial frequency rises linearly to 0.35 cycles/px at radius R.
float zone(float2 q, float R) { return 0.5 + 0.5 * cos(PI * dot(q, q) / (2.0 * R) * 0.7); }

float4 main(float4 pos : SV_Position) : SV_Target
{
    float2 px = pos.xy;        // real pixels
    float2 p  = px / S;        // base units
    float3 col;
    if (p.x < 2400.0)
    {
        col = waves(p, T);                                                // menu + tile row
    }
    else if (p.x < 3120.0 && p.y < 1080.0)
    {
        col = float3(0.07, 0.07, 0.08);                                   // scrolling list
    }
    else if (p.x >= 3120.0 && p.y < 540.0)
    {
        col = float3(0.04, 0.04, 0.05);                                   // tickers
    }
    else if (p.x >= 3120.0 && p.y < 1080.0)
    {
        float2 o = float2(3120.0, 540.0) * S;                             // static hard patterns
        float2 q = px - o;
        float half_w = 360.0 * S;
        if (q.x < half_w) col = zone(q - float2(half_w * 0.5, 270.0 * S), 270.0 * S);
        else
        {
            float2 r = q - float2(half_w, 0.0);
            float band = floor(r.y / (135.0 * S));
            float2 ip = floor(r);
            if      (band < 1.0) col = fmod(ip.x + ip.y, 2.0);                       // 1 px checker
            else if (band < 2.0) col = fmod(floor(ip.x / 2.0) + floor(ip.y / 2.0), 2.0); // 2 px checker
            else if (band < 3.0) col = fmod(ip.x, 2.0);                               // 1 px vertical lines
            else                 col = fmod(ip.y, 3.0) < 1.0 ? 1.0 : 0.0;             // 1 px lines, pitch 3
            col = 0.1 + 0.8 * col;
        }
    }
    else if (p.x < 3120.0)
    {
        float2 o = float2(2400.0, 1080.0) * S;
        float2 q = px - o;
        if (p.y < 1620.0)
        {
            // zone plate moving at (1.3, 0.7) real px/frame, wrapping inside its tile
            float2 size = float2(720.0, 540.0) * S;
            float2 c = fmod(float2(360.0, 270.0) * S + float2(1.3, 0.7) * T, size);
            float2 d = q - c;
            d -= size * round(d / size);
            col = zone(d, 300.0 * S);
        }
        else
        {
            float2 r = q - float2(0.0, 540.0 * S);
            if (r.x < 360.0 * S)
            {
                float2 m = (r - float2(2.0, 1.0) * T) / (6.0 * S);        // smooth noise at (2, 1) px/frame
                col = 0.15 + 0.7 * (0.6 * vnoise(m) + 0.4 * vnoise(m * 3.1));
            }
            else
            {
                float2 m = floor((r - float2(-1.0, 0.0) * T) / 4.0);      // 4 px checker at (-1, 0)
                col = 0.15 + 0.6 * fmod(abs(m.x + m.y), 2.0);
            }
        }
    }
    else if (p.y < 1620.0)
    {
        col = p.y < 1350.0 ? float3(0.42, 0.42, 0.42) : float3(0.0, 0.0, 0.0);   // dense static text
    }
    else
    {
        // slow translucent shapes: 0.2-0.6 px/frame, low contrast
        float2 q = p - float2(3120.0, 1620.0);
        col = float3(0.09, 0.10, 0.12);
        for (int i = 0; i < 4; ++i)
        {
            float fi = i;
            float2 v = float2(0.2 + 0.4 * h1(fi + 21.0), 0.1 + 0.3 * h1(fi + 23.0)) * (fmod(fi, 2.0) < 1.0 ? 1.0 : -1.0);
            float2 c = float2(360.0, 270.0) + float2(220.0 * sin(fi * 2.1), 150.0 * cos(fi * 1.3)) + v * fmod(T, 600.0) * 0.5;
            float2 d = q - c;
            col += float3(0.10, 0.07 + 0.03 * fi, 0.14) * exp(-dot(d, d) / (2.0 * 110.0 * 110.0));
        }
        col += 0.035 * sin((q.x + q.y) * 0.05 + T * 0.02);                 // 125 px stripes at ~0.4 px/frame
        col += 0.05 * saturate(1.0 - abs(frac((q.x - 0.3 * T) / 90.0) - 0.5) * 90.0 / 1.5); // thin lines at 0.3 px/frame
    }
    return float4(saturate(col), 1.0);
}
)";

static const char kBlitHlsl[] = R"(
Texture2D<float4> src : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 ps(float4 pos : SV_Position) : SV_Target { return float4(src.Load(int3(pos.xy, 0)).rgb, 1.0); }
// The SDR picture as HDR10: display gamma off, paper white at 203 nits, the top of the range
// lifted to ~600 nits (so there is something above paper white), BT.709 -> BT.2020, ST.2084.
// LogoRect (pixels) / LogoNits: inside the rectangle the picture is taken as linear light with
// 1.0 at LogoNits -- a logo screen mastered in HDR, whose white is far above paper white.
cbuffer P : register(b1) { float4 LogoRect; float LogoNits; float3 PadP; };
float4 ps_pq(float4 pos : SV_Position) : SV_Target
{
    float3 lin = pow(saturate(src.Load(int3(pos.xy, 0)).rgb), 2.2);
    float  l   = dot(lin, float3(0.2126, 0.7152, 0.0722));
    if (LogoNits > 0.0 && all(pos.xy >= LogoRect.xy) && all(pos.xy < LogoRect.zw)) lin *= LogoNits;
    else lin *= 203.0 * (1.0 + 2.0 * l * l * l * l);
    float3 c = float3(dot(lin, float3(0.6274, 0.3293, 0.0433)), dot(lin, float3(0.0691, 0.9195, 0.0114)), dot(lin, float3(0.0164, 0.0880, 0.8956)));
    float3 y = pow(saturate(c / 10000.0), 0.1593017578125);
    return float4(pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375), 1.0);
}
)";

// ---------------------------------------------------------------------------------------------

struct App
{
    int W = 3840, H = 2160;
    float S = 1.0f;
    int frames = 560;
    int latency = 2;
    bool show = false, full = false;
    bool scene3d = false;   // --scene 3d
    bool hdr = false;       // --hdr: HDR10 (PQ, BT.2020) instead of SDR
    int  cadence = 1;       // --cadence N: a new scene frame every N presents, the same one in between (2 = a 30 fps game at 60)
    bool oracle = false;    // --oracle: stamp the scene frame numbers into the first two pixels, for fxlab_Oracle.fx
    int  hold_run = 0, hold_still = 0;   // --hold A,B: the scene moves for A presents, then stands still for B, over and over
    bool loop = false;      // run until Esc instead of for g.frames
    bool paused = false;    // Space, in --loop: the frame number stands still
    int  fade = 0;          // the whole picture fades down and up again, once every this many frames (a logo fading in, a scene change)
    int  flash = 0;         // the picture is at 45% and jumps to full for three frames, every this many frames (lightning)
    int  logo = 0;          // 3D scene: a still studio logo (grey plate, black mark, lettering) whose brightness drifts a little
    bool fx = false;        // 3D scene: a flame, smoke, mist and heat haze (scored apart as 3d.fx.*)
    int  depth_flicker = 0; // 3D scene: the depth buffer is empty every Nth frame (a buffer that does not hold)
    bool nodepth = false;   // 3D scene: leave ReShade no depth buffer to read (what a capture is like)
    bool mesh = false;      // 3D scene: a perforated-metal floor panel in front of the camera (fine repeating dots)
    bool mvdump = false;    // the read-back is DLSS5_Feed_Debug view 10 (vectors), not a picture
    double preroll = 0.0;   // seconds of frame 0 before the clock starts (ReShade compiles its effects meanwhile)
    double fps = 0.0;       // 0 = as fast as it goes
    float noise = 0.0f;     // temporal noise added to the picture, +- this many 8-bit steps (grain, dither, an analogue source)
    DXGI_FORMAT bb_fmt = DXGI_FORMAT_R10G10B10A2_UNORM;
    std::vector<int> captures;
    std::string out = "fxlab-out", tag = "run";

    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11Texture2D> scene, scene_staging, bb_staging;
    ComPtr<ID3D11RenderTargetView> scene_rtv;
    ComPtr<ID3D11ShaderResourceView> scene_srv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_bg, ps_blit, ps_pq, ps_noise_add, ps_noise_sub;
    ComPtr<ID3D11BlendState> bs_add, bs_sub;
    ComPtr<ID3D11Buffer> cb, cb_pq;
    float logo_nits = 1000.0f;   // --logo-nits: the logo's white in nits on an HDR10 swap chain (0 = the normal SDR-to-HDR mapping)
    ComPtr<ID3D11RasterizerState> rs;

    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> d2d_target;
    ComPtr<IDWriteFactory> dw;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::map<int, ComPtr<IDWriteTextFormat>> formats;
    ComPtr<IWICImagingFactory> wic;

    FILE *csv = nullptr;
};
static App g;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { DestroyWindow(h); return 0; }
    if (m == WM_KEYDOWN && w == VK_SPACE && !(l & (1 << 30))) { g.paused = !g.paused; return 0; }
    return DefWindowProcW(h, m, w, l);
}

static ComPtr<ID3DBlob> Compile(const char *src, const char *entry, const char *target)
{
    ComPtr<ID3DBlob> code, err;
    const HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
    {
        fprintf(stderr, "%s\n", err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
        Die("shader compile", hr);
    }
    return code;
}

static IDWriteTextFormat *Format(float size, bool bold)
{
    const int key = static_cast<int>(size * 16.0f) * 2 + (bold ? 1 : 0);
    auto it = g.formats.find(key);
    if (it != g.formats.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    CHECK(g.dw->CreateTextFormat(L"Segoe UI", nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f), "CreateTextFormat");
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    g.formats[key] = f;
    return f.Get();
}

static void Text(const wchar_t *s, float size, float x, float y, float w, float grey = 1.0f, bool bold = false, float alpha = 1.0f)
{
    g.brush->SetColor(D2D1::ColorF(grey, grey, grey, alpha));
    g.dc->DrawText(s, static_cast<UINT32>(wcslen(s)), Format(size, bold), D2D1::RectF(x, y, x + w, y + 4000.0f), g.brush.Get(),
                   D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
}

static const wchar_t kPara[] =
    L"Continue your adventure from the last checkpoint, or browse the library for something new. "
    L"Trophies earned: 128 of 340. Storage: 512.4 GB free of 825 GB. Controller battery 3/4, headset connected. "
    L"Press the PS button to return to the control centre at any time; hold it to open the quick menu. "
    L"Downloads (2): Patch 1.07 - 3.2 GB of 11.8 GB, 14 min left; Add-on pack - queued.";

static float Smooth(float u) { u = std::clamp(u, 0.0f, 1.0f); return u * u * (3.0f - 2.0f * u); }

static void DrawUi(int frame)
{
    const float t = static_cast<float>(frame);
    auto dc = g.dc.Get();
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Scale(g.S, g.S));

    // ---- menu: static UI over the waves ------------------------------------------------------
    Text(L"Game Library", 64, 120, 70, 1400, 1.0f, true);
    Text(L"Games      Media      Store      Settings      Friends      Notifications", 36, 120, 190, 1400, 0.85f);
    Text(kPara, 28, 120, 310, 1040, 0.95f);
    Text(kPara, 20, 120, 620, 1040, 0.9f);
    Text(kPara, 14, 120, 900, 1040, 0.9f);
    Text(kPara, 14, 120, 1000, 1040, 0.6f);
    Text(L"12:45", 48, 2180, 70, 300, 1.0f);
    static const wchar_t *const items[] = { L"Resume game", L"Start new game", L"Load checkpoint - Chapter 7: The Lighthouse",
                                            L"Options & accessibility", L"Photo mode gallery (37)", L"Quit to title screen" };
    for (int i = 0; i < 6; ++i) Text(items[i], 30, 1320, 320.0f + 62.0f * i, 960, i == 2 ? 1.0f : 0.8f, i == 2);
    g.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.9f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1300.5f, 438.5f, 2280.5f, 494.5f), 8, 8), g.brush.Get(), 1.0f / g.S);

    // ---- tiles: a carousel that steps one tile every 90 frames -------------------------------
    Text(L"Recently played", 32, 120, 1335, 1100, 1.0f, true);
    {
        const float pitch = 440.0f, total = pitch * 10.0f;
        const int   step  = frame / 90;
        const float off   = pitch * (static_cast<float>(step) + Smooth(static_cast<float>(frame % 90) / 24.0f));
        dc->PushAxisAlignedClip(D2D1::RectF(0, 1410, 2400, 1910), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < 10; ++i)
        {
            float x = fmodf(100.0f + pitch * i - off, total);
            if (x < 0) x += total;
            x -= pitch;
            const float y = 1420.0f;
            const float hue = i / 10.0f;
            g.brush->SetColor(D2D1::ColorF(0.25f + 0.5f * hue, 0.55f - 0.3f * hue, 0.35f + 0.4f * fabsf(0.5f - hue), 1.0f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + 400, y + 400), 18, 18), g.brush.Get());
            g.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.35f));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 280, y + 130), 70, 70), g.brush.Get());
            g.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.8f));
            for (int k = 0; k < 5; ++k) dc->DrawLine(D2D1::Point2F(x + 30, y + 300 + 14.0f * k), D2D1::Point2F(x + 370 - 40.0f * k, y + 300 + 14.0f * k), g.brush.Get(), 2.0f);
            wchar_t big[2] = { static_cast<wchar_t>(L'A' + i), 0 };
            Text(big, 150, x + 30, y + 10, 300, 1.0f, true);
            wchar_t label[64];
            swprintf_s(label, L"Game Title %d - Deluxe", i + 1);
            Text(label, 26, x + 4, y + 412, 420, 0.95f);
        }
        dc->PopAxisAlignedClip();
    }
    Text(L"\x2715  Select      \x25CB  Back      \x25B3  Options      L1 / R1  Switch tab", 26, 120, 2050, 1400, 0.9f);

    // ---- scrolling list ----------------------------------------------------------------------
    {
        dc->PushAxisAlignedClip(D2D1::RectF(2400, 120, 3120, 1080), D2D1_ANTIALIAS_MODE_ALIASED);
        const float pitch = 64.0f, total = pitch * 24.0f;
        for (int i = 0; i < 24; ++i)
        {
            float y = fmodf(pitch * i - 2.5f * t, total);
            if (y < 0) y += total;
            y += 120.0f - pitch;
            if (y > 1090.0f) continue;
            wchar_t line[96];
            swprintf_s(line, L"%02d  Save slot - %d h %02d min - Level %d", i + 1, 3 + i * 2, (i * 37) % 60, 10 + i * 3);
            Text(line, 30, 2430, y, 680, 0.9f);
        }
        Text(L"Static overlay 0123456789 AaBbCc", 34, 2430, 940, 680, 1.0f, true);
        dc->PopAxisAlignedClip();
        g.brush->SetColor(D2D1::ColorF(0.02f, 0.02f, 0.03f, 1.0f));
        dc->FillRectangle(D2D1::RectF(2400, 0, 3120, 120), g.brush.Get());
        Text(L"SCROLLING LIST - header stays", 40, 2430, 30, 680, 1.0f, true);
    }

    // ---- tickers -----------------------------------------------------------------------------
    {
        dc->PushAxisAlignedClip(D2D1::RectF(3120, 0, 3840, 540), D2D1_ANTIALIAS_MODE_ALIASED);
        static const wchar_t fast[] = L"BREAKING: fast ticker at four pixels per frame  +++  0123456789  +++  ";
        static const wchar_t slow[] = L"slow ticker, three quarters of a pixel per frame - sub-pixel motion - ";
        const float wf = 1500.0f, ws = 1100.0f;
        for (int k = 0; k < 3; ++k)
        {
            Text(fast, 44, 3120.0f + wf * k - fmodf(4.0f * t, wf), 110, 4000, 1.0f);
            Text(slow, 30, 3120.0f - ws + ws * k + fmodf(0.75f * t, ws), 290, 4000, 0.9f);
        }
        Text(L"Static label under two tickers - 1234", 24, 3140, 430, 680, 0.9f);
        dc->PopAxisAlignedClip();
    }

    // ---- dense static text on flat grey and on black ------------------------------------------
    Text(kPara, 16, 3140, 1090, 680, 0.0f);
    Text(kPara, 22, 3140, 1200, 680, 0.05f);
    Text(kPara, 16, 3140, 1360, 680, 1.0f);
    Text(kPara, 22, 3140, 1470, 680, 0.85f);

    // a few static hairlines over the slow shapes
    g.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.7f));
    for (int k = 0; k < 4; ++k)
        dc->DrawLine(D2D1::Point2F(3160.5f + 160.0f * k, 1640), D2D1::Point2F(3160.5f + 160.0f * k + 60.0f, 2140), g.brush.Get(), 1.0f / g.S);

    CHECK(dc->EndDraw(), "D2D EndDraw");
}

static void SavePng(const std::string &path, const std::vector<uint8_t> &bgr, int w, int h)
{
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wpath, MAX_PATH);
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    CHECK(g.wic->CreateStream(&stream), "WIC stream");
    CHECK(stream->InitializeFromFilename(wpath, GENERIC_WRITE), "WIC open file");
    CHECK(g.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc), "WIC encoder");
    CHECK(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache), "WIC init");
    CHECK(enc->CreateNewFrame(&fr, nullptr), "WIC frame");
    CHECK(fr->Initialize(nullptr), "WIC frame init");
    CHECK(fr->SetSize(w, h), "WIC size");
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    CHECK(fr->SetPixelFormat(&pf), "WIC format");
    CHECK(fr->WritePixels(h, w * 3, static_cast<UINT>(bgr.size()), const_cast<BYTE *>(bgr.data())), "WIC write");
    CHECK(fr->Commit(), "WIC commit");
    CHECK(enc->Commit(), "WIC commit");
}

// input | output | difference x8, as one PNG
static void SaveCrop(const char *name, int frame, const std::vector<float> &pre, const std::vector<float> &post, int x0, int y0, int x1, int y1)
{
    const int W = g.W, cw = x1 - x0, ch = y1 - y0;
    if (cw <= 0 || ch <= 0) return;
    std::vector<uint8_t> img(static_cast<size_t>(cw) * 3 * ch * 3);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x)
        {
            const size_t s = static_cast<size_t>(y0 + y) * W + (x0 + x);
            uint8_t *row = img.data() + static_cast<size_t>(y) * cw * 9;
            for (int c = 0; c < 3; ++c)
            {
                const float a = pre[s * 3 + c], b = post[s * 3 + c];
                row[x * 3 + (2 - c)]               = static_cast<uint8_t>(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
                row[(cw + x) * 3 + (2 - c)]        = static_cast<uint8_t>(std::clamp(b * 255.0f + 0.5f, 0.0f, 255.0f));
                row[(2 * cw + x) * 3 + (2 - c)]    = static_cast<uint8_t>(std::clamp(fabsf(b - a) * 8.0f * 255.0f, 0.0f, 255.0f));
            }
        }
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s_f%d_%s.png", g.out.c_str(), g.tag.c_str(), frame, name);
    SavePng(path, img, cw * 3, ch);
}

// One finished frame against the picture fxlab drew for it.
static void Analyse(int frame, const std::vector<float> &pre, const std::vector<float> &post /* both rgb, 0..1 */)
{
    const int W = g.W, H = g.H;
    std::vector<float> lp(static_cast<size_t>(W) * H), lq(static_cast<size_t>(W) * H);
    for (size_t i = 0, n = lp.size(); i < n; ++i)
    {
        lp[i] = 0.299f * pre[i * 3] + 0.587f * pre[i * 3 + 1] + 0.114f * pre[i * 3 + 2];
        lq[i] = 0.299f * post[i * 3] + 0.587f * post[i * 3 + 1] + 0.114f * post[i * 3 + 2];
    }
    for (const Probe &p : kProbes)
    {
        const int x0 = std::clamp(static_cast<int>(p.x0 * g.S), 1, W - 2), x1 = std::clamp(static_cast<int>(p.x1 * g.S), 2, W - 1);
        const int y0 = std::clamp(static_cast<int>(p.y0 * g.S), 1, H - 2), y1 = std::clamp(static_cast<int>(p.y1 * g.S), 2, H - 1);
        if (g.mvdump)
        {
            // Vectors against the truth, over the pixels that have enough structure to have a
            // well-defined motion at all (a flat pixel can be given any vector and look the same).
            float tx = 0, ty = 0;
            const int   now_f = frame - frame % g.cadence, was_f = (frame - 1) - (frame - 1) % g.cadence;
            const float steps = static_cast<float>(now_f - was_f);     // 0 on the second showing of a picture
            if (p.truth == 2) { tx = p.vx * g.S * steps; ty = p.vy * g.S * steps; }
            if (p.truth == 3) { tx = p.vx * steps; ty = p.vy * steps; }
            if (p.truth == 4)
            {
                auto off = [](int f) { return 440.0f * (static_cast<float>(f / 90) + Smooth(static_cast<float>(f % 90) / 24.0f)); };
                tx = (off(now_f) - off(was_f)) * g.S;
            }
            double n = 0, serr = 0, in025 = 0, in1 = 0, zero = 0, sx = 0, sy = 0, smask = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                {
                    const size_t i = static_cast<size_t>(y) * W + x;
                    const float grad = fabsf(lp[i + 1] - lp[i - 1]) + fabsf(lp[i + W] - lp[i - W]);
                    if (grad < (p.kind[1] == 'i' ? 0.30f : 0.08f)) continue;   // "mixed": glyph edges only, not the soft background
                    const float mx_ = (post[i * 3] * 1023.0f - 512.0f) / 8.0f, my_ = (post[i * 3 + 1] * 1023.0f - 512.0f) / 8.0f;
                    const float e = hypotf(mx_ - tx, my_ - ty);
                    n += 1; serr += e; sx += mx_; sy += my_; smask += post[i * 3 + 2];
                    if (e < 0.25f) in025 += 1;
                    if (e < 1.0f) in1 += 1;
                    if (fabsf(mx_) < 0.01f && fabsf(my_) < 0.01f) zero += 1;
                }
            if (n < 1) n = 1;
            if (p.truth == 0)
                printf("  f%-4d %-15s %-7s truth    -          mean (%6.2f,%6.2f)  zero %5.1f%%  mask %4.2f\n", frame, p.name, p.kind, sx / n, sy / n, 100.0 * zero / n, smask / n);
            else
                printf("  f%-4d %-15s %-7s truth (%5.2f,%5.2f) mean (%6.2f,%6.2f)  err %5.2f px  <0.25: %5.1f%%  <1: %5.1f%%  zero %5.1f%%  mask %4.2f\n",
                       frame, p.name, p.kind, tx, ty, sx / n, sy / n, serr / n, 100.0 * in025 / n, 100.0 * in1 / n, 100.0 * zero / n, smask / n);
            if (g.csv) fprintf(g.csv, "%s,%d,%s,%s,%.3f,%.2f,%.4f,%.1f\n", g.tag.c_str(), frame, p.name, p.kind, serr / n, 100.0 * in025 / n, 100.0 * in1 / n, 100.0 * zero / n);
            continue;
        }
        double sae = 0, sse = 0, gp = 0, gq = 0, mx = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
            {
                const size_t i = static_cast<size_t>(y) * W + x;
                const double d = fabs(static_cast<double>(lq[i]) - lp[i]);
                sae += d; sse += d * d; mx = std::max(mx, d);
                gp += fabs(lp[i + 1] - lp[i]) + fabs(lp[i + W] - lp[i]);
                gq += fabs(lq[i + 1] - lq[i]) + fabs(lq[i + W] - lq[i]);
            }
        const double n = static_cast<double>(x1 - x0) * (y1 - y0);
        const double mae = 255.0 * sae / n, mse = sse / n;
        const double psnr = mse > 1e-12 ? 10.0 * log10(1.0 / mse) : 99.0;
        const double sharp = gp > 1e-9 ? gq / gp : 1.0;
        printf("  f%-4d %-15s %-7s mae %6.2f  psnr %5.1f  sharp %5.3f  max %5.1f\n", frame, p.name, p.kind, mae, psnr, sharp, 255.0 * mx);
        if (g.csv) fprintf(g.csv, "%s,%d,%s,%s,%.3f,%.2f,%.4f,%.1f\n", g.tag.c_str(), frame, p.name, p.kind, mae, psnr, sharp, 255.0 * mx);

        SaveCrop(p.name, frame, pre, post, x0, y0, x1, y1);
    }
    if (g.full)
    {
        std::vector<uint8_t> img(static_cast<size_t>(W) * H * 3);
        for (size_t i = 0, n = static_cast<size_t>(W) * H; i < n; ++i)
            for (int c = 0; c < 3; ++c) img[i * 3 + (2 - c)] = static_cast<uint8_t>(std::clamp(post[i * 3 + c] * 255.0f + 0.5f, 0.0f, 255.0f));
        char path[MAX_PATH];
        sprintf_s(path, "%s\\%s_f%d_full.png", g.out.c_str(), g.tag.c_str(), frame);
        SavePng(path, img, W, H);
    }
}

#include "fxlab3d.h"

static void Crop3(const char *name, int frame, const Cap3 &c, float bx0, float by0, float bx1, float by1)
{
    SaveCrop(name, frame, c.pre, c.post, std::clamp(static_cast<int>(bx0 * g.S), 0, g.W), std::clamp(static_cast<int>(by0 * g.S), 0, g.H),
             std::clamp(static_cast<int>(bx1 * g.S), 0, g.W), std::clamp(static_cast<int>(by1 * g.S), 0, g.H));
}

static std::map<int, Cap3> g_caps;   // frame -> what fxlab drew for it, and later what came back

// The current back buffer as RGB 0..1 (its code values, whatever they encode).
static void ReadBuffer(std::vector<float> &post)
{
    ComPtr<ID3D11Texture2D> bb;
    CHECK(g.swap->GetBuffer(0, IID_PPV_ARGS(&bb)), "GetBuffer");
    g.ctx->CopyResource(g.bb_staging.Get(), bb.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(g.ctx->Map(g.bb_staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map backbuffer");
    post.resize(static_cast<size_t>(g.W) * g.H * 3);
    for (int y = 0; y < g.H; ++y)
    {
        const uint32_t *row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch);
        float *o = post.data() + static_cast<size_t>(y) * g.W * 3;
        for (int x = 0; x < g.W; ++x)
        {
            const uint32_t v = row[x];
            if (g.bb_fmt == DXGI_FORMAT_R10G10B10A2_UNORM)
            {
                o[x * 3] = (v & 1023) / 1023.0f; o[x * 3 + 1] = ((v >> 10) & 1023) / 1023.0f; o[x * 3 + 2] = ((v >> 20) & 1023) / 1023.0f;
            }
            else
            {
                o[x * 3] = (v & 255) / 255.0f; o[x * 3 + 1] = ((v >> 8) & 255) / 255.0f; o[x * 3 + 2] = ((v >> 16) & 255) / 255.0f;
            }
        }
    }
    g.ctx->Unmap(g.bb_staging.Get(), 0);
}

static void ReadBack(int frame)
{
    auto it = g_caps.find(frame);
    if (it == g_caps.end()) return;
    Cap3 &cap = it->second;
    std::vector<float> &post = cap.post;
    ReadBuffer(post);
    if (cap.report)
    {
        if (g.scene3d)
        {
            auto pv = g_caps.find(frame - 1);
            Analyse3D(frame, cap, pv != g_caps.end() ? &pv->second : nullptr);
        }
        else Analyse(frame, cap.pre, post);
    }
    g_caps.erase(g_caps.begin(), g_caps.lower_bound(frame));   // this frame stays: it may be the next one's "previous"
}

// The frame just drawn, copied out for the comparison two presents from now: the back buffer as
// it is about to be presented (that is the input ReShade gets), and for the 3D scene what its
// analysis needs besides.
static void Capture(int frame, bool report, const std::vector<uint8_t> *nohud)
{
    auto grab = [](ID3D11Texture2D *staging, ID3D11Texture2D *src, void *dst, size_t row_bytes)
    {
        g.ctx->CopyResource(staging, src);
        D3D11_MAPPED_SUBRESOURCE m;
        CHECK(g.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m), "Map");
        for (int y = 0; y < g.H; ++y)
            memcpy(static_cast<uint8_t *>(dst) + static_cast<size_t>(y) * row_bytes, static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch, row_bytes);
        g.ctx->Unmap(staging, 0);
    };
    Cap3 &cap = g_caps[frame];
    cap.report = report;
    ReadBuffer(cap.pre);
    if (g.scene3d)
    {
        cap.scene.resize(static_cast<size_t>(g.W) * g.H * 4);
        grab(g.scene_staging.Get(), g.scene.Get(), cap.scene.data(), static_cast<size_t>(g.W) * 4);
        cap.nohud = *nohud;
        cap.truth.resize(static_cast<size_t>(g.W) * g.H * 4);
        grab(s3.truth_staging.Get(), s3.truth.Get(), cap.truth.data(), static_cast<size_t>(g.W) * 8);
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if      (a == "--size")    { sscanf_s(v, "%dx%d", &g.W, &g.H); ++i; }
        else if (a == "--frames")  { g.frames = atoi(v); ++i; }
        else if (a == "--lat")     { g.latency = atoi(v); ++i; }
        else if (a == "--out")     { g.out = v; ++i; }
        else if (a == "--tag")     { g.tag = v; ++i; }
        else if (a == "--fmt")     { g.bb_fmt = atoi(v) == 8 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM; ++i; }
        else if (a == "--preroll") { g.preroll = atof(v); ++i; }
        else if (a == "--fps")     { g.fps = atof(v); ++i; }
        else if (a == "--noise")   { g.noise = static_cast<float>(atof(v)); ++i; }
        else if (a == "--show")    g.show = true;
        else if (a == "--loop")    g.loop = true;
        else if (a == "--hdr")     g.hdr = true;
        else if (a == "--cadence") { g.cadence = std::max(1, atoi(v)); ++i; }
        else if (a == "--oracle")  g.oracle = true;
        else if (a == "--hold")    { sscanf_s(v, "%d,%d", &g.hold_run, &g.hold_still); ++i; }
        else if (a == "--scene")   { g.scene3d = std::string(v) == "3d"; ++i; }
        else if (a == "--full")    g.full = true;
        else if (a == "--mv")      g.mvdump = true;
        else if (a == "--mesh")    g.mesh = true;
        else if (a == "--no-depth") g.nodepth = true;
        else if (a == "--depth-flicker") { g.depth_flicker = atoi(v); ++i; }
        else if (a == "--fx")      g.fx = true;
        else if (a == "--logo")    { g.logo = atoi(v); ++i; }
        else if (a == "--logo-nits") { g.logo_nits = static_cast<float>(atof(v)); ++i; }
        else if (a == "--fade")    { g.fade = atoi(v); ++i; }
        else if (a == "--flash")   { g.flash = atoi(v); ++i; }
        else if (a == "--capture") { for (const char *s = v; *s; ) { g.captures.push_back(atoi(s)); while (*s && *s != ',') ++s; if (*s) ++s; } ++i; }
        else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    // Defaults. 2D: mid-slide of the carousel, six frames after it stopped, and at rest.
    // 3D: camera still, the pan at full speed, the pan turning round (slow), the dolly at
    // full speed past the fence, the strafe.
    if (g.captures.empty() && !g.loop) g.captures = g.scene3d ? std::vector<int>{ 100, 210, 255, 345, 560 } : std::vector<int>{ 458, 480, 510 };
    if (g.captures.empty()) g.captures = { -100 };
    g.frames = std::max(g.frames, *std::max_element(g.captures.begin(), g.captures.end()) + g.latency + 2);
    g.S = g.W / 3840.0f;
    CreateDirectoryA(g.out.c_str(), nullptr);
    // 1 ms sleeps for the frame pacing -- and the import that makes Windows load winmm.dll,
    // which is the name the OptiScaler fork goes by when the rig has a neural consumer in it.
    timeBeginPeriod(1);
    CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitialize");
    CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g.wic)), "WIC factory");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"fxlab"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g.hwnd = CreateWindowExW(g.show ? 0 : WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"fxlab", L"fxlab - DLSS5_Feed.fx test feed (Esc quits, Space freezes)", WS_POPUP,
                             0, 0, g.W, g.H, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g.hwnd) Die("CreateWindow", HRESULT_FROM_WIN32(GetLastError()));
    if (g.show) { ShowWindow(g.hwnd, g.loop ? SW_SHOW : SW_SHOWNOACTIVATE); if (g.loop) SetForegroundWindow(g.hwnd); }

    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx),
          "D3D11CreateDevice");
    ComPtr<IDXGIDevice> dxgi_dev; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIFactory2> factory;
    CHECK(g.dev.As(&dxgi_dev), "IDXGIDevice");
    CHECK(dxgi_dev->GetAdapter(&adapter), "GetAdapter");
    CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2");
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = g.W; sd.Height = g.H; sd.Format = g.bb_fmt; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT; sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.Scaling = DXGI_SCALING_STRETCH; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    CHECK(factory->CreateSwapChainForHwnd(g.dev.Get(), g.hwnd, &sd, nullptr, nullptr, &g.swap), "CreateSwapChainForHwnd");
    if (g.hdr)
    {
        ComPtr<IDXGISwapChain3> sc3;
        UINT support = 0;
        CHECK(g.swap.As(&sc3), "IDXGISwapChain3");
        sc3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support);
        const HRESULT hr = sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        printf("HDR10: ST.2084 / BT.2020 colour space %s (support flags 0x%X, SetColorSpace1 0x%08lX)\n",
               SUCCEEDED(hr) ? "set" : "NOT set", support, static_cast<unsigned long>(hr));
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g.W; td.Height = g.H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    CHECK(g.dev->CreateTexture2D(&td, nullptr, &g.scene), "scene texture");
    CHECK(g.dev->CreateRenderTargetView(g.scene.Get(), nullptr, &g.scene_rtv), "scene RTV");
    CHECK(g.dev->CreateShaderResourceView(g.scene.Get(), nullptr, &g.scene_srv), "scene SRV");
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(g.dev->CreateTexture2D(&td, nullptr, &g.scene_staging), "scene staging");
    td.Format = g.bb_fmt;
    CHECK(g.dev->CreateTexture2D(&td, nullptr, &g.bb_staging), "backbuffer staging");

    {
        auto v = Compile(kBlitHlsl, "vs", "vs_5_0");
        auto b = Compile(kBlitHlsl, "ps", "ps_5_0");
        auto p = Compile(kBackgroundHlsl, "main", "ps_5_0");
        CHECK(g.dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &g.vs), "VS");
        CHECK(g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g.ps_blit), "blit PS");
        auto q = Compile(kBlitHlsl, "ps_pq", "ps_5_0");
        CHECK(g.dev->CreatePixelShader(q->GetBufferPointer(), q->GetBufferSize(), nullptr, &g.ps_pq), "PQ PS");
        CHECK(g.dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &g.ps_bg), "background PS");
        auto na = Compile(kBackgroundHlsl, "noise_add", "ps_5_0");
        auto ns = Compile(kBackgroundHlsl, "noise_sub", "ps_5_0");
        CHECK(g.dev->CreatePixelShader(na->GetBufferPointer(), na->GetBufferSize(), nullptr, &g.ps_noise_add), "noise PS");
        CHECK(g.dev->CreatePixelShader(ns->GetBufferPointer(), ns->GetBufferSize(), nullptr, &g.ps_noise_sub), "noise PS");
        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0].BlendEnable = TRUE; bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        CHECK(g.dev->CreateBlendState(&bl, &g.bs_add), "blend state");
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
        CHECK(g.dev->CreateBlendState(&bl, &g.bs_sub), "blend state");
        D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        CHECK(g.dev->CreateBuffer(&bd, nullptr, &g.cb), "constant buffer");
        bd.ByteWidth = 32;
        CHECK(g.dev->CreateBuffer(&bd, nullptr, &g.cb_pq), "PQ constant buffer");
        D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        CHECK(g.dev->CreateRasterizerState(&rd, &g.rs), "rasterizer");
    }
    {
        ComPtr<ID2D1Factory1> f; ComPtr<ID2D1Device> d; ComPtr<IDXGISurface> surf;
        CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, reinterpret_cast<void **>(f.GetAddressOf())), "D2D factory");
        CHECK(f->CreateDevice(dxgi_dev.Get(), &d), "D2D device");
        CHECK(d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g.dc), "D2D context");
        CHECK(g.scene.As(&surf), "scene surface");
        const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                                   D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        CHECK(g.dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &g.d2d_target), "D2D target");
        g.dc->SetTarget(g.d2d_target.Get());
        g.dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        CHECK(g.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &g.brush), "brush");
        CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(g.dw.GetAddressOf())), "DWrite factory");
    }

    if (g.scene3d) Init3D();

    char csv_path[MAX_PATH];
    sprintf_s(csv_path, "%s\\%s.csv", g.out.c_str(), g.tag.c_str());
    fopen_s(&g.csv, csv_path, "w");
    if (g.csv) fprintf(g.csv, "tag,frame,probe,kind,mae255,psnr,sharp,max255\n");
    printf("fxlab %dx%d, %d frames, tag %s\n", g.W, g.H, g.frames, g.tag.c_str());

    const D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(g.W), static_cast<float>(g.H), 0, 1 };
    bool quit = false;
    LARGE_INTEGER qpf, q0, qlast, q100 = {}, q400 = {};
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&q0);
    qlast = q0;
    int presents = 0;
    std::vector<uint8_t> nohud;
    ComPtr<ID3D11Query> q_disjoint, q_t0, q_t1;
    {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        CHECK(g.dev->CreateQuery(&qd, &q_disjoint), "query");
        qd.Query = D3D11_QUERY_TIMESTAMP;
        CHECK(g.dev->CreateQuery(&qd, &q_t0), "query");
        CHECK(g.dev->CreateQuery(&qd, &q_t1), "query");
    }
    double gpu_ms = 0.0; int gpu_n = 0;
    int  rolled = 0;
    int  last_shown = 0;
    bool was_moving = false;
    FILE *events = nullptr;
    {
        char ev_path[MAX_PATH];
        sprintf_s(ev_path, "%s\\%s.events.txt", g.out.c_str(), g.tag.c_str());
        fopen_s(&events, ev_path, "w");
    }
    for (int frame = 0; (g.loop || frame < g.frames) && !quit; ++presents)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) quit = true; TranslateMessage(&msg); DispatchMessageW(&msg); }
        LARGE_INTEGER qn;
        if (g.fps > 0.0)
            for (;;)
            {
                QueryPerformanceCounter(&qn);
                const double left = 1.0 / g.fps - static_cast<double>(qn.QuadPart - qlast.QuadPart) / qpf.QuadPart;
                if (left <= 0.0) break;
                if (left > 0.002) Sleep(1);
            }
        QueryPerformanceCounter(&qn);
        qlast = qn;
        const bool rolling = static_cast<double>(qn.QuadPart - q0.QuadPart) / qpf.QuadPart >= g.preroll;

        ReadBack(frame - g.latency);   // the buffer about to be overwritten still holds that finished frame

        // A frame is kept if it is reported on, or (3D) if the next one is: stability is
        // measured from one frame to the next.
        const int  shown  = frame - frame % g.cadence;     // the scene frame this present shows
        const bool report = std::find(g.captures.begin(), g.captures.end(), frame) != g.captures.end();
        const bool keep   = report || (g.scene3d && std::find(g.captures.begin(), g.captures.end(), frame + 1) != g.captures.end());
        ID3D11ShaderResourceView *null_srv = nullptr;
        ID3D11RenderTargetView *null_rtv = nullptr;
        if (g.scene3d)
        {
            Render3D(shown, last_shown);
            if (keep)
            {
                g.ctx->CopyResource(g.scene_staging.Get(), g.scene.Get());
                D3D11_MAPPED_SUBRESOURCE m;
                CHECK(g.ctx->Map(g.scene_staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map scene");
                nohud.resize(static_cast<size_t>(g.W) * g.H * 4);
                for (int y = 0; y < g.H; ++y)
                    memcpy(nohud.data() + static_cast<size_t>(y) * g.W * 4, static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch, static_cast<size_t>(g.W) * 4);
                g.ctx->Unmap(g.scene_staging.Get(), 0);
            }
            DrawHud3D(shown);
        }
        else
        {
            const float cbv[4] = { static_cast<float>(shown), g.S, static_cast<float>(g.W), static_cast<float>(g.H) };
            g.ctx->UpdateSubresource(g.cb.Get(), 0, nullptr, cbv, 0, 0);
            g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.ctx->IASetInputLayout(nullptr);
            g.ctx->RSSetState(g.rs.Get());
            g.ctx->RSSetViewports(1, &vp);
            g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            g.ctx->OMSetDepthStencilState(nullptr, 0);
            g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
            g.ctx->PSSetShader(g.ps_bg.Get(), nullptr, 0);
            g.ctx->PSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
            g.ctx->PSSetShaderResources(0, 1, &null_srv);
            g.ctx->OMSetRenderTargets(1, g.scene_rtv.GetAddressOf(), nullptr);
            g.ctx->Draw(3, 0);
            g.ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
            g.ctx->Flush();

            DrawUi(shown);
        }

        if (g.noise > 0.0f)
        {
            const float nv[4] = { static_cast<float>(shown), g.S, static_cast<float>(g.W), g.noise };   // H carries the amplitude here
            g.ctx->UpdateSubresource(g.cb.Get(), 0, nullptr, nv, 0, 0);
            g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.ctx->IASetInputLayout(nullptr);
            g.ctx->RSSetState(g.rs.Get());
            g.ctx->RSSetViewports(1, &vp);
            g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
            g.ctx->PSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
            g.ctx->OMSetRenderTargets(1, g.scene_rtv.GetAddressOf(), nullptr);
            g.ctx->OMSetBlendState(g.bs_add.Get(), nullptr, 0xFFFFFFFF);
            g.ctx->PSSetShader(g.ps_noise_add.Get(), nullptr, 0);
            g.ctx->Draw(3, 0);
            g.ctx->OMSetBlendState(g.bs_sub.Get(), nullptr, 0xFFFFFFFF);
            g.ctx->PSSetShader(g.ps_noise_sub.Get(), nullptr, 0);
            g.ctx->Draw(3, 0);
            g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            g.ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
        }

        if (g.fade > 0 || g.flash > 0)
        {
            // Brightness alone changes, nothing moves: scene *= k.
            float k = 1.0f;
            if (g.fade > 0)  k *= 0.15f + 0.85f * (0.5f - 0.5f * cosf(6.2831853f * (shown % g.fade) / g.fade));
            if (g.flash > 0) k *= (shown % g.flash) < 3 ? 1.0f : 0.45f;
            static ComPtr<ID3D11BlendState> bs_mul;
            if (!bs_mul)
            {
                D3D11_BLEND_DESC bl = {};
                bl.RenderTarget[0].BlendEnable = TRUE; bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO; bl.RenderTarget[0].DestBlend = D3D11_BLEND_BLEND_FACTOR;
                bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
                bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                CHECK(g.dev->CreateBlendState(&bl, &bs_mul), "multiply blend");
            }
            const float kk[4] = { k, k, k, 1.0f };
            g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.ctx->IASetInputLayout(nullptr);
            g.ctx->RSSetState(g.rs.Get());
            g.ctx->RSSetViewports(1, &vp);
            g.ctx->OMSetDepthStencilState(nullptr, 0);
            g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
            g.ctx->PSSetShader(g.ps_noise_sub.Get(), nullptr, 0);
            g.ctx->PSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
            g.ctx->PSSetShaderResources(0, 1, &null_srv);
            g.ctx->OMSetRenderTargets(1, g.scene_rtv.GetAddressOf(), nullptr);
            g.ctx->OMSetBlendState(bs_mul.Get(), kk, 0xFFFFFFFF);
            g.ctx->Draw(3, 0);
            g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            g.ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
        }

        ComPtr<ID3D11Texture2D> bb; ComPtr<ID3D11RenderTargetView> bb_rtv;
        CHECK(g.swap->GetBuffer(0, IID_PPV_ARGS(&bb)), "GetBuffer");
        CHECK(g.dev->CreateRenderTargetView(bb.Get(), nullptr, &bb_rtv), "backbuffer RTV");
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->RSSetState(g.rs.Get());
        g.ctx->RSSetViewports(1, &vp);
        g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        g.ctx->OMSetDepthStencilState(nullptr, 0);
        g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(g.hdr ? g.ps_pq.Get() : g.ps_blit.Get(), nullptr, 0);
        if (g.hdr)
        {
            const bool on = g.scene3d && g.logo > 0 && g.logo_nits > 0.0f;
            const float pq[8] = { kLogo[0] * g.S, kLogo[1] * g.S, kLogo[2] * g.S, kLogo[3] * g.S, on ? g.logo_nits : 0.0f, 0, 0, 0 };
            g.ctx->UpdateSubresource(g.cb_pq.Get(), 0, nullptr, pq, 0, 0);
            g.ctx->PSSetConstantBuffers(1, 1, g.cb_pq.GetAddressOf());
        }
        g.ctx->OMSetRenderTargets(1, bb_rtv.GetAddressOf(), nullptr);
        g.ctx->PSSetShaderResources(0, 1, g.scene_srv.GetAddressOf());
        g.ctx->Draw(3, 0);
        g.ctx->PSSetShaderResources(0, 1, &null_srv);
        g.ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
        bb_rtv.Reset();
        if (g.oracle && g.bb_fmt == DXGI_FORMAT_R10G10B10A2_UNORM)
        {
            auto code = [](int f) { const uint32_t u = static_cast<uint32_t>(std::max(f, 0)); return (u & 1023u) | (((u >> 10) & 1023u) << 10) | (3u << 30); };
            const uint32_t stamp[2] = { code(shown), code(last_shown) };
            const D3D11_BOX box = { 0, 0, 0, 2, 1, 1 };
            g.ctx->UpdateSubresource(bb.Get(), 0, &box, stamp, 8, 8);
        }
        bb.Reset();
        last_shown = shown;

        if (keep && g_caps.find(frame) == g_caps.end()) Capture(frame, report, &nohud);

        // GPU time of everything issued on this device inside Present -- that is ReShade's whole
        // effect chain, DLSS5_Feed.fx and its provider included. (The DLSS evaluate itself runs
        // on the add-on's D3D12 device and is not in it.) Wall-clock frame time says little
        // here: the loop is bound by its own draw calls and the cross-device hand-off, not by
        // the shaders.
        const bool timed = rolling && frame >= 100 && frame < 400 && (frame % 4) == 0;
        if (timed) { g.ctx->Begin(q_disjoint.Get()); g.ctx->End(q_t0.Get()); }
        const HRESULT hr = g.swap->Present(0, 0);
        if (FAILED(hr)) Die("Present", hr);
        if (timed)
        {
            g.ctx->End(q_t1.Get());
            g.ctx->End(q_disjoint.Get());
            UINT64 t0 = 0, t1 = 0;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
            while (g.ctx->GetData(q_disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) Sleep(0);
            g.ctx->GetData(q_t0.Get(), &t0, sizeof(t0), 0);
            g.ctx->GetData(q_t1.Get(), &t1, sizeof(t1), 0);
            if (!dj.Disjoint && dj.Frequency && t1 > t0) { gpu_ms += 1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(dj.Frequency); gpu_n += 1; }
        }
        // during the pre-roll the same frame 0 is presented over and over
        bool moving = rolling && !g.paused;
        if (moving && g.hold_run > 0 && g.hold_still > 0) moving = (rolled % (g.hold_run + g.hold_still)) < g.hold_run;
        if (rolling) ++rolled;
        if (moving != was_moving && events != nullptr)
        {
            SYSTEMTIME st; GetLocalTime(&st);
            fprintf(events, "%02d:%02d:%02d.%03d  %s  (scene frame %d, present %d)\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                    moving ? "starts moving" : "stands still", frame, presents);
            fflush(events);
        }
        was_moving = moving;
        if (moving) ++frame;
        if (rolling && frame == 100) QueryPerformanceCounter(&q100);
        if (rolling && frame == 400) QueryPerformanceCounter(&q400);
    }
    if (g.csv) fclose(g.csv);
    if (events) fclose(events);
    if (q400.QuadPart > q100.QuadPart)
        printf("frame time %.2f ms (frames 100-400%s)\n", 1000.0 * static_cast<double>(q400.QuadPart - q100.QuadPart) / qpf.QuadPart / 300.0,
               g.fps > 0.0 ? ", paced" : ", unpaced");
    if (gpu_n > 0) printf("effects GPU time %.2f ms (mean of %d frames, D3D11 side of Present)\n", gpu_ms / gpu_n, gpu_n);
    printf("done.\n");
    return 0;
}
