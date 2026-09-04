// spike-d3d10client32 - phase 0 for the Direct3D 10 backend.
//
// The D3D10 path adds exactly one link that no other transport in this repo has: a
// texture written by a D3D10.1 device and read by a SEPARATE D3D11 device in the same
// process. Everything downstream of that -- NT-handle sharing, the D3D12 fence, the
// pipe, the host -- is the proven D3D11 client, so this spike deliberately does NOT
// start a host: it answers the new questions only.
//
//   1. Can a D3D10.1 device create a keyed-mutex shared texture at all? ANSWERED: NO.
//      Every combination returns E_INVALIDARG on real hardware, which is why
//      src\feed_d3d10.h uses legacy MISC_SHARED plus event queries instead -- see the
//      note at the top of that header. The check stays so that a driver which ever grows
//      the capability shows up here as a passing line rather than as folklore.
//   2. Does ID3D11Device::OpenSharedResource accept the LEGACY shared handle of a D3D10
//      texture, from a different device in the same process, with usable views on it?
//   3. Do the pixels actually survive, both ways -- game -> relay and relay -> game?
//   4. What does the event-query crossing COST? There is no fence on D3D10, so each
//      drain stops that device, and this is the real price of the backend.
//
// It is 32-bit on purpose: compiling src\feed_d3d10.h as x86 is itself one of the
// answers, exactly as it was for feed_vk.h and spike-vkclient32.
//
// Unlike the add-on, this spike CREATES a D3D10.1 device, so it links d3d10_1.lib. The
// add-on never calls a d3d10_1.dll export -- it borrows the game's device -- and so
// still needs no new link dependency.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>

#include "../src/feed_d3d10.h"

static int g_fail = 0;

static void Check(const char *what, bool ok, const char *detail = nullptr)
{
    printf("  %-48s %s%s%s\n", what, ok ? "PASS" : "FAIL",
           detail != nullptr ? "  " : "", detail != nullptr ? detail : "");
    if (!ok) ++g_fail;
}

