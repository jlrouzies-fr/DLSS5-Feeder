#pragma once

// ---------------------------------------------------------------------------
// D3D12 adjustable work-resolution patch (isolated module)
//
// This block is the only custom D3D12-resolution implementation added to the
// 0.12.0 base. The native 0.12.0 D3D12 path remains intact at 100%.
// Future upstream merges: copy this marked block + the Feed fields + the
// four explicit call-sites documented in PATCH_INTEGRATION below.
// ---------------------------------------------------------------------------
namespace D3D12ResolutionPatch
{

static void D3D12ResolutionPatch_ReleaseD3D12WorkResampler()
{
    SafeRelease(g.resample_pso12);
    SafeRelease(g.upscale_pso12);
    SafeRelease(g.resample_root12);
    for (int i = 0; i < Feed::kFrames; ++i)
    {
        SafeRelease(g.resample_heap12[i]);
        SafeRelease(g.upscale_rtv_heap12[i]);
    }
    SafeRelease(g.color_stage12);
    SafeRelease(g.mask_dummy12);
    g.resample_descriptor_size12 = 0;
}
static bool D3D12ResolutionPatch_CompileD3D12Shader(const char *src, const char *entry, const char *profile, ID3DBlob **out)
{
    *out = nullptr;
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (compile == nullptr)
    {
        Log("[feed] d3dcompiler_47.dll unavailable for D3D12 work-resolution shaders");
        return false;
    }
    ID3DBlob *err = nullptr;
    const HRESULT hr = compile(src, strlen(src), "dlss5-feed-d3d12-workres", nullptr, nullptr,
                               entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr))
    {
        Log("[feed] D3D12 shader %s compile failed 0x%08X: %s", entry, hr,
            err ? (const char *)err->GetBufferPointer() : "");
        SafeRelease(err);
        SafeRelease(*out);
        return false;
    }
    SafeRelease(err);
    return true;
}
static bool D3D12ResolutionPatch_CreateD3D12WorkResampler(UINT native_w, UINT native_h, UINT work_w, UINT work_h, DXGI_FORMAT bb_fmt)
{
    if (native_w == work_w && native_h == work_h) return true;
    if (g.dev12 == nullptr) return false;

    static const char kShader[] =
        "Texture2D<float4> gColor : register(t0);\n"
        "Texture2D<float2> gMV : register(t1);\n"
        "Texture2D<float> gDepth : register(t2);\n"
        "Texture2D<float> gMask : register(t3);\n"
        "RWTexture2D<float4> oColor : register(u0);\n"
        "RWTexture2D<float2> oMV : register(u1);\n"
        "RWTexture2D<float> oDepth : register(u2);\n"
        "RWTexture2D<float> oMask : register(u3);\n"
        "SamplerState linearSmp : register(s0);\n"
        "SamplerState pointSmp : register(s1);\n"
        "cbuffer WorkResConstants : register(b0) { float2 mvScale; float2 srcToWork; }\n"
        "[numthreads(8,8,1)]\n"
        "void CSMain(uint3 id : SV_DispatchThreadID) {\n"
        "  uint2 dim; oColor.GetDimensions(dim.x, dim.y);\n"
        "  if (id.x >= dim.x || id.y >= dim.y) return;\n"
        "  float2 uv = (float2(id.xy) + 0.5) / float2(dim);\n"
        "  oColor[id.xy] = gColor.SampleLevel(linearSmp, uv, 0);\n"
        "  oMV[id.xy] = gMV.SampleLevel(pointSmp, uv, 0) * mvScale;\n"
        "  oDepth[id.xy] = gDepth.SampleLevel(pointSmp, uv, 0);\n"
        "  oMask[id.xy] = gMask.SampleLevel(pointSmp, uv, 0);\n"
        "}\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2); o.uv = uv; o.pos = float4(uv * float2(2,-2) + float2(-1,1),0,1); return o; }\n"
        "float4 PSMain(VSOut i) : SV_Target0 { return gColor.Sample(linearSmp, i.uv); }\n";

    ID3DBlob *cs = nullptr, *vs = nullptr, *ps = nullptr;
    if (!D3D12ResolutionPatch_CompileD3D12Shader(kShader, "CSMain", "cs_5_0", &cs) ||
        !D3D12ResolutionPatch_CompileD3D12Shader(kShader, "VSMain", "vs_5_0", &vs) ||
        !D3D12ResolutionPatch_CompileD3D12Shader(kShader, "PSMain", "ps_5_0", &ps))
    {
        SafeRelease(cs); SafeRelease(vs); SafeRelease(ps);
        return false;
    }

    D3D12_DESCRIPTOR_RANGE sr = {};
    sr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    sr.NumDescriptors = 4;
    sr.BaseShaderRegister = 0;
    sr.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE ur = {};
    ur.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ur.NumDescriptors = 4;
    ur.BaseShaderRegister = 0;
    ur.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &sr;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &ur;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[2].Constants.ShaderRegister = 0;
    rp[2].Constants.RegisterSpace = 0;
    rp[2].Constants.Num32BitValues = 4;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sam[2] = {};
    sam[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sam[0].AddressU = sam[0].AddressV = sam[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sam[0].ShaderRegister = 0;
    sam[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sam[1] = sam[0];
    sam[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sam[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 3;
    rsd.pParameters = rp;
    rsd.NumStaticSamplers = 2;
    rsd.pStaticSamplers = sam;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob *sig = nullptr, *sig_err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sig_err);
    if (FAILED(hr))
    {
        Log("[feed] D3D12 work-resolution root signature failed 0x%08X: %s", hr,
            sig_err ? (const char *)sig_err->GetBufferPointer() : "");
        SafeRelease(sig_err); SafeRelease(sig); SafeRelease(cs); SafeRelease(vs); SafeRelease(ps);
        return false;
    }
    SafeRelease(sig_err);
    hr = g.dev12->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                      __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&g.resample_root12));
    SafeRelease(sig);
    if (FAILED(hr))
    {
        Log("[feed] D3D12 work-resolution root signature creation failed 0x%08X", hr);
        SafeRelease(cs); SafeRelease(vs); SafeRelease(ps); return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd = {};
    cpd.pRootSignature = g.resample_root12;
    cpd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    hr = g.dev12->CreateComputePipelineState(&cpd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&g.resample_pso12));
    if (FAILED(hr))
    {
        Log("[feed] D3D12 work-resolution compute PSO failed 0x%08X", hr);
        SafeRelease(cs); SafeRelease(vs); SafeRelease(ps); return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd = {};
    gpd.pRootSignature = g.resample_root12;
    gpd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    gpd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    gpd.BlendState.AlphaToCoverageEnable = FALSE;
    gpd.BlendState.IndependentBlendEnable = FALSE;
    gpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gpd.SampleMask = UINT_MAX;
    gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gpd.RasterizerState.FrontCounterClockwise = FALSE;
    gpd.RasterizerState.DepthClipEnable = TRUE;
    gpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gpd.NumRenderTargets = 1;
    gpd.RTVFormats[0] = TypedColorFormat(bb_fmt);
    gpd.SampleDesc.Count = 1;
    hr = g.dev12->CreateGraphicsPipelineState(&gpd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&g.upscale_pso12));
    SafeRelease(cs); SafeRelease(vs); SafeRelease(ps);
    if (FAILED(hr))
    {
        Log("[feed] D3D12 work-resolution upscale PSO failed 0x%08X", hr);
        return false;
    }

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = native_w; rd.Height = native_h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = bb_fmt; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    hr = g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g.color_stage12));
    if (FAILED(hr))
    {
        Log("[feed] D3D12 native work-resolution color staging failed 0x%08X", hr);
        return false;
    }

    D3D12_RESOURCE_DESC md = rd;
    md.Width = 1; md.Height = 1; md.Format = DXGI_FORMAT_R8_UNORM;
    UINT8 zero = 0;
    D3D12_SUBRESOURCE_DATA init = {};
    (void)zero; (void)init;
    // A zero mask is used only as a descriptor placeholder when the optional mask is absent.
    // It is populated through a tiny upload below on the first resource build if needed.
    hr = g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &md, D3D12_RESOURCE_STATE_COMMON,
                                          nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g.mask_dummy12));
    if (FAILED(hr))
    {
        Log("[feed] D3D12 dummy mask creation failed 0x%08X", hr);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 9; // 4 SRV inputs + 4 UAV outputs + 1 final-output SRV
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g.dev12->GetDescriptorHandleIncrementSize(hd.Type);
    for (int i = 0; i < Feed::kFrames; ++i)
    {
        hr = g.dev12->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap),
                                           reinterpret_cast<void **>(&g.resample_heap12[i]));
        if (FAILED(hr))
        {
            Log("[feed] D3D12 work-resolution descriptor heap failed 0x%08X", hr);
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC rh = {};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = 1;
        rh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = g.dev12->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap),
                                           reinterpret_cast<void **>(&g.upscale_rtv_heap12[i]));
        if (FAILED(hr))
        {
            Log("[feed] D3D12 upscale RTV heap failed 0x%08X", hr);
            return false;
        }
    }
    g.resample_descriptor_size12 = g.dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Log("[feed] D3D12 native work-resolution resampler ready");
    return true;
}
static D3D12_CPU_DESCRIPTOR_HANDLE D3D12ResolutionPatch_ResampleCpuHandle(int frame_slot, UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.resample_heap12[frame_slot]->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * g.resample_descriptor_size12;
    return h;
}
static D3D12_GPU_DESCRIPTOR_HANDLE D3D12ResolutionPatch_ResampleGpuHandle(int frame_slot, UINT index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = g.resample_heap12[frame_slot]->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(index) * g.resample_descriptor_size12;
    return h;
}
static void D3D12ResolutionPatch_CreateD3D12ResampleDescriptors(int frame_slot, ID3D12Resource *color_src,
                                           ID3D12Resource *mv_src, ID3D12Resource *depth_src,
                                           ID3D12Resource *mask_src)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;

    sv.Format = g.color_fmt;
    g.dev12->CreateShaderResourceView(color_src, &sv, D3D12ResolutionPatch_ResampleCpuHandle(frame_slot, 0));
    sv.Format = DXGI_FORMAT_R16G16_FLOAT;
    g.dev12->CreateShaderResourceView(mv_src, &sv, D3D12ResolutionPatch_ResampleCpuHandle(frame_slot, 1));
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    g.dev12->CreateShaderResourceView(depth_src, &sv, D3D12ResolutionPatch_ResampleCpuHandle(frame_slot, 2));
    sv.Format = DXGI_FORMAT_R8_UNORM;
    g.dev12->CreateShaderResourceView(mask_src ? mask_src : g.mask_dummy12, &sv, D3D12ResolutionPatch_ResampleCpuHandle(frame_slot, 3));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = g.color_fmt;
    g.dev12->CreateUnorderedAccessView(g.tex12[SLOT_COLOR], nullptr, &uv, D3D12ResolutionPatch_ResampleCpuHandle(frame_slot, 4));
    uv.Format = DXGI_FORMAT_R16G16_FLOAT;
    g.dev12->CreateUnorderedAccessView(g.tex12[SLOT_MV], nullptr, &uv, D3D12ResolutionPatch_ResampleCpuHandle(frame_slot, 5));
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    g.dev12->CreateUnorderedAccessView(g.tex12[SLOT_DEPTH], nullptr, &uv, D3D12ResolutionPatch_ResampleCpuHandle(frame_slot, 6));
    uv.Format = DXGI_FORMAT_R8_UNORM;
    g.dev12->CreateUnorderedAccessView(g.tex12[SLOT_MASK], nullptr, &uv, D3D12ResolutionPatch_ResampleCpuHandle(frame_slot, 7));

    sv.Format = g.output_fmt;
    g.dev12->CreateShaderResourceView(g.tex12[SLOT_OUTPUT], &sv, D3D12ResolutionPatch_ResampleCpuHandle(frame_slot, 8));
}
static bool D3D12ResolutionPatch_RecordD3D12WorkResample(ID3D12Resource *bb, ID3D12Resource *mv, ID3D12Resource *depth,
                                    ID3D12Resource *mask, UINT native_w, UINT native_h)
{
    const int slot = g.frame_slot;
    D3D12ResolutionPatch_CreateD3D12ResampleDescriptors(slot, g.color_stage12, mv, depth, mask);

    Barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyResource(g.color_stage12, bb);
    Barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(g.color_stage12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Barrier(g.tex12[SLOT_COLOR], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(g.tex12[SLOT_MV], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(g.tex12[SLOT_DEPTH], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(g.tex12[SLOT_MASK], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap *heaps[] = { g.resample_heap12[slot] };
    g.list->SetDescriptorHeaps(1, heaps);
    g.list->SetComputeRootSignature(g.resample_root12);
    g.list->SetPipelineState(g.resample_pso12);
    g.list->SetComputeRootDescriptorTable(0, D3D12ResolutionPatch_ResampleGpuHandle(slot, 0));
    g.list->SetComputeRootDescriptorTable(1, D3D12ResolutionPatch_ResampleGpuHandle(slot, 4));
    const float constants[4] = { g_cfg.mv_scale_x, g_cfg.mv_scale_y,
                                 static_cast<float>(native_w) / static_cast<float>(g.width),
                                 static_cast<float>(native_h) / static_cast<float>(g.height) };
    g.list->SetComputeRoot32BitConstants(2, 4, constants, 0);
    g.list->Dispatch((g.width + 7) / 8, (g.height + 7) / 8, 1);

    Barrier(g.tex12[SLOT_COLOR], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g.tex12[SLOT_MV], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g.tex12[SLOT_DEPTH], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g.tex12[SLOT_MASK], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Keep the staging copy in COPY_DEST between frames. This makes the next frame's
    // backbuffer copy deterministic without tracking another per-resource state flag.
    Barrier(g.color_stage12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    return true;
}
static bool D3D12ResolutionPatch_RecordD3D12Upscale(ID3D12Resource *bb)
{
    const int slot = g.frame_slot;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.upscale_rtv_heap12[slot]->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = g.color_fmt;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g.dev12->CreateRenderTargetView(bb, &rd, rtv);

    ID3D12DescriptorHeap *heaps[] = { g.resample_heap12[slot] };
    g.list->SetDescriptorHeaps(1, heaps);
    g.list->SetGraphicsRootSignature(g.resample_root12);
    g.list->SetPipelineState(g.upscale_pso12);
    g.list->SetGraphicsRootDescriptorTable(0, D3D12ResolutionPatch_ResampleGpuHandle(slot, 8));
    const UINT zero_constants[4] = {};
    g.list->SetGraphicsRoot32BitConstants(2, 4, zero_constants, 0);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(g.backbuffer_width), static_cast<float>(g.backbuffer_height), 0.0f, 1.0f };
    const D3D12_RECT sc = { 0, 0, static_cast<LONG>(g.backbuffer_width), static_cast<LONG>(g.backbuffer_height) };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g.list->DrawInstanced(3, 1, 0, 0);
    return true;
}

    static bool Create(UINT native_w, UINT native_h, UINT work_w, UINT work_h, DXGI_FORMAT bb_fmt)
    {
        return D3D12ResolutionPatch_CreateD3D12WorkResampler(native_w, native_h, work_w, work_h, bb_fmt);
    }

    static void Shutdown()
    {
        D3D12ResolutionPatch_ReleaseD3D12WorkResampler();
    }

    static bool Resample(ID3D12Resource *bb, ID3D12Resource *mv, ID3D12Resource *depth,
                         ID3D12Resource *mask, UINT native_w, UINT native_h)
    {
        return D3D12ResolutionPatch_RecordD3D12WorkResample(bb, mv, depth, mask, native_w, native_h);
    }

    static bool Upscale(ID3D12Resource *bb)
    {
        return D3D12ResolutionPatch_RecordD3D12Upscale(bb);
    }
}
