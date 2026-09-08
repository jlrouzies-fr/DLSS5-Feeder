// Deterministic GPU regression for an early effect flush bypassing present waits.
// Needs Vulkan 1.2, two queues and host-coherent memory; no game or NGX needed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../src/feed_vk_present_order.h"

static void check(VkResult r) { if (r != VK_SUCCESS) { std::printf("Vulkan failure %d\n", r); std::exit(1); } }
static void require(bool b, const char *what) { if (!b) { std::printf("FAIL: %s\n", what); std::exit(1); } }
#define FN(name) PFN_vk##name name

int main()
{
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    require(loader != nullptr, "Vulkan loader");
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    auto CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "feeder-present-order-test"; app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &app;
    const char *instance_ext = VK_KHR_SURFACE_EXTENSION_NAME;
    ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = &instance_ext;
    VkInstance instance; check(CreateInstance(&ici, nullptr, &instance));
#define INSTANCE(name) auto name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name)); require(name != nullptr, #name)
    INSTANCE(EnumeratePhysicalDevices); INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(GetPhysicalDeviceMemoryProperties); INSTANCE(GetPhysicalDeviceProperties);
    INSTANCE(CreateDevice); INSTANCE(GetDeviceProcAddr); INSTANCE(DestroyInstance);
    uint32_t count = 0; check(EnumeratePhysicalDevices(instance, &count, nullptr));
    require(count != 0, "physical device");
    std::vector<VkPhysicalDevice> devices(count); check(EnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice phys = devices[0];
    VkPhysicalDeviceProperties props; GetPhysicalDeviceProperties(phys, &props);
    std::printf("GPU: %s\n", props.deviceName);
    GetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count); GetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueCount >= 2 && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
    require(family != UINT32_MAX, "two graphics queues");
    float priorities[2] = { 1, 1 };
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = family; qci.queueCount = 2; qci.pQueuePriorities = priorities;
    VkPhysicalDeviceTimelineSemaphoreFeatures feature = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    feature.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; dci.pNext = &feature;
    const char *device_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = &device_ext;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev; check(CreateDevice(phys, &dci, nullptr, &dev));
    const auto present = GetDeviceProcAddr(dev, "vkQueuePresentKHR");
    HMODULE present_module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(present), &present_module);
    char module_path[MAX_PATH] = {}; GetModuleFileNameA(present_module, module_path, MAX_PATH);
    std::printf("Present dispatch: %p in %s; same as loader export=%d\n", reinterpret_cast<void *>(present), module_path,
        present == reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(loader, "vkQueuePresentKHR")));