// A pattern that depends on both coordinates and on a caller-chosen salt, so a copy that
// silently does nothing, or copies the wrong resource, cannot pass by accident.
static UINT Pattern(UINT x, UINT y, UINT salt)
{
    const UINT r = (x * 7u + salt * 31u) & 0xFFu;
    const UINT g = (y * 13u + salt * 17u) & 0xFFu;
    const UINT b = ((x ^ y) + salt) & 0xFFu;
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

static void FillPattern(UINT *px, UINT w, UINT h, UINT salt)
{
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
            px[y * w + x] = Pattern(x, y, salt);
}

static bool VerifyPattern(const void *base, UINT w, UINT h, UINT salt, UINT row_pitch, UINT *bad_out)
{
    UINT bad = 0;
    for (UINT y = 0; y < h; ++y)
    {
        const UINT *row = reinterpret_cast<const UINT *>(static_cast<const BYTE *>(base) + y * row_pitch);
        for (UINT x = 0; x < w; ++x)
            if (row[x] != Pattern(x, y, salt)) ++bad;
    }
    *bad_out = bad;
    return bad == 0;
}

int main()
{
    const UINT W = 256, H = 128;
    const DXGI_FORMAT FMT = DXGI_FORMAT_R8G8B8A8_UNORM;

    printf("spike-d3d10client32: D3D10.1 <-> private D3D11 relay, %ux%u\n\n", W, H);

    ID3D10Device1 *dev10 = nullptr;
    HRESULT hr = D3D10CreateDevice1(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                    D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &dev10);
    if (FAILED(hr))
    {
        printf("  D3D10CreateDevice1 (10_1) failed 0x%08X -- no D3D10.1 device on this machine.\n",
               static_cast<unsigned>(hr));
        return 2;
    }
    printf("  D3D10.1 device created.\n");

    FeedD3D10 d;
    if (!FeedD3D10Open(&d, dev10))
    {
        printf("  FeedD3D10Open failed at %s: 0x%08X\n", d.where != nullptr ? d.where : "?",
               static_cast<unsigned>(d.hr));
        dev10->Release();
        return 2;
    }
    printf("  relay: D3D11 device on adapter LUID %08lX:%08lX, feature level %d_%d\n\n",
           static_cast<unsigned long>(d.luid.HighPart), static_cast<unsigned long>(d.luid.LowPart),
           (d.relay_fl >> 12) & 0xF, (d.relay_fl >> 8) & 0xF);

    // ---------------------------------------------------------------- Q1
    printf("Q1  is a D3D10.1 keyed-mutex shared texture creatable?\n");
    {
        D3D10_TEXTURE2D_DESC km = {};
        km.Width = W; km.Height = H; km.MipLevels = 1; km.ArraySize = 1;
        km.Format = FMT; km.SampleDesc.Count = 1;
        km.Usage = D3D10_USAGE_DEFAULT; km.BindFlags = D3D10_BIND_SHADER_RESOURCE;
        km.MiscFlags = D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ID3D10Texture2D *t = nullptr;
        const HRESULT kmhr = dev10->CreateTexture2D(&km, nullptr, &t);
        printf("  %-48s 0x%08X\n  %s\n", "CreateTexture2D with KEYEDMUTEX", static_cast<unsigned>(kmhr),
               SUCCEEDED(kmhr) ? "-> supported after all; revisit the note in feed_d3d10.h"
                               : "-> rejected, as expected: event queries it is");
        if (t != nullptr) t->Release();
    }

    // ---------------------------------------------------------------- Q2 / Q3, inbound
    printf("\nQ2/Q3  game -> relay\n");

    FeedD3D10Bridge in_bridge;
    Check("bridge created and opened on the relay",
          FeedD3D10MakeBridge(&d, &in_bridge, W, H, FMT, false), d.where);
    Check("relay got a shader resource view on it", in_bridge.srv11 != nullptr);

    UINT *seed = static_cast<UINT *>(malloc(W * H * 4));
    FillPattern(seed, W, H, 1);
    D3D10_SUBRESOURCE_DATA sd = {};
    sd.pSysMem     = seed;
    sd.SysMemPitch = W * 4;
    D3D10_TEXTURE2D_DESC td10 = {};
    td10.Width = W; td10.Height = H; td10.MipLevels = 1; td10.ArraySize = 1;
    td10.Format = FMT; td10.SampleDesc.Count = 1;
    td10.Usage = D3D10_USAGE_DEFAULT; td10.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    ID3D10Texture2D *src10 = nullptr;
    Check("D3D10 source texture", SUCCEEDED(dev10->CreateTexture2D(&td10, &sd, &src10)));

    // The handover under test: queue the copy, then drain the game's device.
    if (src10 != nullptr) FeedD3D10Deposit(&d, &in_bridge, src10);
    Check("FeedD3D10SyncGame (the copy has landed)", src10 != nullptr && FeedD3D10SyncGame(&d));

    D3D11_TEXTURE2D_DESC stg11 = {};
    stg11.Width = W; stg11.Height = H; stg11.MipLevels = 1; stg11.ArraySize = 1;
    stg11.Format = FMT; stg11.SampleDesc.Count = 1;
    stg11.Usage = D3D11_USAGE_STAGING; stg11.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *readback11 = nullptr;
    Check("D3D11 staging texture", SUCCEEDED(d.relay->CreateTexture2D(&stg11, nullptr, &readback11)));

    UINT bad = 0;
    bool pixels_ok = false;
    if (readback11 != nullptr && in_bridge.tex11 != nullptr)
    {
        d.relay_ctx->CopyResource(readback11, in_bridge.tex11);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(d.relay_ctx->Map(readback11, 0, D3D11_MAP_READ, 0, &m)))
        {
            pixels_ok = VerifyPattern(m.pData, W, H, 1, m.RowPitch, &bad);
            d.relay_ctx->Unmap(readback11, 0);
        }
    }
    char detail[64];
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "%u wrong pixels", bad);
    Check("relay reads the game's pixels", pixels_ok, pixels_ok ? nullptr : detail);

    // ---------------------------------------------------------------- Q3, outbound
    printf("\nQ3  relay -> game (the Output bridge)\n");

    FeedD3D10Bridge out_bridge;
    Check("Output bridge created",
          FeedD3D10MakeBridge(&d, &out_bridge, W, H, FMT, true), d.where);
    Check("has a render target view for the blit chain", out_bridge.rtv11 != nullptr);

    UINT *seed2 = static_cast<UINT *>(malloc(W * H * 4));
    FillPattern(seed2, W, H, 2);
    D3D11_SUBRESOURCE_DATA sd11 = {};
    sd11.pSysMem     = seed2;
    sd11.SysMemPitch = W * 4;
    D3D11_TEXTURE2D_DESC td11 = {};
    td11.Width = W; td11.Height = H; td11.MipLevels = 1; td11.ArraySize = 1;
    td11.Format = FMT; td11.SampleDesc.Count = 1;
    td11.Usage = D3D11_USAGE_DEFAULT; td11.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *src11 = nullptr;
    Check("D3D11 source texture", SUCCEEDED(d.relay->CreateTexture2D(&td11, &sd11, &src11)));

    // The relay writes, standing in for BlitOutputToBackbuffer.
    if (src11 != nullptr && out_bridge.tex11 != nullptr)
        d.relay_ctx->CopyResource(out_bridge.tex11, src11);

    // The game takes it home -- the CopyResource that, in the add-on, lands on the game's
    // own render target and touches no pipeline state at all.
    ID3D10Texture2D *home10 = nullptr;
    td10.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
    Check("D3D10 destination texture", SUCCEEDED(dev10->CreateTexture2D(&td10, nullptr, &home10)));
    Check("FeedD3D10Collect (drain the relay, then copy)",
          home10 != nullptr && FeedD3D10Collect(&d, &out_bridge, home10));

    D3D10_TEXTURE2D_DESC stg10 = td10;
    stg10.BindFlags      = 0;
    stg10.Usage          = D3D10_USAGE_STAGING;
    stg10.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    ID3D10Texture2D *readback10 = nullptr;
    hr = dev10->CreateTexture2D(&stg10, nullptr, &readback10);
    bad = 0;
    bool home_ok = false;
    if (SUCCEEDED(hr) && home10 != nullptr)
    {
        dev10->CopyResource(readback10, home10);
        D3D10_MAPPED_TEXTURE2D m10 = {};
        if (SUCCEEDED(readback10->Map(0, D3D10_MAP_READ, 0, &m10)))
        {
            home_ok = VerifyPattern(m10.pData, W, H, 2, m10.RowPitch, &bad);
            readback10->Unmap(0);
        }
    }
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "%u wrong pixels", bad);
    Check("the game reads what the relay wrote", home_ok, home_ok ? nullptr : detail);

    // ---------------------------------------------------------------- Q4: the price
    printf("\nQ4  cost of the event-query crossing\n");

    const UINT BW = 1920, BH = 1080;
    FeedD3D10Bridge big;
    ID3D10Texture2D *big_src = nullptr;
    if (FeedD3D10MakeBridge(&d, &big, BW, BH, FMT, false))
    {
        D3D10_TEXTURE2D_DESC bd = {};
        bd.Width = BW; bd.Height = BH; bd.MipLevels = 1; bd.ArraySize = 1;
        bd.Format = FMT; bd.SampleDesc.Count = 1;
        bd.Usage = D3D10_USAGE_DEFAULT; bd.BindFlags = D3D10_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(dev10->CreateTexture2D(&bd, nullptr, &big_src)))
        {
            const int kIters = 300;
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            bool all = true;
            // Warm up: the first crossing on a fresh resource pays allocation costs the
            // steady state does not.
            for (int i = 0; i < 20; ++i) { FeedD3D10Deposit(&d, &big, big_src); FeedD3D10SyncGame(&d); }
            QueryPerformanceCounter(&t0);
            for (int i = 0; i < kIters; ++i)
            {
                // What one frame's input crossing actually costs: three copies, one drain.
                FeedD3D10Deposit(&d, &big, big_src);
                FeedD3D10Deposit(&d, &big, big_src);
                FeedD3D10Deposit(&d, &big, big_src);
                if (!FeedD3D10SyncGame(&d)) { all = false; break; }
            }
            QueryPerformanceCounter(&t1);
            const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                              static_cast<double>(f.QuadPart) / kIters;
            Check("300 x (3 copies + one drain) at 1920x1080", all);
            printf("  -> %.3f ms per frame for the input crossing at 1080p\n", ms);
            printf("  -> read that as a FLOOR, not as the cost in a game. The drain waits\n");
            printf("     for everything the device has outstanding, and here that is only\n");
            printf("     our own three copies on an otherwise idle GPU. In a game it also\n");
            printf("     waits for the frame the game just queued, so the real price is the\n");
            printf("     lost CPU/GPU pipelining. Measure that in place, not here.\n");
        }
        FeedD3D10ReleaseBridge(&big);
    }

    // ---------------------------------------------------------------- teardown
    if (big_src    != nullptr) big_src->Release();
    if (readback10 != nullptr) readback10->Release();
    if (home10     != nullptr) home10->Release();
    if (src11      != nullptr) src11->Release();
    if (readback11 != nullptr) readback11->Release();
    if (src10      != nullptr) src10->Release();
    FeedD3D10ReleaseBridge(&out_bridge);
    FeedD3D10ReleaseBridge(&in_bridge);
    FeedD3D10Close(&d);
    dev10->Release();
    free(seed2);
    free(seed);

    printf("\n%s\n", g_fail == 0 ? "spike-d3d10client32: all checks passed."
                                 : "spike-d3d10client32: FAILURES ABOVE.");
    return g_fail == 0 ? 0 : 1;
}
