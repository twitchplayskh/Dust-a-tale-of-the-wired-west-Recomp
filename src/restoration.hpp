// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace dust {
struct RestorationSettings {
    bool enabled = false;
    int deblock = 40, chroma = 45, detail = 15;
    bool multiscale = true, temporal = false;
    int temporalStrength = 35;
    bool operator==(const RestorationSettings &) const = default;
};
struct RestorationNeighbor {
    uint32_t identity = 0;
    int width = 0, height = 0, offsetX = 0, offsetY = 0, distance = 1;
    std::span<const uint32_t> rgb;
    std::span<const uint8_t> mask;
    bool continuous = true;
};
struct RestoredFrame {
    int width = 0, height = 0;
    std::vector<uint32_t> rgb;
    uint32_t temporalPixels = 0, temporalTiles = 0, rejectedTiles = 0;
};
struct RestorationStats {
    uint64_t hits = 0, misses = 0, evictions = 0;
    uint64_t temporalFrames = 0, temporalPixels = 0;
    size_t bytes = 0, budget = 0, entries = 0;
};
// One managed LRU per renderer, used on its owning thread. Keys include the
// original resource/container identity, source pixels/mask, algorithm revision,
// and settings plus explicit authored neighbors. No playback-history feedback.
// Internal processing entry point, also used by deterministic temporal tests.
void restoreTemporal(RestoredFrame &, std::span<const uint32_t>, std::span<const uint8_t>,
                     std::span<const RestorationNeighbor>, RestorationSettings);
class RestorationCache {
  public:
    explicit RestorationCache(size_t budget = 128u * 1024 * 1024);
    ~RestorationCache();
    RestorationCache(const RestorationCache &) = delete;
    RestorationCache &operator=(const RestorationCache &) = delete;
    std::shared_ptr<const RestoredFrame>
    get(const std::string &asset, uint32_t container, int width, int height,
        std::span<const uint32_t> source, std::span<const uint8_t> mask,
        RestorationSettings settings, std::span<const RestorationNeighbor> neighbors = {});
    void setBudget(size_t);
    void clear();
    RestorationStats stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace dust