#define DEVICE(name) auto name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(dev, "vk" #name)); require(name != nullptr, #name)
    DEVICE(GetDeviceQueue); DEVICE(CreateBuffer); DEVICE(GetBufferMemoryRequirements); DEVICE(AllocateMemory);
    DEVICE(BindBufferMemory); DEVICE(MapMemory); DEVICE(UnmapMemory); DEVICE(DestroyBuffer); DEVICE(FreeMemory);
    DEVICE(CreateCommandPool); DEVICE(AllocateCommandBuffers); DEVICE(BeginCommandBuffer); DEVICE(EndCommandBuffer);
    DEVICE(CmdFillBuffer); DEVICE(CmdCopyBuffer); DEVICE(CmdPipelineBarrier); DEVICE(QueueSubmit);
    DEVICE(CreateSemaphore); DEVICE(SignalSemaphore); DEVICE(DestroySemaphore); DEVICE(CreateFence);
    DEVICE(WaitForFences); DEVICE(ResetFences); DEVICE(DestroyFence); DEVICE(DeviceWaitIdle);
    DEVICE(DestroyCommandPool); DEVICE(DestroyDevice); DEVICE(ResetCommandPool);
    VkQueue producer, graphics; GetDeviceQueue(dev, family, 0, &producer); GetDeviceQueue(dev, family, 1, &graphics);
    VkPhysicalDeviceMemoryProperties memory; GetPhysicalDeviceMemoryProperties(phys, &memory);
    VkBuffer buffers[2]; VkDeviceMemory allocations[2]; uint32_t *mapped[2];
    for (int i = 0; i < 2; ++i)
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bci.size = 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(CreateBuffer(dev, &bci, nullptr, &buffers[i]));
        VkMemoryRequirements req; GetBufferMemoryRequirements(dev, buffers[i], &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < memory.memoryTypeCount; ++t)
            if ((req.memoryTypeBits & (1u << t)) && (memory.memoryTypes[t].propertyFlags &
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = t; break; }
        require(type != UINT32_MAX, "coherent host memory");
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; mai.allocationSize = req.size; mai.memoryTypeIndex = type;
        check(AllocateMemory(dev, &mai, nullptr, &allocations[i]));
        check(BindBufferMemory(dev, buffers[i], allocations[i], 0));
        check(MapMemory(dev, allocations[i], 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void **>(&mapped[i])));
    }
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; pci.queueFamilyIndex = family;
    VkCommandPool pool; check(CreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
    VkCommandBuffer cmds[2]; check(AllocateCommandBuffers(dev, &cai, cmds));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence;
    check(CreateFence(dev, &fci, nullptr, &fence));
    VkSemaphoreTypeCreateInfo sti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO }; sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; sci.pNext = &sti;
    VkSemaphore release; check(CreateSemaphore(dev, &sci, nullptr, &release)); sci.pNext = nullptr;
    VkSemaphore ready[2]; for (auto &sem : ready) check(CreateSemaphore(dev, &sci, nullptr, &sem));
    uint64_t serial = 0;
    for (int repetition = 0; repetition < 8; ++repetition)
    for (int mode = 0; mode < 3; ++mode) // deferred, broken early flush, fixed early flush
    {
        ++serial;
        *mapped[0] = 0x11111111; *mapped[1] = 0;
        const uint32_t fresh = 0x22220000 | static_cast<uint32_t>(serial);
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        check(BeginCommandBuffer(cmds[0], &begin)); CmdFillBuffer(cmds[0], buffers[0], 0, 4, fresh); check(EndCommandBuffer(cmds[0]));
        check(BeginCommandBuffer(cmds[1], &begin));
        VkBufferCopy copy = { 0, 0, 4 }; CmdCopyBuffer(cmds[1], buffers[0], buffers[1], 1, &copy);
        VkMemoryBarrier host = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        CmdPipelineBarrier(cmds[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        check(EndCommandBuffer(cmds[1]));
        const VkPipelineStageFlags stages[2] = { VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT };
        VkTimelineSemaphoreSubmitInfo timeline = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        timeline.waitSemaphoreValueCount = 1; timeline.pWaitSemaphoreValues = &serial;
        VkSubmitInfo produce = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; produce.pNext = &timeline;
        produce.waitSemaphoreCount = 1; produce.pWaitSemaphores = &release; produce.pWaitDstStageMask = stages;
        produce.commandBufferCount = 1; produce.pCommandBuffers = &cmds[0]; produce.signalSemaphoreCount = 2; produce.pSignalSemaphores = ready;
        check(QueueSubmit(producer, 1, &produce, VK_NULL_HANDLE));
        auto flush = [&](VkFence done) {
            VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmds[1];
            check(QueueSubmit(graphics, 1, &submit, done));
        };
        if (mode == 1)
        {
            flush(fence);
            check(WaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
            require(*mapped[1] == 0x11111111, "negative control must capture stale source");
            check(ResetFences(dev, 1, &fence));
        }
        if (mode == 2)
        {
            bool flushed = false;
            require(FeedVkWaitAndRearm(2, [&](uint32_t i) {
                uint64_t value = 0;
                VkTimelineSemaphoreSubmitInfo values = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
                values.waitSemaphoreValueCount = 1; values.pWaitSemaphoreValues = &value;
                VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; submit.pNext = &values;
                submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &ready[i]; submit.pWaitDstStageMask = stages;
                return QueueSubmit(graphics, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
            }, [&](uint32_t i) {
                if (!flushed) { flush(VK_NULL_HANDLE); flushed = true; }
                uint64_t value = 0;
                VkTimelineSemaphoreSubmitInfo values = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
                values.signalSemaphoreValueCount = 1; values.pSignalSemaphoreValues = &value;
                VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; submit.pNext = &values;
                submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &ready[i];
                return QueueSubmit(graphics, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
            }), "gate submits");
        }
        VkSubmitInfo final = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        final.waitSemaphoreCount = 2; final.pWaitSemaphores = ready; final.pWaitDstStageMask = stages;
        if (mode == 0) { final.commandBufferCount = 1; final.pCommandBuffers = &cmds[1]; }
        check(QueueSubmit(graphics, 1, &final, fence));
        require(WaitForFences(dev, 1, &fence, VK_TRUE, 1000000) == VK_TIMEOUT, "must wait for producer");
        VkSemaphoreSignalInfo release_info = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO }; release_info.semaphore = release; release_info.value = serial;
        check(SignalSemaphore(dev, &release_info));
        check(WaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
        require(*mapped[1] == (mode == 1 ? 0x11111111 : fresh), "captured frame contents");
        check(DeviceWaitIdle(dev)); check(ResetFences(dev, 1, &fence)); check(ResetCommandPool(dev, pool, 0));
    }
    std::puts("PASS: deferred=8 fresh; ungated early flush=8 stale; gated early flush=8 fresh; final binary waits all retire");
    for (auto sem : ready) DestroySemaphore(dev, sem, nullptr); DestroySemaphore(dev, release, nullptr);
    DestroyFence(dev, fence, nullptr); DestroyCommandPool(dev, pool, nullptr);
    for (int i = 0; i < 2; ++i) { UnmapMemory(dev, allocations[i]); DestroyBuffer(dev, buffers[i], nullptr); FreeMemory(dev, allocations[i], nullptr); }
    DestroyDevice(dev, nullptr); DestroyInstance(instance, nullptr); FreeLibrary(loader);
}
