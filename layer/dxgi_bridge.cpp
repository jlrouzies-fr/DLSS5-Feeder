// DLSS5 CK3 app-local DXGI bootstrap.
// Loaded explicitly by VK_LAYER_feed_vk before Vulkan device creation.

#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <sl.h>
#include <vulkan/vulkan.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

static HMODULE g_self;
static HMODULE g_system_dxgi;
static HMODULE g_reshade;
static HMODULE g_streamline;
static HMODULE g_system_vulkan;
static thread_local bool g_bypass_reshade_dxgi;

using LoadLibraryAFn = HMODULE (WINAPI *)(LPCSTR);
using LoadLibraryWFn = HMODULE (WINAPI *)(LPCWSTR);
using LoadLibraryExWFn = HMODULE (WINAPI *)(LPCWSTR, HANDLE, DWORD);
using GetProcAddressFn = FARPROC (WINAPI *)(HMODULE, LPCSTR);
static LoadLibraryAFn g_real_load_library_a;
static LoadLibraryWFn g_real_load_library_w;
static LoadLibraryExWFn g_real_load_library_ex_w;
static GetProcAddressFn g_real_get_proc_address;
static PFN_vkGetInstanceProcAddr g_streamline_gipa;
static PFN_vkGetDeviceProcAddr g_streamline_gdpa;
static PFN_vkGetInstanceProcAddr g_system_gipa;
static VkInstance g_last_instance;
static VkDevice g_last_device;

static SRWLOCK g_instance_lock = SRWLOCK_INIT;
static VkInstance g_instances[16] = {};

struct QueueOwner
{
    VkQueue queue;
    VkDevice device;
};

static SRWLOCK g_queue_lock = SRWLOCK_INIT;
static QueueOwner g_queue_owners[16] = {};

static void BridgeLog(const char *format, ...)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    if (wchar_t *slash = wcsrchr(path, L'\\'))
        wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"dlss5-dxgi.log");
    char message[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    FILE *file = nullptr;
    if (_wfopen_s(&file, path, L"a") == 0 && file != nullptr)
    {
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        fprintf(file, "%02u:%02u:%02u.%03u  %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
        fclose(file);
    }
}

static bool GetBinaryPath(wchar_t (&path)[MAX_PATH], const wchar_t *relative)
{
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return false;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), relative);
    return true;
}

static bool NativeStreamlineEnabled()
{
    wchar_t marker[MAX_PATH] = {};
    return GetBinaryPath(marker, L"dlss-active\\streamline-native.enabled") &&
           GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
}

static bool IsVulkanLoaderName(const wchar_t *name)
{
    if (name == nullptr) return false;
    const wchar_t *base = wcsrchr(name, L'\\');
    base = base != nullptr ? base + 1 : name;
    return _wcsicmp(base, L"vulkan-1.dll") == 0;
}

static FARPROC RealGetProcAddress(HMODULE module, const char *name)
{
    return g_real_get_proc_address != nullptr ? g_real_get_proc_address(module, name) : GetProcAddress(module, name);
}

static HMODULE LoadSystemVulkan()
{
    if (g_system_vulkan != nullptr) return g_system_vulkan;
    wchar_t path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 14) return nullptr;
    wcscat_s(path, L"\\vulkan-1.dll");
    g_system_vulkan = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_system_vulkan == nullptr)
        BridgeLog("Native Streamline shim failed to load the system Vulkan loader (Win32 %lu).", GetLastError());
    return g_system_vulkan;
}

