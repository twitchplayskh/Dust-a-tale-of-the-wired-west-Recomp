// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "dust.hpp"
namespace dust {
enum class ContextKind { Stage, Cast, Shop };
struct ContextRegion {
    std::string name;
    uint32_t scriptRef = 0;
    int top = 0, left = 0, bottom = 0, right = 0;
};
struct ContextFrame {
    uint32_t spriteRef = 0;
    int step = 0, direction = 0, angle = 0, scale = 0;
    int width = 0, height = 0, offsetX = 0, offsetY = 0;
};
struct ContextState {
    std::string name;
    uint32_t logicRef = 0;
    // Authored zero-based step indexes, including repetitions; no invented timing.
    std::vector<int> playOrder;
    std::vector<ContextFrame> frames;
};
struct ContextEntry {
    std::string name;
    uint32_t scriptRef = 0, logicRef = 0, frameRef = 0;
    int width = 0, height = 0;
    std::vector<ContextRegion> regions;
    std::vector<ContextState> states; // Cast poses or shop views.
    // Preserve the uninterpreted logic prefix; this is not actor runtime state.
    Bytes logicPrefix;
    const ContextState *state(const std::string &) const;
};
struct ResourceContexts {
    ContextKind kind;
    File file;
    std::string name;
    uint32_t mainScriptRef = 0;
    std::string sourceName; // Host filename stem; authored name may differ.
    int width = 0, height = 0;
    std::array<uint32_t, 256> palette{};
    std::vector<ContextEntry> entries;
    static ResourceContexts open(const std::filesystem::path &, ContextKind);
    const ContextEntry *find(const std::string &) const;
    View script(const std::string &) const;
    View main() const;
};
} // namespace dust
