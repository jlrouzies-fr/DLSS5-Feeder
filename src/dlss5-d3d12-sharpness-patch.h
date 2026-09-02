#pragma once

// ---------------------------------------------------------------------------
// D3D12 RCAS sharpening patch (isolated module)
//
// This module is independent from the D3D12 work-resolution patch and from
// the existing D3D11 FSR1/EASU/RCAS path.
//
// Safety changes in this version:
//   - the UI/render value is kept in an interlocked module-local copy
//   - RCAS never reads/writes the same resource in one graphics pass
//   - the backbuffer is returned to the exact state supplied by the caller
//   - no redundant same-state resource barriers
//   - corrected shader alpha handling (float4 -> float3)
// ---------------------------------------------------------------------------
namespace D3D12SharpnessPatch
{
    static ID3D12Resource *s_input[Feed::kFrames] = {};
    static ID3D12Resource *s_stage[Feed::kFrames] = {};
    static ID3D12DescriptorHeap *s_srv_heap[Feed::kFrames] = {};
    static ID3D12DescriptorHeap *s_rtv_heap[Feed::kFrames] = {};
    static ID3D12RootSignature *s_root = nullptr;
    static ID3D12PipelineState *s_pso = nullptr;
    static UINT s_width = 0;
    static UINT s_height = 0;
    static DXGI_FORMAT s_fmt = DXGI_FORMAT_UNKNOWN;
    static UINT s_desc_size = 0;
    static volatile LONG s_sharpness_bits = 0;

    static void SetSharpness(float value)
    {
        value = std::max(0.0f, std::min(1.0f, value));
        uint32_t bits = 0;
        memcpy(&bits, &value, sizeof(bits));
        InterlockedExchange(&s_sharpness_bits, static_cast<LONG>(bits));
    }

    static float GetSharpness()
    {
        const uint32_t bits = static_cast<uint32_t>(InterlockedCompareExchange(&s_sharpness_bits, 0, 0));
        float value = 0.0f;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }

