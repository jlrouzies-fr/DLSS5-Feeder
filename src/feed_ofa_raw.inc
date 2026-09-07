// The driver's own optical flow engine
//
// Why this exists: Skyrim Special Edition, 2026-08-30. The synthetic contract
// was built, the feature was created, frames were delivered, and fine detail
// dissolved along the direction of movement -- worse slowly than quickly. Five
// observations settled the cause by experiment: all four mv_sign combinations
// smeared, so not the sign; mode=1 was clean, so not the transport;
// reset_every=1 was clean, so it is the temporal accumulation; worse slowly, so
// the sub-pixel regime; the trail ran ALONG the movement, so under-estimated
// rather than reversed. vort_Shaders estimates at mip 1, its finest pass is
// PS_Motion1, there is no PS_Motion0, and below one pixel it reports ~0.
//
// NVOFA never reports zero. Measured on this machine at 1920x1080, MEDIUM,
// "exactly zero: 0.00%" at every step above 0.10 px, and at a true 0.10 px it
// answers 0.156 at grid 2. That is the whole reason to build this.
//
// D3D11 ONLY. The function list below was recovered empirically for
// NvOFAPICreateInstanceD3D11 and for nothing else: the D3D12 and Vulkan entry
// points are different exports with different, unrecovered slot tables and
// different accepted API versions. On those two runtimes the kSynthMv[1..]
// ReShade providers are the only ones, which is what they already were.
//
// NOT MEASURED and not claimed anywhere below: rotation and zoom against real
// game content, transparency, per-object motion, and temporal hints -- every
// number quoted here was taken with disableTemporalHints = 1 over rigid warps of
// a synthetic pattern.
// ---------------------------------------------------------------------------

typedef long NvOfStatus;
static const NvOfStatus kOfaSuccess = 0;      // NV_OF_SUCCESS
static const NvOfStatus kOfaRaised  = 0x7FFFFFFF;   // our own marker, as NGX_EXCEPTION_MARKER is

// Plain C structs from a published ABI, hand-written for the same reason the
// Vulkan structs next door are: there is no lib to link and no header worth
// vendoring for five exports. This layout is additionally confirmed by
// experiment on this machine -- breaking one field made the driver name that
// exact field back through nvOFGetLastError.
struct NvOfInitParams
{
    uint32_t width;               //  0
    uint32_t height;              //  4
    uint32_t outGridSize;         //  8   1, 2 or 4
    uint32_t hintGridSize;        // 12   same value; unread with hints disabled
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
    void    *inputFrame;           //  0  the CURRENT frame
    void    *referenceFrame;       //  8  the PREVIOUS frame
    void    *externalHints;        // 16
    uint32_t disableTemporalHints; // 24
    uint32_t padding;              // 28
    void    *hPrivData;            // 32
    uint32_t padding2;             // 40
    uint32_t numRois;              // 44
    void    *roiData;              // 48
};

struct NvOfExecuteOutputParams { void *outputBuffer; void *outputCostBuffer; void *hPrivData; };

// The same discipline as the PanelImGui asserts: a miscounted pad is a compile
// error here rather than a call through the wrong offset at runtime.
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
    // A width or height the engine will not take comes back as 0x80004005,
    // which is E_FAIL and not an NV_OF_STATUS at all. Every test below is
    // != kOfaSuccess for exactly that reason, and this names the case rather
    // than reporting it as an enum value nobody can look up.
    case static_cast<NvOfStatus>(0x80004005): return "E_FAIL, not an NV_OF_STATUS";
    default: return "a status this build has no name for";
    }
}

// 5, 10 and 20 are NVOFA's own values; anything else was refused by the config
// validator, so the default arm can only be reached if that check is changed.
static const char *SynthPerfName(int p)
{
    return p == 5 ? "SLOW" : p == 10 ? "MEDIUM" : p == 20 ? "FAST" : "?";
}

struct SynthOfa
{
    bool    tried;      // the first open was attempted; a failed first open is never retried
    bool    ready;      // dll, instance, session, textures, shaders all in hand
    bool    stopped;    // gave up after being ready; no ReShade provider is substituted
    bool    primed;     // src[prev] holds a real frame, so an execute is possible
    bool    first_said;
    HMODULE lib;
    void   *slot[64];
    void   *session;
    UINT    w, h, grid;
    int     perf;       // the NV_OF_PERF_LEVEL this session was opened with
    DXGI_FORMAT back_fmt;               // the shape the session was opened for
    // The device every texture below was created on, compared rather than
    // assumed. A game that re-creates its D3D11 device -- ReShade answers that
    // with a fresh effect runtime and this add-on keeps running -- would
    // otherwise leave a CopyResource between two devices, which is undefined
    // rather than an error. Held as a bare pointer and never dereferenced: the
    // textures below hold their own references, so the address cannot be reused
    // by a second device while this session exists, and the comparison is exact.
    ID3D11Device *dev;
    void   *reg_src[2], *reg_flow;      // registration handles
    ID3D11Texture2D *copy, *src[2], *flow, *mv;
    ID3D11ShaderResourceView  *copy_srv, *flow_srv;
    ID3D11UnorderedAccessView *src_uav[2], *mv_uav;
    ID3D11ComputeShader *luma_cs, *flow_cs;
    int    cur;         // which of src[] this frame writes
    int    fails;       // consecutive execute failures
    bool   checked;     // the self-check has run for this shape
    int    quiet;       // frames seen with no motion anywhere, for the self-check
    UINT64 frames;
};

static SynthOfa g_ofa;

