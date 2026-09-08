// 64-bit feeder only. ReShade 6.8 attaches the game's present waits AFTER
// present_effect_runtime returns. An add-on flushing inside a technique must
// bring those dependencies forward before submitting any recorded effects.
#pragma once
#include "feed_vk_present_order.h"

struct FeedVkPresentContext
{
    VkQueue queue;
    const VkPresentInfoKHR *info;
    bool ordered = false;
    reshade::api::command_queue *graphics = nullptr;
};
static thread_local FeedVkPresentContext *g_vk_present_context;
static PFN_vkQueuePresentKHR g_vk_frame_present_orig;
static void *g_vk_frame_present_target;

// Latched by the frame path when the context never appears at all. Some installs can never
// satisfy the gate -- the technique callback is simply not nested inside the hooked present
// there -- and before this the precaution meant "no session, ever" rather than "no ordering"
// (#13). Session-scoped, never read from the config.
static bool g_vk_present_sync_off;

static VKAPI_ATTR VkResult VKAPI_CALL FeedVkFramePresent(VkQueue queue, const VkPresentInfoKHR *info)
{
    FeedVkPresentContext context = { queue, info };
    struct Scope
    {
        FeedVkPresentContext *previous;
        ~Scope() { g_vk_present_context = previous; }
    } scope = { g_vk_present_context };
    g_vk_present_context = &context;
    return g_vk_frame_present_orig(queue, info);
}

static void FeedVkFramePresentRemove();

static bool FeedVkFramePresentInstall(reshade::api::effect_runtime *rt)
{
    if (rt->get_device()->get_api() != reshade::api::device_api::vulkan) return true;
    HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    const auto gdpa = loader ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(loader, "vkGetDeviceProcAddr")) : nullptr;
    void *target = gdpa ? reinterpret_cast<void *>(gdpa(FeedVkDispatch<VkDevice>(rt->get_device()->get_native()), "vkQueuePresentKHR")) : nullptr;
    if (!target) return false;
    if (g_vk_frame_present_target == target) return true;
    // A different dispatch entry than the one we hold means a new device (or a changed
    // layer chain) -- re-hook rather than reporting failure and leaving the old device's
    // entry hooked, which is what returning false here used to do.
    if (g_vk_frame_present_target) FeedVkFramePresentRemove();
    // The device dispatch entry includes ReShade and catches engines bypassing
    // the loader export used by the old, counting-only present hook.
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    status = MH_CreateHook(target, reinterpret_cast<void *>(&FeedVkFramePresent), reinterpret_cast<void **>(&g_vk_frame_present_orig));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("[feed] Vulkan present dependency hook failed: %s", MH_StatusToString(status));
        // Do not remove another owner's hook if CreateHook returned ALREADY_CREATED.
        if (g_vk_frame_present_orig) MH_RemoveHook(target);
        g_vk_frame_present_orig = nullptr;
        return false;
    }
    g_vk_frame_present_target = target;
    Log("[feed] Vulkan present dependency hook installed at device dispatch %p", target);
    return true;
}

static void FeedVkFramePresentRemove()
{
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
    for (uint32_t i = 0; i < info->swapchainCount; ++i)
        if (FeedVkValue(info->pSwapchains[i]) == rt->get_native()) return static_cast<int>(i);
    return -1;
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

static void FeedVkIdentity(reshade::api::effect_runtime *rt, reshade::api::command_list *cl,
    reshade::api::resource_view rtv, reshade::api::resource captured, uint64_t frame, uint64_t cb_in,
    uint64_t color_vk, uint64_t output_vk, const void *color12, const void *output12,
    uint64_t in_value, uint64_t out_value, bool copied)
{
    const int slot = FeedVkPresentSlot(rt);
    const uint32_t index = rt->get_current_back_buffer_index();
    const uint32_t present_index = slot >= 0 ? g_vk_present_context->info->pImageIndices[slot] : UINT32_MAX;
    const auto current = rt->get_back_buffer(index);
    const auto presented = present_index < rt->get_back_buffer_count() ? rt->get_back_buffer(present_index) : reshade::api::resource {};
    const auto home = rt->get_device()->get_resource_from_view(rtv);
    Log("[feed] vk identity frame=%llu swap=%llx runtime_idx=%u present_idx=%u present_image=%llx current=%llx rtv=%llx capture=%llx home=%llx intermediate=%d stable=%d copied=%d color_vk=%llx output_vk=%llx color12=%p output12=%p gfx_q=%llx present_q=%llx cb_in=%llx cb_home=%llx in=%llu out=%llu waits=%u ordered=%d",
        frame, rt->get_native(), index, present_index, presented.handle, current.handle, rtv.handle,
        captured.handle, home.handle, captured != current, captured == home && index == present_index, copied,
        color_vk, output_vk, color12, output12, rt->get_command_queue()->get_native(),
        g_vk_present_context ? FeedVkValue(g_vk_present_context->queue) : 0,
        cb_in, cl->get_native(), in_value, out_value,
        g_vk_present_context ? g_vk_present_context->info->waitSemaphoreCount : 0,
        g_vk_present_context && g_vk_present_context->ordered);
}
