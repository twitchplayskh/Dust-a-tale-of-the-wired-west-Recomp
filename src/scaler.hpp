// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "dust.hpp"
namespace dust {
struct ScaleTarget {
    int top = 0, left = 0, bottom = 0, right = 0;
    std::function<bool(int, int)> drawable;
    std::function<void(int, int, uint8_t)> plot;
    std::function<void(int, int, size_t)> plotSource;
};
// Adapted from pinned GPL dreamREfactory runtime/sprite-scaler.ts. Its golden
// corpus records upstream DF1 engine output; matching this installation's
// ALT31 executable remains a separate verification step.
void drawScaledSprite(const Sprite &, int anchorX, int anchorY, int width, int height, bool flip,
                      const ScaleTarget &);
bool scaledSpriteCovers(const Sprite &, int left, int top, int width, int height, int x, int y);
} // namespace dust
