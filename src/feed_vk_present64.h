// 64-bit feeder only. ReShade 6.8 attaches the game's present waits AFTER
// present_effect_runtime returns. An add-on flushing inside a technique must
// bring those dependencies forward before submitting any recorded effects.
#pragma once
#include "feed_vk_present_order.h"
#include <vector>

struct FeedVkPresentContext
{
    VkQueue queue;
    const VkPresentInfoKHR *info;
    bool ordered = false;
    reshade::api::command_queue *graphics = nullptr;
};
static thread_local FeedVkPresentContext *g_vk_present_context;

// Adapted from upstream 9febd0d. Stack lifetime avoids a fixed TLS nesting limit.
// Loader and device hooks may enclose the same present; keep their ordering gate
// shared so an input binary semaphore is consumed and re-armed only once.
struct FeedVkPresentScope
{
    FeedVkPresentContext context;
    FeedVkPresentContext *previous;
    FeedVkPresentScope(VkQueue queue, const VkPresentInfoKHR *info)
        : context { queue, info }, previous(g_vk_present_context)
    {
        if (!previous || previous->queue != queue || previous->info != info)
            g_vk_present_context = &context;
    }
    ~FeedVkPresentScope() { g_vk_present_context = previous; }
    FeedVkPresentScope(const FeedVkPresentScope &) = delete;
    FeedVkPresentScope &operator=(const FeedVkPresentScope &) = delete;
};

// The CK3 bridge patches proc-address imports in the executable. Resolve loader
// exports through Windows itself, including in executable-based regression tests.
static FARPROC FeedVkLoaderProc(HMODULE loader, const char *name)
{
    using Proc = FARPROC (WINAPI *)(HMODULE, LPCSTR);
    const auto get = reinterpret_cast<Proc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress"));
    return get && loader ? get(loader, name) : nullptr;
}

static PFN_vkQueuePresentKHR g_vk_loader_present_orig;
static void *g_vk_loader_present_target;
static PFN_vkQueuePresentKHR g_vk_frame_present_orig;
static void *g_vk_frame_present_target;
static VkDevice g_vk_frame_present_device;
static std::vector<VkDevice> g_vk_present_devices;

static void FeedVkPresentRememberDevice(VkDevice device)
{
    for (auto known : g_vk_present_devices) if (known == device) return;
    g_vk_present_devices.push_back(device);
}

static VKAPI_ATTR VkResult VKAPI_CALL FeedVkLoaderPresent(VkQueue queue, const VkPresentInfoKHR *info)
{
    FeedVkPresentScope scope(queue, info);
    return g_vk_loader_present_orig(queue, info);
}

static VKAPI_ATTR VkResult VKAPI_CALL FeedVkFramePresent(VkQueue queue, const VkPresentInfoKHR *info)
{
    FeedVkPresentScope scope(queue, info);
    return g_vk_frame_present_orig(queue, info);
}

static void FeedVkFramePresentRemove();

static bool FeedVkLoaderPresentInstall()
{
    if (g_vk_loader_present_target) return true;
    HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    void *target = reinterpret_cast<void *>(FeedVkLoaderProc(loader, "vkQueuePresentKHR"));
    if (!target) return false;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    status = MH_CreateHook(target, reinterpret_cast<void *>(&FeedVkLoaderPresent), reinterpret_cast<void **>(&g_vk_loader_present_orig));
    const bool created = status == MH_OK;
    if (created) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        if (created) MH_RemoveHook(target);
        g_vk_loader_present_orig = nullptr;
        Log("[feed] Vulkan loader present context hook failed: %s", MH_StatusToString(status));
        return false;
    }
    g_vk_loader_present_target = target;
    Log("[feed] Vulkan present context hook installed at loader export %p", target);
    return true;
}

static void FeedVkLoaderPresentRemove()
{
    if (!g_vk_loader_present_target) return;
    MH_DisableHook(g_vk_loader_present_target);
    MH_RemoveHook(g_vk_loader_present_target);
    g_vk_loader_present_target = nullptr;
    g_vk_loader_present_orig = nullptr;
}

