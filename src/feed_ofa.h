// feed_ofa.h -- D3D11 NVIDIA Optical Flow (NVOFA) motion vectors for DLSS5-Feeder.
//
// Ported from NIGos/dlss5-bridge synth.inc OFA path (D3D11 only). Header-only;
// include from dlss5-feed.cpp AFTER Log()/Warn() are defined.
//
// Returns full-res RG16F delta-UV motion vectors (prev = uv + mv), same convention
// as Lumenite -- bridge measured no sign flip with inputFrame=current,
// referenceFrame=previous. Default cfg is OFF so Lumenite remains the MV source
// until the caller enables this path.

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>

// Log() must already be defined by the including translation unit (dlss5-feed.cpp).

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

struct FeedOfaCfg
{
    int enabled;   // 0/1
    int grid;      // 1|2|4 (0 = engine off)
    int perf;      // 5 SLOW | 10 MEDIUM | 20 FAST
};

static FeedOfaCfg g_feed_ofa_cfg = { 0, 2, 10 };

static bool FeedOfaReady();
static void FeedOfaShutdown();

// Borrowed pointer to the module-owned RG16F full-res MV texture, or nullptr.
// Do NOT AddRef -- lifetime is owned here until shutdown/rebuild.
static ID3D11Texture2D *FeedOfaUpdate(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                      ID3D11Texture2D *color_src, DXGI_FORMAT color_fmt,
                                      UINT w, UINT h);

// ---------------------------------------------------------------------------
// Local helpers (self-contained; do not depend on bridge.inc)
// Anonymous namespace so FormatName does not collide with dlss5-feed.cpp's own.
// ---------------------------------------------------------------------------

namespace {

static const char *OfaFormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R11G11B10_FLOAT:       return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8X8_UNORM:        return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_R16G16_FLOAT:          return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_SINT:           return "R16G16_SINT";
    case DXGI_FORMAT_R32_FLOAT:             return "R32_FLOAT";
    default:                                return "?";
    }
}

static DXGI_FORMAT FeedOfaTypedFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    default:
        return f;
    }
}

static bool FeedOfaFormatCanExceedOne(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}

typedef HRESULT (WINAPI *PFN_FeedOfa_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static PFN_FeedOfa_D3DCompile LoadSystemD3DCompiler()
{
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    if (m == nullptr) return nullptr;
    return reinterpret_cast<PFN_FeedOfa_D3DCompile>(GetProcAddress(m, "D3DCompile"));
}

} // namespace (FormatName / LoadSystemD3DCompiler)

// ---------------------------------------------------------------------------
// NVOFA ABI (hand-written; measured against nvofapi64.dll D3D11 entry)
// ---------------------------------------------------------------------------

typedef long NvOfStatus;
static const NvOfStatus kOfaSuccess = 0;
static const NvOfStatus kOfaRaised  = 0x7FFFFFFF;

struct NvOfInitParams
{
    uint32_t width;               //  0
    uint32_t height;              //  4
    uint32_t outGridSize;         //  8   1, 2 or 4
    uint32_t hintGridSize;        // 12
    uint32_t mode;                // 16   1 = NV_OF_MODE_OPTICALFLOW
    uint32_t perfLevel;           // 20   5 SLOW, 10 MEDIUM, 20 FAST
    uint32_t enableExternalHints; // 24
    uint32_t enableOutputCost;    // 28
    void    *hPrivData;           // 32
    uint32_t disparityRange;      // 40
    uint32_t enableRoi;           // 44
};

struct NvOfExecuteInputParams
{
    void    *inputFrame;           //  0  CURRENT
    void    *referenceFrame;       //  8  PREVIOUS
    void    *externalHints;        // 16
    uint32_t disableTemporalHints; // 24
    uint32_t padding;              // 28
    void    *hPrivData;            // 32
    uint32_t padding2;             // 40
    uint32_t numRois;              // 44
    void    *roiData;              // 48
};

struct NvOfExecuteOutputParams { void *outputBuffer; void *outputCostBuffer; void *hPrivData; };

static_assert(sizeof(NvOfInitParams) == 48 && offsetof(NvOfInitParams, hPrivData) == 32,
              "NvOfInitParams does not match the layout measured against the driver");
