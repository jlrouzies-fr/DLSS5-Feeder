#pragma once
#include <cstdint>
struct VkHookTestStats
{
    uint32_t presents;
    uint32_t covered;
    uint32_t ordered;
    uint32_t early_devices;
    uint32_t active_context;
};
using VkHookTestReadStats = void (*)(VkHookTestStats *);
