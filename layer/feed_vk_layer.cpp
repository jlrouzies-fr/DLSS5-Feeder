// VK_LAYER_feed_vk - a minimal Vulkan layer for DLSS5-Feeder.
//
// The Vulkan transport imports the feeder's D3D12 fences and textures into the
// game's own VkDevice. That needs four KHR external-interop extensions plus the
// timelineSemaphore feature, all of which must be enabled at vkCreateDevice --
// long before our add-on ever sees the device. Games enable whatever they happen
// to need and no more:
//
//   DOOM 2016          all four present (ReShade enables them) -> layer NOT needed
//   Tekken 3 Recomp    only timeline_semaphore + external_memory_win32 -> feed fails
//
// This layer's entire job: in vkCreateDevice, append the missing extensions the
// driver actually supports, and make sure timelineSemaphore is switched on.
// Everything else passes straight through.
//
// Load it per-game, without touching the registry (implicit-layer keys get
// clobbered by other hook software):
//     set VK_LAYER_PATH=<folder with this DLL + its .json>
//     set VK_INSTANCE_LAYERS=VK_LAYER_feed_vk
// See run-with-feed-layer.bat.

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#include <vulkan/vk_layer.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#define FEED_LAYER_NAME "VK_LAYER_feed_vk"

// ---------------------------------------------------------------------------
// Logging: next to this DLL, so a failed run leaves evidence.
// ---------------------------------------------------------------------------

static void LayerLog(const char *fmt, ...)
{
    static char path[MAX_PATH];
    if (path[0] == '\0')
    {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LayerLog), &self);
        GetModuleFileNameA(self, path, MAX_PATH);
        if (char *s = strrchr(path, '\\')) strcpy_s(s + 1, MAX_PATH - (s + 1 - path), "feed-vk-layer.log");
    }
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    FILE *f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f != nullptr)
    {
        fprintf(f, "%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
        fclose(f);
    }
}

// ---------------------------------------------------------------------------
// Dispatch: one next-GIPA per instance, one next-GDPA per device. A layer this
// small does not need real dispatch tables -- just the "call down" pointers.
// ---------------------------------------------------------------------------

static PFN_vkGetInstanceProcAddr g_next_gipa;
static PFN_vkGetDeviceProcAddr   g_next_gdpa;

// Reproduce the DXGI initialization path that RHI gets from an app-local
// ReShade proxy before the feeder creates its private D3D12 device.
static void BootstrapPrivateDxgi()
{
    static LONG attempted = 0;
    if (InterlockedCompareExchange(&attempted, 1, 0) != 0) return;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"dxgi.dll");
    const HMODULE bridge = LoadLibraryExW(path, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (bridge == nullptr)
    {
        LayerLog("[layer] private DXGI bridge failed to load from %ls (Win32 %lu)", path, GetLastError());
        return;
    }
    using BootstrapFn = HRESULT (WINAPI *)();
    const auto bootstrap = reinterpret_cast<BootstrapFn>(GetProcAddress(bridge, "DLSS5Bootstrap"));
    if (bootstrap == nullptr)
    {
        LayerLog("[layer] %ls is not the DLSS5 DXGI bridge; DLSS5Bootstrap export missing", path);
        return;
    }
    const HRESULT result = bootstrap();
    LayerLog("[layer] private DXGI/ReShade bootstrap -> 0x%08lX", static_cast<unsigned long>(result));
}

// The extensions the DLSS5-Feeder transport needs on the device.
static const char *kWanted[] = {
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,       // VkMemoryDedicatedAllocateInfo
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,  // dependency of the above
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,         // core in 1.2, still advertised by drivers
};

