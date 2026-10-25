// SPDX-License-Identifier: GPL-3.0-only
// DF1 ballistic scheduling, adapted from pinned GPL dreamREfactory balls.ts.
#include "game.hpp"
#include <algorithm>
#include <cmath>
namespace dust {
void GameHost::updateBalls() {
    if (servicingBalls)
        return;
    servicingBalls = true;
    const auto reset = std::unique_ptr<GameHost, std::function<void(GameHost *)>>(
        this, [this](GameHost *) { servicingBalls = false; });
    auto blocked = [&](int x, int y) {
        return set && std::any_of(set->cells.begin(), set->cells.end(), [&](const auto &c) {
                   return c.x == x && c.z == y && c.building;
               });
    };
    auto hits = [](const Ball &from, const Ball &to, int x, int y, int z, int rz, int rxy) {
        if (rz <= 0 || rxy <= 0)
            return false;
        auto miss = [](int a, int b, int center, int radius) {
            return (a < center - radius && b < center - radius) ||
                   (a > center + radius && b > center + radius);
        };
        return !miss(from.x, to.x, x, rxy) && !miss(from.y, to.y, y, rxy) &&
               !miss(from.z, to.z, z, rz);
    };
    for (size_t slot = 0; slot < balls.size(); ++slot) {
        if (!balls[slot] || balls[slot]->pause)
            continue;
        auto b = *balls[slot];
        const auto from = b;
        const auto name = b.name;
        auto &prop = entity(name, false);
        std::string reason;
        b.frames = int16_t(b.frames - 1);
        if (b.frames < 0)
            reason = "frames";
        else {
            b.x = int16_t(b.x + b.vx);
            b.y = int16_t(b.y + b.vy);
            b.z = int16_t(b.z + b.vz);
            b.tx = b.x >> 8;
            b.ty = b.y >> 8;
            if (b.gravity)
                b.vz = int16_t(b.vz - 2);
            if (b.bounce) {
                if (b.z <= 0) {
                    b.z = 0;
                    b.vz = int16_t(-b.vz / 2);
                    if (std::abs(b.vz) <= 2)
                        b.vz = 0;
                }
                if (blocked(b.tx, b.ty)) {
                    if (from.tx != b.tx) {
                        b.x = int16_t((from.tx + b.tx + 1) * 128);
                        b.tx = b.x >> 8;
                        b.vx = int16_t(-b.vx / 2);
                    }
                    if (from.ty != b.ty) {
                        b.y = int16_t((from.ty + b.ty + 1) * 128);
                        b.ty = b.y >> 8;
                        b.vy = int16_t(-b.vy / 2);
                    }
                }
            } else if (b.z <= 0 || blocked(b.tx, b.ty))
                reason = "building";
            if (reason.empty() && set)
                for (const auto &actorName : actorOrder) {
                    const auto &actor = actorState.at(actorName);
                    if (actor.visible && actor.is3d && actor.set == set->name &&
                        hits(from, b, actor.x, actor.y, actor.z, actor.hitW, actor.hitH)) {
                        reason = actorName;
                        break;
                    }
                }
            if (reason.empty() && hits(from, b, position.x * 256 + 128, position.z * 256 + 128, 0,
                                       playerHitbox[0], playerHitbox[1]))
                reason = "player";
            prop.x = b.x;
            prop.y = b.y;
            prop.z = b.z;
            prop.degree = b.degree;
            prop.is3d = true;
            prop.starPending = false;
        }
        if (reason.empty())
            balls[slot] = b;
        else {
            balls[slot].reset();
            call(propContexts.at(name), "endball", {ScriptValue::string(reason)}, name, name);
        }
    }
}
} // namespace dust
