// Phase-0 spike for the 32-bit Vulkan transport (PLAN-VULKAN32 §Phase 0): the
// 32-bit Vulkan half. Run spike-vkhost64.exe first, then this.
//
// It answers, at 32 bits and on the machine that will actually run the game, every
// question the plan flags as gating:
//
//   (a) does the 32-bit ICD expose external_memory_win32 / external_semaphore_win32 /
//       timeline_semaphore at all?  -- the extension list is printed in full
//   (b) does src/feed_vk.h compile and resolve as x86 code?  -- it is included and
//       used verbatim, so a build failure here is the answer
//   (c) does a D3D12 committed texture import at 32 bits, both with and without
//       storage usage (COLOR vs OUTPUT in the real path)?  -- both are imported
//   (d) does the lowest-set-bit memory-type heuristic in FeedVkImportImage pick a
//       usable type?  -- the allowed mask and the chosen index are printed
//
// The round trip itself: import the host's texture and both fences, put the image
// into GENERAL and say so, wait the first timeline semaphore, read the image back and
// verify the host's pattern, write our own pattern in, signal the second semaphore.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

// feed_vk.h forward-declares the add-on's own static Log() and calls it from its inline
// helpers. In the add-on that resolves against dlss5-feed.cpp; here there is no such
// translation unit, and MSVC rejects a static function that is used but never defined
// (C2129) -- which broke this spike's build without saying anything about the spike.
// The spike prints to stdout, so give it the same shape and send it there.
static void Log(const char *fmt, ...);
#include "../src/feed_vk.h"     // the add-on's own transport header, compiled x86
#include "spike-vk-share.h"

static void Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

// feed_vk_hook.h's extension list, without dragging MinHook into the spike.
static const char *kWanted[] = {
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
};

// The extra entry points the spike needs and the add-on does not (it records into
// ReShade's command buffer and submits through ReShade; here there is no ReShade).
struct VkExtra
{
    PFN_vkGetInstanceProcAddr                   GetInstanceProcAddr;
    PFN_vkCreateInstance                        CreateInstance;
    PFN_vkEnumeratePhysicalDevices              EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties           GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties     GetPhysicalDeviceMemoryProperties;
    PFN_vkEnumerateDeviceExtensionProperties    EnumerateDeviceExtensionProperties;
    PFN_vkCreateDevice                          CreateDevice;
    PFN_vkGetDeviceProcAddr                     GetDeviceProcAddr;

    PFN_vkGetDeviceQueue        GetDeviceQueue;
    PFN_vkCreateCommandPool     CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer    BeginCommandBuffer;
    PFN_vkEndCommandBuffer      EndCommandBuffer;
    PFN_vkQueueSubmit           QueueSubmit;
    PFN_vkQueueWaitIdle         QueueWaitIdle;
    PFN_vkCreateBuffer          CreateBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory      BindBufferMemory;
    PFN_vkMapMemory             MapMemory;
    PFN_vkCmdCopyImageToBuffer  CmdCopyImageToBuffer;
    PFN_vkCmdCopyBufferToImage  CmdCopyBufferToImage;
};

static VkExtra x;
static FeedVk  vk;

#define GET_INST(member, name) \
    x.member = reinterpret_cast<PFN_vk##member>(x.GetInstanceProcAddr(inst, name)); \
    if (x.member == nullptr) { printf("FAIL: missing %s\n", name); return false; }
#define GET_DEV(member, name) \
    x.member = reinterpret_cast<PFN_vk##member>(x.GetDeviceProcAddr(dev, name)); \
    if (x.member == nullptr) { printf("FAIL: missing %s\n", name); return false; }

static VkInstance       g_inst;
static VkPhysicalDevice g_phys;
static VkDevice         g_dev;
static VkQueue          g_queue;
static uint32_t         g_family = UINT32_MAX;
static VkCommandPool    g_pool;
static VkCommandBuffer  g_cb;

