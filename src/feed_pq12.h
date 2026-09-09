// ---------------------------------------------------------------------------------------
// feed_pq12.h -- one D3D12 compute pass that converts between PQ and linear light.
//
// The D3D11 add-on could bridge HDR10 by extending shaders it already had. The other three
// transports had no programmable pass at all: same-device D3D12, Vulkan and OpenGL move
// pixels with copy_resource and nothing else, so there was nowhere to put a transfer
// function. This is that missing pass, written once and shared by all three.
//
// It is D3D12 only, and that is the trick rather than a limitation:
//
//   * Vulkan and OpenGL import shared textures that WE created on OUR private D3D12 device
//     (MakeSharedTexVk / MakeSharedTexGl), and we record through our own command list.
//     Converting there touches nothing the game owns, so there is no SPIR-V and no GLSL.
//   * the same-device transport is the one exception -- there the device IS the game's, so
//     this runs on it. That is why hdr_bridge can be turned off.
//
// COMPUTE, not a full-screen draw, and that is the reason the shared textures need no new
// flags: the Output is already a UAV because DLSS writes it, and the Color is already a
// shader resource. A pixel-shader version would have needed ALLOW_RENDER_TARGET on textures
// the game imports, which is exactly the sort of change that breaks an import for reasons
// that surface a long way from here. It also means ONE pipeline state instead of one per
// destination format, because a compute PSO does not bake in a render-target format.
//
// The maths is the same ST.2084 pair the D3D11 path uses, verified the same way: a
// PQ -> linear -> fp16 -> PQ round trip lands within 0.06 of one 10-bit code value.
// ---------------------------------------------------------------------------------------

#pragma once

// Self-contained: this header is included with the others, before the add-on's own
// SafeRelease exists, so it brings its own.
template <typename T> static void FeedPqRelease1(T *&p) { if (p) { p->Release(); p = nullptr; } }

struct FeedPq12
{
    ID3D12Device         *dev;
    ID3D12RootSignature  *root;
    ID3D12PipelineState  *pso;
    ID3D12DescriptorHeap *heap;      // shader-visible ring: one SRV + one UAV per pass
    UINT                  stride;
    UINT                  next;      // ring cursor, in pairs
    bool                  ok;
};

// Each pass burns one SRV/UAV pair, written just before the dispatch that reads them while
// the GPU may still be reading an earlier frame's. The allocator ring bounds frames in
// flight well below this.
static const UINT kFeedPqPairs = 16;

static const char kFeedPqHlsl[] =
    "Texture2D<float4>   src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "cbuffer C : register(b0) { float scale; uint encode; uint2 size; };\n"
    "// SMPTE ST.2084. Decode returns 0..1 where 1.0 is 10000 nits; the caller's scale puts\n"
    "// paper white at 1.0 on the way in and takes it back out on the way home.\n"
    "static const float M1 = 0.1593017578125, M2 = 78.84375;\n"
    "static const float C1 = 0.8359375, C2 = 18.8515625, C3 = 18.6875;\n"
    "float3 PqDecode(float3 n) {\n"
    "  float3 p = pow(max(n, 0.0), 1.0 / M2);\n"
    "  return pow(max(p - C1, 0.0) / max(C2 - C3 * p, 1e-6), 1.0 / M1); }\n"
    "float3 PqEncode(float3 y) {\n"
    "  float3 p = pow(saturate(y), M1);\n"
    "  return pow((C1 + C2 * p) / (1.0 + C3 * p), M2); }\n"
    "[numthreads(8, 8, 1)]\n"
    "void cs(uint3 id : SV_DispatchThreadID) {\n"
    "  if (id.x >= size.x || id.y >= size.y) return;\n"
    "  float4 c = src.Load(int3(id.xy, 0));\n"
    "  float3 o = encode != 0 ? PqEncode(max(c.rgb, 0.0) * scale) : PqDecode(c.rgb) * scale;\n"
    "  dst[id.xy] = float4(o, 1.0); }\n";