static bool FeedVkFramePresentInstallDevice(VkDevice device)
{
    if (!device) return false;
    HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(FeedVkLoaderProc(loader, "vkGetDeviceProcAddr"));
    // At init_device the system loader has not yet published this device's
    // dispatch table. ReShade has registered it and its layer resolver is ready.
    const auto layer_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        FeedVkLoaderProc(reshade::internal::get_reshade_module_handle(), "vkGetDeviceProcAddr"));
    void *target = layer_gdpa ? reinterpret_cast<void *>(layer_gdpa(device, "vkQueuePresentKHR")) : nullptr;
    const bool layer_target = target != nullptr;
    if (!target && gdpa) target = reinterpret_cast<void *>(gdpa(device, "vkQueuePresentKHR"));
    if (!target) return false;
    if (g_vk_frame_present_target == target || g_vk_loader_present_target == target)
    {
        g_vk_frame_present_device = device;
        FeedVkPresentRememberDevice(device);
        return true;
    }
    // A different dispatch entry than the one we hold means a new device (or a changed
    // layer chain) -- re-hook rather than reporting failure and leaving the old device's
    // entry hooked, which is what returning false here used to do.
    if (g_vk_frame_present_target) FeedVkFramePresentRemove();
    // The layer/device entry catches engines and interposers that bypass the
    // loader present export, including CK3's Native Streamline shim.
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    status = MH_CreateHook(target, reinterpret_cast<void *>(&FeedVkFramePresent), reinterpret_cast<void **>(&g_vk_frame_present_orig));
    const bool created = status == MH_OK;
    if (created) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("[feed] Vulkan present dependency hook failed: %s", MH_StatusToString(status));
        // Do not remove another owner's hook if CreateHook returned ALREADY_CREATED.
        if (created) MH_RemoveHook(target);
        g_vk_frame_present_orig = nullptr;
        return false;
    }
    g_vk_frame_present_target = target;
    g_vk_frame_present_device = device;
    FeedVkPresentRememberDevice(device);
    Log("[feed] Vulkan present dependency hook installed at %s %p", layer_target ? "ReShade layer dispatch" : "device dispatch", target);
    return true;
}

static bool FeedVkFramePresentInstall(reshade::api::effect_runtime *rt)
{
    if (rt->get_device()->get_api() != reshade::api::device_api::vulkan) return true;
    return FeedVkFramePresentInstallDevice(FeedVkDispatch<VkDevice>(rt->get_device()->get_native()));
}

static void FeedVkFramePresentRemove()
{
    g_vk_frame_present_device = VK_NULL_HANDLE;
    if (!g_vk_frame_present_target) return;
    MH_DisableHook(g_vk_frame_present_target);
    MH_RemoveHook(g_vk_frame_present_target);
    g_vk_frame_present_target = nullptr;
    g_vk_frame_present_orig = nullptr;
}

static int FeedVkPresentSlot(reshade::api::effect_runtime *rt)
{
    if (!g_vk_present_context || !g_vk_present_context->info) return -1;
    const auto *info = g_vk_present_context->info;
    if (!info->pSwapchains || !info->pImageIndices ||
        (info->waitSemaphoreCount && !info->pWaitSemaphores)) return -1;
    for (uint32_t i = 0; i < info->swapchainCount; ++i)
        if (FeedVkValue(info->pSwapchains[i]) == rt->get_native()) return static_cast<int>(i);
    return -1;
}

static void FeedVkPresentForgetDevice(VkDevice device)
{
    for (auto it = g_vk_present_devices.begin(); it != g_vk_present_devices.end(); ++it)
        if (*it == device) { g_vk_present_devices.erase(it); break; }
    // ReShade's present entry point is shared by its devices. A temporary device
    // going away must not remove the dependency hook from a surviving device.
    if (g_vk_present_devices.empty()) FeedVkFramePresentRemove();
    else if (g_vk_frame_present_device == device) g_vk_frame_present_device = g_vk_present_devices.back();
}

// No raw QueueSubmit and no CPU drain. These calls run inside ReShade's present
// queue locks. Wait ALL original binary semaphores before signal() can flush the
// recorded effect list. Re-arm them once so ReShade's later present submission
// can still consume its unchanged wait list. Timeline values are ignored for
// binary semaphores by Vulkan. One gate per present, including multi-swapchain.
static bool FeedVkOrderPresent(reshade::api::effect_runtime *rt, reshade::api::command_list *cl)
{
    const int slot = FeedVkPresentSlot(rt);
    if (slot < 0 || cl != rt->get_command_queue()->get_immediate_command_list()) return false;
    auto &context = *g_vk_present_context;
    auto *queue = rt->get_command_queue();
    if (context.ordered) return context.graphics == queue;
    const auto *info = context.info;
    if (!FeedVkWaitAndRearm(info->waitSemaphoreCount,
        [&](uint32_t i) { return queue->wait({ FeedVkValue(info->pWaitSemaphores[i]) }, 0); },
        [&](uint32_t i) { return queue->signal({ FeedVkValue(info->pWaitSemaphores[i]) }, 0); })) return false;
    context.ordered = true;
    context.graphics = queue;
    return true;
}
