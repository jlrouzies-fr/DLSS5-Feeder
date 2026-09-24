// ---------------------------------------------------------------------------------------
// feed_hold12.h -- the output stabiliser (experimentHold): one D3D12 compute pass that keeps
// the shown picture where the game's frame did not change, and follows the model where it did.
//
// Why it exists. The neural consumer re-decides what a region "should" look like from small
// frame-to-frame differences in its input: a distant slope under a tree is bright while the
// camera pans and darkens over the frames after the stop, although the game drew it the same
// way on every one of them. Measured on the branch: it is not the model's history catching up
// (nine evaluates a frame changed nothing) and it is not the history at all (a reset on every
// frame flickers and still darkens). So the fix cannot be on the input side or in the evaluate
// count; it has to be on the output, and it has to tell model drift from a real change.
//
// The tell is right there: a real change alters the game's frame FIRST. So per output pixel,
// compare a 3x3 box of the input against an ANCHOR -- the same box as it was when this pixel
// last moved -- relative to the local brightness; where it moved, show the model's answer as is
// and move the anchor with it; where it did not, keep what was shown last frame, let the
// model's new opinion creep in at (1 - strength) per frame, and leave the anchor where it is.
// A pan unlocks everything (the native look comes through); a stop locks the picture where the
// pan left it; a hand or a torch unlocks only its own pixels. No reprojection, so nothing to
// ghost: any pixel whose input changed shows the current model output that frame.
//
// Why an anchor and not last frame's input: a slow change (clouds, a light fading) moves the
// input by less than the tolerance EVERY frame, so against last frame it never counts as change
// and the pixel stays locked on a picture that is quietly going stale -- then unlocks all at
// once when something finally crosses the line, which is the "pattern repeat" seen in a sky at
// strength 1. Against the anchor the drift accumulates, the gate opens gradually as it nears
// the tolerance, and the anchor follows at the same rate, so a slow drift is tracked slowly
// and a step is taken at once.
//
// D3D12 only, private textures, one dispatch and two copies; the caller's list, the caller's
// barriers at entry (Color a non-pixel-shader resource, Output an unordered access) and the
// same states on exit, so it drops in right after the evaluate on either the helper or the
// same-device transport. The private textures are SIMULTANEOUS_ACCESS so they decay to COMMON
// at every submission and are promoted afresh -- no state to track across frames.
// ---------------------------------------------------------------------------------------

#pragma once

template <typename T> static void FeedHoldRelease1(T *&p) { if (p) { p->Release(); p = nullptr; } }

struct FeedHold12
{
    ID3D12Device         *dev;
    ID3D12RootSignature  *root;
    ID3D12PipelineState  *pso;
    ID3D12DescriptorHeap *heap;       // shader-visible ring: four SRVs + two UAVs per pass
    UINT                  stride;
    UINT                  next;       // ring cursor, in sets
    bool                  ok;
    bool                  failed;     // init failed once: do not retry every frame

    ID3D12Resource       *shown[2];   // what went home last frame / this frame, at Output size/format
    ID3D12Resource       *anchor[2];  // per output pixel, the 3x3 input box as it was when the pixel
                                      // last moved (RGBA16F: private, so the format is ours to pick)
    UINT                  out_w, out_h;
    DXGI_FORMAT           out_fmt;
    int                   cur;        // shown[cur] / anchor[cur] are last frame's
    bool                  valid;      // they hold real frames
};

static const UINT kFeedHoldSets = 16;
static const UINT kFeedHoldDescs = 6;

static const char kFeedHoldHlsl[] =
    "Texture2D<float4>   now_in      : register(t0);\n"
    "Texture2D<float4>   anchor_prev : register(t1);\n"
    "Texture2D<float4>   shown_prev  : register(t2);\n"
    "Texture2D<float4>   model       : register(t3);\n"
    "RWTexture2D<float4> shown_out   : register(u0);\n"
    "RWTexture2D<float4> anchor_out  : register(u1);\n"
    "SamplerState        lin         : register(s0);\n"
    "cbuffer C : register(b0) { float strength; float tolerance; uint2 size; uint valid; };\n"
    "float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "[numthreads(8, 8, 1)]\n"
    "void cs(uint3 id : SV_DispatchThreadID) {\n"
    "  if (id.x >= size.x || id.y >= size.y) return;\n"
    "  float4 m = model.Load(int3(id.xy, 0));\n"
    "  float2 px = 1.0 / float2(size);\n"
    "  float2 uv = (float2(id.xy) + 0.5) * px;\n"
    "  // 3x3 box of the input, so one shimmering texel is not 'change'. Sampled by uv: the\n"
    "  // input may be smaller than the output (DLSS Super Resolution).\n"
    "  float3 a = 0.0;\n"
    "  [unroll] for (int y = -1; y <= 1; ++y)\n"
    "    [unroll] for (int x = -1; x <= 1; ++x)\n"
    "      a += now_in.SampleLevel(lin, uv + float2(x, y) * px, 0).rgb;\n"
    "  a /= 9.0;\n"
    "  if (valid == 0) { shown_out[id.xy] = m; anchor_out[id.xy] = float4(a, 1.0); return; }\n"
    "  float3 b = anchor_prev.Load(int3(id.xy, 0)).rgb;\n"
    "  float3 d = abs(a - b);\n"
    "  // Relative to local brightness: a global exposure drift moves dark pixels by tiny\n"
    "  // absolute amounts, and it must not unlock the whole picture.\n"
    "  float rel = max(d.r, max(d.g, d.b)) / max(max(Luma(a), Luma(b)), 0.02);\n"
    "  // 0 = still, 1 = moved (from the tolerance up to twice it), so the gate has a soft edge.\n"
    "  float change = saturate((rel - tolerance) / max(tolerance, 1e-4));\n"
    "  float keep = strength * (1.0 - change);\n"
    "  shown_out[id.xy]  = lerp(m, shown_prev.Load(int3(id.xy, 0)), keep);\n"
    "  anchor_out[id.xy] = float4(lerp(b, a, change), 1.0); }\n";