static_assert(sizeof(NvOfExecuteInputParams) == 56 &&
              offsetof(NvOfExecuteInputParams, numRois) == 44,
              "NvOfExecuteInputParams does not match the layout measured against the driver");

typedef NvOfStatus (__stdcall *PFN_OfaCreateInstance)(uint32_t, void *);
typedef NvOfStatus (__stdcall *PFN_OfaCreateD3D11)(ID3D11Device *, ID3D11DeviceContext *, void **);
typedef NvOfStatus (__stdcall *PFN_OfaInit)(void *, const NvOfInitParams *);
typedef NvOfStatus (__stdcall *PFN_OfaRegister)(void *, ID3D11Resource *, void **);
typedef NvOfStatus (__stdcall *PFN_OfaUnregister)(void *);
typedef NvOfStatus (__stdcall *PFN_OfaExecute)(void *, const NvOfExecuteInputParams *,
                                               NvOfExecuteOutputParams *);
typedef NvOfStatus (__stdcall *PFN_OfaDestroy)(void *);
typedef NvOfStatus (__stdcall *PFN_OfaLastError)(void *, char *, uint32_t *);

static const char *OfaStatusName(NvOfStatus s)
{
    switch (s)
    {
    case 0:  return "SUCCESS";
    case 1:  return "INVALID_PTR";
    case 2:  return "INVALID_PARAM";
    case 3:  return "INVALID_CALL";
    case 4:  return "INVALID_VERSION";
    case 5:  return "OUT_OF_MEMORY";
    case 6:  return "NOT_INITIALIZED";
    case 7:  return "UNSUPPORTED_FEATURE";
    case 8:  return "GENERIC";
    case 9:  return "OF_NOT_AVAILABLE";
    case 10: return "UNSUPPORTED_DEVICE";
    case 11: return "DEVICE_DOES_NOT_EXIST";
    case static_cast<NvOfStatus>(0x80004005): return "E_FAIL, not an NV_OF_STATUS";
    default: return "a status this build has no name for";
    }
}

static const char *FeedOfaPerfName(int p)
{
    return p == 5 ? "SLOW" : p == 10 ? "MEDIUM" : p == 20 ? "FAST" : "?";
}

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------

struct FeedOfaState
{
    bool    tried;
    bool    ready;
    bool    stopped;
    bool    primed;
    bool    first_said;
    HMODULE lib;
    void   *slot[64];
    void   *session;
    UINT    w, h, grid;
    int     perf;
    DXGI_FORMAT back_fmt;
    ID3D11Device *dev;          // bare address for comparison only
    void   *reg_src[2], *reg_flow;
    ID3D11Texture2D *copy, *src[2], *flow, *mv;
    ID3D11ShaderResourceView  *copy_srv, *flow_srv;
    ID3D11UnorderedAccessView *src_uav[2], *mv_uav;
    ID3D11ComputeShader *luma_cs, *flow_cs;
    int    cur;
    int    fails;
    UINT64 frames;
};

static FeedOfaState g_ofa = {};

static bool FeedOfaReady() { return g_ofa.ready; }

// ---------------------------------------------------------------------------
// SEH wrappers (EXCEPTION_EXECUTE_HANDLER -- no CaptureFault)
// ---------------------------------------------------------------------------

static NvOfStatus OfaSafeCreateInstance(PFN_OfaCreateInstance fn, uint32_t ver, void *slots)
{
    __try { return fn(ver, slots); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfaRaised; }
}