    static const char *kShader = R"HLSL(
Texture2D<float4> Src : register(t0);
SamplerState PointClamp : register(s0);

cbuffer Params : register(b0)
{
    float Sharpness;
    float2 InvSize;
    float _Pad;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 p;
    if (id == 0) p = float2(-1.0, -1.0);
    else if (id == 1) p = float2(-1.0, 3.0);
    else p = float2(3.0, -1.0);
    o.pos = float4(p, 0.0, 1.0);
    o.uv = p * float2(0.5, -0.5) + 0.5;
    return o;
}

// FSR1-style RCAS. This is deliberately kept as a standalone post-process.
float4 PSMain(VSOut i) : SV_Target
{
    uint w, h;
    Src.GetDimensions(w, h);
    int2 ip = int2(i.pos.xy);
    int2 maxp = int2(w, h) - 1;
    int2 z = int2(0, 0);

    int2 pN = clamp(ip + int2( 0,-1), z, maxp);
    int2 pW = clamp(ip + int2(-1, 0), z, maxp);
    int2 pE = clamp(ip + int2( 1, 0), z, maxp);
    int2 pS = clamp(ip + int2( 0, 1), z, maxp);

    float3 b  = Src.Load(int3(pN, 0)).rgb;
    float3 d  = Src.Load(int3(pW, 0)).rgb;
    float4 e4 = Src.Load(int3(clamp(ip, z, maxp), 0));
    float3 e  = e4.rgb;
    float3 f  = Src.Load(int3(pE, 0)).rgb;
    float3 hC = Src.Load(int3(pS, 0)).rgb;

    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = hC.b * 0.5 + (hC.r * 0.5 + hC.g);

    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    float ringMaxL = max(max(bL, dL), max(fL, hL));
    float ringMinL = min(min(bL, dL), min(fL, hL));
    float rangeL = max(ringMaxL - ringMinL, 1e-6);
    nz = saturate(abs(nz) / rangeL);
    nz = -0.5 * nz + 1.0;

    float3 mn4 = min(min(b, d), min(f, hC));
    float3 mx4 = max(max(b, d), max(f, hC));

    const float RCAS_LIMIT = 0.25;
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, float3(1e-6, 1e-6, 1e-6));
    float3 hitMax = (1.0.xxx - max(mx4, e)) / min(4.0 * mn4 - 4.0.xxx, -float3(1e-6, 1e-6, 1e-6));
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0));

    float sharp = max(0.0, saturate(Sharpness) - 0.2);
    // The UI is a direct 0..1 strength control: 0 = off, 1 = maximum RCAS.
    // A mild sqrt curve makes low slider values visibly useful without changing the RCAS kernel.
    sharp = sqrt(sharp);
    lobe *= sharp * nz;

    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    float3 outRgb = (lobe * (b + d + hC + f) + e) * rcpL;
    return float4(saturate(outRgb), e4.a);
}
)HLSL";

    static void ReleaseAll()
    {
        for (int i = 0; i < Feed::kFrames; ++i)
        {
            SafeRelease(s_input[i]);
            SafeRelease(s_stage[i]);
            SafeRelease(s_srv_heap[i]);
            SafeRelease(s_rtv_heap[i]);
        }
        SafeRelease(s_pso);
        SafeRelease(s_root);
        s_desc_size = 0;
        s_width = s_height = 0;
        s_fmt = DXGI_FORMAT_UNKNOWN;
    }

    static bool Compile(const char *src, const char *entry, const char *profile, ID3DBlob **out)
    {
        *out = nullptr;
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = m != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
        if (compile == nullptr)
        {
            Log("[feed] d3dcompiler_47.dll unavailable for D3D12 sharpness shader");
            return false;
        }
        ID3DBlob *err = nullptr;
        const HRESULT hr = compile(src, strlen(src), "dlss5-feed-d3d12-sharpness", nullptr, nullptr,
                                   entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
        if (FAILED(hr))
        {
            if (err != nullptr) Log("[feed] D3D12 sharpness %s compile: %s", entry, static_cast<const char *>(err->GetBufferPointer()));
            SafeRelease(err);
            return false;
        }
        SafeRelease(err);
        return true;
    }

    static bool Create(UINT width, UINT height, DXGI_FORMAT fmt)
    {
        if (width == 0 || height == 0 || fmt == DXGI_FORMAT_UNKNOWN || g.dev12 == nullptr)
            return false;
        if (s_pso != nullptr && width == s_width && height == s_height && fmt == s_fmt)
            return true;

        ReleaseAll();
        s_width = width;
        s_height = height;
        s_fmt = fmt;
        s_desc_size = g.dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        ID3DBlob *vs = nullptr, *ps = nullptr, *sig_blob = nullptr;
        if (!Compile(kShader, "VSMain", "vs_5_0", &vs) || !Compile(kShader, "PSMain", "ps_5_0", &ps))
        {
            SafeRelease(vs); SafeRelease(ps); ReleaseAll();
            return false;
        }

        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.Num32BitValues = 4;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxAnisotropy = 1;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &samp;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, nullptr);
        if (FAILED(hr) || sig_blob == nullptr || FAILED(g.dev12->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                                                                         __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&s_root))))
        {
            SafeRelease(sig_blob); SafeRelease(vs); SafeRelease(ps); ReleaseAll();
            Log("[feed] D3D12 sharpness root signature failed 0x%08X", hr);
            return false;
        }
        SafeRelease(sig_blob);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = s_root;
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = fmt;
        pd.SampleDesc.Count = 1;
        hr = g.dev12->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&s_pso));
        SafeRelease(vs); SafeRelease(ps);
        if (FAILED(hr))
        {
            Log("[feed] D3D12 sharpness PSO failed 0x%08X", hr);
            ReleaseAll();
            return false;
        }

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width;
        rd.Height = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv = {};
        cv.Format = fmt;
        cv.Color[3] = 1.0f;

        for (int i = 0; i < Feed::kFrames; ++i)
        {
            hr = g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, &cv,
                                                  __uuidof(ID3D12Resource), reinterpret_cast<void **>(&s_input[i]));
            if (FAILED(hr)) { Log("[feed] D3D12 RCAS input[%d] creation failed 0x%08X", i, hr); ReleaseAll(); return false; }
            hr = g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, &cv,
                                                  __uuidof(ID3D12Resource), reinterpret_cast<void **>(&s_stage[i]));
            if (FAILED(hr)) { Log("[feed] D3D12 RCAS output[%d] creation failed 0x%08X", i, hr); ReleaseAll(); return false; }

            D3D12_DESCRIPTOR_HEAP_DESC hd = {};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 1;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g.dev12->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&s_srv_heap[i]))))
            { Log("[feed] D3D12 sharpness SRV heap[%d] creation failed", i); ReleaseAll(); return false; }

            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(g.dev12->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&s_rtv_heap[i]))))
            { Log("[feed] D3D12 sharpness RTV heap[%d] creation failed", i); ReleaseAll(); return false; }

            D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = fmt;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            g.dev12->CreateShaderResourceView(s_input[i], &sv, s_srv_heap[i]->GetCPUDescriptorHandleForHeapStart());

            D3D12_RENDER_TARGET_VIEW_DESC rv = {};
            rv.Format = fmt;
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            g.dev12->CreateRenderTargetView(s_stage[i], &rv, s_rtv_heap[i]->GetCPUDescriptorHandleForHeapStart());
        }
        return true;
    }

    static void Shutdown() { ReleaseAll(); }

    static bool Sharpen(ID3D12Resource *src, D3D12_RESOURCE_STATES src_state, UINT width, UINT height)
    {
        const float sharpness = GetSharpness();
        if (src == nullptr || sharpness <= 0.0f) return true;
        if (width == 0 || height == 0 || s_pso == nullptr) return false;

        const int slot = g.frame_slot;
        if (slot < 0 || slot >= Feed::kFrames || s_input[slot] == nullptr || s_stage[slot] == nullptr) return false;

        // Copy the current result to an owned SRV texture. The game backbuffer is
        // never simultaneously used as the RCAS input and render target.
        Barrier(src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.list->CopyResource(s_input[slot], src);
        Barrier(src, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
        Barrier(s_input[slot], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(s_stage[slot], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12DescriptorHeap *heaps[] = { s_srv_heap[slot] };
        g.list->SetDescriptorHeaps(1, heaps);
        g.list->SetGraphicsRootSignature(s_root);
        g.list->SetPipelineState(s_pso);
        g.list->SetGraphicsRootDescriptorTable(0, s_srv_heap[slot]->GetGPUDescriptorHandleForHeapStart());
        const float constants[4] = { sharpness, 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), 0.0f };
        g.list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        const D3D12_RECT sc = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        g.list->RSSetViewports(1, &vp);
        g.list->RSSetScissorRects(1, &sc);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = s_rtv_heap[slot]->GetCPUDescriptorHandleForHeapStart();
        g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g.list->DrawInstanced(3, 1, 0, 0);

        Barrier(s_stage[slot], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(src, src_state, D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyResource(src, s_stage[slot]);
        Barrier(src, D3D12_RESOURCE_STATE_COPY_DEST, src_state);
        Barrier(s_input[slot], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(s_stage[slot], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        return true;
    }
}