static bool FeedHold12Init(FeedHold12 &p, ID3D12Device *dev, pD3DCompile compile,
                           void (*log)(const char *, ...))
{
    FeedHold12 blank = {};
    p = blank;
    p.failed = true;   // until proven otherwise
    if (dev == nullptr || compile == nullptr) return false;
    p.dev = dev;

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors                    = 4;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors                    = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges   = ranges;
    params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 5;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters     = 2;
    rs.pParameters       = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers   = &samp;

    typedef HRESULT (WINAPI *PFN_SerializeRootSig)(const D3D12_ROOT_SIGNATURE_DESC *,
                                                   D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (d3d12 == nullptr) d3d12 = LoadLibraryW(L"d3d12.dll");
    auto serialize = d3d12 != nullptr
        ? reinterpret_cast<PFN_SerializeRootSig>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))
        : nullptr;
    if (serialize == nullptr) { log("[hold] d3d12.dll has no D3D12SerializeRootSignature"); return false; }

    ID3DBlob *sig = nullptr, *err = nullptr;
    HRESULT hr = serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr))
    {
        log("[hold] the root signature would not serialise 0x%08X: %s", hr,
            err ? (const char *)err->GetBufferPointer() : "");
        FeedHoldRelease1(err); FeedHoldRelease1(sig);
        return false;
    }
    FeedHoldRelease1(err);
    hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                  __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&p.root));
    FeedHoldRelease1(sig);
    if (FAILED(hr)) { log("[hold] the root signature would not create 0x%08X", hr); return false; }

    ID3DBlob *cs = nullptr;
    hr = compile(kFeedHoldHlsl, sizeof(kFeedHoldHlsl) - 1, "feedhold", nullptr, nullptr, "cs", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr))
    {
        log("[hold] the shader would not compile 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : "");
        FeedHoldRelease1(err); FeedHoldRelease1(cs); FeedHoldRelease1(p.root);
        return false;
    }
    FeedHoldRelease1(err);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = p.root;
    pd.CS             = { cs->GetBufferPointer(), cs->GetBufferSize() };
    hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&p.pso));
    FeedHoldRelease1(cs);
    if (FAILED(hr)) { log("[hold] the pipeline state failed 0x%08X", hr); FeedHoldRelease1(p.root); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kFeedHoldSets * kFeedHoldDescs;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&p.heap));
    if (FAILED(hr))
    {
        log("[hold] the descriptor heap failed 0x%08X", hr);
        FeedHoldRelease1(p.pso); FeedHoldRelease1(p.root);
        return false;
    }
    p.heap->SetName(L"dlss5-feed hold descriptors");
    p.root->SetName(L"dlss5-feed hold root signature");
    p.pso->SetName(L"dlss5-feed hold pipeline");
    p.stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    p.ok     = true;
    p.failed = false;
    log("[hold] output stabiliser ready (one compute pass after the evaluate)");
    return true;
}

// Drop the history textures (a resize, or the caller's slots were rebuilt). The GPU must be
// done with them -- the callers drain before they rebuild their own textures.
static void FeedHold12DropHistory(FeedHold12 &p)
{
    FeedHoldRelease1(p.shown[0]);
    FeedHoldRelease1(p.shown[1]);
    FeedHoldRelease1(p.anchor[0]);
    FeedHoldRelease1(p.anchor[1]);
    p.valid = false;
}

static void FeedHold12Release(FeedHold12 &p)
{
    FeedHold12DropHistory(p);
    FeedHoldRelease1(p.heap);
    FeedHoldRelease1(p.pso);
    FeedHoldRelease1(p.root);
    FeedHold12 blank = {};
    p = blank;
}

static ID3D12Resource *FeedHold12MakeTex(ID3D12Device *dev, UINT w, UINT h, DXGI_FORMAT fmt, bool uav, const wchar_t *name)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                          (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource *t = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            __uuidof(ID3D12Resource), reinterpret_cast<void **>(&t))))
        return nullptr;
    t->SetName(name);
    return t;
}