static NvOfStatus OfaSafeCreateSession(PFN_OfaCreateD3D11 fn, ID3D11Device *d,
                                       ID3D11DeviceContext *c, void **out)
{
    __try { return fn(d, c, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfaRaised; }
}

static NvOfStatus OfaSafeInit(PFN_OfaInit fn, void *s, const NvOfInitParams *p)
{
    __try { return fn(s, p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfaRaised; }
}

static NvOfStatus OfaSafeRegister(PFN_OfaRegister fn, void *s, ID3D11Resource *r, void **out)
{
    __try { return fn(s, r, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfaRaised; }
}

static NvOfStatus OfaSafeUnregister(PFN_OfaUnregister fn, void *h)
{
    __try { return fn(h); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfaRaised; }
}

static NvOfStatus OfaSafeDestroy(PFN_OfaDestroy fn, void *s)
{
    __try { return fn(s); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfaRaised; }
}

static NvOfStatus OfaSafeExecute(PFN_OfaExecute fn, void *s, const NvOfExecuteInputParams *in,
                                 NvOfExecuteOutputParams *out)
{
    __try { return fn(s, in, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kOfaRaised; }
}

static void OfaLastError(char *dst, size_t cap)
{
    dst[0] = '\0';
    if (g_ofa.session == nullptr || g_ofa.slot[8] == nullptr) return;
    uint32_t n = static_cast<uint32_t>(cap);
    auto fn = reinterpret_cast<PFN_OfaLastError>(g_ofa.slot[8]);
    __try { fn(g_ofa.session, dst, &n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { dst[0] = '\0'; }
    dst[cap - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Teardown (session + GPU objects; DLL kept until FeedOfaShutdown)
// ---------------------------------------------------------------------------

static void FeedOfaClose()
{
    auto unreg = reinterpret_cast<PFN_OfaUnregister>(g_ofa.slot[5]);
    if (unreg != nullptr)
    {
        for (int i = 0; i < 2; ++i)
            if (g_ofa.reg_src[i] != nullptr) OfaSafeUnregister(unreg, g_ofa.reg_src[i]);
        if (g_ofa.reg_flow != nullptr) OfaSafeUnregister(unreg, g_ofa.reg_flow);
    }
    g_ofa.reg_src[0] = g_ofa.reg_src[1] = g_ofa.reg_flow = nullptr;

    if (g_ofa.copy_srv != nullptr) { g_ofa.copy_srv->Release(); g_ofa.copy_srv = nullptr; }
    if (g_ofa.flow_srv != nullptr) { g_ofa.flow_srv->Release(); g_ofa.flow_srv = nullptr; }
    if (g_ofa.mv_uav   != nullptr) { g_ofa.mv_uav->Release();   g_ofa.mv_uav   = nullptr; }
    for (int i = 0; i < 2; ++i)
        if (g_ofa.src_uav[i] != nullptr) { g_ofa.src_uav[i]->Release(); g_ofa.src_uav[i] = nullptr; }

    if (g_ofa.copy != nullptr) { g_ofa.copy->Release(); g_ofa.copy = nullptr; }
    for (int i = 0; i < 2; ++i)
        if (g_ofa.src[i] != nullptr) { g_ofa.src[i]->Release(); g_ofa.src[i] = nullptr; }
    if (g_ofa.flow != nullptr) { g_ofa.flow->Release(); g_ofa.flow = nullptr; }
    if (g_ofa.mv   != nullptr) { g_ofa.mv->Release();   g_ofa.mv   = nullptr; }

    if (g_ofa.luma_cs != nullptr) { g_ofa.luma_cs->Release(); g_ofa.luma_cs = nullptr; }
    if (g_ofa.flow_cs != nullptr) { g_ofa.flow_cs->Release(); g_ofa.flow_cs = nullptr; }

    if (g_ofa.session != nullptr)
    {
        auto destroy = reinterpret_cast<PFN_OfaDestroy>(g_ofa.slot[7]);
        if (destroy != nullptr) OfaSafeDestroy(destroy, g_ofa.session);
        g_ofa.session = nullptr;
    }

    g_ofa.ready  = false;
    g_ofa.primed = false;
    g_ofa.fails  = 0;
    g_ofa.cur    = 0;
    g_ofa.dev    = nullptr;
}

static void FeedOfaShutdown()
{
    FeedOfaClose();
    if (g_ofa.lib != nullptr)
    {
        FreeLibrary(g_ofa.lib);
        g_ofa.lib = nullptr;
    }
    memset(g_ofa.slot, 0, sizeof(g_ofa.slot));
    g_ofa.tried      = false;
    g_ofa.stopped    = false;
    g_ofa.first_said = false;
    g_ofa.frames     = 0;
    g_ofa.w = g_ofa.h = g_ofa.grid = 0;
    g_ofa.perf = 0;
    g_ofa.back_fmt = DXGI_FORMAT_UNKNOWN;
}

// ---------------------------------------------------------------------------
// Shaders (HLSL copied from bridge kOfaLuma* / kOfaFlowSrc)
// ---------------------------------------------------------------------------

static const char kOfaLumaHead[] =
    "Texture2D<float4>   src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    uint w, h; dst.GetDimensions(w, h);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    float3 c = src[id.xy].rgb;\n"
    "    float  l = dot(c, float3(0.2126, 0.7152, 0.0722));\n";
static const char kOfaLumaTonemap[] = "    l = l / (l + 1.0);\n";
static const char kOfaLumaClamp[]   = "    l = saturate(l);\n";
static const char kOfaLumaTail[] =
    "    dst[id.xy] = float4(l, l, l, 1.0);\n"
    "}\n";

static const char kOfaFlowSrc[] =
    "Texture2D<int2>     flow : register(t0);\n"
    "RWTexture2D<float2> dst  : register(u0);\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    uint w, h, fw, fh;\n"
    "    dst.GetDimensions(w, h);\n"
    "    flow.GetDimensions(fw, fh);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    uint2 c = min(uint2(id.x * fw / w, id.y * fh / h), uint2(fw - 1, fh - 1));\n"
    "    dst[id.xy] = float2(flow[c]) * (1.0 / 32.0) / float2(w, h);\n"
    "}\n";

static bool OfaMakeShader(ID3D11Device *dev11, const char *src, const char *name,
                          ID3D11ComputeShader **out)
{
    if (*out != nullptr) return true;

    PFN_FeedOfa_D3DCompile compile = LoadSystemD3DCompiler();
    if (compile == nullptr)
    {
        Log("[feed] optical flow: d3dcompiler_47.dll is unavailable, so the flow "
            "conversion shader cannot be built");
        return false;
    }

    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                         &code, &err);
    if (FAILED(hr) || code == nullptr)
    {
        Log("[feed] optical flow shader compile failed 0x%08X: %s", hr,
            err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    hr = dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
    code->Release();
    if (FAILED(hr))
    {
        Log("[feed] optical flow CreateComputeShader failed 0x%08X", hr);
        return false;
    }
    return true;
}

static ID3D11Texture2D *OfaMakeTexture(ID3D11Device *dev11, UINT w, UINT h, DXGI_FORMAT fmt,
                                       UINT bind, const char *what)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;

    ID3D11Texture2D *t = nullptr;
    const HRESULT hr = dev11->CreateTexture2D(&td, nullptr, &t);
    if (FAILED(hr))
    {
        Log("[feed] optical flow: could not create the %ux%u %s texture for %s: 0x%08X",
            w, h, OfaFormatName(fmt), what, hr);
        return nullptr;
    }
    return t;
}

// ---------------------------------------------------------------------------
// Session open
// ---------------------------------------------------------------------------

static bool FeedOfaOpen(ID3D11Device *dev11, ID3D11DeviceContext *ctx, UINT w, UINT h,
                        DXGI_FORMAT back_fmt)
{
    g_ofa.tried = true;
    g_ofa.w = w; g_ofa.h = h; g_ofa.back_fmt = back_fmt; g_ofa.dev = dev11;
    g_ofa.grid = static_cast<UINT>(g_feed_ofa_cfg.grid);
    g_ofa.perf = g_feed_ofa_cfg.perf;

    if (g_ofa.grid == 0)
    {
        Log("[feed] optical flow: grid is 0, which switches the engine off, so "
            "the session is not opened.");
        return false;
    }

    if (g_ofa.lib == nullptr)
    {
        g_ofa.lib = LoadLibraryW(L"nvofapi64.dll");
        if (g_ofa.lib == nullptr)
        {
            Log("[feed] optical flow: nvofapi64.dll is not loadable in this process. It is "
                "installed with the display driver, so this means no NVIDIA driver here "
                "rather than a driver too old.");
            return false;
        }
        auto create = reinterpret_cast<PFN_OfaCreateInstance>(
            GetProcAddress(g_ofa.lib, "NvOFAPICreateInstanceD3D11"));
        if (create == nullptr)
        {
            Log("[feed] optical flow: nvofapi64.dll exports no NvOFAPICreateInstanceD3D11.");
            return false;
        }
        const NvOfStatus r = OfaSafeCreateInstance(create, 0x20, g_ofa.slot);
        if (r != kOfaSuccess)
        {
            Log("[feed] optical flow: this driver refused API version 0x20 with status %ld "
                "(%s). The D3D11 entry accepts 0x10 through 0x50 on the driver this was "
                "measured against.", r, OfaStatusName(r));
            memset(g_ofa.slot, 0, sizeof(g_ofa.slot));
            return false;
        }
    }

    {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 f2 = {};
        f2.InFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (FAILED(dev11->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &f2, sizeof(f2))) ||
            (f2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
        {
            Log("[feed] optical flow: this device cannot store to a B8G8R8A8_UNORM "
                "unordered access view, so the colour frame cannot be converted into the "
                "only 4-channel format the flow engine accepts.");
            return false;
        }
    }

    {
        const bool hdrish = FeedOfaFormatCanExceedOne(back_fmt);
        char luma[sizeof(kOfaLumaHead) + sizeof(kOfaLumaTonemap) + sizeof(kOfaLumaTail) + 8];
        _snprintf_s(luma, sizeof(luma), _TRUNCATE, "%s%s%s", kOfaLumaHead,
                    hdrish ? kOfaLumaTonemap : kOfaLumaClamp, kOfaLumaTail);
        if (!OfaMakeShader(dev11, luma, "ofaluma", &g_ofa.luma_cs)) return false;
        Log("[feed] optical flow: luma is %s, because the colour source is %s.",
            hdrish ? "tone-mapped with l/(l+1) before the 8-bit conversion"
                   : "clamped rather than tone-mapped, so all eight bits of the "
                     "engine's input carry signal",
            OfaFormatName(back_fmt));
    }
    if (!OfaMakeShader(dev11, kOfaFlowSrc, "ofaflow", &g_ofa.flow_cs)) return false;

    const UINT fw = (w + g_ofa.grid - 1) / g_ofa.grid;
    const UINT fh = (h + g_ofa.grid - 1) / g_ofa.grid;

    g_ofa.copy = OfaMakeTexture(dev11, w, h, FeedOfaTypedFormat(back_fmt),
                                D3D11_BIND_SHADER_RESOURCE, "the colour copy");
    for (int i = 0; i < 2; ++i)
        g_ofa.src[i] = OfaMakeTexture(dev11, w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                                      i == 0 ? "the current frame" : "the previous frame");
    g_ofa.flow = OfaMakeTexture(dev11, fw, fh, DXGI_FORMAT_R16G16_SINT,
                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                "the flow field");
    g_ofa.mv = OfaMakeTexture(dev11, w, h, DXGI_FORMAT_R16G16_FLOAT,
                              D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                              "the motion vectors");
    if (g_ofa.copy == nullptr || g_ofa.src[0] == nullptr || g_ofa.src[1] == nullptr ||
        g_ofa.flow == nullptr || g_ofa.mv == nullptr)
    { FeedOfaClose(); return false; }

    HRESULT hr = dev11->CreateShaderResourceView(g_ofa.copy, nullptr, &g_ofa.copy_srv);
    if (SUCCEEDED(hr)) hr = dev11->CreateShaderResourceView(g_ofa.flow, nullptr, &g_ofa.flow_srv);
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
        hr = dev11->CreateUnorderedAccessView(g_ofa.src[i], nullptr, &g_ofa.src_uav[i]);
    if (SUCCEEDED(hr)) hr = dev11->CreateUnorderedAccessView(g_ofa.mv, nullptr, &g_ofa.mv_uav);
    if (FAILED(hr))
    {
        Log("[feed] optical flow: a view over one of the textures could not be "
            "created: 0x%08X", hr);
        FeedOfaClose();
        return false;
    }

    auto create_ses = reinterpret_cast<PFN_OfaCreateD3D11>(g_ofa.slot[0]);
    NvOfStatus r = OfaSafeCreateSession(create_ses, dev11, ctx, &g_ofa.session);
    if (r != kOfaSuccess || g_ofa.session == nullptr)
    {
        Log("[feed] optical flow: the driver will not open a session on this D3D11 device: "
            "status %ld (%s). This is what a game rendering on an adapter other than the "
            "NVIDIA one looks like, which happens on a hybrid laptop.", r, OfaStatusName(r));
        g_ofa.session = nullptr;
        FeedOfaClose();
        return false;
    }

    NvOfInitParams ip;
    memset(&ip, 0, sizeof(ip));
    ip.width = w; ip.height = h;
    ip.outGridSize = g_ofa.grid;
    ip.hintGridSize = g_ofa.grid;
    ip.mode = 1;
    ip.perfLevel = static_cast<uint32_t>(g_ofa.perf);
    auto init = reinterpret_cast<PFN_OfaInit>(g_ofa.slot[1]);
    r = OfaSafeInit(init, g_ofa.session, &ip);
    if (r != kOfaSuccess)
    {
        char msg[512];
        OfaLastError(msg, sizeof(msg));
        Log("[feed] optical flow: nvOFInit refused %ux%u at grid %u with 0x%08lX (%s). %s",
            w, h, g_ofa.grid, static_cast<unsigned long>(r), OfaStatusName(r), msg);
        if (r == 5)
            Log("[feed] optical flow: Grid 2 needs about 109 MB driver-side at 3840x1600; "
                "grid 4 needs about 63 MB. Try grid=4.");
        FeedOfaClose();
        return false;
    }

    auto reg = reinterpret_cast<PFN_OfaRegister>(g_ofa.slot[4]);
    NvOfStatus r0 = OfaSafeRegister(reg, g_ofa.session, g_ofa.src[0],  &g_ofa.reg_src[0]);
    NvOfStatus r1 = OfaSafeRegister(reg, g_ofa.session, g_ofa.src[1],  &g_ofa.reg_src[1]);
    NvOfStatus r2 = OfaSafeRegister(reg, g_ofa.session, g_ofa.flow,    &g_ofa.reg_flow);
    if (r0 != kOfaSuccess || r1 != kOfaSuccess || r2 != kOfaSuccess ||
        g_ofa.reg_src[0] == nullptr || g_ofa.reg_src[1] == nullptr || g_ofa.reg_flow == nullptr)
    {
        char msg[512];
        OfaLastError(msg, sizeof(msg));
        Log("[feed] optical flow: registering the three buffers was refused (%ld, %ld, %ld). %s",
            r0, r1, r2, msg);
        FeedOfaClose();
        return false;
    }

    g_ofa.ready   = true;
    g_ofa.stopped = false;

    Log("[feed] optical flow: nvofapi64.dll loaded, instance at API 0x20, session open on "
        "the game's D3D11 device. Grid %u, perf %s, flow %ux%u R16G16_SINT -> %ux%u "
        "R16G16_FLOAT.",
        g_ofa.grid, FeedOfaPerfName(g_ofa.perf), fw, fh, w, h);
    Log("[feed] optical flow: conversion shaders ready (cs_5_0)");
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame dispatch (save/restore CS bindings)
// ---------------------------------------------------------------------------

static void OfaDispatch(ID3D11DeviceContext *ctx, ID3D11ComputeShader *cs,
                        ID3D11ShaderResourceView *srv, ID3D11UnorderedAccessView *uav,
                        UINT w, UINT h)
{
    ID3D11ComputeShader       *old_cs  = nullptr;
    ID3D11ShaderResourceView  *old_srv = nullptr;
    ID3D11UnorderedAccessView *old_uav = nullptr;
    ctx->CSGetShader(&old_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &old_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &old_uav);

    UINT keep = static_cast<UINT>(-1);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    ID3D11ShaderResourceView  *no_srv = nullptr;
    ID3D11UnorderedAccessView *no_uav = nullptr;
    ctx->CSSetShaderResources(0, 1, &no_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, &keep);

    ctx->CSSetShader(old_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &old_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &old_uav, &keep);
    if (old_cs  != nullptr) old_cs->Release();
    if (old_srv != nullptr) old_srv->Release();
    if (old_uav != nullptr) old_uav->Release();
}

static bool FeedOfaShapeChanged(ID3D11Device *dev, DXGI_FORMAT fmt, UINT w, UINT h)
{
    return g_ofa.w != w || g_ofa.h != h || g_ofa.back_fmt != fmt || g_ofa.dev != dev ||
           g_ofa.grid != static_cast<UINT>(g_feed_ofa_cfg.grid) ||
           g_ofa.perf != g_feed_ofa_cfg.perf;
}

// ---------------------------------------------------------------------------
// FeedOfaUpdate -- one colour frame in, full-res RG16F MV out (or nullptr)
// ---------------------------------------------------------------------------

static ID3D11Texture2D *FeedOfaUpdate(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                      ID3D11Texture2D *color_src, DXGI_FORMAT color_fmt,
                                      UINT w, UINT h)
{
    if (g_feed_ofa_cfg.enabled == 0 || g_feed_ofa_cfg.grid == 0)
        return nullptr;
    if (dev == nullptr || ctx == nullptr || color_src == nullptr || w == 0 || h == 0)
        return nullptr;

    // Rebuild when shape or cfg knobs change (including retry after stopped/failed open).
    if ((g_ofa.ready || g_ofa.tried) && FeedOfaShapeChanged(dev, color_fmt, w, h))
    {
        Log("[feed] optical flow: colour became %ux%u %s at grid %d perf %s on "
            "device %p, so the session is rebuilt.", w, h, OfaFormatName(color_fmt),
            g_feed_ofa_cfg.grid, FeedOfaPerfName(g_feed_ofa_cfg.perf),
            static_cast<void *>(dev));
        FeedOfaClose();
        g_ofa.tried = false;
        g_ofa.stopped = false;
        if (!FeedOfaOpen(dev, ctx, w, h, color_fmt))
        {
            Log("[feed] optical flow: the session could not be rebuilt for the new size, "
                "so this stops.");
            g_ofa.stopped = true;
            return nullptr;
        }
    }
    else if (!g_ofa.ready && !g_ofa.tried && !g_ofa.stopped)
    {
        if (!FeedOfaOpen(dev, ctx, w, h, color_fmt))
            return nullptr;
    }

    if (!g_ofa.ready || g_ofa.stopped)
        return nullptr;

    ctx->CopyResource(g_ofa.copy, color_src);
    OfaDispatch(ctx, g_ofa.luma_cs, g_ofa.copy_srv, g_ofa.src_uav[g_ofa.cur], w, h);

    if (!g_ofa.primed)
    {
        g_ofa.primed = true;
        g_ofa.cur ^= 1;
        if (!g_ofa.first_said)
        {
            g_ofa.first_said = true;
            Log("[feed] optical flow: the first frame has no previous frame to compare "
                "against, so nothing is delivered on it. Flow starts on the next one.");
        }
        return nullptr;
    }

    NvOfExecuteInputParams  in;
    NvOfExecuteOutputParams eo;
    memset(&in, 0, sizeof(in));
    memset(&eo, 0, sizeof(eo));
    in.inputFrame           = g_ofa.reg_src[g_ofa.cur];
    in.referenceFrame       = g_ofa.reg_src[g_ofa.cur ^ 1];
    in.disableTemporalHints = 1;
    eo.outputBuffer         = g_ofa.reg_flow;

    auto exec = reinterpret_cast<PFN_OfaExecute>(g_ofa.slot[6]);
    const NvOfStatus r = OfaSafeExecute(exec, g_ofa.session, &in, &eo);
    ++g_ofa.frames;

    if (r == kOfaRaised)
    {
        Log("[feed] optical flow: nvOFExecute raised an exception, so this stops here. "
            "The game keeps rendering on its own.");
        g_ofa.ready   = false;
        g_ofa.stopped = true;
        return nullptr;
    }
    if (r != kOfaSuccess)
    {
        if (g_ofa.fails == 0 || g_ofa.fails == 2)
        {
            char msg[512];
            OfaLastError(msg, sizeof(msg));
            Log("[feed] optical flow: execute %llu refused with status %ld (%s). %s",
                g_ofa.frames, r, OfaStatusName(r), msg);
        }
        // Advance cur: luma already overwrote src[cur]; leaving it would compare
        // against a two-frame-old reference on the next success.
        g_ofa.cur ^= 1;
        if (++g_ofa.fails >= 3)
        {
            Log("[feed] optical flow: three consecutive failures, so this stops.");
            g_ofa.ready   = false;
            g_ofa.stopped = true;
        }
        return nullptr;
    }

    g_ofa.fails = 0;
    OfaDispatch(ctx, g_ofa.flow_cs, g_ofa.flow_srv, g_ofa.mv_uav, w, h);
    g_ofa.cur ^= 1;

    // Borrowed -- module owns g_ofa.mv until Close/Shutdown.
    return g_ofa.mv;
}

