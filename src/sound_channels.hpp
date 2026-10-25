// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
namespace dust {
// DF1's two effect channels: singlesound avoids restarting an equal priority;
// multiplesound can restart it. The lower priority is replaced, slot 2 on ties.
// Decision rule ported from the pinned GPL dreamREfactory sound-channels-v1.ts.
inline int soundChannel(uint32_t priority, uint32_t one, uint32_t two, bool multiple) {
    const int lower = one < two ? 0 : 1;
    const auto low = lower == 0 ? one : two, high = lower == 0 ? two : one;
    if (!multiple)
        return priority > low && priority != high ? lower : -1;
    if (priority < low)
        return -1;
    return priority == high ? 1 - lower : lower;
}
} // namespace dust