static HMODULE LoadNativeStreamline()
{
    static LONG attempted = 0;
    static bool initialized = false;
    if (InterlockedCompareExchange(&attempted, 1, 0) != 0) return initialized ? g_streamline : nullptr;
    wchar_t module_path[MAX_PATH] = {};
    static wchar_t plugin_path[MAX_PATH] = {};
    if (!GetBinaryPath(module_path, L"dlss-active\\sl.interposer.dll") || !GetBinaryPath(plugin_path, L"dlss-active")) return nullptr;
    const LoadLibraryExWFn load_ex = g_real_load_library_ex_w != nullptr ? g_real_load_library_ex_w : &LoadLibraryExW;
    g_streamline = load_ex(module_path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_streamline == nullptr) return nullptr;
    const auto init = reinterpret_cast<PFun_slInit *>(RealGetProcAddress(g_streamline, "slInit"));
    if (init == nullptr) return nullptr;
    static const sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
    static const wchar_t *plugin_paths[] = { plugin_path };
    sl::Preferences preferences = {};
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = plugin_path;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = static_cast<uint32_t>(_countof(features));
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "CK3-DLSS-Feeder-Native-Streamline";
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    g_bypass_reshade_dxgi = true;
    const sl::Result result = init(preferences, sl::kSDKVersion);
    g_bypass_reshade_dxgi = false;
    initialized = result == sl::Result::eOk;
    BridgeLog("Native Streamline slInit returned %d.", static_cast<int>(result));
    if (initialized)
    {
        g_streamline_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(RealGetProcAddress(g_streamline, "vkGetInstanceProcAddr"));
        g_streamline_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(RealGetProcAddress(g_streamline, "vkGetDeviceProcAddr"));
        if (g_streamline_gipa == nullptr || g_streamline_gdpa == nullptr)
        {
            BridgeLog("Native Streamline shim is missing Vulkan proc-address exports; falling back to the system loader.");
            initialized = false;
        }
    }
    return initialized ? g_streamline : nullptr;
}

template <typename T>
static T StreamlineInstanceProc(VkInstance instance, const char *name)
{
    return g_streamline_gipa != nullptr ? reinterpret_cast<T>(g_streamline_gipa(instance, name)) : nullptr;
}

template <typename T>
static T StreamlineDeviceProc(VkDevice device, const char *name)
{
    return g_streamline_gdpa != nullptr ? reinterpret_cast<T>(g_streamline_gdpa(device, name)) : nullptr;
}

template <typename T>
static T SystemInstanceProc(VkInstance instance, const char *name)
{
    if (g_system_gipa == nullptr)
    {
        if (HMODULE loader = LoadSystemVulkan())
            g_system_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(RealGetProcAddress(loader, "vkGetInstanceProcAddr"));
    }
    return g_system_gipa != nullptr ? reinterpret_cast<T>(g_system_gipa(instance, name)) : nullptr;
}

static void RememberInstance(VkInstance instance)
{
    if (instance == VK_NULL_HANDLE) return;
    AcquireSRWLockExclusive(&g_instance_lock);
    for (VkInstance &entry : g_instances)
    {
        if (entry == instance || entry == VK_NULL_HANDLE)
        {
            entry = instance;
            break;
        }
    }
    g_last_instance = instance;
    ReleaseSRWLockExclusive(&g_instance_lock);
}

