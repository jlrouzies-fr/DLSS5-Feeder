// feed_vk_hook.h - make the Vulkan transport possible without any launcher.
//
// The transport (feed_vk.h) imports the feeder's D3D12 fences and textures into the
// game's own VkDevice. That needs the KHR external-interop extensions plus the
// timelineSemaphore feature, and Vulkan fixes both at vkCreateDevice. Games enable
// only what they use, so most of them would leave the feed with no entry points.
//
// The add-on is loaded early enough to fix that itself: ReShade's Vulkan layer calls
// reshade::load_addons() INSIDE its vkCreateInstance hook, before the game ever calls
// vkCreateDevice. So on the create_device event (fired from that same place) we put an
// inline hook on vulkan-1.dll's exported vkCreateDevice. The loader hands that very
// export back for vkGetInstanceProcAddr(instance, "vkCreateDevice") too, so a direct
// link, volk, or any other loading style all land here -- above every layer,
// including ReShade's, which then simply passes the extended list down.
//
// In the hook: append the extensions the driver supports and the app did not ask
// for, switch timelineSemaphore on if nothing in the chain does, call the original.
// If the driver then refuses, retry with the app's untouched create info -- the
// hook can never be the reason a game does not start.
//
// The hook MUST come out again on DLL unload (FeedVkHookRemove from DllMain):
// ReShade refcounts add-on loading per instance, and a game that creates a probe
// instance, destroys it, then creates the real one unloads and reloads this DLL in
// between. A jmp left behind into unmapped memory would crash the next vkCreateDevice.
//
// layer/VkLayer_feed_vk.dll does the same job from outside the process and remains
// the fallback for whatever loading style slips past this.

#pragma once
#include <MinHook.h>
#include <vector>
#include <string>

static void Log(const char *fmt, ...);   // dlss5-feed.cpp

static const char *kFeedVkWanted[] = {
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,       // VkMemoryDedicatedAllocateInfo
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,  // dependency of the above
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,         // core in 1.2, still advertised by drivers
};

static PFN_vkCreateDevice g_vk_create_device_orig;   // MinHook trampoline to the loader's export
static void              *g_vk_create_device_target;  // the export itself (hook target)
static int                g_vk_hook_devices;          // how many vkCreateDevice calls we have seen

// The graphics queue family the game asked for. ReShade's effect runtime (and so the
// command buffer every raw vkCmd* here records into) rides the game's present queue,
// which is a graphics queue -- the queue-family ownership transfers to/from
// VK_QUEUE_FAMILY_EXTERNAL (FeedVkExternalTransfer) need its family index.
// VK_QUEUE_FAMILY_IGNORED until a vkCreateDevice has been seen.
static uint32_t g_vk_gfx_family = VK_QUEUE_FAMILY_IGNORED;

// ---------------------------------------------------------------------------
// vkQueuePresentKHR hook -- the pacer detector.
//
// An external frame pacer (NVIDIA Smooth Motion above all) presents MORE times
// than the game renders frames. On D3D that is NvPresent64.dll, which
// DetectSmoothMotion() can see as a loaded module; on Vulkan the driver does it
// inside its own ICD, below every layer, so there is no module and no implicit
// layer to find -- the module check is structurally blind there (issues #1, #10).
//
// Counting presents against frames actually fed is vendor- and API-agnostic: it
// detects any interposer that adds presents, whoever wrote it. The image indices
// come along for free and answer the other open question (whether the swapchain
// ring the copy home writes into is the one being scanned out).
//
// Read-only: this hook never changes what it is handed, it only counts and calls
// through. Hooking present is otherwise exactly what this add-on avoids doing.
// ---------------------------------------------------------------------------

static PFN_vkQueuePresentKHR g_vk_present_orig;
static void                 *g_vk_present_target;
static volatile LONG64       g_vk_presents;        // vkQueuePresentKHR calls seen
static uint32_t              g_vk_last_image;      // last pImageIndices[0]
static bool                  g_vk_pacer_warned;