static void FeedHold12Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r,
                              D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    list->ResourceBarrier(1, &b);
}

// One pass. Entry states: `color` NON_PIXEL_SHADER_RESOURCE, `output` UNORDERED_ACCESS (the
// evaluate's contract). Exit states: the same. `reset` drops the history (a camera cut is not a
// still picture, and the model's own reset frame should be shown as it is).
static void FeedHold12Run(FeedHold12 &p, ID3D12GraphicsCommandList *list,
                          ID3D12Resource *color, ID3D12Resource *output,
                          float strength, float tolerance, bool reset, void (*log)(const char *, ...))
{
    if (!p.ok || list == nullptr || color == nullptr || output == nullptr) return;

    const D3D12_RESOURCE_DESC cd = color->GetDesc(), od = output->GetDesc();
    const UINT out_w = static_cast<UINT>(od.Width);
    if (p.shown[0] == nullptr || p.shown[1] == nullptr || p.anchor[0] == nullptr || p.anchor[1] == nullptr ||
        p.out_w != out_w || p.out_h != od.Height || p.out_fmt != od.Format)
    {
        FeedHold12DropHistory(p);
        p.shown[0]  = FeedHold12MakeTex(p.dev, out_w, od.Height, od.Format, true, L"dlss5-feed hold shown A");
        p.shown[1]  = FeedHold12MakeTex(p.dev, out_w, od.Height, od.Format, true, L"dlss5-feed hold shown B");
        p.anchor[0] = FeedHold12MakeTex(p.dev, out_w, od.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, true, L"dlss5-feed hold anchor A");
        p.anchor[1] = FeedHold12MakeTex(p.dev, out_w, od.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, true, L"dlss5-feed hold anchor B");
        if (p.shown[0] == nullptr || p.shown[1] == nullptr || p.anchor[0] == nullptr || p.anchor[1] == nullptr)
        {
            log("[hold] history textures failed (%ux%u fmt %u); stabiliser off", out_w, od.Height, od.Format);
            FeedHold12DropHistory(p);
            p.ok = false;
            return;
        }
        p.out_w = out_w; p.out_h = od.Height; p.out_fmt = od.Format;
        p.cur = 0;
        log("[hold] history textures: %ux%u (fmt %u) x2 shown + x2 anchor; input %llux%u (fmt %u)",
            out_w, od.Height, od.Format, cd.Width, cd.Height, cd.Format);
    }
    if (reset) p.valid = false;

    ID3D12Resource *shown_prev = p.shown[p.cur], *shown_out = p.shown[p.cur ^ 1];
    ID3D12Resource *anchor_prev = p.anchor[p.cur], *anchor_out = p.anchor[p.cur ^ 1];

    // The model's answer is read as a shader resource: typed UAV loads are optional for some
    // of the formats the Output can have, SRV loads are not.
    FeedHold12Barrier(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const UINT set = p.next % kFeedHoldSets;
    ++p.next;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = p.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = p.heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(set) * kFeedHoldDescs * p.stride;
    gpu.ptr += static_cast<UINT64>(set) * kFeedHoldDescs * p.stride;

    ID3D12Resource *srvs[4]     = { color, anchor_prev, shown_prev, output };
    const DXGI_FORMAT fmts[4]   = { cd.Format, DXGI_FORMAT_R16G16B16A16_FLOAT, od.Format, od.Format };
    for (int i = 0; i < 4; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = fmts[i];
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE at = cpu;
        at.ptr += static_cast<SIZE_T>(i) * p.stride;
        p.dev->CreateShaderResourceView(srvs[i], &sd, at);
    }
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format        = od.Format;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE at = cpu;
        at.ptr += static_cast<SIZE_T>(4) * p.stride;
        p.dev->CreateUnorderedAccessView(shown_out, nullptr, &ud, at);
        ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        at.ptr += p.stride;
        p.dev->CreateUnorderedAccessView(anchor_out, nullptr, &ud, at);
    }

    struct { float strength, tolerance; UINT w, h, valid; } consts =
        { strength, tolerance, out_w, od.Height, p.valid ? 1u : 0u };
    ID3D12DescriptorHeap *heaps[] = { p.heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(p.root);
    list->SetPipelineState(p.pso);
    list->SetComputeRootDescriptorTable(0, gpu);
    list->SetComputeRoot32BitConstants(1, 5, &consts, 0);
    list->Dispatch((out_w + 7) / 8, (od.Height + 7) / 8, 1);

    // The stabilised frame goes back into the Output in place. The private textures were
    // promoted by the dispatch (SRV / UAV) and decay to COMMON when this submission completes;
    // the anchor was written by the dispatch and needs nothing more.
    FeedHold12Barrier(list, shown_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    FeedHold12Barrier(list, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(output, shown_out);
    FeedHold12Barrier(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FeedHold12Barrier(list, shown_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

    p.cur ^= 1;
    p.valid = true;
}
