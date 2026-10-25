// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "restoration.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace dust {
struct Viewport {
    int x = 0, y = 0, width = 0, height = 0, integerScale = 0;
};
inline Viewport scaledViewport(int width, int height, int nativeWidth, int nativeHeight,
                               bool integer) {
    if (width <= 0 || height <= 0 || nativeWidth <= 0 || nativeHeight <= 0)
        return {};
    const int scale = std::min(width / nativeWidth, height / nativeHeight);
    int w, h;
    if (integer && scale > 0) {
        w = nativeWidth * scale;
        h = nativeHeight * scale;
    } else if (int64_t(width) * nativeHeight <= int64_t(height) * nativeWidth) {
        w = width;
        h = int(int64_t(width) * nativeHeight / nativeWidth);
    } else {
        h = height;
        w = int(int64_t(height) * nativeWidth / nativeHeight);
    }
    return {(width - w) / 2, (height - h) / 2, w, h, integer && scale > 0 ? scale : 0};
}
struct DisplaySettings {
    int width = 1024, height = 768;
    bool fullscreen = false, integerScaling = false;
    bool smoothScaling = false; // GPU presentation preference; legacy INI compatibility.
    RestorationSettings restoration;
    int restorationCacheMiB = 128;
    int volume = 100;
    bool captions = false, skipIntro = false;
    std::wstring gameRoot;
};
std::filesystem::path defaultSettingsPath();
DisplaySettings loadSettings(const std::filesystem::path &);
void saveSettings(const std::filesystem::path &, const DisplaySettings &);
inline uint8_t volumeGain(int percent) {
    return uint8_t((std::clamp(percent, 0, 100) * 255 + 50) / 100);
}
inline void scalePcm(std::vector<int16_t> &samples, uint8_t gain) {
    if (gain != 255)
        for (auto &sample : samples)
            sample = int16_t(int32_t(sample) * gain / 255);
}
} // namespace dust
