// feed_async.h -- D3D11 async feed (queue depth ≤ 1, display lag ~1 frame).
//
// Include once for cfg/state (early). Define FEED_ASYNC_IMPL and include again
// LATE (after BeginCommands / SafeEvaluateDLSS / Blit / LightStab) for the worker.
//
// When async_feed=1 the Present path captures inputs and returns without waiting on
// NGX Evaluate. A worker on other cores runs Evaluate; Present blits the last completed
// output (N-1). If the worker is busy, the frame's evaluate is dropped (never queued).
// Off by default. Vulkan keeps async_home — do not map this knob onto Vulkan.

#ifndef FEED_ASYNC_H_
#define FEED_ASYNC_H_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

struct FeedAsyncCfg
{
    int enabled; // async_feed= 0|1
    int key;     // key_async_feed= virtual-key (default VK_F10)
};

static FeedAsyncCfg g_feed_async_cfg = { 0, 0x79 }; // F10

struct FeedHudCfg
{
    int enabled; // hud_stats= 0|1
    int key;     // key_hud= virtual-key (default VK_F9)
};

static FeedHudCfg g_feed_hud_cfg = { 0, 0x78 }; // F9

// Honest metric: 0 when sync, 1 when async path is active.
static int g_display_lag_frames = 0;

static ID3D11Texture2D          *g_async_display = nullptr;
static ID3D11ShaderResourceView *g_async_display_srv = nullptr;
static volatile long             g_async_display_ready = 0;

static HANDLE        g_async_thread = nullptr;
static HANDLE        g_async_wake = nullptr;
static HANDLE        g_async_stop = nullptr;
static volatile long g_async_busy = 0;
static volatile long g_async_alive = 0;
static float         g_async_job_vx = 1.f;
static float         g_async_job_vy = 1.f;
static UINT64        g_async_job_input_fence = 0;
static DWORD         g_async_game_proc = 0;

static bool FeedAsyncBusy()
{
    return InterlockedCompareExchange(&g_async_busy, 0, 0) != 0;
}

static int FeedAsyncDisplayLagFrames()
{
    return (g_feed_async_cfg.enabled != 0) ? 1 : 0;
}

static void FeedAsyncUpdateLagMetric()
{
    g_display_lag_frames = FeedAsyncDisplayLagFrames();
}

static void FeedAsyncNoteGameProc()
{
    g_async_game_proc = GetCurrentProcessorNumber();
}

static void FeedAsyncReleaseDisplay()
{
    if (g_async_display_srv) { g_async_display_srv->Release(); g_async_display_srv = nullptr; }
    if (g_async_display) { g_async_display->Release(); g_async_display = nullptr; }
    InterlockedExchange(&g_async_display_ready, 0);
}

static bool FeedAsyncEnsureDisplay(ID3D11Device *dev, ID3D11Texture2D *output_template)
{
    if (dev == nullptr || output_template == nullptr)
        return false;
    D3D11_TEXTURE2D_DESC td = {};
    output_template->GetDesc(&td);
    if (g_async_display != nullptr)
    {
        D3D11_TEXTURE2D_DESC cur = {};
        g_async_display->GetDesc(&cur);
        if (cur.Width == td.Width && cur.Height == td.Height && cur.Format == td.Format)
            return g_async_display_srv != nullptr;
        FeedAsyncReleaseDisplay();
    }
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = 0;
    td.CPUAccessFlags = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_async_display)) || g_async_display == nullptr)
    {
        Log("[feed] async_feed: display texture create failed");
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(g_async_display, &svd, &g_async_display_srv)))
    {
        FeedAsyncReleaseDisplay();
        Log("[feed] async_feed: display SRV create failed");
        return false;
    }
    return true;
}

static bool FeedAsyncHasDisplay()
{
    return InterlockedCompareExchange(&g_async_display_ready, 0, 0) != 0 &&
           g_async_display_srv != nullptr;
}

