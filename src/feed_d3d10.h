// feed_d3d10.h - the Direct3D 10.1 <-> Direct3D 11 bridge for the 32-bit add-on.
//
// D3D10 cannot reach the host's transport on its own, and not for one reason but three:
//
//  * NT-handle sharing (D3D11_RESOURCE_MISC_SHARED_NTHANDLE) is D3D11.1+. A D3D10
//    device has only the LEGACY shared handle from IDXGIResource::GetSharedHandle, and
//    ID3D12Device::OpenSharedHandle refuses those -- so the host, whose device is
//    D3D12, can never open a texture a D3D10 device made.
//  * D3D10 has no fence of any kind. There is no ID3D11Device5::OpenSharedFence and no
//    ID3D11DeviceContext4::Signal/Wait, which is how every other client here tells the
//    host that a frame is ready and learns that the answer has landed.
//  * No UAVs and no compute, so the Output slot could not even be written directly.
//
// The way through is a PRIVATE D3D11 RELAY DEVICE, created by us, in the game's process,
// on the game's adapter. The game's D3D10.1 device and the relay share textures the only
// way those two APIs can, and from the relay onwards the existing, proven D3D11 client
// runs completely unchanged.
//
//   game D3D10.1 device
//     -> legacy shared bridge textures            (this header)
//     -> private D3D11 relay device at FL11_0     (this header)
//     -> the existing NT-shared set + D3D12 fence, blit chain and pipe  (untouched)
//     -> host64\dlss5-feed-host64.exe
//
// WHY NOT KEYED MUTEXES. LEGACY-API-ADAPTER.md says "D3D10.1 can use IDXGIKeyedMutex;
// D3D10.0 needs explicit Flush/event-query synchronization". On real hardware the first
// half of that is not true: spike\spike-d3d10client32.cpp creates a genuine 10.1 device
// (D3D10CreateDevice1 at D3D10_FEATURE_LEVEL_10_1 succeeds) and every single
// CreateTexture2D carrying D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX comes back E_INVALIDARG
// -- every bind-flag combination, every format, with or without MISC_SHARED alongside --
// while the same call on a D3D11 device succeeds and plain legacy MISC_SHARED on the same
// D3D10 device succeeds. Modern drivers simply do not implement that legacy path.
//
// So BOTH D3D10 versions take the event-query route, and this header has no fast path it
// is quietly failing to use. The cost is honest and worth stating: an event query drains
// the whole device, so each of the two synchronisation points below waits for that
// device's outstanding GPU work to finish. That is one pipeline bubble per device per
// frame, and it is the price of an API with no fence. It is also why the two crossings
// are batched -- all three input copies share ONE FeedD3D10SyncGame, rather than paying
// for a drain each.
//
// Ordering, which is subtler than it looks with no fence anywhere:
//
//   1. game copies Colour/MV/Depth into the input bridges   (D3D10 device)
//   2. FeedD3D10SyncGame  -- those copies have landed
//   3. relay copies the bridges into the shared set, signals the host's fence, and
//      later draws the returned frame into the Output bridge      (relay device)
//   4. FeedD3D10SyncRelay -- that drawing has landed
//   5. game copies the Output bridge into its own render target   (D3D10 device)
//
// Step 3's READ of the input bridges is ordered against the next frame's step 1 by that
// frame's step 4; step 5's read of the Output bridge is ordered against the next frame's
// step 3 by that frame's step 2. Two drains a frame cover all four hazards.
//
// One more consequence, and it is what keeps this small: there is no new FeedClientKind
// and no FEED_IPC_VERSION bump. To the host this is a D3D11 client, because by the time
// the host can see it, it is one. The relay is created at feature level 11_0, so the
// ID3D11Device1 / ID3D11Device5 / ID3D11DeviceContext4 queries the D3D11 client
// hard-fails on all succeed, and the game's own device is asked for nothing but copies.
//
// The frame also comes home by ID3D10Device::CopyResource, NOT by a D3D10 draw: the relay
// runs the whole existing blit chain (scaling, FSR1 EASU/RCAS) into a backbuffer-sized
// bridge, so the D3D10 side never binds a shader, a viewport or a render target. That
// matters more here than anywhere else in this project, because D3D10 has no
// ID3D10DeviceContext -- state lives directly on the device, so anything we set the game
// would silently inherit.
//
// Like feed_gl.h and feed_vk.h this header is self-contained and adds no link-time
// dependency: every call below is an interface method on a device the game already
// created, never an export of d3d10_1.dll. It logs nothing -- failures come back as
// false with hr/where set, so the caller reports them in its own voice.

#pragma once
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstring>

// A drain that never returns would hang the game's render thread for good, so both waits
// are bounded and the caller treats a false as a failed frame. Two seconds is the same
// deadline HostDrain uses for the fence it cannot read.
#define FEED_D3D10_SYNC_MS 2000

struct FeedD3D10Bridge
{
    ID3D10Texture2D          *tex10;   // the game's side; owns the shared handle's lifetime
    ID3D11Texture2D          *tex11;   // the relay's alias of exactly the same memory
    ID3D11ShaderResourceView *srv11;   // the work-resolution resample reads MV and Depth
    ID3D11RenderTargetView   *rtv11;   // Output only: the blit chain draws straight into it
    UINT                      w, h;
    DXGI_FORMAT               fmt;
};