static void ForgetInstance(VkInstance instance)
{
    AcquireSRWLockExclusive(&g_instance_lock);
    for (VkInstance &entry : g_instances)
        if (entry == instance) entry = VK_NULL_HANDLE;
    if (g_last_instance == instance)
    {
        g_last_instance = VK_NULL_HANDLE;
        for (VkInstance entry : g_instances)
            if (entry != VK_NULL_HANDLE) g_last_instance = entry;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
}

static VkInstance CurrentInstance()
{
    AcquireSRWLockShared(&g_instance_lock);
    const VkInstance instance = g_last_instance;
    ReleaseSRWLockShared(&g_instance_lock);
    return instance;
}

static void RememberQueue(VkQueue queue, VkDevice device)
{
    if (queue == VK_NULL_HANDLE || device == VK_NULL_HANDLE) return;
    AcquireSRWLockExclusive(&g_queue_lock);
    for (QueueOwner &entry : g_queue_owners)
    {
        if (entry.queue == queue || entry.queue == VK_NULL_HANDLE)
        {
            entry = { queue, device };
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_queue_lock);
}

static VkDevice DeviceForQueue(VkQueue queue)
{
    VkDevice device = VK_NULL_HANDLE;
    AcquireSRWLockShared(&g_queue_lock);
    for (const QueueOwner &entry : g_queue_owners)
        if (entry.queue == queue)
        {
            device = entry.device;
            break;
        }
    ReleaseSRWLockShared(&g_queue_lock);
    return device != VK_NULL_HANDLE ? device : g_last_device;
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimCreateInstance(const VkInstanceCreateInfo *info,
                                                          const VkAllocationCallbacks *allocator,
                                                          VkInstance *instance)
{
    const auto fn = StreamlineInstanceProc<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
    const VkResult result = fn != nullptr ? fn(info, allocator, instance) : VK_ERROR_INITIALIZATION_FAILED;
    if (result == VK_SUCCESS && instance != nullptr) RememberInstance(*instance);
    return result;
}

static VKAPI_ATTR void VKAPI_CALL ShimDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
    const auto fn = StreamlineInstanceProc<PFN_vkDestroyInstance>(instance, "vkDestroyInstance");
    if (fn != nullptr) fn(instance, allocator);
    ForgetInstance(instance);
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimCreateDevice(VkPhysicalDevice physical_device,
                                                        const VkDeviceCreateInfo *info,
                                                        const VkAllocationCallbacks *allocator,
                                                        VkDevice *device)
{
    const auto fn = StreamlineInstanceProc<PFN_vkCreateDevice>(CurrentInstance(), "vkCreateDevice");
    const VkResult result = fn != nullptr ? fn(physical_device, info, allocator, device) : VK_ERROR_INITIALIZATION_FAILED;
    if (result == VK_SUCCESS && device != nullptr) g_last_device = *device;
    return result;
}

static VKAPI_ATTR void VKAPI_CALL ShimDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator)
{
    const auto fn = StreamlineDeviceProc<PFN_vkDestroyDevice>(device, "vkDestroyDevice");
    if (fn != nullptr) fn(device, allocator);
    if (g_last_device == device) g_last_device = VK_NULL_HANDLE;
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimCreateWin32SurfaceKHR(VkInstance instance,
                                                                const VkWin32SurfaceCreateInfoKHR *info,
                                                                const VkAllocationCallbacks *allocator,
                                                                VkSurfaceKHR *surface)
{
    static LONG logged = 0;
    if (InterlockedCompareExchange(&logged, 1, 0) == 0)
        BridgeLog("Native Streamline compatibility: routing Vulkan surface operations through the system loader.");
    const auto fn = SystemInstanceProc<PFN_vkCreateWin32SurfaceKHR>(instance, "vkCreateWin32SurfaceKHR");
    return fn != nullptr ? fn(instance, info, allocator, surface) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR void VKAPI_CALL ShimDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                         const VkAllocationCallbacks *allocator)
{
    const auto fn = SystemInstanceProc<PFN_vkDestroySurfaceKHR>(instance, "vkDestroySurfaceKHR");
    if (fn != nullptr) fn(instance, surface, allocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physical_device,
                                                                             uint32_t queue_family,
                                                                             VkSurfaceKHR surface,
                                                                             VkBool32 *supported)
{
    const auto fn = SystemInstanceProc<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        CurrentInstance(), "vkGetPhysicalDeviceSurfaceSupportKHR");
    return fn != nullptr ? fn(physical_device, queue_family, surface, supported) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *capabilities)
{
    const auto fn = SystemInstanceProc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        CurrentInstance(), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    return fn != nullptr ? fn(physical_device, surface, capabilities) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats)
{
    const auto fn = SystemInstanceProc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        CurrentInstance(), "vkGetPhysicalDeviceSurfaceFormatsKHR");
    return fn != nullptr ? fn(physical_device, surface, count, formats) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes)
{
    const auto fn = SystemInstanceProc<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
        CurrentInstance(), "vkGetPhysicalDeviceSurfacePresentModesKHR");
    return fn != nullptr ? fn(physical_device, surface, count, modes) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL ShimGetPhysicalDeviceWin32PresentationSupportKHR(
    VkPhysicalDevice physical_device, uint32_t queue_family)
{
    const auto fn = SystemInstanceProc<PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR>(
        CurrentInstance(), "vkGetPhysicalDeviceWin32PresentationSupportKHR");
    return fn != nullptr ? fn(physical_device, queue_family) : VK_FALSE;
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimCreateSwapchainKHR(VkDevice device,
                                                             const VkSwapchainCreateInfoKHR *info,
                                                             const VkAllocationCallbacks *allocator,
                                                             VkSwapchainKHR *swapchain)
{
    const auto fn = StreamlineDeviceProc<PFN_vkCreateSwapchainKHR>(device, "vkCreateSwapchainKHR");
    return fn != nullptr ? fn(device, info, allocator, swapchain) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR void VKAPI_CALL ShimDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                           const VkAllocationCallbacks *allocator)
{
    const auto fn = StreamlineDeviceProc<PFN_vkDestroySwapchainKHR>(device, "vkDestroySwapchainKHR");
    if (fn != nullptr) fn(device, swapchain, allocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                                 uint32_t *count, VkImage *images)
{
    const auto fn = StreamlineDeviceProc<PFN_vkGetSwapchainImagesKHR>(device, "vkGetSwapchainImagesKHR");
    return fn != nullptr ? fn(device, swapchain, count, images) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                               uint64_t timeout, VkSemaphore semaphore,
                                                               VkFence fence, uint32_t *image_index)
{
    const auto fn = StreamlineDeviceProc<PFN_vkAcquireNextImageKHR>(device, "vkAcquireNextImageKHR");
    return fn != nullptr ? fn(device, swapchain, timeout, semaphore, fence, image_index) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimDeviceWaitIdle(VkDevice device)
{
    const auto fn = StreamlineDeviceProc<PFN_vkDeviceWaitIdle>(device, "vkDeviceWaitIdle");
    return fn != nullptr ? fn(device) : VK_ERROR_DEVICE_LOST;
}

static VKAPI_ATTR void VKAPI_CALL ShimGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue)
{
    const auto fn = StreamlineDeviceProc<PFN_vkGetDeviceQueue>(device, "vkGetDeviceQueue");
    if (fn != nullptr) fn(device, family, index, queue);
    if (queue != nullptr) RememberQueue(*queue, device);
}

static VKAPI_ATTR void VKAPI_CALL ShimGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
    const auto fn = StreamlineDeviceProc<PFN_vkGetDeviceQueue2>(device, "vkGetDeviceQueue2");
    if (fn != nullptr) fn(device, info, queue);
    if (queue != nullptr) RememberQueue(*queue, device);
}

static VKAPI_ATTR VkResult VKAPI_CALL ShimQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    const VkDevice device = DeviceForQueue(queue);
    const auto fn = StreamlineDeviceProc<PFN_vkQueuePresentKHR>(device, "vkQueuePresentKHR");
    return fn != nullptr ? fn(queue, info) : VK_ERROR_DEVICE_LOST;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ShimGetInstanceProcAddr(VkInstance instance, const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ShimGetDeviceProcAddr(VkDevice device, const char *name);

static PFN_vkVoidFunction ShimmedVulkanProc(const char *name)
{
    if (name == nullptr) return nullptr;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimGetInstanceProcAddr);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimGetDeviceProcAddr);
    if (strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimCreateInstance);
    if (strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimDestroyInstance);
    if (strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimCreateDevice);
    if (strcmp(name, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimDestroyDevice);
    if (strcmp(name, "vkCreateWin32SurfaceKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimCreateWin32SurfaceKHR);
    if (strcmp(name, "vkDestroySurfaceKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimDestroySurfaceKHR);
    if (strcmp(name, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&ShimGetPhysicalDeviceSurfaceSupportKHR);
    if (strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&ShimGetPhysicalDeviceSurfaceCapabilitiesKHR);
    if (strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&ShimGetPhysicalDeviceSurfaceFormatsKHR);
    if (strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&ShimGetPhysicalDeviceSurfacePresentModesKHR);
    if (strcmp(name, "vkGetPhysicalDeviceWin32PresentationSupportKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&ShimGetPhysicalDeviceWin32PresentationSupportKHR);
    if (strcmp(name, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimCreateSwapchainKHR);
    if (strcmp(name, "vkDestroySwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimDestroySwapchainKHR);
    if (strcmp(name, "vkGetSwapchainImagesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimGetSwapchainImagesKHR);
    if (strcmp(name, "vkAcquireNextImageKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimAcquireNextImageKHR);
    if (strcmp(name, "vkDeviceWaitIdle") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimDeviceWaitIdle);
    if (strcmp(name, "vkGetDeviceQueue") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimGetDeviceQueue);
    if (strcmp(name, "vkGetDeviceQueue2") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimGetDeviceQueue2);
    if (strcmp(name, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ShimQueuePresentKHR);
    return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ShimGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (PFN_vkVoidFunction proc = ShimmedVulkanProc(name)) return proc;
    return g_streamline_gipa != nullptr ? g_streamline_gipa(instance, name) : nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ShimGetDeviceProcAddr(VkDevice device, const char *name)
{
    if (PFN_vkVoidFunction proc = ShimmedVulkanProc(name)) return proc;
    return g_streamline_gdpa != nullptr ? g_streamline_gdpa(device, name) : nullptr;
}

static FARPROC WINAPI BridgeGetProcAddress(HMODULE module, LPCSTR name)
{
    if (module != g_system_vulkan || name == nullptr || g_streamline_gipa == nullptr)
        return RealGetProcAddress(module, name);

    if (PFN_vkVoidFunction proc = ShimmedVulkanProc(name)) return reinterpret_cast<FARPROC>(proc);

    if (FARPROC proc = RealGetProcAddress(g_streamline, name)) return proc;
    return RealGetProcAddress(module, name);
}

static HMODULE WINAPI BridgeLoadLibraryA(LPCSTR name)
{
    if (name != nullptr)
    {
        wchar_t wide[MAX_PATH] = {};
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH) > 0 && IsVulkanLoaderName(wide))
            if (LoadNativeStreamline() != nullptr)
                if (HMODULE loader = LoadSystemVulkan()) return loader;
    }
    return g_real_load_library_a != nullptr ? g_real_load_library_a(name) : LoadLibraryA(name);
}

static HMODULE WINAPI BridgeLoadLibraryW(LPCWSTR name)
{
    if (IsVulkanLoaderName(name))
        if (LoadNativeStreamline() != nullptr)
            if (HMODULE loader = LoadSystemVulkan()) return loader;
    return g_real_load_library_w != nullptr ? g_real_load_library_w(name) : LoadLibraryW(name);
}

static HMODULE WINAPI BridgeLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    if (IsVulkanLoaderName(name))
        if (LoadNativeStreamline() != nullptr)
            if (HMODULE loader = LoadSystemVulkan()) return loader;
    return g_real_load_library_ex_w != nullptr ? g_real_load_library_ex_w(name, file, flags) : LoadLibraryExW(name, file, flags);
}

static bool PatchMainImport(const char *function_name, void *replacement, void **original)
{
    auto *base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    if (base == nullptr) return false;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const DWORD import_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (import_rva == 0) return false;

    auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + import_rva);
    for (; descriptor->Name != 0; ++descriptor)
    {
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->OriginalFirstThunk);
        auto *iat = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->FirstThunk);
        if (descriptor->OriginalFirstThunk == 0) names = iat;
        for (; names->u1.AddressOfData != 0; ++names, ++iat)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto *entry = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char *>(entry->Name), function_name) != 0) continue;
            DWORD old_protect = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &old_protect)) return false;
            *original = reinterpret_cast<void *>(iat->u1.Function);
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            DWORD ignored = 0;
            VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), old_protect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &iat->u1.Function, sizeof(iat->u1.Function));
            return true;
        }
    }
    return false;
}

static void InstallNativeStreamlineRedirect()
{
    if (!NativeStreamlineEnabled()) return;
    PatchMainImport("LoadLibraryA", reinterpret_cast<void *>(&BridgeLoadLibraryA), reinterpret_cast<void **>(&g_real_load_library_a));
    PatchMainImport("LoadLibraryW", reinterpret_cast<void *>(&BridgeLoadLibraryW), reinterpret_cast<void **>(&g_real_load_library_w));
    PatchMainImport("LoadLibraryExW", reinterpret_cast<void *>(&BridgeLoadLibraryExW), reinterpret_cast<void **>(&g_real_load_library_ex_w));
    PatchMainImport("GetProcAddress", reinterpret_cast<void *>(&BridgeGetProcAddress), reinterpret_cast<void **>(&g_real_get_proc_address));
}

static HMODULE LoadSystemDxgi()
{
    if (g_system_dxgi != nullptr) return g_system_dxgi;
    wchar_t path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 10) return nullptr;
    wcscat_s(path, L"\\dxgi.dll");
    g_system_dxgi = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return g_system_dxgi;
}

static HMODULE LoadPrivateReShade()
{
    if (g_reshade != nullptr) return g_reshade;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return nullptr;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"dlss5-vulkan\\ReShade64.dll");
    g_reshade = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_reshade == nullptr)
        BridgeLog("[bridge] failed to load private ReShade64.dll (Win32 %lu)", GetLastError());
    else
        BridgeLog("[bridge] private ReShade64.dll loaded from %ls", path);
    return g_reshade;
}

template <typename T>
static T Resolve(const char *name)
{
    if (!g_bypass_reshade_dxgi)
        if (HMODULE reshade = LoadPrivateReShade())
            if (FARPROC proc = GetProcAddress(reshade, name)) return reinterpret_cast<T>(proc);
    if (HMODULE system = LoadSystemDxgi())
        if (FARPROC proc = GetProcAddress(system, name)) return reinterpret_cast<T>(proc);
    return nullptr;
}

extern "C" HRESULT WINAPI BridgeCreateDXGIFactory(REFIID riid, void **factory)
{
    using Fn = HRESULT (WINAPI *)(REFIID, void **);
    const Fn fn = Resolve<Fn>("CreateDXGIFactory");
    return fn ? fn(riid, factory) : DXGI_ERROR_NOT_FOUND;
}

extern "C" HRESULT WINAPI BridgeCreateDXGIFactory1(REFIID riid, void **factory)
{
    using Fn = HRESULT (WINAPI *)(REFIID, void **);
    const Fn fn = Resolve<Fn>("CreateDXGIFactory1");
    return fn ? fn(riid, factory) : DXGI_ERROR_NOT_FOUND;
}

extern "C" HRESULT WINAPI BridgeCreateDXGIFactory2(UINT flags, REFIID riid, void **factory)
{
    using Fn = HRESULT (WINAPI *)(UINT, REFIID, void **);
    const Fn fn = Resolve<Fn>("CreateDXGIFactory2");
    return fn ? fn(flags, riid, factory) : DXGI_ERROR_NOT_FOUND;
}

extern "C" HRESULT WINAPI BridgeDXGIGetDebugInterface1(UINT flags, REFIID riid, void **object)
{
    using Fn = HRESULT (WINAPI *)(UINT, REFIID, void **);
    const Fn fn = Resolve<Fn>("DXGIGetDebugInterface1");
    return fn ? fn(flags, riid, object) : DXGI_ERROR_NOT_FOUND;
}

static ID3D12Device *g_boot_device;
static ID3D12CommandQueue *g_boot_queue;
static IDXGISwapChain3 *g_boot_swapchain;
static HWND g_boot_window;

static LRESULT CALLBACK BridgeWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

static HRESULT CreateHiddenD3D12Runtime()
{
    HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (d3d12 == nullptr) return HRESULT_FROM_WIN32(GetLastError());
    using CreateDeviceFn = HRESULT (WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    const auto create_device = reinterpret_cast<CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (create_device == nullptr) return HRESULT_FROM_WIN32(GetLastError());

    HRESULT result = create_device(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                   reinterpret_cast<void **>(&g_boot_device));
    if (FAILED(result)) return result;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = g_boot_device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue),
                                                reinterpret_cast<void **>(&g_boot_queue));
    if (FAILED(result)) return result;

    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = BridgeWindowProc;
    window_class.hInstance = g_self;
    window_class.lpszClassName = L"CK3DLSS5HiddenDXGI";
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32(GetLastError());
    g_boot_window = CreateWindowExW(0, window_class.lpszClassName, L"CK3 DLSS5 DXGI bootstrap",
                                    WS_OVERLAPPEDWINDOW, 0, 0, 960, 540,
                                    nullptr, nullptr, g_self, nullptr);
    if (g_boot_window == nullptr) return HRESULT_FROM_WIN32(GetLastError());

    IDXGIFactory2 *factory = nullptr;
    result = BridgeCreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory));
    if (FAILED(result)) return result;

    DXGI_SWAP_CHAIN_DESC1 swap_desc = {};
    swap_desc.Width = 960;
    swap_desc.Height = 540;
    swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 2;
    swap_desc.Scaling = DXGI_SCALING_STRETCH;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    IDXGISwapChain1 *swapchain = nullptr;
    result = factory->CreateSwapChainForHwnd(g_boot_queue, g_boot_window, &swap_desc,
                                              nullptr, nullptr, &swapchain);
    factory->Release();
    if (FAILED(result)) return result;
    result = swapchain->QueryInterface(__uuidof(IDXGISwapChain3),
                                       reinterpret_cast<void **>(&g_boot_swapchain));
    swapchain->Release();
    if (FAILED(result)) return result;

    result = g_boot_swapchain->Present(0, 0);
    BridgeLog("[bridge] hidden D3D12 runtime present -> 0x%08lX", static_cast<unsigned long>(result));
    return result;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DLSS5Bootstrap()
{
    static LONG attempted = 0;
    if (InterlockedCompareExchange(&attempted, 1, 0) != 0)
        return g_boot_swapchain != nullptr ? S_OK : E_FAIL;
    if (LoadPrivateReShade() == nullptr) return HRESULT_FROM_WIN32(GetLastError());
    const HRESULT result = CreateHiddenD3D12Runtime();
    BridgeLog("[bridge] DXGI/ReShade/D3D12 bootstrap -> 0x%08lX", static_cast<unsigned long>(result));
    return result;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        InstallNativeStreamlineRedirect();
    }
    return TRUE;
}