// The loader hands several VK_STRUCTURE_TYPE_LOADER_*_CREATE_INFO nodes down the
// pNext chain, distinguished by 'function'. Matching on sType alone will happily
// return the FEATURES node -- whose union member is a 32-bit flag where a pointer
// is expected -- and dereferencing that is an instant access violation.
template <typename T>
static const T *FindLayerChainInfo(const void *pNext, VkStructureType type)
{
    for (const auto *s = static_cast<const VkBaseInStructure *>(pNext); s != nullptr; s = s->pNext)
    {
        const auto *c = reinterpret_cast<const T *>(s);
        if (s->sType == type && c->function == VK_LAYER_LINK_INFO)
            return c;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL FeedCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                         const VkAllocationCallbacks *pAllocator,
                                                         VkInstance *pInstance)
{
    BootstrapPrivateDxgi();

    const auto *chain = FindLayerChainInfo<VkLayerInstanceCreateInfo>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (chain == nullptr || chain->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    const PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(nullptr, "vkCreateInstance"));
    if (create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    // Advance the chain for the layer below us before calling down.
    const_cast<VkLayerInstanceCreateInfo *>(chain)->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    const VkResult r = create(pCreateInfo, pAllocator, pInstance);
    if (r == VK_SUCCESS)
    {
        g_next_gipa = next_gipa;
        LayerLog("[layer] instance created; app requested %u instance extension(s)",
                 pCreateInfo->enabledExtensionCount);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL FeedCreateDevice(VkPhysicalDevice physicalDevice,
                                                       const VkDeviceCreateInfo *pCreateInfo,
                                                       const VkAllocationCallbacks *pAllocator,
                                                       VkDevice *pDevice)
{
    const auto *chain = FindLayerChainInfo<VkLayerDeviceCreateInfo>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (chain == nullptr || chain->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    const PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr   next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(nullptr, "vkCreateDevice"));
    if (create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    // What does the driver actually offer? vkEnumerateDeviceExtensionProperties is
    // resolved by name through the instance GIPA rather than linked: current headers
    // do not always declare it, and it exists at runtime regardless.
    std::vector<std::string> supported;
    if (const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            next_gipa(VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties")))
    {
        uint32_t n = 0;
        if (enumerate(physicalDevice, nullptr, &n, nullptr) == VK_SUCCESS && n > 0)
        {
            std::vector<VkExtensionProperties> props(n);
            if (enumerate(physicalDevice, nullptr, &n, props.data()) == VK_SUCCESS)
                for (const auto &p : props) supported.emplace_back(p.extensionName);
        }
    }
    const bool know_supported = !supported.empty();
    if (!know_supported)
        LayerLog("[layer] could not enumerate device extensions; assuming the four Win32 "
                 "external-interop extensions are supported (they are, on every Windows driver)");

    // Start from what the app asked for, append what is missing.
    std::vector<const char *> exts(pCreateInfo->ppEnabledExtensionNames,
                                   pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    auto already = [&](const char *name) {
        // Answered against the ORIGINAL list, never against the vector we append to --
        // otherwise the logging pass below sees our own additions and reports every one
        // of them as having come from the app.
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], name) == 0) return true;
        return false;
    };
    auto driver_has = [&](const char *name) {
        if (!know_supported) return true;   // fall back to "supported"
        for (const auto &s : supported) if (s == name) return true;
        return false;
    };

    int added = 0;
    for (const char *want : kWanted)
        if (!already(want) && driver_has(want)) { exts.push_back(want); ++added; }

    // timelineSemaphore is a FEATURE, not just an extension: importing a D3D12 fence
    // yields a timeline semaphore, so it has to be switched on as well. If the app
    // already chains a features struct that covers it, leave everything alone;
    // otherwise chain our own (stack lifetime is fine -- the call below is synchronous).
    VkDeviceCreateInfo ci = *pCreateInfo;
    VkPhysicalDeviceTimelineSemaphoreFeatures tl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    bool have_timeline_feature = false;
    for (const auto *s = static_cast<const VkBaseInStructure *>(pCreateInfo->pNext); s != nullptr; s = s->pNext)
    {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES &&
            reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures *>(s)->timelineSemaphore)
            have_timeline_feature = true;
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES &&
            reinterpret_cast<const VkPhysicalDeviceVulkan12Features *>(s)->timelineSemaphore)
            have_timeline_feature = true;
    }
    if (!have_timeline_feature)
    {
        tl.timelineSemaphore = VK_TRUE;
        tl.pNext = const_cast<void *>(ci.pNext);
        ci.pNext = &tl;
    }

    ci.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();

    LayerLog("[layer] vkCreateDevice: app asked for %u extension(s), added %d, timelineSemaphore %s",
             pCreateInfo->enabledExtensionCount, added,
             have_timeline_feature ? "already enabled by the app" : "enabled by this layer");
    for (const char *want : kWanted)
        LayerLog("[layer]   %-40s %s", want,
                 already(want) ? "(app)" : driver_has(want) ? "ADDED" : "unsupported by driver");

    // Advance the chain, then call down with our modified create info.
    const_cast<VkLayerDeviceCreateInfo *>(chain)->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    VkResult r = create(physicalDevice, &ci, pAllocator, pDevice);

    if (r != VK_SUCCESS && added > 0)
    {
        // A driver that advertised an extension but refuses it is not worth arguing
        // with: retry untouched so the layer can never stop a game from starting.
        LayerLog("[layer] vkCreateDevice failed (%d) with the added extensions; retrying with the app's original list", r);
        r = create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }
    if (r == VK_SUCCESS) g_next_gdpa = next_gdpa;
    LayerLog("[layer] vkCreateDevice -> %d", r);
    return r;
}

// ---------------------------------------------------------------------------
// Entry points. We intercept only the two creates; everything else falls through.
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
FeedGetDeviceProcAddr(VkDevice device, const char *pName)
{
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(FeedGetDeviceProcAddr);
    return g_next_gdpa != nullptr ? g_next_gdpa(device, pName) : nullptr;
}

extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
FeedGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    if (strcmp(pName, "vkCreateInstance")      == 0) return reinterpret_cast<PFN_vkVoidFunction>(FeedCreateInstance);
    if (strcmp(pName, "vkCreateDevice")        == 0) return reinterpret_cast<PFN_vkVoidFunction>(FeedCreateDevice);
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(FeedGetInstanceProcAddr);
    if (strcmp(pName, "vkGetDeviceProcAddr")   == 0) return reinterpret_cast<PFN_vkVoidFunction>(FeedGetDeviceProcAddr);
    return g_next_gipa != nullptr ? g_next_gipa(instance, pName) : nullptr;
}

// The modern negotiation entry point; the loader prefers this over the name-based
// vkGetInstanceProcAddr export. vk_layer.h already declares it (inside extern "C"),
// so it is defined plainly here and exported by the linker -- see build-layer.bat.
VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
    if (pVersionStruct == nullptr || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion > 2)
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr       = FeedGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr         = FeedGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    LayerLog("[layer] " FEED_LAYER_NAME " negotiated (interface version %u)",
             pVersionStruct->loaderLayerInterfaceVersion);
    return VK_SUCCESS;
}