struct FeedD3D10
{
    bool                 ok;
    ID3D10Device1       *game;        // not owned: it is the game's, and the game outlives us
    ID3D11Device        *relay;       // owned
    ID3D11DeviceContext *relay_ctx;   // owned
    ID3D10Query         *q10;         // the two drains, made once rather than per frame
    ID3D11Query         *q11;
    D3D_FEATURE_LEVEL    relay_fl;
    LUID                 luid;        // the adapter both devices are on
    HRESULT              hr;          // last failure ...
    const char          *where;       // ... and the call that produced it
};

static void FeedD3D10Fail(FeedD3D10 *d, const char *where, HRESULT hr)
{
    d->where = where;
    d->hr    = hr;
}

// Open the bridge against the game's device.
static bool FeedD3D10Open(FeedD3D10 *d, ID3D10Device *game_device)
{
    memset(d, 0, sizeof(*d));
    if (game_device == NULL) { FeedD3D10Fail(d, "no D3D10 device", E_POINTER); return false; }

    // 10.1 is not required for the transport -- the event-query route works on 10.0 too --
    // but it is what the shader model and the resample below assume, and a 10.0-only
    // device in 2026 means something stranger is going on than this add-on should guess at.
    HRESULT hr = game_device->QueryInterface(__uuidof(ID3D10Device1), (void **)&d->game);
    if (FAILED(hr)) { FeedD3D10Fail(d, "ID3D10Device1 (this is a Direct3D 10.0 device)", hr); return false; }
    d->game->Release();   // not owned; the QI reference would outlive our interest in it

    // Match the game's adapter. A private device on a different GPU could not open the
    // game's shared textures at all, and on a machine with more than one adapter the
    // default is routinely the wrong one -- LEGACY-API-ADAPTER.md, "match the host
    // adapter LUID for every private device".
    IDXGIDevice *dxgi = NULL;
    hr = d->game->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi);
    if (FAILED(hr)) { FeedD3D10Fail(d, "IDXGIDevice", hr); return false; }
    IDXGIAdapter *adapter = NULL;
    hr = dxgi->GetAdapter(&adapter);
    dxgi->Release();
    if (FAILED(hr)) { FeedD3D10Fail(d, "GetAdapter", hr); return false; }
    DXGI_ADAPTER_DESC ad;
    memset(&ad, 0, sizeof(ad));
    adapter->GetDesc(&ad);
    d->luid = ad.AdapterLuid;

    // Feature level 11_0 is the floor, not a preference: the Output slot is created with
    // a UAV bind, which is a feature-level 11 feature, and the whole point of the relay
    // is to be the modern device the game is not. Naming an explicit adapter means the
    // driver type has to be UNKNOWN.
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                           levels, (UINT)(sizeof(levels) / sizeof(levels[0])),
                           D3D11_SDK_VERSION, &d->relay, &d->relay_fl, &d->relay_ctx);
    adapter->Release();
    if (FAILED(hr)) { FeedD3D10Fail(d, "D3D11CreateDevice (relay)", hr); return false; }

    D3D10_QUERY_DESC qd10;
    memset(&qd10, 0, sizeof(qd10));
    qd10.Query = D3D10_QUERY_EVENT;
    hr = d->game->CreateQuery(&qd10, &d->q10);
    if (FAILED(hr)) { FeedD3D10Fail(d, "D3D10 CreateQuery (event)", hr); return false; }

    D3D11_QUERY_DESC qd11;
    memset(&qd11, 0, sizeof(qd11));
    qd11.Query = D3D11_QUERY_EVENT;
    hr = d->relay->CreateQuery(&qd11, &d->q11);
    if (FAILED(hr)) { FeedD3D10Fail(d, "D3D11 CreateQuery (event)", hr); return false; }

    d->ok = true;
    return true;
}

static void FeedD3D10ReleaseBridge(FeedD3D10Bridge *b)
{
    if (b->rtv11 != NULL) { b->rtv11->Release(); b->rtv11 = NULL; }
    if (b->srv11 != NULL) { b->srv11->Release(); b->srv11 = NULL; }
    if (b->tex11 != NULL) { b->tex11->Release(); b->tex11 = NULL; }
    // The legacy shared handle is not an NT handle: it belongs to the resource and must
    // never be CloseHandle'd. Releasing tex10 is what disposes of it.
    if (b->tex10 != NULL) { b->tex10->Release(); b->tex10 = NULL; }
    b->w   = 0;
    b->h   = 0;
    b->fmt = DXGI_FORMAT_UNKNOWN;
}

static void FeedD3D10Close(FeedD3D10 *d)
{
    if (d->q11 != NULL) { d->q11->Release(); d->q11 = NULL; }
    if (d->q10 != NULL) { d->q10->Release(); d->q10 = NULL; }
    if (d->relay_ctx != NULL)
    {
        d->relay_ctx->ClearState();
        d->relay_ctx->Flush();
        d->relay_ctx->Release();
        d->relay_ctx = NULL;
    }
    if (d->relay != NULL) { d->relay->Release(); d->relay = NULL; }
    d->game = NULL;   // never owned
    d->ok   = false;
}

