// feed_vk.h - raw-Vulkan interop for the Vulkan transport (PLAN-VULKAN, case B).
//
// ReShade's create_resource/create_fence import D3D12 shared handles as the wrong
// external type (OPAQUE_WIN32), so they refuse a D3D12-created handle. We import the
// D3D12 fence and textures ourselves, with the correct D3D12_FENCE / D3D12_RESOURCE
// external types, then hand the resulting VkSemaphore / VkImage BACK to ReShade as
// api::fence / api::resource handles (which in the Vulkan backend are exactly those
// native objects). That keeps the per-frame queue signal/wait inside ReShade's own
// locks -- a raw vkQueueSubmit would race ReShade's and the game's submits.
//
// Our own images are kept permanently in VK_IMAGE_LAYOUT_GENERAL and transitioned
// only by the raw barriers here; ReShade only ever touches the game's own resources.
// The actual copies are raw vkCmd* recorded into ReShade's command buffer.

#pragma once
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_core.h>    // needs /Iexternal\vulkan (so vk_video/* resolves)
#include <vulkan/vulkan_win32.h>

static void Log(const char *fmt, ...);
template <typename H> static inline uint64_t FeedVkValue(H h)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
}
template <typename H> static inline H FeedVkDispatch(uint64_t v)
{
    return reinterpret_cast<H>(static_cast<uintptr_t>(v));
}

struct FeedVk
{
    HMODULE lib;
    VkDevice dev;

    PFN_vkGetDeviceProcAddr           GetDeviceProcAddr;
    PFN_vkCreateSemaphore             CreateSemaphore;
    PFN_vkDestroySemaphore            DestroySemaphore;
    PFN_vkImportSemaphoreWin32HandleKHR ImportSemaphoreWin32HandleKHR;
    PFN_vkCreateImage                 CreateImage;
    PFN_vkDestroyImage                DestroyImage;
    PFN_vkGetImageMemoryRequirements  GetImageMemoryRequirements;
    PFN_vkAllocateMemory              AllocateMemory;
    PFN_vkFreeMemory                  FreeMemory;
    PFN_vkBindImageMemory             BindImageMemory;
    PFN_vkCmdPipelineBarrier          CmdPipelineBarrier;
    PFN_vkCmdCopyImage                CmdCopyImage;
    PFN_vkCmdBlitImage                CmdBlitImage;

    bool ok;
};

// Resolve everything from the game's VkDevice. Returns false if any entry is missing
// (which, given the phase-0 probe already found the extensions present, should not
// happen -- but it is logged by the caller if it does).
static bool FeedVkLoad(FeedVk *vk, VkDevice device)
{
    *vk = {};
    vk->dev = device;
    vk->lib = LoadLibraryW(L"vulkan-1.dll");
    if (vk->lib == nullptr) return false;
    vk->GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vk->lib, "vkGetDeviceProcAddr"));
    if (vk->GetDeviceProcAddr == nullptr) return false;

    #define FEED_VK_GET(member, name) \
        vk->member = reinterpret_cast<PFN_vk##member>(vk->GetDeviceProcAddr(device, name)); \
        if (vk->member == nullptr) return false;
    FEED_VK_GET(CreateSemaphore,             "vkCreateSemaphore")
    FEED_VK_GET(DestroySemaphore,            "vkDestroySemaphore")
    FEED_VK_GET(ImportSemaphoreWin32HandleKHR, "vkImportSemaphoreWin32HandleKHR")
    FEED_VK_GET(CreateImage,                 "vkCreateImage")
    FEED_VK_GET(DestroyImage,                "vkDestroyImage")
    FEED_VK_GET(GetImageMemoryRequirements,  "vkGetImageMemoryRequirements")
    FEED_VK_GET(AllocateMemory,              "vkAllocateMemory")
    FEED_VK_GET(FreeMemory,                  "vkFreeMemory")
    FEED_VK_GET(BindImageMemory,             "vkBindImageMemory")
    FEED_VK_GET(CmdPipelineBarrier,          "vkCmdPipelineBarrier")
    FEED_VK_GET(CmdCopyImage,                "vkCmdCopyImage")
    FEED_VK_GET(CmdBlitImage,                "vkCmdBlitImage")
    #undef FEED_VK_GET

    vk->ok = true;
    return true;
}

