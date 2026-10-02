// Instrument the real feeder; test-only callbacks never enter the shipped DLL.
#define DllMain FeederDllMain
#include "../src/dlss5-feed.cpp"
#undef DllMain
#include "vk-hook-test-api.h"
#include <algorithm>
#include <vector>

static VkHookTestStats stats;
static std::vector<reshade::api::effect_runtime *> runtimes;

static void ObserveDevice(reshade::api::device *dev)
{
    if (dev->get_api() == reshade::api::device_api::vulkan &&
        g_vk_frame_present_device == FeedVkDispatch<VkDevice>(dev->get_native()) &&
        (g_vk_frame_present_target || g_vk_loader_present_target)) ++stats.early_devices;
}
static void ObserveRuntime(reshade::api::effect_runtime *rt)
{
    if (rt->get_device()->get_api() == reshade::api::device_api::vulkan) runtimes.push_back(rt);
}
static void ForgetRuntime(reshade::api::effect_runtime *rt)
{
    runtimes.erase(std::remove(runtimes.begin(), runtimes.end(), rt), runtimes.end());
}
static void ObservePresent(reshade::api::command_queue *, reshade::api::swapchain *swapchain,
    const reshade::api::rect *, const reshade::api::rect *, uint32_t, const reshade::api::rect *)
{
    if (swapchain->get_device()->get_api() != reshade::api::device_api::vulkan) return;
    ++stats.presents;
    if (g_vk_present_context && g_vk_present_context->info)
    {
        const auto *info = g_vk_present_context->info;
        for (uint32_t i = 0; i < info->swapchainCount; ++i)
            if (FeedVkValue(info->pSwapchains[i]) == swapchain->get_native()) { ++stats.covered; break; }
    }
    for (auto *rt : runtimes)
        if (rt->get_native() == swapchain->get_native())
        {
            // Run the actual semaphore gate inside ReShade's present queue locks.
            auto *cl = rt->get_command_queue()->get_immediate_command_list();
            if (FeedVkOrderPresent(rt, cl) && FeedVkOrderPresent(rt, cl)) ++stats.ordered;
            break;
        }
}
extern "C" __declspec(dllexport) void ReadVkHookTestStats(VkHookTestStats *out)
{
    *out = stats;
    out->active_context = g_vk_present_context != nullptr;
}
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH)
    {
        reshade::unregister_event<reshade::addon_event::init_device>(ObserveDevice);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(ObserveRuntime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(ForgetRuntime);
        reshade::unregister_event<reshade::addon_event::present>(ObservePresent);
    }
    const BOOL result = FeederDllMain(module, reason, reserved);
    if (reason == DLL_PROCESS_ATTACH && result)
    {
        reshade::register_event<reshade::addon_event::init_device>(ObserveDevice);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(ObserveRuntime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(ForgetRuntime);
        reshade::register_event<reshade::addon_event::present>(ObservePresent);
    }
    return result;
}