// Create one bridge texture on the game's device and open it on the relay. fmt must be
// TYPED -- a shared resource cannot be typeless -- which is also why the caller passes
// the same typed formats the shared set already uses.
static bool FeedD3D10MakeBridge(FeedD3D10 *d, FeedD3D10Bridge *b,
                                UINT w, UINT h, DXGI_FORMAT fmt, bool want_rtv)
{
    memset(b, 0, sizeof(*b));
    b->w   = w;
    b->h   = h;
    b->fmt = fmt;

    D3D10_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D10_USAGE_DEFAULT;
    // SHADER_RESOURCE so the relay can sample MV and Depth when the work resolution is
    // below 100%; RENDER_TARGET on the Output so the blit chain can draw into it. The
    // D3D11 alias inherits both -- OpenSharedResource cannot add bind flags of its own.
    td.BindFlags        = D3D10_BIND_SHADER_RESOURCE | (want_rtv ? D3D10_BIND_RENDER_TARGET : 0);
    td.MiscFlags        = D3D10_RESOURCE_MISC_SHARED;   // legacy; see the header note

    HRESULT hr = d->game->CreateTexture2D(&td, NULL, &b->tex10);
    if (FAILED(hr)) { FeedD3D10Fail(d, "D3D10 CreateTexture2D (bridge)", hr); return false; }

    IDXGIResource *res = NULL;
    hr = b->tex10->QueryInterface(__uuidof(IDXGIResource), (void **)&res);
    if (FAILED(hr)) { FeedD3D10Fail(d, "IDXGIResource (bridge)", hr); return false; }
    HANDLE shared = NULL;
    hr = res->GetSharedHandle(&shared);
    res->Release();
    if (FAILED(hr) || shared == NULL)
    { FeedD3D10Fail(d, "GetSharedHandle (bridge)", FAILED(hr) ? hr : E_FAIL); return false; }

    hr = d->relay->OpenSharedResource(shared, __uuidof(ID3D11Texture2D), (void **)&b->tex11);
    if (FAILED(hr)) { FeedD3D10Fail(d, "OpenSharedResource (relay)", hr); return false; }

    hr = d->relay->CreateShaderResourceView(b->tex11, NULL, &b->srv11);
    if (FAILED(hr)) { FeedD3D10Fail(d, "CreateShaderResourceView (bridge)", hr); return false; }

    if (want_rtv)
    {
        hr = d->relay->CreateRenderTargetView(b->tex11, NULL, &b->rtv11);
        if (FAILED(hr)) { FeedD3D10Fail(d, "CreateRenderTargetView (bridge)", hr); return false; }
    }
    return true;
}

// Drain the game's device. Everything it had outstanding -- including the copies just
// queued into the bridges -- has finished on the GPU when this returns true.
static bool FeedD3D10SyncGame(FeedD3D10 *d)
{
    if (d->q10 == NULL || d->game == NULL) return false;
    d->q10->End();
    d->game->Flush();
    BOOL done = FALSE;
    const ULONGLONG deadline = GetTickCount64() + FEED_D3D10_SYNC_MS;
    HRESULT hr;
    while ((hr = d->q10->GetData(&done, sizeof(done), 0)) == S_FALSE)
        if (GetTickCount64() > deadline) return false;
    return SUCCEEDED(hr);
}

// The same for the relay.
static bool FeedD3D10SyncRelay(FeedD3D10 *d)
{
    if (d->q11 == NULL || d->relay_ctx == NULL) return false;
    d->relay_ctx->End(d->q11);
    d->relay_ctx->Flush();
    BOOL done = FALSE;
    const ULONGLONG deadline = GetTickCount64() + FEED_D3D10_SYNC_MS;
    HRESULT hr;
    while ((hr = d->relay_ctx->GetData(d->q11, &done, sizeof(done), 0)) == S_FALSE)
        if (GetTickCount64() > deadline) return false;
    return SUCCEEDED(hr);
}

// Queue one input copy. Deliberately unsynchronised: the caller queues all three and
// then pays for a single FeedD3D10SyncGame.
static void FeedD3D10Deposit(FeedD3D10 *d, FeedD3D10Bridge *b, ID3D10Resource *src)
{
    if (d->game != NULL && b->tex10 != NULL && src != NULL) d->game->CopyResource(b->tex10, src);
}

// Relay -> game: wait for the relay to have finished writing the bridge, then let the
// game copy it into its own render target. That copy is what puts the neural frame on
// screen, and it touches no pipeline state at all.
static bool FeedD3D10Collect(FeedD3D10 *d, FeedD3D10Bridge *b, ID3D10Resource *dst)
{
    if (!FeedD3D10SyncRelay(d)) return false;
    if (d->game == NULL || b->tex10 == NULL || dst == NULL) return false;
    d->game->CopyResource(dst, b->tex10);
    return true;
}