// Set by the feed each time it completes a frame, so the hook can compare.
// (Plain long long: single writer, and a torn read only misprints one log line.)
static volatile LONG64       g_vk_feed_frames;

static VKAPI_ATTR VkResult VKAPI_CALL FeedVkHookQueuePresent(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    const LONG64 presents = InterlockedIncrement64(&g_vk_presents);
    if (pPresentInfo != nullptr && pPresentInfo->swapchainCount > 0 && pPresentInfo->pImageIndices != nullptr)
        g_vk_last_image = pPresentInfo->pImageIndices[0];

    // The pacer signature. Only meaningful once the feed has run for a while, and
    // only said once: a real pacer keeps the ratio up for the whole session.
    const LONG64 fed = g_vk_feed_frames;
    if (!g_vk_pacer_warned && fed > 120 && presents > fed + fed / 4)
    {
        g_vk_pacer_warned = true;
        Log("[feed] an external frame pacer is presenting this swapchain: %lld presents against %lld frames fed "
            "(%.2fx). NVIDIA Smooth Motion does exactly this, and on Vulkan it lives inside the driver, so no "
            "module check can see it. This combination is NOT verified and is the subject of issues #1 and #10 "
            "-- if the image holds old frames or corrupts, turn Smooth Motion off for THIS API only in NVIDIA "
            "Profile Inspector: \"Smooth Motion - Enabled APIs\" (0xB0CC0875), clear bit 4 for Vulkan.",
            static_cast<long long>(presents), static_cast<long long>(fed),
            fed > 0 ? static_cast<double>(presents) / static_cast<double>(fed) : 0.0);
    }
    return g_vk_present_orig(queue, pPresentInfo);
}

// Called by the feed once per delivered frame; also drives the periodic report.
static void FeedVkPresentTick(unsigned long long fed_frames, int every)
{
    g_vk_feed_frames = static_cast<LONG64>(fed_frames);
    if (g_vk_present_orig == nullptr || every <= 0 || (fed_frames % static_cast<unsigned long long>(every)) != 0)
        return;
    const LONG64 presents = g_vk_presents;
    Log("[feed] present probe: %lld presents / %llu frames fed (%.2fx), last swapchain image index %u",
        static_cast<long long>(presents), fed_frames,
        fed_frames > 0 ? static_cast<double>(presents) / static_cast<double>(fed_frames) : 0.0,
        g_vk_last_image);
}

