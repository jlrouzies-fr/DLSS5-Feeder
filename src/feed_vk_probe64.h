// Opt-in six-stage transport probe. Included after Feed and Barrier are defined.
// Three 16x16 tiles, at 1/4, 1/2 and 3/4 of the image, read after BOTH APIs retire.
// No hashes include undefined row padding. B/E follow the active image/buffer route.
#pragma once
static constexpr UINT kVkProbeSide = 16, kVkProbePitch = 256;
static constexpr UINT kVkProbeTile = kVkProbeSide * kVkProbePitch;
static constexpr UINT kVkProbeStage = 3 * kVkProbeTile;
struct FeedVkProbe
{
    reshade::api::device *dev = nullptr;
    reshade::api::resource read[4] = {}; // A, B, E, F
    reshade::api::fence fence = {};
    ID3D12Resource *d12 = nullptr;       // C, D
    PFN_vkCmdCopyBuffer copy_buffer = nullptr;
    uint64_t serial = 0, pending = 0, frame = 0, d12_fence = 0;
    bool active = false, staged = false, intermediate = false, valid = false;
    UINT color_bpp = 0, output_bpp = 0;
} static g_vk_probe;

static void FeedVkProbeRelease() // Caller has drained both APIs.
{
    if (g_vk_probe.dev)
    {
        for (auto res : g_vk_probe.read) if (res.handle) g_vk_probe.dev->destroy_resource(res);
        if (g_vk_probe.fence.handle) g_vk_probe.dev->destroy_fence(g_vk_probe.fence);
    }
    SafeRelease(g_vk_probe.d12);
    g_vk_probe = {};
}

static uint64_t FeedVkProbeHash(const uint8_t *p, UINT bpp, bool *uniform)
{
    uint64_t hash = 1469598103934665603ull;
    *uniform = true;
    for (UINT tile = 0; tile < 3; ++tile)
        for (UINT y = 0; y < kVkProbeSide; ++y)
            for (UINT x = 0; x < kVkProbeSide * bpp; ++x)
            {
                const auto byte = p[tile * kVkProbeTile + y * kVkProbePitch + x];
                hash = (hash ^ byte) * 1099511628211ull;
                *uniform &= byte == p[x % bpp];
            }
    return hash;
}

static void FeedVkProbeAnalyse()
{
    auto &p = g_vk_probe;
    if (!p.pending || p.dev->get_completed_fence_value(p.fence) < p.pending ||
        g.fence12->GetCompletedValue() < p.d12_fence) return;
    if (!p.valid) { p.pending = 0; return; }
    uint64_t hash[6] = {};
    bool uniform[6] = {}, valid = true;
    const int stages[4] = { 0, 1, 4, 5 };
    for (int i = 0; i < 4; ++i)
    {
        void *data = nullptr;
        if (!p.dev->map_buffer_region(p.read[i], 0, kVkProbeStage, reshade::api::map_access::read_only, &data) || !data)
        { valid = false; continue; }
        const UINT bpp = i < 2 || i == 3 ? p.color_bpp : p.output_bpp;
        hash[stages[i]] = FeedVkProbeHash(static_cast<const uint8_t *>(data), bpp, &uniform[stages[i]]);
        p.dev->unmap_buffer_region(p.read[i]);
    }
    void *data = nullptr;
    const D3D12_RANGE range = { 0, 2 * kVkProbeStage }, none = { 0, 0 };
    if (SUCCEEDED(p.d12->Map(0, &range, &data)) && data)
    {
        hash[2] = FeedVkProbeHash(static_cast<const uint8_t *>(data), p.color_bpp, &uniform[2]);
        hash[3] = FeedVkProbeHash(static_cast<const uint8_t *>(data) + kVkProbeStage, p.output_bpp, &uniform[3]);
        p.d12->Unmap(0, &none);
    }
    else valid = false;
    Log("[feed] vk stages frame=%llu valid=%d route=%s F=%s A=%016llx B=%016llx C=%016llx D=%016llx E=%016llx F=%016llx uniform=%d%d%d%d%d%d bpp=%u/%u (3 tiles; C before evaluate, D after)",
        p.frame, valid, p.staged ? "buffer" : "image", p.intermediate ? "effect-target" : "swapchain",
        hash[0], hash[1], hash[2], hash[3], hash[4], hash[5],
        uniform[0], uniform[1], uniform[2], uniform[3], uniform[4], uniform[5], p.color_bpp, p.output_bpp);
    p.pending = 0;
}

