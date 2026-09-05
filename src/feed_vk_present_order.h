#pragma once
#include <cstdint>

// signal() may flush recorded work. Queue every input dependency before any
// signal, then restore the binary tokens expected by the caller's final present.
template <typename Wait, typename Signal>
static bool FeedVkWaitAndRearm(uint32_t count, Wait wait, Signal signal)
{
    for (uint32_t i = 0; i < count; ++i) if (!wait(i)) return false;
    for (uint32_t i = 0; i < count; ++i) if (!signal(i)) return false;
    return true;
}