static void FeedAsyncShutdownWorker()
{
    if (g_async_stop != nullptr)
        SetEvent(g_async_stop);
    if (g_async_thread != nullptr)
    {
        WaitForSingleObject(g_async_thread, 5000);
        CloseHandle(g_async_thread);
        g_async_thread = nullptr;
    }
    if (g_async_wake != nullptr) { CloseHandle(g_async_wake); g_async_wake = nullptr; }
    if (g_async_stop != nullptr) { CloseHandle(g_async_stop); g_async_stop = nullptr; }
    InterlockedExchange(&g_async_busy, 0);
    InterlockedExchange(&g_async_alive, 0);
    InterlockedExchange(&g_async_display_ready, 0);
}

// Stub until FEED_ASYNC_IMPL (needs SLOT_OUTPUT / g.tex11).
static void FeedAsyncCopyOutputToDisplay(ID3D11DeviceContext *ctx);

#endif // FEED_ASYNC_H_

#if defined(FEED_ASYNC_IMPL) && !defined(FEED_ASYNC_IMPL_DONE)
#define FEED_ASYNC_IMPL_DONE

// Defined just above the FEED_ASYNC_IMPL include in dlss5-feed.cpp.
static bool FeedAsyncEvaluateJob(float vel_scale_x, float vel_scale_y);

static void FeedAsyncApplyAffinity(HANDLE th)
{
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || proc == 0)
        return;
    DWORD_PTR mask = proc;
    if (g_async_game_proc < 64)
        mask &= ~(static_cast<DWORD_PTR>(1) << g_async_game_proc);
    if (mask == 0)
        mask = proc;
    SetThreadAffinityMask(th, mask);
}

static DWORD WINAPI FeedAsyncThreadProc(void *)
{
    FeedAsyncApplyAffinity(GetCurrentThread());
    for (;;)
    {
        HANDLE waits[2] = { g_async_stop, g_async_wake };
        const DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0)
            break;
        if (wr != WAIT_OBJECT_0 + 1)
            continue;
        FeedAsyncEvaluateJob(g_async_job_vx, g_async_job_vy);
        InterlockedExchange(&g_async_busy, 0);
    }
    InterlockedExchange(&g_async_alive, 0);
    return 0;
}

static void FeedAsyncEnsureWorker()
{
    if (g_async_thread != nullptr)
        return;
    g_async_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_async_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_async_wake == nullptr || g_async_stop == nullptr)
        return;
    InterlockedExchange(&g_async_alive, 1);
    g_async_thread = CreateThread(nullptr, 0, FeedAsyncThreadProc, nullptr, 0, nullptr);
    if (g_async_thread == nullptr)
    {
        InterlockedExchange(&g_async_alive, 0);
        CloseHandle(g_async_wake); g_async_wake = nullptr;
        CloseHandle(g_async_stop); g_async_stop = nullptr;
        Log("[feed] async_feed: could not start worker thread; staying sync");
        g_feed_async_cfg.enabled = 0;
        return;
    }
    Log("[feed] async_feed: worker started (display lag ~1 frame when enabled)");
}

static void FeedAsyncCopyOutputToDisplay(ID3D11DeviceContext *ctx)
{
    if (ctx == nullptr || g_async_display == nullptr || g.tex11[SLOT_OUTPUT] == nullptr)
        return;
    ctx->CopyResource(g_async_display, g.tex11[SLOT_OUTPUT]);
    InterlockedExchange(&g_async_display_ready, 1);
}

// Present: try to hand work to the worker. Returns true if submitted, false if dropped (busy).
static bool FeedAsyncTrySubmit(float vel_scale_x, float vel_scale_y, UINT64 input_fence)
{
    if (g_feed_async_cfg.enabled == 0)
        return false;
    FeedAsyncEnsureWorker();
    if (g_async_thread == nullptr)
        return false;
    if (InterlockedCompareExchange(&g_async_busy, 1, 0) != 0)
        return false; // queue depth ≤ 1: drop, never grow
    g_async_job_vx = vel_scale_x;
    g_async_job_vy = vel_scale_y;
    g_async_job_input_fence = input_fence;
    SetEvent(g_async_wake);
    return true;
}

#endif // FEED_ASYNC_IMPL