static VKAPI_ATTR VkResult VKAPI_CALL FeedVkHookCreateDevice(VkPhysicalDevice physicalDevice,
                                                             const VkDeviceCreateInfo *pCreateInfo,
                                                             const VkAllocationCallbacks *pAllocator,
                                                             VkDevice *pDevice)
{
    ++g_vk_hook_devices;
    if (pCreateInfo == nullptr) return g_vk_create_device_orig(physicalDevice, pCreateInfo, pAllocator, pDevice);

    // What does the driver actually offer? The enumerate entry point is a plain
    // vulkan-1.dll export; no GIPA needed.
    std::vector<std::string> supported;
    if (HMODULE lib = GetModuleHandleW(L"vulkan-1.dll"))
    {
        if (const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                GetProcAddress(lib, "vkEnumerateDeviceExtensionProperties")))
        {
            uint32_t n = 0;
            if (enumerate(physicalDevice, nullptr, &n, nullptr) == VK_SUCCESS && n > 0)
            {
                std::vector<VkExtensionProperties> props(n);
                if (enumerate(physicalDevice, nullptr, &n, props.data()) == VK_SUCCESS)
                    for (const auto &p : props) supported.emplace_back(p.extensionName);
            }
        }
    }
    const bool know_supported = !supported.empty();
    if (!know_supported)
        Log("[feed] vkCreateDevice hook: could not enumerate device extensions; assuming the Win32 "
            "external-interop extensions are supported (they are, on every Windows driver)");

    // Start from what the app asked for, append what is missing. "Did the app ask for
    // this?" is answered against the ORIGINAL list, never against the vector we are
    // appending to -- otherwise the logging pass below sees our own additions and
    // reports every one of them as having come from the app.
    std::vector<const char *> exts(pCreateInfo->ppEnabledExtensionNames,
                                   pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    auto already = [&](const char *name) {
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], name) == 0) return true;
        return false;
    };
    auto driver_has = [&](const char *name) {
        if (!know_supported) return true;
        for (const auto &s : supported) if (s == name) return true;
        return false;
    };

    int added = 0;
    for (const char *want : kFeedVkWanted)
        if (!already(want) && driver_has(want)) { exts.push_back(want); ++added; }

    // timelineSemaphore is a FEATURE as well: importing a D3D12 fence yields a timeline
    // semaphore, so it has to be on. If the app chains a struct that covers it, fine. If
    // it chains VkPhysicalDeviceVulkan12Features with the bit off, flip that bit in place
    // (a second timeline struct next to Vulkan12Features is not allowed). Otherwise chain
    // our own (stack lifetime is fine -- the call below is synchronous).
    VkDeviceCreateInfo ci = *pCreateInfo;
    VkPhysicalDeviceTimelineSemaphoreFeatures tl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    const char *timeline_how = "enabled by the add-on";
    bool have_timeline_feature = false;
    for (const auto *s = static_cast<const VkBaseInStructure *>(pCreateInfo->pNext); s != nullptr; s = s->pNext)
    {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
        {
            auto *f = const_cast<VkPhysicalDeviceTimelineSemaphoreFeatures *>(
                reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures *>(s));
            timeline_how = f->timelineSemaphore ? "already enabled by the app" : "switched on in the app's features struct";
            f->timelineSemaphore = VK_TRUE;
            have_timeline_feature = true;
        }
        else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        {
            auto *f = const_cast<VkPhysicalDeviceVulkan12Features *>(
                reinterpret_cast<const VkPhysicalDeviceVulkan12Features *>(s));
            timeline_how = f->timelineSemaphore ? "already enabled by the app" : "switched on in the app's Vulkan 1.2 features";
            f->timelineSemaphore = VK_TRUE;
            have_timeline_feature = true;
        }
    }
    if (!have_timeline_feature)
    {
        tl.timelineSemaphore = VK_TRUE;
        tl.pNext = const_cast<void *>(ci.pNext);
        ci.pNext = &tl;
    }

    ci.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();

    // Which of the requested queue families is the graphics one? The enumerate entry
    // point is a plain vulkan-1.dll export, same as the extension query above.
    if (HMODULE lib = GetModuleHandleW(L"vulkan-1.dll"))
    {
        if (const auto qprops = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                GetProcAddress(lib, "vkGetPhysicalDeviceQueueFamilyProperties")))
        {
            uint32_t nf = 0;
            qprops(physicalDevice, &nf, nullptr);
            std::vector<VkQueueFamilyProperties> fams(nf);
            if (nf > 0) qprops(physicalDevice, &nf, fams.data());
            for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i)
            {
                const uint32_t f = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
                if (f < nf && (fams[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) { g_vk_gfx_family = f; break; }
            }
        }
    }

    Log("[feed] vkCreateDevice #%d: app asked for %u extension(s), added %d, timelineSemaphore %s, graphics queue family %u",
        g_vk_hook_devices, pCreateInfo->enabledExtensionCount, added, timeline_how,
        g_vk_gfx_family);
    for (const char *want : kFeedVkWanted)
        Log("[feed]   %-40s %s", want,
            already(want) ? "(app)" : driver_has(want) ? "ADDED" : "unsupported by driver");

    VkResult r = g_vk_create_device_orig(physicalDevice, &ci, pAllocator, pDevice);
    if (r != VK_SUCCESS && (added > 0 || !have_timeline_feature))
    {
        // A driver that advertised an extension but refuses it is not worth arguing
        // with: retry untouched so the hook can never stop a game from starting.
        Log("[feed] vkCreateDevice failed (%d) with the added extensions; retrying with the app's original create info", r);
        r = g_vk_create_device_orig(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }
    Log("[feed] vkCreateDevice -> %d", r);
    return r;
}

