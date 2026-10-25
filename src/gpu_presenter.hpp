// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
namespace dust {
// GPU presentation of the composed canvas, with original pixel sampling.
// Character cleanup is performed by the source-frame restoration cache.
class GpuPresenter {
  public:
    GpuPresenter();
    ~GpuPresenter();
    void present(HWND, std::span<const uint32_t> image, std::span<const uint32_t> overlay,
                 int sourceWidth, int sourceHeight, RECT viewport, int width, int height,
                 bool capture = false);
    void reset();
    std::vector<uint32_t> readback(int &width, int &height);
    uint64_t frames() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace dust
