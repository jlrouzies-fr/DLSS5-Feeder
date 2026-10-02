#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cwchar>
#include <vector>
#include <cstring>
#include "vk-hook-test-api.h"

static LRESULT CALLBACK SmokeWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

static bool ExecutableSibling(wchar_t (&path)[MAX_PATH], const wchar_t *name)
{
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return false;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), name);
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    bool native = true, present_hooks = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--system") == 0) native = false;
        if (wcscmp(argv[i], L"--present-hooks") == 0) present_hooks = true;
    }
    wchar_t bridge_path[MAX_PATH] = {};
    if (native && (!ExecutableSibling(bridge_path, L"dxgi.dll") || LoadLibraryW(bridge_path) == nullptr))
    {
        std::printf("Loading fixture dxgi.dll failed: %lu\n", GetLastError());
        return 2;
    }

    HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (vulkan == nullptr)
    {
        std::printf("LoadLibraryW(vulkan-1.dll) failed: %lu\n", GetLastError());
        return 3;
    }

    wchar_t loader_path[MAX_PATH] = {};
    GetModuleFileNameW(vulkan, loader_path, MAX_PATH);
    if (wcsstr(loader_path, L"sl.interposer.dll") != nullptr)
    {
        std::wprintf(L"Shim returned the interposer module instead of the system loader: %ls\n", loader_path);
        return 4;
    }

    const auto get_instance_proc_addr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
    const auto get_device_proc_addr =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vulkan, "vkGetDeviceProcAddr"));
    const auto create_instance =
        get_instance_proc_addr != nullptr
            ? reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc_addr(VK_NULL_HANDLE, "vkCreateInstance"))
            : nullptr;
    if (get_instance_proc_addr == nullptr || get_device_proc_addr == nullptr || create_instance == nullptr)
    {
        std::puts("The shimmed Vulkan proc-address entry points were not returned.");
        return 5;
    }

    HMODULE proc_owner = nullptr;
    wchar_t proc_owner_path[MAX_PATH] = {};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(get_instance_proc_addr), &proc_owner);
    if (proc_owner != nullptr) GetModuleFileNameW(proc_owner, proc_owner_path, MAX_PATH);
    if (native && wcsstr(proc_owner_path, L"dxgi.dll") == nullptr)
    {
        std::wprintf(L"vkGetInstanceProcAddr did not come from the compatibility shim: %ls\n", proc_owner_path);
        return 6;
    }

    VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app_info.pApplicationName = "CK3 Native Streamline smoke";
    app_info.applicationVersion = 1;
    app_info.pEngineName = "CK3-DLSS-Feeder";
    app_info.engineVersion = 1;
    app_info.apiVersion = VK_API_VERSION_1_3;
    const char *extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instance_info.pApplicationInfo = &app_info;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(sizeof(extensions) / sizeof(extensions[0]));
    instance_info.ppEnabledExtensionNames = extensions;

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult instance_result = create_instance(&instance_info, nullptr, &instance);
    if (instance_result != VK_SUCCESS)
    {
        std::printf("Shimmed vkCreateInstance failed: %d\n", static_cast<int>(instance_result));
        return 7;
    }

    const auto destroy_instance =
        reinterpret_cast<PFN_vkDestroyInstance>(get_instance_proc_addr(instance, "vkDestroyInstance"));
    const auto create_surface =
        reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(get_instance_proc_addr(instance, "vkCreateWin32SurfaceKHR"));
    const auto destroy_surface =
        reinterpret_cast<PFN_vkDestroySurfaceKHR>(get_instance_proc_addr(instance, "vkDestroySurfaceKHR"));
    const auto enumerate_physical_devices =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_instance_proc_addr(instance, "vkEnumeratePhysicalDevices"));
    const auto get_queue_family_properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            get_instance_proc_addr(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    const auto get_surface_support =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
            get_instance_proc_addr(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
    const auto get_surface_capabilities =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            get_instance_proc_addr(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    const auto get_surface_formats =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
            get_instance_proc_addr(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    const auto create_device =
        reinterpret_cast<PFN_vkCreateDevice>(get_instance_proc_addr(instance, "vkCreateDevice"));
    if (destroy_instance == nullptr || create_surface == nullptr || destroy_surface == nullptr ||
        enumerate_physical_devices == nullptr || get_queue_family_properties == nullptr ||
        get_surface_support == nullptr || get_surface_capabilities == nullptr ||
        get_surface_formats == nullptr || create_device == nullptr)
    {
        std::puts("One or more instance-level proc-address lookups failed.");
        return 8;
    }

    uint32_t physical_count = 0;
    if (enumerate_physical_devices(instance, &physical_count, nullptr) != VK_SUCCESS || physical_count == 0)
    {
        destroy_instance(instance, nullptr);
        std::puts("No Vulkan physical device was available.");
        return 11;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    if (enumerate_physical_devices(instance, &physical_count, physical_devices.data()) != VK_SUCCESS)
    {
        destroy_instance(instance, nullptr);
        std::puts("Enumerating Vulkan physical devices failed.");
        return 12;
    }

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;
    for (VkPhysicalDevice candidate : physical_devices)
    {
        uint32_t family_count = 0;
        get_queue_family_properties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        get_queue_family_properties(candidate, &family_count, families.data());
        for (uint32_t family = 0; family < family_count; ++family)
        {
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            {
                physical_device = candidate;
                queue_family = family;
                break;
            }
        }
        if (physical_device != VK_NULL_HANDLE) break;
    }
    if (physical_device == VK_NULL_HANDLE)
    {
        destroy_instance(instance, nullptr);
        std::puts("No graphics queue family was available.");
        return 13;
    }

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;
    const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    VkDevice device = VK_NULL_HANDLE;
    const VkResult device_result = create_device(physical_device, &device_info, nullptr, &device);
    if (device_result != VK_SUCCESS)
    {
        destroy_instance(instance, nullptr);
        std::printf("Shimmed vkCreateDevice failed: %d\n", static_cast<int>(device_result));
        return 16;
    }

    const auto destroy_device =
        reinterpret_cast<PFN_vkDestroyDevice>(get_device_proc_addr(device, "vkDestroyDevice"));
    const auto get_device_queue =
        reinterpret_cast<PFN_vkGetDeviceQueue>(get_device_proc_addr(device, "vkGetDeviceQueue"));
    const auto create_swapchain =
        reinterpret_cast<PFN_vkCreateSwapchainKHR>(get_device_proc_addr(device, "vkCreateSwapchainKHR"));
    const auto destroy_swapchain =
        reinterpret_cast<PFN_vkDestroySwapchainKHR>(get_device_proc_addr(device, "vkDestroySwapchainKHR"));
    const auto get_swapchain_images =
        reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(get_device_proc_addr(device, "vkGetSwapchainImagesKHR"));
    const auto device_wait_idle =
        reinterpret_cast<PFN_vkDeviceWaitIdle>(get_device_proc_addr(device, "vkDeviceWaitIdle"));
    if (destroy_device == nullptr || get_device_queue == nullptr || create_swapchain == nullptr ||
        destroy_swapchain == nullptr || get_swapchain_images == nullptr || device_wait_idle == nullptr)
    {
        destroy_instance(instance, nullptr);
        std::puts("One or more device-level proc-address lookups failed.");
        return 17;
    }

    for (unsigned int probe_index = 0; probe_index < 8; ++probe_index)
    {
        VkInstance probe_instance = VK_NULL_HANDLE;
        const VkResult probe_result = create_instance(&instance_info, nullptr, &probe_instance);
        if (probe_result != VK_SUCCESS)
        {
            destroy_device(device, nullptr);
            destroy_instance(instance, nullptr);
            std::printf("Post-device probe vkCreateInstance #%u failed: %d\n",
                        probe_index + 1, static_cast<int>(probe_result));
            return 7;
        }
        const auto destroy_probe =
            reinterpret_cast<PFN_vkDestroyInstance>(get_instance_proc_addr(probe_instance, "vkDestroyInstance"));
        if (destroy_probe == nullptr)
        {
            destroy_device(device, nullptr);
            destroy_instance(instance, nullptr);
            std::printf("Post-device probe vkDestroyInstance #%u was not returned.\n", probe_index + 1);
            return 7;
        }
        destroy_probe(probe_instance, nullptr);
    }

    HMODULE surface_proc_owner = nullptr;
    wchar_t surface_proc_owner_path[MAX_PATH] = {};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(create_surface), &surface_proc_owner);
    if (surface_proc_owner != nullptr)
        GetModuleFileNameW(surface_proc_owner, surface_proc_owner_path, MAX_PATH);
    if (native && wcsstr(surface_proc_owner_path, L"dxgi.dll") == nullptr)
    {
        destroy_device(device, nullptr);
        destroy_instance(instance, nullptr);
        std::wprintf(L"vkCreateWin32SurfaceKHR bypassed the compatibility shim: %ls\n", surface_proc_owner_path);
        return 9;
    }

    const HINSTANCE app_instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = SmokeWindowProc;
    window_class.hInstance = app_instance;
    window_class.lpszClassName = L"CK3NativeStreamlineSmoke";
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        destroy_device(device, nullptr);
        destroy_instance(instance, nullptr);
        std::printf("RegisterClassW failed: %lu\n", GetLastError());
        return 10;
    }
    const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Streamline smoke",
                                        WS_OVERLAPPEDWINDOW, 0, 0, present_hooks ? 256 : 64, present_hooks ? 256 : 64,
                                        nullptr, nullptr, app_instance, nullptr);
    if (window == nullptr)
    {
        destroy_device(device, nullptr);
        destroy_instance(instance, nullptr);
        std::printf("CreateWindowExW failed: %lu\n", GetLastError());
        return 11;
    }

    VkWin32SurfaceCreateInfoKHR surface_info = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    surface_info.hinstance = app_instance;
    surface_info.hwnd = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult surface_result = create_surface(instance, &surface_info, nullptr, &surface);
    if (surface_result != VK_SUCCESS)
    {
        destroy_device(device, nullptr);
        DestroyWindow(window);
        destroy_instance(instance, nullptr);
        std::printf("System-loader bypass vkCreateWin32SurfaceKHR failed: %d\n", static_cast<int>(surface_result));
        return 12;
    }

    VkBool32 present_supported = VK_FALSE;
    if (get_surface_support(physical_device, queue_family, surface, &present_supported) != VK_SUCCESS ||
        present_supported != VK_TRUE)
    {
        destroy_surface(instance, surface, nullptr);
        destroy_device(device, nullptr);
        DestroyWindow(window);
        destroy_instance(instance, nullptr);
        std::puts("The selected graphics queue cannot present to the Win32 surface.");
        return 13;
    }

    VkQueue queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family, 0, &queue);
    VkSurfaceCapabilitiesKHR capabilities = {};
    uint32_t format_count = 0;
    if (queue == VK_NULL_HANDLE ||
        get_surface_capabilities(physical_device, surface, &capabilities) != VK_SUCCESS ||
        get_surface_formats(physical_device, surface, &format_count, nullptr) != VK_SUCCESS ||
        format_count == 0)
    {
        destroy_device(device, nullptr);
        destroy_surface(instance, surface, nullptr);
        DestroyWindow(window);
        destroy_instance(instance, nullptr);
        std::puts("Direct surface capability queries failed.");
        return 18;
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    if (get_surface_formats(physical_device, surface, &format_count, formats.data()) != VK_SUCCESS)
    {
        destroy_device(device, nullptr);
        destroy_surface(instance, surface, nullptr);
        DestroyWindow(window);
        destroy_instance(instance, nullptr);
        std::puts("Enumerating direct surface formats failed.");
        return 19;
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0 && image_count > capabilities.maxImageCount)
        image_count = capabilities.maxImageCount;
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) extent = { 64, 64 };
    VkSwapchainCreateInfoKHR swapchain_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swapchain_info.surface = surface;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = formats[0].format;
    swapchain_info.imageColorSpace = formats[0].colorSpace;
    swapchain_info.imageExtent = extent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (present_hooks ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0);
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult swapchain_result = create_swapchain(device, &swapchain_info, nullptr, &swapchain);
    if (swapchain_result != VK_SUCCESS)
    {
        destroy_device(device, nullptr);
        destroy_surface(instance, surface, nullptr);
        DestroyWindow(window);
        destroy_instance(instance, nullptr);
        std::printf("Direct shimmed vkCreateSwapchainKHR failed: %d\n", static_cast<int>(swapchain_result));
        return 20;
    }
    uint32_t swapchain_image_count = 0;
    const VkResult images_result = get_swapchain_images(device, swapchain, &swapchain_image_count, nullptr);
    if (images_result != VK_SUCCESS || swapchain_image_count == 0)
    {
        destroy_swapchain(device, swapchain, nullptr);
        destroy_device(device, nullptr);
        destroy_surface(instance, surface, nullptr);
        DestroyWindow(window);
        destroy_instance(instance, nullptr);
        std::printf("Direct shimmed vkGetSwapchainImagesKHR failed: %d\n", static_cast<int>(images_result));
        return 21;
    }

    if (present_hooks)
    {
        // ReShade uses a shared present entry point. Destroy a temporary device
        // while the real swapchain/device survive; their hook must stay armed.
        VkDevice temporary = VK_NULL_HANDLE;
        const VkResult temporary_result = create_device(physical_device, &device_info, nullptr, &temporary);
        if (temporary_result != VK_SUCCESS) return 29;
        const auto destroy_temporary = reinterpret_cast<PFN_vkDestroyDevice>(get_device_proc_addr(temporary, "vkDestroyDevice"));
        if (!destroy_temporary) return 29;
        destroy_temporary(temporary, nullptr);
#define PROC(name) const auto name = reinterpret_cast<PFN_vk##name>(get_device_proc_addr(device, "vk" #name)); if (!name) return 30
#define CHECK(call) do { const VkResult r = (call); if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { std::printf("Present test failure: %s -> %d\n", #call, r); return 31; } } while (0)
        PROC(AcquireNextImageKHR); PROC(QueuePresentKHR); PROC(QueueSubmit);
        PROC(CreateSemaphore); PROC(DestroySemaphore); PROC(CreateCommandPool); PROC(DestroyCommandPool);
        PROC(AllocateCommandBuffers); PROC(BeginCommandBuffer); PROC(EndCommandBuffer); PROC(ResetCommandPool);
        PROC(CmdPipelineBarrier); PROC(CmdClearColorImage);
        const auto loader_present = reinterpret_cast<PFN_vkQueuePresentKHR>(GetProcAddress(vulkan, "vkQueuePresentKHR"));
        HMODULE observer = GetModuleHandleW(L"vk-hook-observer.addon64");
        const auto read_stats = observer ? reinterpret_cast<VkHookTestReadStats>(GetProcAddress(observer, "ReadVkHookTestStats")) : nullptr;
        if (!read_stats || !loader_present) { std::puts("The instrumented feeder was not loaded by ReShade."); return 32; }
        std::vector<VkImage> images(swapchain_image_count);
        CHECK(get_swapchain_images(device, swapchain, &swapchain_image_count, images.data()));
        std::vector<bool> initialized(swapchain_image_count, false);
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore acquired, rendered;
        CHECK(CreateSemaphore(device, &sci, nullptr, &acquired));
        CHECK(CreateSemaphore(device, &sci, nullptr, &rendered));
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.queueFamilyIndex = queue_family;
        VkCommandPool pool;
        CHECK(CreateCommandPool(device, &pci, nullptr, &pool));
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VkCommandBuffer commands;
        CHECK(AllocateCommandBuffers(device, &cai, &commands));
        for (unsigned frame = 0; frame < 32; ++frame)
        {
            uint32_t index;
            CHECK(AcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index));
            CHECK(ResetCommandPool(device, pool, 0));
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            CHECK(BeginCommandBuffer(commands, &bi));
            VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            barrier.oldLayout = initialized[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[index]; barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkClearColorValue color = { { 0.1f, 0.2f, frame / 32.0f, 1.0f } };
            CmdClearColorImage(commands, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0;
            CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            CHECK(EndCommandBuffer(commands));
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &stage;
            submit.commandBufferCount = 1; submit.pCommandBuffers = &commands;
            submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &rendered;
            CHECK(QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
            VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            present.waitSemaphoreCount = 1; present.pWaitSemaphores = &rendered;
            present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &index;
            CHECK((frame % 2 ? loader_present : QueuePresentKHR)(queue, &present));
            CHECK(device_wait_idle(device));
            initialized[index] = true;
            VkHookTestStats current = {};
            read_stats(&current);
            if (current.active_context) { std::puts("Present context leaked after returning to the caller."); return 33; }
        }
        VkHookTestStats stats = {};
        read_stats(&stats);
        std::printf("Present hooks (%s): presents=%u covered=%u ordered=%u early devices=%u\n", native ? "Streamline shim" : "system loader", stats.presents, stats.covered, stats.ordered, stats.early_devices);
        if (stats.presents != 32 || stats.covered != 32 || stats.ordered != 32 || stats.early_devices != 2) return 34;
        DestroyCommandPool(device, pool, nullptr);
        DestroySemaphore(device, rendered, nullptr); DestroySemaphore(device, acquired, nullptr);
#undef CHECK
#undef PROC
    }

    device_wait_idle(device);
    destroy_swapchain(device, swapchain, nullptr);
    destroy_device(device, nullptr);
    destroy_surface(instance, surface, nullptr);
    DestroyWindow(window);
    destroy_instance(instance, nullptr);

    std::wprintf(L"Vulkan loader handle: %ls\n", loader_path);
    std::wprintf(L"Shim proc owner: %ls\n", proc_owner_path);
    std::wprintf(L"Surface proc owner: %ls\n", surface_proc_owner_path);
    std::puts("Vulkan 1.3 CK3-order device/probe/surface/swapchain smoke test passed.");
    return 0;
}