// Each driver entry point gets its own guarded wrapper, for the reason the NGX
// wrappers in dlss5-bridge.cpp were given theirs: these are undocumented
// exports reached through signatures recovered from disassembly, and a wrong
// guess has to land in the log rather than take the game down. Each is its own
// function with no C++ objects, so __try is legal and no unwinding is needed.
static NvOfStatus OfaSafeCreateInstance(PFN_OfaCreateInstance fn, uint32_t ver, void *slots)
{
    __try { return fn(ver, slots); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeCreateSession(PFN_OfaCreateD3D11 fn, ID3D11Device *d,
                                       ID3D11DeviceContext *c, void **out)
{
    __try { return fn(d, c, out); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeInit(PFN_OfaInit fn, void *s, const NvOfInitParams *p)
{
    __try { return fn(s, p); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeRegister(PFN_OfaRegister fn, void *s, ID3D11Resource *r, void **out)
{
    __try { return fn(s, r, out); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeUnregister(PFN_OfaUnregister fn, void *h)
{
    __try { return fn(h); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeDestroy(PFN_OfaDestroy fn, void *s)
{
    __try { return fn(s); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeExecute(PFN_OfaExecute fn, void *s, const NvOfExecuteInputParams *in,
                                 NvOfExecuteOutputParams *out)
{
    __try { return fn(s, in, out); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

// The driver's own account of what it refused, which is what makes an nvOFInit
// failure actionable: it names the offending field. Empty when the call has
// nothing to say or is not safe to make.
static void OfaLastError(char *dst, size_t cap)
{
    dst[0] = '\0';
    if (g_ofa.session == nullptr || g_ofa.slot[8] == nullptr) return;
    uint32_t n = static_cast<uint32_t>(cap);
    auto fn = reinterpret_cast<PFN_OfaLastError>(g_ofa.slot[8]);
    __try { fn(g_ofa.session, dst, &n); }
    __except (CaptureFault(GetExceptionInformation())) { dst[0] = '\0'; }
    // Nothing in the recovered signature promises a terminator, and every caller
    // hands the result straight to a %s.
    dst[cap - 1] = '\0';
}

// Unregister before release, on every path including a half-built open: the
// driver holds these handles and releasing the texture under one is a use after
// free inside the display driver rather than in this process.
//
// lib, slot and the two compiled shaders are deliberately left alone. They are
// shape-independent, and recompiling two compute shaders on a resolution change
// is a D3DCompile for nothing.
static void OfaClose()
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

    // The two compute shaders, which this function did not release. OfaOpen
    // compiles a fresh pair on every shape change, so each change leaked one of
    // each -- and a shader holds a reference on the D3D11 device it was compiled
    // by, so the private device outlived the session that owned it and could not
    // be reclaimed for the life of the process.
    if (g_ofa.luma_cs != nullptr) { g_ofa.luma_cs->Release(); g_ofa.luma_cs = nullptr; }
    if (g_ofa.flow_cs != nullptr) { g_ofa.flow_cs->Release(); g_ofa.flow_cs = nullptr; }

    if (g_ofa.session != nullptr)
    {
        auto destroy = reinterpret_cast<PFN_OfaDestroy>(g_ofa.slot[7]);
        if (destroy != nullptr) OfaSafeDestroy(destroy, g_ofa.session);
        g_ofa.session = nullptr;
    }

    g_ofa.ready   = false;
    g_ofa.primed  = false;
    g_ofa.checked = false;
    g_ofa.fails   = 0;
    g_ofa.quiet   = 0;
    g_ofa.cur     = 0;
    g_ofa.dev     = nullptr;
}

// There is no teardown at DLL_PROCESS_DETACH, and that is deliberate rather than
// an oversight: g_s12 and g_svk have none either, for the same reasons. Releasing
// GPU objects from DllMain under the loader lock is a hazard, most games never
// reach detach at all, and the process is exiting. OfaClose on a shape change is
// the only teardown that exists.

static bool OfaMakeShader(ID3D11Device *dev11, const char *src, const char *name,
                          ID3D11ComputeShader **out)
{
    if (*out != nullptr) return true;

    PFN_D3DCompile_ compile = LoadSystemD3DCompiler();
    if (compile == nullptr) { Log("[synth] optical flow: d3dcompiler_47.dll is unavailable, so the flow "
                                     "conversion shader cannot be built"); return false; }

    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                         &code, &err);
    if (FAILED(hr) || code == nullptr)
    {
        Log("[synth] optical flow shader compile failed 0x%08X: %s", hr,
            err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    hr = dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
    code->Release();
    if (FAILED(hr))
    { Log("[synth] optical flow CreateComputeShader failed 0x%08X", hr); return false; }
    return true;
}

// Grey rather than colour: the estimator matches on structure and a single
// channel is what it reduces to anyway. Reinhard rather than saturate, because
// an R16G16B16A16_FLOAT back buffer clips every highlight to white under
// saturate and the estimator then has nothing to lock onto in exactly the bright
// regions that carry the most detail. Reinhard is monotone, so nothing it
// matches on is reordered.
// Split in two so the middle line can be chosen from the back buffer format.
//
// The engine's input surface is B8G8R8A8_UNORM -- eight bits of luma. The
// Reinhard curve l/(l+1) was written for a float back buffer that can exceed
// 1.0, where compressing the range is the only way to get it into eight bits at
// all. Applied to a UNORM buffer, whose luma is already in [0,1], it maps that
// range onto [0,0.5] and hands the estimator seven bits instead of eight, for
// nothing. Red Dead Redemption 2 and Baldur's Gate 3 both present
// R10G10B10A2_UNORM, so both were paying it, and so is every 8-bit SDR title.
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

// Three things here are load-bearing and each one is a measurement.
//
//  1/32   the output is S10.5 fixed point, so the stored value is pixels x 32.
//  /(w,h) the contract stores UV and MV.Scale multiplies by (W,H) to get back to
//         pixels, so the round trip is exact.
//  no sign flip: with inputFrame = this frame and referenceFrame = the previous
//         one, the vector already points from the current pixel back to where
//         that content was, which is verbatim the DLSS definition. Measured over
//         1,166,400 vectors, 100% within 0.5 px, both axes.
//
// The reconstruction filter is NEAREST, not bilinear, and that is the one place
// this departs from what the transport note first sketched. The disocclusion
// band is 12-16 px wide in pixels at every grid, and the values inside it
// correspond to no surface in the scene. Bilinear across that band manufactures
// more such values and spreads it wider; nearest quantises but never invents a
// vector no cell measured. Where the field is smooth it varies far more slowly
// than one grid cell and both filters land inside the 0.1 px noise floor.
//
// id.x * fw / w is the exact integer divide whenever fw*grid == w, which is
// every resolution in practice; the min keeps the sample inside the texture at
// the ones where it is not.
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
        Log("[synth] optical flow: could not create the %ux%u %s texture for %s: 0x%08X",
            w, h, FormatName(fmt), what, hr);
        return nullptr;
    }
    return t;
}
// The private D3D11 device the Vulkan transport runs the engine on, or null when
// there is none. Declared here because OfaOpen has to name which device it opened
// on and g_ovk is defined far below it.
static ID3D11Device *SynthOfaVkDevice();


static bool OfaOpen(ID3D11Device *dev11, ID3D11DeviceContext *ctx, UINT w, UINT h,
                    DXGI_FORMAT back_fmt)
{
    // Before anything that can fail, exactly as Synth12Open and SynthVkOpen set
    // theirs: a first open that failed is never retried.
    g_ofa.tried = true;
    g_ofa.w = w; g_ofa.h = h; g_ofa.back_fmt = back_fmt; g_ofa.dev = dev11;
    g_ofa.grid = static_cast<UINT>(g_cfg.ofa_grid);
    // Validated where it is divided by rather than only where it is set. Every
    // caller checks for zero before getting here, and that is exactly the kind of
    // agreement that a later caller breaks silently: the two divisions below turn a
    // zero into an integer divide fault, which takes the game with it rather than
    // refusing. The config parser accepts 0 as "engine off", so this value reaches
    // the function from a user's file and from a panel click.
    if (g_ofa.grid == 0)
    {
        Log("[synth] optical flow: ofa_grid is 0, which switches the engine off, so "
            "the session is not opened. The motion vectors come from a ReShade effect.");
        return false;
    }

    if (g_ofa.lib == nullptr)
    {
        g_ofa.lib = LoadLibraryW(L"nvofapi64.dll");
        if (g_ofa.lib == nullptr)
        {
            Log("[synth] optical flow: nvofapi64.dll is not loadable in this process. It is "
                "installed with the display driver, so this means no NVIDIA driver here "
                "rather than a driver too old.");
            return false;
        }
        auto create = reinterpret_cast<PFN_OfaCreateInstance>(
            GetProcAddress(g_ofa.lib, "NvOFAPICreateInstanceD3D11"));
        if (create == nullptr)
        {
            Log("[synth] optical flow: nvofapi64.dll exports no NvOFAPICreateInstanceD3D11.");
            return false;
        }
        // 0x20 is the public 2.0 layout, which is the one the structs above were
        // written to and confirmed against on this machine. The D3D11 entry was
        // measured to accept 0x10 through 0x50.
        const NvOfStatus r = OfaSafeCreateInstance(create, 0x20, g_ofa.slot);
        if (r != kOfaSuccess)
        {
            Log("[synth] optical flow: this driver refused API version 0x20 with status %ld "
                "(%s). The D3D11 entry accepts 0x10 through 0x50 on the driver this was "
                "measured against.", r, OfaStatusName(r));
            memset(g_ofa.slot, 0, sizeof(g_ofa.slot));
            return false;
        }
    }

    // Asked once, before a texture exists. BGRA8 typed UAV store is not in
    // D3D11's guaranteed set, and without it the conversion writes nothing and
    // the engine is handed a blank frame -- which looks like bad flow rather
    // than like a missing feature.
    {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 f2 = {};
        f2.InFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (FAILED(dev11->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &f2, sizeof(f2))) ||
            (f2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
        {
            Log("[synth] optical flow: this device cannot store to a B8G8R8A8_UNORM "
                "unordered access view, so the colour frame cannot be converted into the "
                "only 4-channel format the flow engine accepts.");
            return false;
        }
    }

    {
        const bool hdrish = SynthFormatCanExceedOne(back_fmt);
        char luma[sizeof(kOfaLumaHead) + sizeof(kOfaLumaTonemap) + sizeof(kOfaLumaTail) + 8];
        _snprintf_s(luma, sizeof(luma), _TRUNCATE, "%s%s%s", kOfaLumaHead,
                    hdrish ? kOfaLumaTonemap : kOfaLumaClamp, kOfaLumaTail);
        if (!OfaMakeShader(dev11, luma, "ofaluma", &g_ofa.luma_cs)) return false;
        Log("[synth] optical flow: luma is %s, because the back buffer is %s.",
            hdrish ? "tone-mapped with l/(l+1) before the 8-bit conversion"
                   : "clamped rather than tone-mapped, so all eight bits of the "
                     "engine's input carry signal",
            FormatName(back_fmt));
    }
    if (!OfaMakeShader(dev11, kOfaFlowSrc, "ofaflow", &g_ofa.flow_cs)) return false;

    const UINT fw = (w + g_ofa.grid - 1) / g_ofa.grid;
    const UINT fh = (h + g_ofa.grid - 1) / g_ofa.grid;

    // The back buffer is not guaranteed to carry BIND_SHADER_RESOURCE -- a DXGI
    // swapchain only promises a render target view -- so a compute shader cannot
    // read it directly. CopyResource into a texture we created with the same
    // format always works.
    g_ofa.copy = OfaMakeTexture(dev11, w, h, TypedFormat(back_fmt),
                                D3D11_BIND_SHADER_RESOURCE, "the colour copy");
    // BGRA8 because the engine's legal input list is BGRA8, NV12 and R8_UNORM,
    // and Skyrim SE presents R8G8B8A8_UNORM. SHADER_RESOURCE alongside the UAV
    // because that is the binding the registration was proven with in the
    // harness, and it costs nothing.
    for (int i = 0; i < 2; ++i)
        g_ofa.src[i] = OfaMakeTexture(dev11, w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                                      i == 0 ? "the current frame" : "the previous frame");
    g_ofa.flow = OfaMakeTexture(dev11, fw, fh, DXGI_FORMAT_R16G16_SINT,
                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                "the flow field");
    // Exactly the shape SynthArmRefusal already demands of a motion vector
    // texture, so the gate needs no new rule. That is the whole point of
    // converting here rather than handing NGX the engine's own output.
    g_ofa.mv = OfaMakeTexture(dev11, w, h, DXGI_FORMAT_R16G16_FLOAT,
                              D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                              "the motion vectors");
    if (g_ofa.copy == nullptr || g_ofa.src[0] == nullptr || g_ofa.src[1] == nullptr ||
        g_ofa.flow == nullptr || g_ofa.mv == nullptr)
    { OfaClose(); return false; }

    HRESULT hr = dev11->CreateShaderResourceView(g_ofa.copy, nullptr, &g_ofa.copy_srv);
    if (SUCCEEDED(hr)) hr = dev11->CreateShaderResourceView(g_ofa.flow, nullptr, &g_ofa.flow_srv);
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
        hr = dev11->CreateUnorderedAccessView(g_ofa.src[i], nullptr, &g_ofa.src_uav[i]);
    if (SUCCEEDED(hr)) hr = dev11->CreateUnorderedAccessView(g_ofa.mv, nullptr, &g_ofa.mv_uav);
    if (FAILED(hr))
    {
        Log("[synth] optical flow: a view over one of the four textures could not be "
            "created: 0x%08X", hr);
        OfaClose();
        return false;
    }

    LogVideoMemory("before opening the optical flow session");

    auto create_ses = reinterpret_cast<PFN_OfaCreateD3D11>(g_ofa.slot[0]);
    NvOfStatus r = OfaSafeCreateSession(create_ses, dev11, ctx, &g_ofa.session);
    if (r != kOfaSuccess || g_ofa.session == nullptr)
    {
        // The real case this covers: nvofapi64.dll is present because an NVIDIA
        // GPU is installed, while the game renders on the iGPU.
        Log("[synth] optical flow: the driver will not open a session on this D3D11 device: "
            "status %ld (%s). This is what a game rendering on an adapter other than the "
            "NVIDIA one looks like, which happens on a hybrid laptop.", r, OfaStatusName(r));
        g_ofa.session = nullptr;
        OfaClose();
        return false;
    }

    NvOfInitParams ip;
    memset(&ip, 0, sizeof(ip));
    ip.width = w; ip.height = h;
    ip.outGridSize = g_ofa.grid;
    ip.hintGridSize = g_ofa.grid;      // unread with hints disabled; set to match
    ip.mode = 1;                       // NV_OF_MODE_OPTICALFLOW
    // From the config, validated there to 5, 10 or 20 -- NVOFA's own
    // NV_OF_PERF_LEVEL values, so a number quoted in a report reads against
    // NVIDIA's own table. FAST by default because it is the only level that
    // fits a frame at 4K: measured at 3840x1600 grid 2, FAST costs 1.6 ms
    // where MEDIUM costs 6.2 and SLOW 11.5. The slower levels estimate more
    // accurately, which is the knob to reach for when fine detail drifts.
    ip.perfLevel = static_cast<uint32_t>(g_cfg.ofa_perf);
    g_ofa.perf   = g_cfg.ofa_perf;
    auto init = reinterpret_cast<PFN_OfaInit>(g_ofa.slot[1]);
    r = OfaSafeInit(init, g_ofa.session, &ip);
    if (r != kOfaSuccess)
    {
        // Tested against SUCCESS rather than switched on the enum, because a
        // width or height error comes back as E_FAIL, which is not one.
        char msg[512];
        OfaLastError(msg, sizeof(msg));
        Log("[synth] optical flow: nvOFInit refused %ux%u at grid %u with 0x%08lX (%s). %s",
            w, h, g_ofa.grid, static_cast<unsigned long>(r), OfaStatusName(r), msg);
        if (r == 5)   // NV_OF_ERR_OUT_OF_MEMORY
            Log("[synth]   Grid 2 needs about 109 MB driver-side at 3840x1600; grid 4 needs "
                "about 63 MB. Try ofa_grid=4 in dlss5-bridge.cfg.");
        OfaClose();
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
        Log("[synth] optical flow: registering the three buffers was refused (%ld, %ld, %ld). %s",
            r0, r1, r2, msg);
        OfaClose();
        return false;
    }

    g_ofa.ready   = true;
    g_ofa.stopped = false;

    // Which device, said rather than assumed. This named "the game's own D3D11
    // device" unconditionally, and on the Vulkan transport that is false: the
    // game has no D3D11 device there and the session opens on a private one this
    // add-on created. The two are told apart by the pointer, because g_ovk.dev is
    // non-null only when this file made it.
    Log("[synth] optical flow: nvofapi64.dll loaded, instance at API 0x20, session open on "
        "%s. Grid %u, perf %s, flow %ux%u R16G16_SINT -> %ux%u "
        "R16G16_FLOAT. This is kSynthMv[0] and the three ReShade motion-vector names are "
        "not consulted while it is running.",
        (SynthOfaVkDevice() != nullptr && SynthOfaVkDevice() == dev11)
            ? "a private D3D11 device this add-on created, because the game is on Vulkan"
            : "the game's own D3D11 device",
        g_ofa.grid, SynthPerfName(g_ofa.perf), fw, fh, w, h);
    Log("[synth] optical flow conversion shaders ready (cs_5_0)");
    LogVideoMemory("after opening the optical flow session");
    return true;
}

// One compute dispatch, with the game's own compute bindings saved and put back
// exactly as BridgeBlitDepth does it. This runs on the immediate context in the
// middle of the game's frame. Two separate calls rather than one block spanning
// both dispatches, because nvOFExecute runs between them on the same context.
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

// The one runnable check this stage leaves behind, and it ships enabled. It
// checks the arithmetic rather than the plumbing -- the plumbing announces its
// own failures above -- by recomputing what the conversion shader should have
// written from the flow texture the DRIVER actually filled, not from anything
// this add-on assumed.
//
// It runs once per shape, and only on a frame whose flow field contains a
// non-zero vector: a frame with no motion proves nothing, because 0 == 0 passes
// a shader that has the scale wrong, the grid ratio wrong, or is not running at
// all.
static void OfaSelfCheck(ID3D11Device *dev11, ID3D11DeviceContext *ctx)
{
    // The first attempt is taken immediately, because the frame right after
    // priming is the one most likely to have motion in it. Every attempt after
    // that waits for a multiple of sixteen: each one stages and maps about 30 MB
    // and blocks until the GPU has caught up, and a game that opens on a static
    // menu would otherwise take that stall on a hundred frames in a row with
    // nothing in the log to attribute the hitch to.
    if (g_ofa.quiet > 0 && (g_ofa.frames & 15) != 0) return;

    // Three assertions off GetDesc alone, which cost nothing and catch a
    // mis-sized texture before a single texel is read.
    D3D11_TEXTURE2D_DESC fd = {}, md = {};
    g_ofa.flow->GetDesc(&fd);
    g_ofa.mv->GetDesc(&md);
    const UINT want_fw = (g_ofa.w + g_ofa.grid - 1) / g_ofa.grid;
    const UINT want_fh = (g_ofa.h + g_ofa.grid - 1) / g_ofa.grid;
    if (fd.Width != want_fw || fd.Height != want_fh || fd.Format != DXGI_FORMAT_R16G16_SINT ||
        md.Width != g_ofa.w || md.Height != g_ofa.h || md.Format != DXGI_FORMAT_R16G16_FLOAT ||
        g_ofa.flow == reinterpret_cast<ID3D11Texture2D *>(g_ofa.mv))
    {
        g_ofa.checked = true;
        Log("[synth] DEFECT: the optical flow textures are not the shape this build claims. "
            "Flow is %ux%u fmt %u, expected %ux%u R16G16_SINT; motion vectors are %ux%u "
            "fmt %u, expected %ux%u R16G16_FLOAT.",
            fd.Width, fd.Height, fd.Format, want_fw, want_fh,
            md.Width, md.Height, md.Format, g_ofa.w, g_ofa.h);
        return;
    }

    D3D11_TEXTURE2D_DESC sd = fd;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D *fs = nullptr, *ms = nullptr;
    if (FAILED(dev11->CreateTexture2D(&sd, nullptr, &fs))) return;
    sd = md;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (FAILED(dev11->CreateTexture2D(&sd, nullptr, &ms))) { fs->Release(); return; }

    ctx->CopyResource(fs, g_ofa.flow);
    ctx->CopyResource(ms, g_ofa.mv);

    D3D11_MAPPED_SUBRESOURCE fm = {}, mm = {};
    if (FAILED(ctx->Map(fs, 0, D3D11_MAP_READ, 0, &fm)) ||
        FAILED(ctx->Map(ms, 0, D3D11_MAP_READ, 0, &mm)))
    { fs->Release(); ms->Release(); return; }

    int   sampled = 0, bad = 0, nonzero = 0;
    float worst = 0.0f;
    // How far past its own tolerance the worst offender was, so a real defect
    // report carries a number that is not confusable with rounding.
    float worst_over = 0.0f;
    int   bx = 0, by = 0, bfx = 0, bfy = 0;
    float bex = 0.0f, bey = 0.0f, bax = 0.0f, bay = 0.0f;

    for (UINT cy = 0; cy < fd.Height; cy += 16)
    {
        const int16_t *frow = reinterpret_cast<const int16_t *>(
            static_cast<const unsigned char *>(fm.pData) + static_cast<size_t>(cy) * fm.RowPitch);
        const UINT py = cy * g_ofa.grid + g_ofa.grid / 2;
        if (py >= md.Height) continue;
        const unsigned short *mrow = reinterpret_cast<const unsigned short *>(
            static_cast<const unsigned char *>(mm.pData) + static_cast<size_t>(py) * mm.RowPitch);

        for (UINT cx = 0; cx < fd.Width; cx += 16)
        {
            const UINT px = cx * g_ofa.grid + g_ofa.grid / 2;
            if (px >= md.Width) continue;

            const int fx = frow[cx * 2], fy = frow[cx * 2 + 1];
            if (fx != 0 || fy != 0) ++nonzero;

            // expected: what the shader should have written, computed from the
            // flow texture the driver actually filled.
            const float ex = static_cast<float>(fx) * (1.0f / 32.0f) / static_cast<float>(g_ofa.w);
            const float ey = static_cast<float>(fy) * (1.0f / 32.0f) / static_cast<float>(g_ofa.h);
            const float ax = DirectX::PackedVector::XMConvertHalfToFloat(mrow[px * 2]);
            const float ay = DirectX::PackedVector::XMConvertHalfToFloat(mrow[px * 2 + 1]);

            // Compared in PIXELS, which is the unit a reader of the log can act
            // on. The tolerance has to scale, and the flat 0.02 px it used to be
            // was below the storage precision:
            //
            // The texture holds the vector in UV, in half-float. Half has a
            // 10-bit mantissa, so the step between representable values at
            // magnitude m is about m * 2^-10 -- and multiplied back by the axis
            // that is (m * axis) * 2^-10, which is a PIXEL magnitude times 2^-10.
            // At 36.5 px one step is 0.0293 px, larger than the 0.02 px this
            // compared against. Measured in Red Dead Redemption 2 on 2026-09-01:
            // expected -36.500 px, stored -36.475 px, a difference of 0.025 px
            // that is less than one representable step -- and the log reported a
            // DEFECT, told the user the image would smear, and blamed this
            // add-on for arithmetic that was exact.
            //
            // The comment that justified 0.02 said rounding was "about 5e-4 px at
            // these magnitudes", which is the relative precision without the
            // multiplication back into pixels.
            //
            // 2^-9 rather than 2^-10 leaves one step of headroom, and still
            // catches every way this can be wrong: a missing /32 errs by 32x, a
            // wrong grid ratio picks another cell's vector entirely, a Y flip
            // errs by twice the value, and a shader that never ran leaves zeros.
            const float dx = fabsf(ax - ex) * static_cast<float>(g_ofa.w);
            const float dy = fabsf(ay - ey) * static_cast<float>(g_ofa.h);
            const float d  = dx > dy ? dx : dy;
            const float mx = fabsf(ex) * static_cast<float>(g_ofa.w);
            const float my = fabsf(ey) * static_cast<float>(g_ofa.h);
            const float tol = 0.02f + (mx > my ? mx : my) * (1.0f / 512.0f);
            ++sampled;
            if (d > worst)
            {
                worst = d;
                bx = static_cast<int>(cx); by = static_cast<int>(cy);
                bfx = fx; bfy = fy;
                bex = ex * static_cast<float>(g_ofa.w); bey = ey * static_cast<float>(g_ofa.h);
                bax = ax * static_cast<float>(g_ofa.w); bay = ay * static_cast<float>(g_ofa.h);
            }
            if (d > tol) ++bad;
            if (d > tol && d > worst_over) worst_over = d - tol;
        }
    }

    ctx->Unmap(fs, 0);
    ctx->Unmap(ms, 0);
    fs->Release();
    ms->Release();

    if (nonzero == 0)
    {
        // A still frame disproves nothing, so this is not a verdict. Same voice
        // as the depth report's "unverified".
        // Eight attempts, one immediately and the rest sixteen frames apart, so
        // the window is still about 120 frames wide -- the count is of what was
        // actually looked at, because that is the number a reader can check.
        if (++g_ofa.quiet >= 8)
        {
            g_ofa.checked = true;
            Log("[synth] optical flow self-check: 8 checks spread over the first 120 or so "
                "frames found no non-zero vector anywhere, so the conversion could not be "
                "checked. Nothing was disproved.");
        }
        return;
    }

    g_ofa.checked = true;
    if (bad == 0)
        Log("[synth] optical flow self-check: %d cells sampled, flow %ux%u R16G16_SINT -> "
            "%ux%u R16G16_FLOAT, worst disagreement %.3f px. The conversion, the 1/32 scale "
            "and the grid mapping are what this build claims they are.",
            sampled, fd.Width, fd.Height, g_ofa.w, g_ofa.h, worst);
    else
        // Delivery continues. This is a defect report about this add-on, and
        // refusing to deliver would hide the numbers a report needs. The text
        // names the likely cause by construction, because it prints both numbers
        // in pixels rather than a verdict: a factor of exactly 32 is the missing
        // scale.
        Log("[synth] DEFECT: the optical flow conversion disagrees with its own input at "
            "cell (%d,%d): flow (%+d,%+d) is %+.3f,%+.3f px, and the motion vector texture "
            "holds %+.3f,%+.3f px. %d of %d cells sampled disagree by more than their own "
            "storage precision, the worst by %.3f px beyond it. The image will smear; this "
            "is a defect in this add-on and not in the estimator.",
            bx, by, bfx, bfy, bex, bey, bax, bay, bad, sampled, worst_over);
}

// One frame. Returns true when out holds a motion vector texture NGX may read.
//
// The copy, both dispatches and the execute are timed together rather than the
// execute alone, because all of it is new cost -- and because nvOFExecute blocks
// the calling thread, which here is ReShade's present thread, so this is CPU
// time inside the game's frame rather than merely GPU time. The one-off session
// open is outside the bracket; see the note at the QueryPerformanceCounter.
// The body, over a colour texture and a device the caller supplies. Two callers:
// OfaFrame below, which is the D3D11 game's own device and its back buffer, and
// SynthOfaVkFrame, which is a private D3D11 device and the D3D12 shared texture
// the Vulkan frame was already copied into. Extracted rather than duplicated --
// everything here except where the two pointers come from is the same code, and
// a second copy of an nvOFExecute call with its own failure counting is exactly
// the thing that drifts.
//
// dev11, ctx and src are BORROWED. This function releases none of them.
static bool OfaFrameOn(ID3D11Device *dev11, ID3D11DeviceContext *ctx,
                       ID3D11Texture2D *src, DXGI_FORMAT src_fmt,
                       UINT w, UINT h, SynthBound *out)
{
    SynthBound blank = {};
    *out = blank;
    if (dev11 == nullptr || ctx == nullptr || src == nullptr) return false;

    LARGE_INTEGER t0, t1;
    bool delivered  = false;
    bool want_check = false;

    // A resolution change, a format change, or an ofa_grid or ofa_perf edit
    // rebuilds the session in place. tried is not consulted here: it governs the
    // first open only, and a failed re-open is recorded by stopped instead.
    //
    // perf was absent from this test until the panel gained a control for it.
    // NV_OF_PERF_LEVEL is only read at nvOFInit, so editing ofa_perf mid-session
    // was accepted by the parser, passed the validator, was reported as a config
    // change -- and estimated at the old level for the rest of the session, with
    // the log still printing the old name. One clause, and the teardown below is
    // the one ofa_grid already proved.
    if (g_ofa.ready && (g_ofa.w != w || g_ofa.h != h || g_ofa.back_fmt != src_fmt ||
                        g_ofa.dev != dev11 ||
                        g_ofa.grid != static_cast<UINT>(g_cfg.ofa_grid) ||
                        g_ofa.perf != g_cfg.ofa_perf))
    {
        Log("[synth] optical flow: the back buffer became %ux%u %s at grid %d perf %s on "
            "device %p, so the session is rebuilt.", w, h, FormatName(src_fmt),
            g_cfg.ofa_grid, SynthPerfName(g_cfg.ofa_perf),
            static_cast<void *>(dev11));
        OfaClose();
        if (!OfaOpen(dev11, ctx, w, h, src_fmt))
        {
            Log("[synth] optical flow: the session could not be rebuilt for the new size, "
                "so this stops.");
            g_ofa.stopped = true;
        }
    }
    else if (!g_ofa.ready && !g_ofa.tried)
    {
        OfaOpen(dev11, ctx, w, h, src_fmt);
    }

    // Nothing below ran, so nothing below is charged for. Counting a frame here
    // would put an "optical flow" clause on the timing line of a session that
    // fell back to a ReShade provider and never ran the engine at all, which is
    // the one thing that line exists to be able to deny.
    if (!g_ofa.ready) return false;

    // Started here rather than at the top of the function, so a one-off session
    // open -- two shader compiles, five textures and an nvOFInit -- is not
    // averaged into the per-frame figure it would otherwise dominate. What is
    // measured from here is the copy, both dispatches and the execute: all of
    // it new cost, and all of it on ReShade's present thread.
    QueryPerformanceCounter(&t0);

    // The colour frame, converted into the only 4-channel format the engine
    // takes. One copy and one dispatch; the copy is ~0.03 ms of bandwidth at
    // 3840x1600 against 1.6 ms of execute, which is why there is one path here
    // and not a second that tries an SRV over the back buffer first.
    ctx->CopyResource(g_ofa.copy, src);
    OfaDispatch(ctx, g_ofa.luma_cs, g_ofa.copy_srv, g_ofa.src_uav[g_ofa.cur], w, h);

    if (!g_ofa.primed)
    {
        g_ofa.primed = true;
        g_ofa.cur ^= 1;
        if (!g_ofa.first_said)
        {
            g_ofa.first_said = true;
            Log("[synth] optical flow: the first frame has no previous frame to compare "
                "against, so nothing is delivered on it. Flow starts on the next one.");
        }
        goto done;
    }

    {
        NvOfExecuteInputParams  in;
        NvOfExecuteOutputParams eo;
        memset(&in, 0, sizeof(in));
        memset(&eo, 0, sizeof(eo));
        in.inputFrame           = g_ofa.reg_src[g_ofa.cur];       // the CURRENT frame
        in.referenceFrame       = g_ofa.reg_src[g_ofa.cur ^ 1];   // the PREVIOUS frame
        in.disableTemporalHints = 1;   // what every measurement for this was taken with
        eo.outputBuffer         = g_ofa.reg_flow;

        auto exec = reinterpret_cast<PFN_OfaExecute>(g_ofa.slot[6]);
        const NvOfStatus r = OfaSafeExecute(exec, g_ofa.session, &in, &eo);
        ++g_ofa.frames;

        if (r == kOfaRaised)
        {
            // Calling back into a component that faulted is what turned a
            // disabled add-on into a killed process on Final Fantasy VII Remake.
            Log("[synth] optical flow: nvOFExecute raised an exception, so this stops here. "
                "The game keeps rendering on its own.");
            g_ofa.ready   = false;
            g_ofa.stopped = true;
            goto done;
        }
        if (r != kOfaSuccess)
        {
            if (g_ofa.fails == 0 || g_ofa.fails == 2)
            {
                char msg[512];
                OfaLastError(msg, sizeof(msg));
                Log("[synth] optical flow: execute %llu refused with status %ld (%s). %s",
                    g_ofa.frames, r, OfaStatusName(r), msg);
            }
            // This frame is not delivered and the game's own frame goes to screen
            // untouched.
            //
            // The slot still advances, and it has to. The luma dispatch above has
            // ALREADY written this frame into src[cur], destroying what was there --
            // so the frame this comment used to call "the previous frame, kept" is
            // gone whatever happens next. Leaving cur alone made the following
            // frame overwrite src[cur] again and compare against a reference two
            // frames old, reporting two frames of motion as one. Advancing it
            // costs the one comparison that was already lost and puts the next
            // pair back in step.
            //
            // Never fired in the sessions measured so far -- Red Dead Redemption 2
            // delivered 1800 frames out of 1800 with no refusal -- so this is a
            // defect found by reading rather than one seen.
            g_ofa.cur ^= 1;
            if (++g_ofa.fails >= 3)
            {
                Log("[synth] optical flow: three consecutive failures, so this stops. No "
                    "ReShade motion-vector texture is substituted: the temporal history "
                    "DLSS has accumulated was built from these vectors and swapping the "
                    "estimator under it is the defect this path was rebuilt to avoid.");
                g_ofa.ready   = false;
                g_ofa.stopped = true;
            }
            goto done;
        }

        g_ofa.fails = 0;
        OfaDispatch(ctx, g_ofa.flow_cs, g_ofa.flow_srv, g_ofa.mv_uav, w, h);
        // Wanted, not run: the check stages and maps about 30 MB and blocks
        // until the GPU catches up, and inside the bracket below that stall
        // would be charged to the per-frame flow figure -- the same error the
        // bracket was moved down the function to avoid for the session open.
        want_check = !g_ofa.checked;

        // No AddRef, and for the reason written over the Get in SynthParams:
        // SynthOfa owns mv for the life of the shape and nothing downstream
        // releases what it takes out of the contract.
        out->res     = reshade::api::resource { reinterpret_cast<uintptr_t>(g_ofa.mv) };
        out->tex11   = nullptr;
        out->w       = w;
        out->h       = h;
        out->fmt     = DXGI_FORMAT_R16G16_FLOAT;
        out->samples = 1;
        out->usage   = 0;

        // Advanced HERE and not at the exit, and the difference is not
        // housekeeping. On a refused execute this is deliberately skipped, so
        // the next frame overwrites src[cur] and keeps src[cur^1] -- which is
        // the input of the last execute that SUCCEEDED, and therefore the last
        // frame DLSS was actually given. A refused frame is not delivered, so
        // DLSS's history is still at that same frame, and the vector it needs is
        // the one spanning the whole gap rather than the last hop of it.
        // Advancing unconditionally would hand a two-frame gap a one-frame
        // vector and under-reproject the history by exactly one frame, which is
        // the smear this path was rebuilt to remove, reintroduced on the
        // recovery frame.
        g_ofa.cur ^= 1;
        delivered = true;
    }

done:
    QueryPerformanceCounter(&t1);
    g_bridge.ofa_ticks += (t1.QuadPart - t0.QuadPart);
    ++g_bridge.ofa_frames;

    // Outside the bracket on purpose. It runs at most a handful of times in a
    // session and it blocks on the GPU; charging it to the per-frame figure
    // would make the first report of a working session read three times its
    // real cost.
    if (want_check) OfaSelfCheck(dev11, ctx);

    // TimingTick is reached only through BridgeFrame, so a session that runs the
    // engine every frame and never delivers one would spend this and never
    // report it -- and a blocking call in the game's present thread that no line
    // in the log accounts for is precisely what this measurement exists to make
    // impossible. Said here when 600 flow frames have gone by with not one
    // delivered frame to carry the figure.
    if (g_bridge.ofa_frames >= 600 && g_bridge.timed_frames == 0)
    {
        // Asked for here rather than read off g_bridge.qpf: that field is filled
        // by TimingTick, and TimingTick not having run is the whole condition
        // this branch is testing for.
        LARGE_INTEGER qpf;
        QueryPerformanceFrequency(&qpf);
        Log("[synth] optical flow: %.2f ms/frame over %llu frames, and not one of them "
            "reached the bridge's own frame path -- so nothing else in this log accounts "
            "for that time. It is spent on ReShade's present thread.",
            1000.0 * double(g_bridge.ofa_ticks) / double(qpf.QuadPart)
                   / double(g_bridge.ofa_frames),
            g_bridge.ofa_frames);
        g_bridge.ofa_ticks  = 0;
        g_bridge.ofa_frames = 0;
    }

    return delivered;
}

// The D3D11 game's own device and its own back buffer, resolved off the runtime.
// Everything this does beyond that is in OfaFrameOn.
static bool OfaFrame(reshade::api::effect_runtime *rt, reshade::api::command_list *cmd,
                     reshade::api::resource_view rtv, UINT w, UINT h, SynthBound *out)
{
    SynthBound blank = {};
    *out = blank;

    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(
        static_cast<uintptr_t>(cmd->get_native()));
    if (ctx == nullptr) return false;

    SynthBound back = {};
    SynthResolveView(rt->get_device(), static_cast<uint32_t>(reshade::api::device_api::d3d11),
                     rtv, &back);
    if (back.tex11 == nullptr) return false;
    if (back.w != w || back.h != h) { back.tex11->Release(); return false; }

    ID3D11Device *dev11 = nullptr;
    ctx->GetDevice(&dev11);
    if (dev11 == nullptr) { back.tex11->Release(); return false; }

    const bool delivered = OfaFrameOn(dev11, ctx, back.tex11, back.fmt, w, h, out);

    dev11->Release();
    back.tex11->Release();
    return delivered;
}