static bool FeedPq12Init(FeedPq12 &p, ID3D12Device *dev, pD3DCompile compile,
                         void (*log)(const char *, ...))
{
    FeedPq12 blank = {};
    p = blank;
    if (dev == nullptr || compile == nullptr) return false;
    p.dev = dev;

    // One SRV, one UAV, four root constants. Nothing else is needed, and on the same-device
    // transport anything else would be one more thing to get wrong on the game's own device.
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors                    = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors                    = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges   = ranges;
    params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 4;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters   = params;

    // Resolved by name, not linked. The add-on must load into games that have no d3d12.dll
    // at all, so nothing here may become a load-time import -- which is why D3D12CreateDevice
    // is fetched the same way a few hundred lines away.
    typedef HRESULT (WINAPI *PFN_SerializeRootSig)(const D3D12_ROOT_SIGNATURE_DESC *,
                                                   D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (d3d12 == nullptr) d3d12 = LoadLibraryW(L"d3d12.dll");
    auto serialize = d3d12 != nullptr
        ? reinterpret_cast<PFN_SerializeRootSig>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))
        : nullptr;
    if (serialize == nullptr)
    {
        log("[feed] PQ pass: d3d12.dll has no D3D12SerializeRootSignature");
        return false;
    }

    ID3DBlob *sig = nullptr, *err = nullptr;
    HRESULT hr = serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr))
    {
        log("[feed] PQ pass: the root signature would not serialise 0x%08X: %s", hr,
            err ? (const char *)err->GetBufferPointer() : "");
        FeedPqRelease1(err); FeedPqRelease1(sig);
        return false;
    }
    FeedPqRelease1(err);
    hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                  __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&p.root));
    FeedPqRelease1(sig);
    if (FAILED(hr)) { log("[feed] PQ pass: the root signature would not create 0x%08X", hr); return false; }

    // cs_5_0 rather than 5_1: a game shipping an old d3dcompiler_47 next to its exe rejects
    // 5_1 outright, which is the trap DetectStaleD3DCompiler exists for.
    ID3DBlob *cs = nullptr;
    hr = compile(kFeedPqHlsl, sizeof(kFeedPqHlsl) - 1, "feedpq", nullptr, nullptr, "cs", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr))
    {
        log("[feed] PQ pass: the shader would not compile 0x%08X: %s", hr,
            err ? (const char *)err->GetBufferPointer() : "");
        FeedPqRelease1(err); FeedPqRelease1(cs); FeedPqRelease1(p.root);
        return false;
    }
    FeedPqRelease1(err);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = p.root;
    pd.CS             = { cs->GetBufferPointer(), cs->GetBufferSize() };
    hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState),
                                         reinterpret_cast<void **>(&p.pso));
    FeedPqRelease1(cs);
    if (FAILED(hr))
    {
        log("[feed] PQ pass: the pipeline state failed 0x%08X", hr);
        FeedPqRelease1(p.root);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kFeedPqPairs * 2;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap),
                                   reinterpret_cast<void **>(&p.heap));
    if (FAILED(hr))
    {
        log("[feed] PQ pass: the descriptor heap failed 0x%08X", hr);
        FeedPqRelease1(p.pso); FeedPqRelease1(p.root);
        return false;
    }

    p.stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    p.ok = true;
    return true;
}

static void FeedPq12Release(FeedPq12 &p)
{
    FeedPqRelease1(p.heap);
    FeedPqRelease1(p.pso);
    FeedPqRelease1(p.root);
    FeedPq12 blank = {};
    p = blank;
}

// One dispatch: `src` read, `dst` written. The caller owns the barriers -- src must already
// be a non-pixel-shader resource and dst an unordered access.
//
// This binds a root signature, a PSO and a descriptor heap on the command list it is given.
// On the private-device transports that list is ours alone. On the same-device transport it
// is ReShade's, which rebinds its own state for every pass that follows -- the same contract
// the D3D11 copy-home blit has relied on since the beginning.
static void FeedPq12Run(FeedPq12 &p, ID3D12GraphicsCommandList *list,
                        ID3D12Resource *src, DXGI_FORMAT src_fmt,
                        ID3D12Resource *dst, DXGI_FORMAT dst_fmt,
                        UINT width, UINT height, bool encode, float scale)
{
    if (!p.ok || list == nullptr || src == nullptr || dst == nullptr) return;

    const UINT pair = p.next % kFeedPqPairs;
    ++p.next;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = p.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = p.heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(pair) * 2 * p.stride;
    gpu.ptr += static_cast<UINT64>(pair) * 2 * p.stride;

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format                  = src_fmt;
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    p.dev->CreateShaderResourceView(src, &sd, cpu);

    D3D12_CPU_DESCRIPTOR_HANDLE ucpu = cpu;
    ucpu.ptr += p.stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format        = dst_fmt;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    p.dev->CreateUnorderedAccessView(dst, nullptr, &ud, ucpu);

    struct { float scale; UINT encode; UINT w, h; } consts = { scale, encode ? 1u : 0u, width, height };

    ID3D12DescriptorHeap *heaps[] = { p.heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(p.root);
    list->SetPipelineState(p.pso);
    list->SetComputeRootDescriptorTable(0, gpu);
    list->SetComputeRoot32BitConstants(1, 4, &consts, 0);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}
