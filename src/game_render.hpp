// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "game.hpp"
namespace dust {
struct GameHit {
    std::string kind, name;
    int left = 0, top = 0, right = 0, bottom = 0;
    // Opaque pixels within the clipped rectangle, for sprite-accurate picking.
    // Empty for authored rectangular stage regions.
    Bytes mask;
};
struct GameFrame {
    int width = 512, height = 384;
    std::vector<uint32_t> rgb;
    std::vector<GameHit> hits; // Back-to-front; pick in reverse order.
    std::vector<GameHost::TextLabel> labels;
};
// An optional authored viewpoint supports read-only inspection. It does not
// move the host, advance scripts or use the live movement interpolation.
GameFrame renderGame(const GameHost &, RestorationCache * = nullptr, RestorationSettings = {},
                     std::optional<Standpoint> viewpoint = {});
const GameHit *hitGame(const GameFrame &, int x, int y);
bool coversGameHit(const GameHit &, int x, int y);
} // namespace dust