// Install from the create_device event (api == vulkan), i.e. from inside ReShade's
// vkCreateInstance hook: vulkan-1.dll is loaded, no device exists yet. Idempotent.
static bool FeedVkHookInstall()
{
    if (g_vk_create_device_target != nullptr) return true;

    HMODULE lib = GetModuleHandleW(L"vulkan-1.dll");
    if (lib == nullptr)
    {
        Log("[feed] vkCreateDevice hook: vulkan-1.dll is not loaded in this process (?)");
        return false;
    }
    void *target = GetProcAddress(lib, "vkCreateDevice");
    if (target == nullptr)
    {
        Log("[feed] vkCreateDevice hook: vulkan-1.dll exports no vkCreateDevice (?)");
        return false;
    }

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("[feed] vkCreateDevice hook: MH_Initialize -> %s", MH_StatusToString(s));
        return false;
    }
    s = MH_CreateHook(target, reinterpret_cast<void *>(&FeedVkHookCreateDevice),
                      reinterpret_cast<void **>(&g_vk_create_device_orig));
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK)
    {
        Log("[feed] vkCreateDevice hook: could not hook vulkan-1!vkCreateDevice (%s); "
            "fall back to layer\\run-with-feed-layer.bat if the interop entry points turn out missing",
            MH_StatusToString(s));
        MH_RemoveHook(target);
        return false;
    }
    g_vk_create_device_target = target;
    Log("[feed] vkCreateDevice hook installed on vulkan-1!vkCreateDevice (%p): the KHR external-interop "
        "extensions will be appended to every device this game creates", target);

    // vkQueuePresentKHR: counting only, and strictly optional -- a game that cannot
    // be hooked here still feeds fine, it just gets no pacer detection.
    if (void *ptarget = GetProcAddress(lib, "vkQueuePresentKHR"))
    {
        MH_STATUS ps = MH_CreateHook(ptarget, reinterpret_cast<void *>(&FeedVkHookQueuePresent),
                                     reinterpret_cast<void **>(&g_vk_present_orig));
        if (ps == MH_OK) ps = MH_EnableHook(ptarget);
        if (ps == MH_OK)
        {
            g_vk_present_target = ptarget;
            Log("[feed] vkQueuePresentKHR hook installed (%p): counting presents against frames fed, to detect "
                "an external frame pacer (Smooth Motion is invisible to a module check on Vulkan)", ptarget);
        }
        else
        {
            MH_RemoveHook(ptarget);
            g_vk_present_orig = nullptr;
            Log("[feed] vkQueuePresentKHR hook: %s -- no pacer detection this session (harmless)",
                MH_StatusToString(ps));
        }
    }
    return true;
}

// From DllMain(DLL_PROCESS_DETACH). See the header comment for why this is mandatory.
static void FeedVkHookRemove()
{
    if (g_vk_create_device_target == nullptr) return;
    if (g_vk_present_target != nullptr)
    {
        MH_DisableHook(g_vk_present_target);
        MH_RemoveHook(g_vk_present_target);
        g_vk_present_target = nullptr;
        g_vk_present_orig   = nullptr;
    }
    MH_DisableHook(g_vk_create_device_target);
    MH_RemoveHook(g_vk_create_device_target);
    MH_Uninitialize();
    g_vk_create_device_target = nullptr;
    g_vk_create_device_orig   = nullptr;
    Log("[feed] vkCreateDevice hook removed");
}
