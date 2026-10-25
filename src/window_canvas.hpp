// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <stdexcept>
namespace dust {
// Compose the complete client area before one screen copy, including borders.
class WindowCanvas {
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ original_ = nullptr;
    int width_ = 0, height_ = 0;

  public:
    WindowCanvas() = default;
    WindowCanvas(const WindowCanvas &) = delete;
    WindowCanvas &operator=(const WindowCanvas &) = delete;
    ~WindowCanvas() {
        if (original_)
            SelectObject(dc_, original_);
        if (bitmap_)
            DeleteObject(bitmap_);
        if (dc_)
            DeleteDC(dc_);
    }
    HDC prepare(HDC screen, int width, int height) {
        if (!dc_)
            dc_ = CreateCompatibleDC(screen);
        if (!dc_)
            throw std::runtime_error("cannot create window back buffer DC");
        if (width != width_ || height != height_) {
            auto next = CreateCompatibleBitmap(screen, width, height);
            if (!next)
                throw std::runtime_error("cannot allocate window back buffer");
            auto previous = SelectObject(dc_, next);
            if (!previous || previous == HGDI_ERROR) {
                DeleteObject(next);
                throw std::runtime_error("cannot select window back buffer");
            }
            if (!original_)
                original_ = previous;
            if (bitmap_)
                DeleteObject(bitmap_);
            bitmap_ = next;
            width_ = width;
            height_ = height;
        }
        return dc_;
    }
};
} // namespace dust