// Import a D3D12 shared fence (from CreateSharedHandle) as a Vulkan TIMELINE
// semaphore. A D3D12 fence and a Vulkan timeline semaphore are the same object.
static VkSemaphore FeedVkImportFence(FeedVk *vk, HANDLE d3d12_fence_handle)
{
    VkSemaphoreTypeCreateInfo tci = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    tci.initialValue  = 0;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sci.pNext = &tci;
    VkSemaphore sem = VK_NULL_HANDLE;
    if (vk->CreateSemaphore(vk->dev, &sci, nullptr, &sem) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkImportSemaphoreWin32HandleInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
    imp.semaphore  = sem;
    imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    imp.handle     = d3d12_fence_handle;   // NOT consumed: the driver duplicates it
    if (vk->ImportSemaphoreWin32HandleKHR(vk->dev, &imp) != VK_SUCCESS)
    {
        vk->DestroySemaphore(vk->dev, sem, nullptr);
        return VK_NULL_HANDLE;
    }
    return sem;
}

// Import a D3D12 shared texture (from CreateSharedHandle) as a VkImage backed by the
// same memory. Dedicated allocation is required for imported D3D12 resources.
static bool FeedVkImportImage(FeedVk *vk, HANDLE d3d12_res_handle, UINT w, UINT h,
                              VkFormat fmt, bool storage, VkImage *out_image, VkDeviceMemory *out_mem,
                              VkDeviceSize d3d12_size)
{
    *out_image = VK_NULL_HANDLE;
    *out_mem   = VK_NULL_HANDLE;

    VkExternalMemoryImageCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext         = &ext;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = fmt;
    ici.extent        = { w, h, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        (storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT);
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk->CreateImage(vk->dev, &ici, nullptr, out_image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req = {};
    vk->GetImageMemoryRequirements(vk->dev, *out_image, &req);
    // No VkPhysicalDevice handle from ReShade, so pick the lowest allowed memory type.
    // Imported D3D12 default-heap memory is device-local; the driver constrains the
    // acceptable bits, and the lowest set bit is a well-worn working choice.
    uint32_t type_index = 0;
    for (uint32_t i = 0; i < 32; ++i)
        if (req.memoryTypeBits & (1u << i)) { type_index = i; break; }

    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.image = *out_image;
    VkImportMemoryWin32HandleInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    imp.pNext      = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    imp.handle     = d3d12_res_handle;     // duplicated by the driver, not consumed
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext           = &imp;
    // Dedicated imports describe the external D3D12 allocation (upstream e4422df).
    if (d3d12_size == 0 || d3d12_size == UINT64_MAX)
    {
        vk->DestroyImage(vk->dev, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        return false;
    }
    if (d3d12_size != req.size)
        Log("[feed] import size: Vulkan=%llu D3D12=%llu; using D3D12 allocation", req.size, d3d12_size);
    mai.allocationSize  = d3d12_size;
    mai.memoryTypeIndex = type_index;
    if (vk->AllocateMemory(vk->dev, &mai, nullptr, out_mem) != VK_SUCCESS)
    {
        vk->DestroyImage(vk->dev, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        return false;
    }
    if (vk->BindImageMemory(vk->dev, *out_image, *out_mem, 0) != VK_SUCCESS)
    {
        vk->FreeMemory(vk->dev, *out_mem, nullptr);
        vk->DestroyImage(vk->dev, *out_image, nullptr);
        *out_image = VK_NULL_HANDLE;
        *out_mem   = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// Barrier one of OUR images between two layouts on the given command buffer, covering
// all stages/access (correctness over precision; these are one copy per frame).
static void FeedVkBarrier(FeedVk *vk, VkCommandBuffer cb, VkImage img,
                          VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = img;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &b);
}

// Copy game image (already in src_layout, set by ReShade) -> our image (GENERAL).
static void FeedVkCopyImage(FeedVk *vk, VkCommandBuffer cb, VkImage src, VkImageLayout src_layout,
                            VkImage dst, VkImageLayout dst_layout, UINT w, UINT h)
{
    VkImageCopy c = {};
    c.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.extent         = { w, h, 1 };
    vk->CmdCopyImage(cb, src, src_layout, dst, dst_layout, 1, &c);
}

// Blit our output (GENERAL) -> game backbuffer (in dst_layout): handles a format /
// channel-order difference that a raw copy cannot.
static void FeedVkBlitImage(FeedVk *vk, VkCommandBuffer cb, VkImage src, VkImageLayout src_layout,
                            VkImage dst, VkImageLayout dst_layout, UINT w, UINT h)
{
    VkImageBlit bl = {};
    bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bl.srcOffsets[1]  = { static_cast<int32_t>(w), static_cast<int32_t>(h), 1 };
    bl.dstOffsets[1]  = { static_cast<int32_t>(w), static_cast<int32_t>(h), 1 };
    vk->CmdBlitImage(cb, src, src_layout, dst, dst_layout, 1, &bl, VK_FILTER_NEAREST);
}

// DXGI_FORMAT -> VkFormat for the shared-resource formats this project uses.
static VkFormat FeedVkFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return VK_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return VK_FORMAT_R16G16B16A16_SFLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT:       return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case DXGI_FORMAT_R32_FLOAT:             return VK_FORMAT_R32_SFLOAT;
    case DXGI_FORMAT_R16G16_FLOAT:          return VK_FORMAT_R16G16_SFLOAT;
    case DXGI_FORMAT_R8_UNORM:              return VK_FORMAT_R8_UNORM;
    default:                                return VK_FORMAT_UNDEFINED;
    }
}
