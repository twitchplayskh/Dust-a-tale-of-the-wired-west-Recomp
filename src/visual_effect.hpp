// SPDX-License-Identifier: GPL-3.0-only
// DF1 reveals adapted from pinned dreamREfactory runtime/visual-effects.ts.
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
namespace dust {
inline int effectPasses(int mode, int height) {
    if (mode < 24001 || mode > 24014 || height <= 0)
        throw std::runtime_error("invalid visual effect");
    return mode <= 24002   ? 8
           : mode == 24003 ? 6
           : mode == 24004 ? 16
           : mode == 24009 ? (height >> 3) + 1
           : mode == 24014 ? 1
                           : 15;
}
inline void composeEffect(int mode, int width, int height, int passes,
                          std::span<const uint32_t> from, std::span<const uint32_t> to,
                          std::span<uint32_t> out) {
    const auto size = size_t(width) * size_t(height);
    if (width <= 0 || height <= 0 || from.size() < size || to.size() < size || out.size() < size)
        throw std::runtime_error("invalid effect canvas");
    const int n = std::clamp(passes, 0, effectPasses(mode, height));
    const int dx = width / 16 + 1, dy = height / 16 + 1;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            bool reveal = false;
            int sx = x, sy = y;
            switch (mode) {
            case 24001:
                reveal = x < n * dx || x >= width - n * dx;
                break;
            case 24002:
                reveal = x >= width / 2 - n * dx && x < width / 2 + n * dx;
                break;
            case 24003:
                reveal = x < n * (width / 12 + 1) || x >= width - n * (width / 12 + 1) ||
                         y < n * (height / 12 + 1) || y >= height - n * (height / 12 + 1);
                break;
            case 24004:
                reveal = x >= width / 2 - n * (width / 32 + 1) &&
                         x < width / 2 + n * (width / 32 + 1) &&
                         y >= height / 2 - n * (height / 32 + 1) &&
                         y < height / 2 + n * (height / 32 + 1);
                break;
            case 24005:
                reveal = y < std::min(height, n * dy);
                sy += height - std::min(height, n * dy);
                break;
            case 24006:
                reveal = y >= height - std::min(height, n * dy);
                sy -= height - std::min(height, n * dy);
                break;
            case 24007:
                reveal = x < std::min(width, n * dx);
                sx += width - std::min(width, n * dx);
                break;
            case 24008:
                reveal = x >= width - std::min(width, n * dx);
                sx -= width - std::min(width, n * dx);
                break;
            case 24009:
                reveal = n > 0 && y % ((height >> 3) + 1) < n + 1;
                break;
            case 24010:
                reveal = y < n * dy;
                break;
            case 24011:
                reveal = y >= height - n * dy;
                break;
            case 24012:
                reveal = x < n * dx;
                break;
            case 24013:
                reveal = x >= width - n * dx;
                break;
            case 24014:
                reveal = n > 0;
                break;
            }
            out[size_t(y) * width + x] =
                reveal ? to[size_t(sy) * width + sx] : from[size_t(y) * width + x];
        }
}
} // namespace dust