static bool MakeInstance()
{
    HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
    if (lib == nullptr) { printf("FAIL: no 32-bit vulkan-1.dll (SysWOW64) -- %lu\n", GetLastError()); return false; }
    x.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
    if (x.GetInstanceProcAddr == nullptr) { printf("FAIL: no vkGetInstanceProcAddr\n"); return false; }

    VkInstance inst = VK_NULL_HANDLE;   // for the GET_INST macro; null is legal for CreateInstance
    GET_INST(CreateInstance, "vkCreateInstance")

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "dlss5-feed-spike-vk32";
    ai.apiVersion       = VK_API_VERSION_1_2;   // timeline semaphores + external memory are core here
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    VkResult r = x.CreateInstance(&ici, nullptr, &g_inst);
    if (r != VK_SUCCESS)
    {
        printf("note: vkCreateInstance at API 1.2 -> %d; retrying at 1.1\n", r);
        ai.apiVersion = VK_API_VERSION_1_1;
        r = x.CreateInstance(&ici, nullptr, &g_inst);
    }
    if (r != VK_SUCCESS) { printf("FAIL: vkCreateInstance -> %d\n", r); return false; }
    inst = g_inst;

    GET_INST(EnumeratePhysicalDevices,               "vkEnumeratePhysicalDevices")
    GET_INST(GetPhysicalDeviceProperties,            "vkGetPhysicalDeviceProperties")
    GET_INST(GetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties")
    GET_INST(GetPhysicalDeviceMemoryProperties,      "vkGetPhysicalDeviceMemoryProperties")
    GET_INST(EnumerateDeviceExtensionProperties,     "vkEnumerateDeviceExtensionProperties")
    GET_INST(CreateDevice,                           "vkCreateDevice")
    GET_INST(GetDeviceProcAddr,                      "vkGetDeviceProcAddr")
    return true;
}

// Probe (a): what does the 32-bit ICD actually offer?
static bool PickDevice()
{
    uint32_t n = 0;
    x.EnumeratePhysicalDevices(g_inst, &n, nullptr);
    if (n == 0) { printf("FAIL: no Vulkan physical devices at 32-bit\n"); return false; }
    std::vector<VkPhysicalDevice> devs(n);
    x.EnumeratePhysicalDevices(g_inst, &n, devs.data());
    g_phys = devs[0];

    VkPhysicalDeviceProperties props = {};
    x.GetPhysicalDeviceProperties(g_phys, &props);
    printf("vk32: device \"%s\" (API %u.%u.%u, driver 0x%08X)\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion), props.driverVersion);

    uint32_t en = 0;
    x.EnumerateDeviceExtensionProperties(g_phys, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    if (en > 0) x.EnumerateDeviceExtensionProperties(g_phys, nullptr, &en, exts.data());
    printf("vk32: %u device extensions. The ones the transport needs:\n", en);
    bool all = true;
    for (const char *want : kWanted)
    {
        bool have = false;
        for (const auto &e : exts) if (strcmp(e.extensionName, want) == 0) { have = true; break; }
        printf("vk32:   %-46s %s\n", want, have ? "present" : "MISSING");
        if (!have) all = false;
    }
    if (!all)
        printf("vk32: (a missing extension here is the answer to probe (a) and stops the 32-bit "
               "Vulkan transport dead -- nothing below can work around it)\n");

    uint32_t qn = 0;
    x.GetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    x.GetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, qs.data());
    for (uint32_t i = 0; i < qn; ++i)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_family = i; break; }
    if (g_family == UINT32_MAX) { printf("FAIL: no graphics queue family\n"); return false; }
    return all;
}

static bool MakeDevice()
{
    // Exactly what feed_vk_hook.h would have appended to a game's create info.
    std::vector<const char *> use(std::begin(kWanted), std::end(kWanted));
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = g_family;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceTimelineSemaphoreFeatures tl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    tl.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext                   = &tl;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = static_cast<uint32_t>(use.size());
    dci.ppEnabledExtensionNames = use.data();

    const VkResult r = x.CreateDevice(g_phys, &dci, nullptr, &g_dev);
    if (r != VK_SUCCESS) { printf("FAIL: vkCreateDevice -> %d\n", r); return false; }
    VkDevice dev = g_dev;   // for GET_DEV

    GET_DEV(GetDeviceQueue,              "vkGetDeviceQueue")
    GET_DEV(CreateCommandPool,           "vkCreateCommandPool")
    GET_DEV(AllocateCommandBuffers,      "vkAllocateCommandBuffers")
    GET_DEV(BeginCommandBuffer,          "vkBeginCommandBuffer")
    GET_DEV(EndCommandBuffer,            "vkEndCommandBuffer")
    GET_DEV(QueueSubmit,                 "vkQueueSubmit")
    GET_DEV(QueueWaitIdle,               "vkQueueWaitIdle")
    GET_DEV(CreateBuffer,                "vkCreateBuffer")
    GET_DEV(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements")
    GET_DEV(BindBufferMemory,            "vkBindBufferMemory")
    GET_DEV(MapMemory,                   "vkMapMemory")
    GET_DEV(CmdCopyImageToBuffer,        "vkCmdCopyImageToBuffer")
    GET_DEV(CmdCopyBufferToImage,        "vkCmdCopyBufferToImage")

    x.GetDeviceQueue(g_dev, g_family, 0, &g_queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_family;
    if (x.CreateCommandPool(g_dev, &pci, nullptr, &g_pool) != VK_SUCCESS) { printf("FAIL: vkCreateCommandPool\n"); return false; }
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool        = g_pool;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    if (x.AllocateCommandBuffers(g_dev, &cbi, &g_cb) != VK_SUCCESS) { printf("FAIL: vkAllocateCommandBuffers\n"); return false; }

    // Probe (b): the add-on's own header, resolving on a 32-bit device.
    if (!FeedVkLoad(&vk, g_dev)) { printf("FAIL: FeedVkLoad could not resolve the interop entry points\n"); return false; }
    printf("vk32: FeedVkLoad resolved every entry point the transport needs\n");
    return true;
}

// Probe (d): FeedVkImportImage picks the lowest allowed memory type. Reproduce the
// choice on a throwaway image with the same create info, so the log says what it was.
static void ReportMemoryTypeChoice(VkFormat fmt, uint32_t w, uint32_t h, bool storage)
{
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
    VkImage probe = VK_NULL_HANDLE;
    if (vk.CreateImage(g_dev, &ici, nullptr, &probe) != VK_SUCCESS) return;
    VkMemoryRequirements req = {};
    vk.GetImageMemoryRequirements(g_dev, probe, &req);
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < 32; ++i) if (req.memoryTypeBits & (1u << i)) { chosen = i; break; }

    VkPhysicalDeviceMemoryProperties mp = {};
    x.GetPhysicalDeviceMemoryProperties(g_phys, &mp);
    const VkMemoryPropertyFlags f = chosen < mp.memoryTypeCount ? mp.memoryTypes[chosen].propertyFlags : 0;
    printf("vk32: %s image: size %llu, allowed memory types 0x%08X -> heuristic picks index %u "
           "(%sDEVICE_LOCAL%s)\n",
           storage ? "storage" : "sampled", (unsigned long long)req.size, req.memoryTypeBits, chosen,
           (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "" : "NOT ",
           (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? ", host visible" : "");
    vk.DestroyImage(g_dev, probe, nullptr);
}

// A host-visible staging buffer big enough for the whole image.
static bool MakeStaging(VkDeviceSize size, VkBuffer *out_buf, VkDeviceMemory *out_mem, void **out_ptr)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (x.CreateBuffer(g_dev, &bci, nullptr, out_buf) != VK_SUCCESS) { printf("FAIL: vkCreateBuffer\n"); return false; }

    VkMemoryRequirements req = {};
    x.GetBufferMemoryRequirements(g_dev, *out_buf, &req);
    VkPhysicalDeviceMemoryProperties mp = {};
    x.GetPhysicalDeviceMemoryProperties(g_phys, &mp);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
    if (type == UINT32_MAX) { printf("FAIL: no host-visible coherent memory type\n"); return false; }

    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = type;
    if (vk.AllocateMemory(g_dev, &mai, nullptr, out_mem) != VK_SUCCESS) { printf("FAIL: vkAllocateMemory(staging)\n"); return false; }
    if (x.BindBufferMemory(g_dev, *out_buf, *out_mem, 0) != VK_SUCCESS) { printf("FAIL: vkBindBufferMemory\n"); return false; }
    if (x.MapMemory(g_dev, *out_mem, 0, VK_WHOLE_SIZE, 0, out_ptr) != VK_SUCCESS) { printf("FAIL: vkMapMemory\n"); return false; }
    return true;
}

// One recorded-and-submitted command buffer, optionally waiting/signalling a timeline.
static bool Submit(void (*record)(VkCommandBuffer, void *), void *ctx,
                   VkSemaphore wait_sem, uint64_t wait_value,
                   VkSemaphore signal_sem, uint64_t signal_value)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (x.BeginCommandBuffer(g_cb, &bi) != VK_SUCCESS) { printf("FAIL: vkBeginCommandBuffer\n"); return false; }
    record(g_cb, ctx);
    if (x.EndCommandBuffer(g_cb) != VK_SUCCESS) { printf("FAIL: vkEndCommandBuffer\n"); return false; }

    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ts = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    ts.waitSemaphoreValueCount   = wait_sem   != VK_NULL_HANDLE ? 1u : 0u;
    ts.pWaitSemaphoreValues      = &wait_value;
    ts.signalSemaphoreValueCount = signal_sem != VK_NULL_HANDLE ? 1u : 0u;
    ts.pSignalSemaphoreValues    = &signal_value;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext                = &ts;
    si.waitSemaphoreCount   = ts.waitSemaphoreValueCount;
    si.pWaitSemaphores      = &wait_sem;
    si.pWaitDstStageMask    = &stage;
    si.signalSemaphoreCount = ts.signalSemaphoreValueCount;
    si.pSignalSemaphores    = &signal_sem;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &g_cb;
    const VkResult r = x.QueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) { printf("FAIL: vkQueueSubmit -> %d\n", r); return false; }
    return true;
}

struct CopyCtx { VkImage img; VkBuffer buf; uint32_t w, h; };

static void RecordLayoutInit(VkCommandBuffer cb, void *ctx)
{
    auto *c = static_cast<CopyCtx *>(ctx);
    FeedVkBarrier(&vk, cb, c->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
}

static void RecordImageToBuffer(VkCommandBuffer cb, void *ctx)
{
    auto *c = static_cast<CopyCtx *>(ctx);
    VkBufferImageCopy r = {};
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent      = { c->w, c->h, 1 };
    x.CmdCopyImageToBuffer(cb, c->img, VK_IMAGE_LAYOUT_GENERAL, c->buf, 1, &r);
}

static void RecordBufferToImage(VkCommandBuffer cb, void *ctx)
{
    auto *c = static_cast<CopyCtx *>(ctx);
    VkBufferImageCopy r = {};
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent      = { c->w, c->h, 1 };
    x.CmdCopyBufferToImage(cb, c->buf, c->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

int main()
{
    printf("spike-vkclient32: 32-bit Vulkan importer (pid %lu)\n", GetCurrentProcessId());

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 100 && pipe == INVALID_HANDLE_VALUE; ++i)
    {
        pipe = CreateFileA(kSpikeVkPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) Sleep(100);
    }
    if (pipe == INVALID_HANDLE_VALUE)
    { printf("FAIL: could not open %s -- start spike-vkhost64.exe first\n", kSpikeVkPipe); return 1; }

    DWORD pid = GetCurrentProcessId(), put = 0, got = 0;
    if (!WriteFile(pipe, &pid, sizeof(pid), &put, nullptr)) { printf("FAIL: sending our pid\n"); return 1; }

    SpikeVkShare s = {};
    if (!ReadFile(pipe, &s, sizeof(s), &got, nullptr) || got != sizeof(s))
    { printf("FAIL: receiving the handles\n"); return 1; }
    printf("client: got texture %ux%u (%llu bytes) + two fences from the host\n",
           s.width, s.height, (unsigned long long)s.tex_size);

    if (!MakeInstance()) return 1;
    const bool have_all_extensions = PickDevice();
    if (!have_all_extensions) return 1;
    if (!MakeDevice()) return 1;

    // Probes (c) and (d): both usages, on the very same D3D12 resource.
    ReportMemoryTypeChoice(VK_FORMAT_R8G8B8A8_UNORM, s.width, s.height, false);
    ReportMemoryTypeChoice(VK_FORMAT_R8G8B8A8_UNORM, s.width, s.height, true);

    const VkSemaphore sem_in  = FeedVkImportFence(&vk, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(s.fence_in)));
    const VkSemaphore sem_out = FeedVkImportFence(&vk, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(s.fence_out)));
    if (sem_in == VK_NULL_HANDLE || sem_out == VK_NULL_HANDLE)
    { printf("FAIL: D3D12 fence -> Vulkan timeline semaphore import\n"); return 1; }
    printf("client: imported both D3D12 fences as timeline semaphores\n");

    HANDLE htex = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(s.tex));
    VkImage img_sampled = VK_NULL_HANDLE, img_storage = VK_NULL_HANDLE;
    VkDeviceMemory mem_sampled = VK_NULL_HANDLE, mem_storage = VK_NULL_HANDLE;
    if (!FeedVkImportImage(&vk, htex, s.width, s.height, VK_FORMAT_R8G8B8A8_UNORM, false, &img_sampled, &mem_sampled))
    { printf("FAIL: importing the D3D12 texture WITHOUT storage usage (the COLOR case)\n"); return 1; }
    if (!FeedVkImportImage(&vk, htex, s.width, s.height, VK_FORMAT_R8G8B8A8_UNORM, true, &img_storage, &mem_storage))
    { printf("FAIL: importing the D3D12 texture WITH storage usage (the OUTPUT case)\n"); return 1; }
    printf("client: imported the D3D12 texture as a VkImage both ways (sampled and storage)\n");

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(s.width) * s.height * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void *mapped = nullptr;
    if (!MakeStaging(bytes, &staging, &staging_mem, &mapped)) return 1;

    CopyCtx ctx = { img_sampled, staging, s.width, s.height };

    // Layout init BEFORE the host writes: UNDEFINED is allowed to discard contents,
    // so this has to happen while there is nothing worth keeping. Then say we are ready.
    if (!Submit(RecordLayoutInit, &ctx, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0)) return 1;
    if (x.QueueWaitIdle(g_queue) != VK_SUCCESS) { printf("FAIL: vkQueueWaitIdle after the layout init\n"); return 1; }
    BYTE ready = 1;
    if (!WriteFile(pipe, &ready, 1, &put, nullptr)) { printf("FAIL: reporting readiness\n"); return 1; }
    printf("client: image is in GENERAL; told the host to write\n");

    BYTE go = 0;
    if (!ReadFile(pipe, &go, 1, &got, nullptr) || got != 1) { printf("FAIL: waiting for the host's go\n"); return 1; }

    // Wait the first timeline semaphore ON THE QUEUE -- the real transport's ordering --
    // and read the image back through it.
    memset(mapped, 0, static_cast<size_t>(bytes));
    if (!Submit(RecordImageToBuffer, &ctx, sem_in, 1, VK_NULL_HANDLE, 0)) return 1;
    if (x.QueueWaitIdle(g_queue) != VK_SUCCESS) { printf("FAIL: vkQueueWaitIdle after the readback\n"); return 1; }
    printf("client: waited the shared fence to 1 and copied the image out\n");

    const BYTE *p = static_cast<const BYTE *>(mapped);
    for (VkDeviceSize i = 0; i < bytes; ++i)
        if (p[i] != static_cast<BYTE>(s.pattern_a))
        {
            printf("FAIL: Vulkan read 0x%02X at offset %llu, expected 0x%02X everywhere\n",
                   p[i], (unsigned long long)i, s.pattern_a);
            return 1;
        }
    printf("client: PASS -- D3D12's pattern 0x%02X arrived intact through the imported VkImage\n", s.pattern_a);

    // Answer with the host's pattern B and release the texture back to D3D12.
    memset(mapped, static_cast<int>(s.pattern_b), static_cast<size_t>(bytes));
    if (!Submit(RecordBufferToImage, &ctx, VK_NULL_HANDLE, 0, sem_out, 1)) return 1;
    printf("client: wrote 0x%02X and signalled the second fence to 1\n", s.pattern_b);

    if (x.QueueWaitIdle(g_queue) != VK_SUCCESS) { printf("FAIL: vkQueueWaitIdle after the write-back\n"); return 1; }
    printf("SPIKE PASS (client side): every import succeeded and the round trip completed. "
           "Check spike-vkhost64's output for the D3D12 half's verdict.\n");
    return 0;
}
