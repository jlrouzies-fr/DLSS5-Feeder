// feed_perf.h -- append FPS/CPU + knobs to dlss5-perf.jsonl next to the add-on.
// oneclick reads this for history + nearest-neighbour expected FPS.
//
// Include after CfgPath / g_cfg / g_feed_ofa_cfg / g_feed_lightstab_cfg / g_feed_auto exist.

#pragma once

#include <ctime>

#ifndef FEED_PERF_MAX_LINES
#define FEED_PERF_MAX_LINES 1500
#endif
#ifndef FEED_PERF_PERIOD_MS
#define FEED_PERF_PERIOD_MS 2000
#endif

static ULONGLONG g_feed_perf_last_write = 0;
static int       g_feed_perf_pending = 0; // set on knob dirty / CfgSave

static void FeedPerfPath(char *out)
{
    GetModuleFileNameA(g_self, out, MAX_PATH);
    if (char *s = strrchr(out, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - out), "dlss5-perf.jsonl");
}

static float FeedPerfCpuPercent()
{
    // Process CPU% over a short window using GetProcessTimes vs wall clock.
    static ULONGLONG last_wall = 0;
    static ULONGLONG last_kernel = 0, last_user = 0;
    FILETIME ct, et, kt, ut;
    if (!GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut))
        return -1.f;
    ULARGE_INTEGER k, u;
    k.LowPart = kt.dwLowDateTime; k.HighPart = kt.dwHighDateTime;
    u.LowPart = ut.dwLowDateTime; u.HighPart = ut.dwHighDateTime;
    const ULONGLONG now = GetTickCount64();
    float pct = -1.f;
    if (last_wall != 0 && now > last_wall)
    {
        const ULONGLONG dwall = (now - last_wall) * 10000ull; // ms → 100ns
        const ULONGLONG dcpu = (k.QuadPart - last_kernel) + (u.QuadPart - last_user);
        if (dwall > 0)
            pct = 100.f * (float)dcpu / (float)dwall;
        if (pct < 0.f) pct = 0.f;
        if (pct > 100.f) pct = 100.f;
    }
    last_wall = now;
    last_kernel = k.QuadPart;
    last_user = u.QuadPart;
    return pct;
}

static void FeedPerfTrim(const char *path)
{
    FILE *f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || f == nullptr) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0 || sz < 256 * 1024) { fclose(f); return; } // small enough
    fseek(f, 0, SEEK_SET);
    // Count lines; if over cap, keep the last FEED_PERF_MAX_LINES.
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = 0;
    int lines = 0;
    for (size_t i = 0; i < nread; ++i) if (buf[i] == '\n') ++lines;
    if (lines <= FEED_PERF_MAX_LINES) { free(buf); return; }
    int skip = lines - FEED_PERF_MAX_LINES;
    char *p = buf;
    while (skip > 0 && *p)
    {
        if (*p == '\n') --skip;
        ++p;
    }
    FILE *w = nullptr;
    if (fopen_s(&w, path, "wb") == 0 && w)
    {
        fputs(p, w);
        fclose(w);
    }
    free(buf);
}

// fps / frame_ms from recent present interval (caller supplies).
static void FeedPerfAppend(float fps, float frame_ms)
{
    char path[MAX_PATH];
    FeedPerfPath(path);
    FILE *f = nullptr;
    if (fopen_s(&f, path, "ab") != 0 || f == nullptr) return;
    const ULONGLONG ts = (ULONGLONG)time(nullptr);
    const float cpu = FeedPerfCpuPercent();
    const char *ap = g_feed_auto.name[0] ? g_feed_auto.name : "";
    fprintf(f,
        "{\"ts\":%llu,\"fps\":%.2f,\"frame_ms\":%.3f,\"cpu_pct\":%.1f,"
        "\"work_resolution\":%d,\"ofa_enabled\":%d,\"ofa_grid\":%d,\"ofa_perf\":%d,"
        "\"reset_mode\":%d,\"light_stab\":%d,\"evaluate_stride\":%d,\"auto_profile\":\"%s\"}\n",
        (unsigned long long)ts,
        fps, frame_ms, cpu,
        g_cfg.work_resolution,
        g_feed_ofa_cfg.enabled ? 1 : 0,
        g_feed_ofa_cfg.grid,
        g_feed_ofa_cfg.perf,
        g_cfg.reset_mode,
        g_feed_lightstab_cfg.enabled ? 1 : 0,
        g_feed_auto.evaluate_stride > 0 ? g_feed_auto.evaluate_stride : 1,
        ap);
    fclose(f);
    static int writes = 0;
    if ((++writes % 32) == 0)
        FeedPerfTrim(path);
}

static void FeedPerfMarkDirty()
{
    g_feed_perf_pending = 1;
}

// Call once per frame from TimingTick with the present interval in ms.
static void FeedPerfTick(double interval_ms)
{
    if (interval_ms < 1.0 || interval_ms > 250.0) return;
    const ULONGLONG now = GetTickCount64();
    const bool due = g_feed_perf_pending ||
                     (g_feed_perf_last_write == 0) ||
                     (now - g_feed_perf_last_write >= FEED_PERF_PERIOD_MS);
    if (!due) return;
    g_feed_perf_pending = 0;
    g_feed_perf_last_write = now;
    const float frame_ms = (float)interval_ms;
    const float fps = frame_ms > 0.1f ? (1000.f / frame_ms) : 0.f;
    FeedPerfAppend(fps, frame_ms);
}