static void FeedVkProbeBegin(reshade::api::effect_runtime *rt, reshade::api::resource target)
{
    auto &p = g_vk_probe;
    FeedVkProbeAnalyse();
    p.active = false;
    // The historical-output diagnostics deliberately mix frame n and n-1. Do not
    // publish a six-stage same-frame comparison for those modes or half copies.
    if (!g_cfg.vk_trace || g_cfg.mode < 2 || g_cfg.async_home || g_cfg.half_home ||
        p.pending || ((g.vk_frame + 1) % 60) != 0 || g.width < 64 || g.height < 64) return;
    const UINT cb = StaleProbeTexelBytes(g.color_fmt), ob = StaleProbeTexelBytes(g.output_fmt);
    if (!cb || !ob || cb * kVkProbeSide > kVkProbePitch || ob * kVkProbeSide > kVkProbePitch) return;
    if (!p.dev)
    {
        p.dev = rt->get_device();
        for (auto &res : p.read)
        {
            // ReShade's Vulkan map does not invalidate noncoherent allocations.
            // Its upload heap REQUIRES HOST_COHERENT; readback only prefers cached.
            // Vulkan allows transfer destinations in upload memory. These tiny
            // diagnostic buffers favour correct CPU visibility over read speed.
            const reshade::api::resource_desc desc(kVkProbeStage, reshade::api::memory_heap::upload, reshade::api::resource_usage::copy_dest);
            if (!p.dev->create_resource(desc, nullptr, reshade::api::resource_usage::copy_dest, &res))
            { FeedVkProbeRelease(); return; }
        }
        if (!p.dev->create_fence(0, reshade::api::fence_flags::none, &p.fence))
        { FeedVkProbeRelease(); return; }
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 2 * kVkProbeStage; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&p.d12))))
        { FeedVkProbeRelease(); return; }
        p.copy_buffer = reinterpret_cast<PFN_vkCmdCopyBuffer>(g.vk.GetDeviceProcAddr(g.vk.dev, "vkCmdCopyBuffer"));
        if (!p.copy_buffer) { FeedVkProbeRelease(); return; }
    }
    p.active = true;
    p.frame = g.vk_frame + 1;
    p.color_bpp = cb; p.output_bpp = ob;
    p.staged = g.vk_in_buf[SLOT_COLOR] != VK_NULL_HANDLE;
    p.intermediate = target != rt->get_current_back_buffer();
}

static void FeedVkProbeVk(VkCommandBuffer command, int stage, VkImage image, VkImageLayout layout,
    VkBuffer buffer = VK_NULL_HANDLE, UINT row_pitch = 0)
{
    auto &p = g_vk_probe;
    if (!p.active) return;
    const UINT bpp = stage == 2 ? p.output_bpp : p.color_bpp;
    const VkBuffer dest = FeedVkHandle<VkBuffer>(p.read[stage].handle);
    if (buffer)
    {
        VkBufferCopy copies[3 * kVkProbeSide] = {};
        for (UINT t = 0; t < 3; ++t)
            for (UINT y = 0; y < kVkProbeSide; ++y)
            {
                auto &copy = copies[t * kVkProbeSide + y];
                copy.srcOffset = VkDeviceSize(g.height * (t + 1) / 4 - kVkProbeSide / 2 + y) * row_pitch +
                    VkDeviceSize(g.width * (t + 1) / 4 - kVkProbeSide / 2) * bpp;
                copy.dstOffset = t * kVkProbeTile + y * kVkProbePitch;
                copy.size = kVkProbeSide * bpp;
            }
        p.copy_buffer(command, buffer, dest, 3 * kVkProbeSide, copies);
    }
    else
    {
        VkBufferImageCopy copies[3] = {};
        for (UINT t = 0; t < 3; ++t)
        {
            auto &copy = copies[t];
            copy.bufferOffset = t * kVkProbeTile; copy.bufferRowLength = kVkProbePitch / bpp;
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageOffset = { int(g.width * (t + 1) / 4 - kVkProbeSide / 2), int(g.height * (t + 1) / 4 - kVkProbeSide / 2), 0 };
            copy.imageExtent = { kVkProbeSide, kVkProbeSide, 1 };
        }
        g.vk.CmdCopyImageToBuffer(command, image, layout, dest, 3, copies);
    }
}

static void FeedVkProbeD12(bool output)
{
    if (!g_vk_probe.active) return;
    auto *resource = g.tex12[output ? SLOT_OUTPUT : SLOT_COLOR];
    const auto state = output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Barrier(resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = resource; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = g_vk_probe.d12; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    for (UINT t = 0; t < 3; ++t)
    {
        const UINT x = g.width * (t + 1) / 4 - kVkProbeSide / 2, y = g.height * (t + 1) / 4 - kVkProbeSide / 2;
        const D3D12_BOX box = { x, y, 0, x + kVkProbeSide, y + kVkProbeSide, 1 };
        dst.PlacedFootprint.Offset = (output ? kVkProbeStage : 0) + t * kVkProbeTile;
        dst.PlacedFootprint.Footprint = { output ? g.output_fmt : g.color_fmt, kVkProbeSide, kVkProbeSide, 1, kVkProbePitch };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    Barrier(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
}

static void FeedVkProbeEnd(bool done)
{
    auto &p = g_vk_probe;
    if (!p.active) return;
    p.active = false;
    VkMemoryBarrier host = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    const auto cb = FeedVkDispatch<VkCommandBuffer>(g.rs_queue->get_immediate_command_list()->get_native());
    g.vk.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &host, 0, nullptr, 0, nullptr);
    // Extra flush once per sample, only with vk_trace=1. Never map unretired work.
    const uint64_t serial = ++p.serial;
    if (g.rs_queue->signal(p.fence, serial))
    {
        p.pending = serial;
        p.d12_fence = g.fence_value;
        p.valid = done;
    }
}
