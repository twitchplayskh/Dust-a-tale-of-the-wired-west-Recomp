// SPDX-License-Identifier: GPL-3.0-only
// Geometry and resource selections informed by pinned dreamREfactory's
// geometry.ts, actors.ts, props.ts and sprite-scaler.ts. Integer destination
// sizing/anchors and depth tests are native. World sprite run-edge scaling is
// ported separately and verified against the pinned upstream emulator corpus.
#include "game_render.hpp"
#include "scaler.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace dust {
namespace {
constexpr double pi = 3.14159265358979323846;
std::string lower(std::string s) {
    for (auto &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
int heading(int facing) {
    constexpr int h[]{0, 192, 64, 0, 128};
    if (facing < 1 || facing > 4)
        throw std::runtime_error("invalid camera facing");
    return h[facing];
}
int sine(int deg) {
    return int(std::round(std::sin((deg & 255) * 2 * pi / 256) * 16384));
}
int cosine(int deg) {
    return int(std::round(std::cos((deg & 255) * 2 * pi / 256) * 16384));
}
int fix14(int value) {
    return value / 16384;
}
int bearing(int dx, int dy) {
    // The engine's 16-bit octant table uses truncating atan ratios, not a
    // rounded full atan2. Preserve its quadrant and exact-axis conventions.
    dx = int16_t(dx);
    dy = int16_t(dy);
    if (!dx && !dy)
        return 0;
    const int ax = std::abs(dx), ay = std::abs(dy);
    if (!dy)
        return dx > 0 ? 0 : 128;
    if (!dx)
        return dy > 0 ? 64 : 192;
    const int small = std::min(ax, ay), large = std::max(ax, ay);
    int octant = int(std::floor(std::atan(double((small << 9) / large) / 512) * 128 / pi));
    int angle = ax >= ay ? octant : 64 - octant;
    if (dx < 0)
        angle = 128 - angle;
    if (dy < 0)
        angle = 256 - angle;
    return angle & 255;
}
int angleApart(int a, int b) {
    const int d = ((a - b) % 256 + 256) % 256;
    return std::min(d, 256 - d);
}
const ContextEntry *entryFor(const ResourceContexts &resource, const GameHost::Entity &e) {
    const auto colon = e.group.find(':');
    return resource.find(colon == std::string::npos ? e.name : e.group.substr(colon + 1));
}
struct Camera {
    int x, y, z, degree, cx, cy, clipW, clipH, groundBias;
};
struct Projected {
    int x, y, depth;
};
Projected project(const Camera &c, const GameHost::Entity &e) {
    const int dx = e.x - c.x, dy = e.y - c.y;
    const int s = sine(c.degree), co = cosine(c.degree);
    const int depth = fix14(dy * s + dx * co);
    if (depth <= 0)
        return {0, 0, depth};
    const int lateral = fix14(dy * co - dx * s);
    return {c.cx + lateral * 310 / depth, c.cy - (e.z - c.z) * 310 / depth, depth};
}
const ContextFrame *actorFrame(const ContextState &state, const GameHost::Entity &entity,
                               const Camera *camera) {
    if (state.frames.empty())
        return nullptr;
    int step = 0;
    if (!state.playOrder.empty()) {
        const auto i = size_t(std::max(0, entity.poseStep)) % state.playOrder.size();
        step = state.playOrder[i];
    }
    const int relative =
        camera ? (entity.degree - bearing(camera->x - entity.x, camera->y - entity.y)) & 255
               : entity.degree & 255;
    const ContextFrame *best = nullptr;
    int bestAngle = 257;
    for (const auto &frame : state.frames)
        if (frame.step == step) {
            const int distance = angleApart(frame.angle, relative);
            if (distance < bestAngle) {
                best = &frame;
                bestAngle = distance;
            }
        }
    return best;
}
const ContextFrame *propFrame(const ContextState &state, const GameHost::Entity &entity) {
    if (state.frames.empty())
        return nullptr;
    // Screen props such as avatar use small stored degree values as selectors.
    // Comparing the authored degree values also covers rotational views.
    const ContextFrame *best = &state.frames.front();
    int bestDistance = std::numeric_limits<int>::max();
    for (const auto &frame : state.frames) {
        const int distance = angleApart(frame.angle, entity.degree);
        if (distance < bestDistance) {
            best = &frame;
            bestDistance = distance;
        }
    }
    std::vector<const ContextFrame *> variant;
    for (const auto &frame : state.frames)
        if (frame.angle == best->angle)
            variant.push_back(&frame);
    if (variant.size() > 1) {
        bool valid = state.playOrder.size() > 1 &&
                     std::all_of(state.playOrder.begin(), state.playOrder.end(),
                                 [&](int i) { return i >= 0 && size_t(i) < variant.size(); });
        const auto step = size_t(std::max(0, entity.poseStep));
        return valid ? variant[size_t(state.playOrder[step % state.playOrder.size()])]
                     : variant[step % variant.size()];
    }
    return best;
}
struct Draw {
    const ResourceContexts *resource;
    const ContextFrame *frame;
    const GameHost::Entity *entity;
    bool actor, world;
    int anchorX, anchorY, depth;
};
void drawSprite(GameFrame &out, const Draw &draw, const Camera *camera,
                const FrameBuffer *sceneDepth, const std::array<uint32_t, 256> &palette,
                RestorationCache *cache, RestorationSettings settings) {
    const auto &e = *draw.entity;
    auto sprite = Sprite::decode(draw.resource->file.at(draw.frame->spriteRef));
    int w = sprite.w, h = sprite.h;
    if (draw.world) {
        if (draw.depth < 32 || e.scale <= 0 || !camera)
            return;
        const int biased = std::max(0, draw.depth - e.zclip + camera->groundBias);
        if (biased > 1536)
            return;
        const int scale = e.scale * draw.frame->scale / 1000;
        w = scale * sprite.w / draw.depth;
        h = scale * sprite.h / draw.depth;
    }
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768)
        return;
    const int left = draw.anchorX - sprite.ax * w / sprite.w;
    const int top = draw.anchorY - sprite.ay * h / sprite.h;
    const int clipW = draw.world ? camera->clipW : out.width;
    const int clipH = draw.world ? camera->clipH : out.height;
    GameHit hit{draw.actor ? "actor" : "prop",
                e.name,
                std::max(0, left),
                std::max(0, top),
                std::min(clipW, left + w),
                std::min(clipH, top + h)};
    if (hit.left >= hit.right || hit.top >= hit.bottom)
        return;
    std::shared_ptr<const RestoredFrame> cleaned;
    if (draw.actor && cache && settings.enabled) {
        std::vector<uint32_t> source(sprite.pixels.size());
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = palette[sprite.pixels[i]];
        cleaned = cache->get("cast:" + draw.resource->sourceName, draw.frame->spriteRef, sprite.w,
                             sprite.h, source, sprite.mask, settings);
    }
    const int hitWidth = hit.right - hit.left;
    hit.mask.resize(size_t(hitWidth) * (hit.bottom - hit.top));
    bool any = false;
    const int level = draw.world ? std::max(0, draw.depth - e.zclip + camera->groundBias) / 64 : 0;
    auto drawable = [&](int x, int y) {
        if (!draw.world || !sceneDepth || !sceneDepth->hasDepth)
            return true;
        int sx = x * sceneDepth->w / camera->clipW, sy = y * sceneDepth->h / camera->clipH;
        return sceneDepth->depth.at(size_t(sy) * sceneDepth->w + sx) >= level;
    };
    if (draw.world) {
        drawScaledSprite(sprite, draw.anchorX, draw.anchorY, w, h, false,
                         {0, 0, clipH, clipW, drawable,
                          [&](int x, int y, uint8_t c) {
                              out.rgb.at(size_t(y) * out.width + x) = palette[c];
                              any = true;
                          },
                          [&](int x, int y, size_t source) {
                              if (cleaned)
                                  out.rgb.at(size_t(y) * out.width + x) = cleaned->rgb.at(source);
                          }});
        if (any)
            for (int y = hit.top; y < hit.bottom; ++y)
                for (int x = hit.left; x < hit.right; ++x)
                    if (drawable(x, y) && scaledSpriteCovers(sprite, left, top, w, h, x, y))
                        hit.mask[size_t(y - hit.top) * hitWidth + x - hit.left] = 1;
        if (any)
            out.hits.push_back(std::move(hit));
        return;
    }
    for (int y = hit.top; y < hit.bottom; ++y)
        for (int x = hit.left; x < hit.right; ++x) {
            const int sx = (x - left) * sprite.w / w, sy = (y - top) * sprite.h / h;
            const auto source = size_t(sy) * sprite.w + sx;
            if (!sprite.mask[source])
                continue;
            if (!drawable(x, y))
                continue;
            out.rgb[size_t(y) * out.width + x] =
                cleaned ? cleaned->rgb[source] : palette[sprite.pixels[source]];
            hit.mask[size_t(y - hit.top) * hitWidth + (x - hit.left)] = 1;
            any = true;
        }
    if (any)
        out.hits.push_back(std::move(hit));
}
} // namespace
GameFrame renderGame(const GameHost &host, RestorationCache *cache, RestorationSettings settings,
                     std::optional<Standpoint> viewpoint) {
    GameFrame out;
    out.rgb.resize(size_t(out.width) * out.height, 0);
    out.labels = host.textLabels();
    const auto *stage = host.stageResource();
    const auto *flat = stage ? stage->find(host.flatName()) : nullptr;
    const auto *set = host.sceneSet();
    const bool showSet = set && host.setVisible(), showStage = stage && flat && host.stageVisible();
    std::array<uint32_t, 256> palette{};
    if (showSet)
        palette = set->palettes[0];
    else if (stage)
        palette = stage->palette;
    if (showStage && showSet)
        std::copy(stage->palette.begin() + 128, stage->palette.end(), palette.begin() + 128);
    host.applyPalette(palette);
    if (showStage && flat->frameRef) {
        FrameBuffer image;
        image.decode(stage->file.at(flat->frameRef));
        for (int y = 0; y < std::min(image.h, out.height); ++y)
            for (int x = 0; x < std::min(image.w, out.width); ++x)
                out.rgb[size_t(y) * out.width + x] = palette[image.pixels[size_t(y) * image.w + x]];
        for (const auto &r : flat->regions)
            out.hits.push_back({"button", lower(r.name), r.left, r.top, r.right, r.bottom, {}});
    }
    std::optional<FrameBuffer> background;
    std::optional<Camera> camera;
    if (showSet) {
        const auto p = viewpoint.value_or(host.standpoint());
        const int degree = heading(p.facing);
        background = !viewpoint && host.motionFrame() ? *host.motionFrame() : set->standing(p);
        for (int y = 0; y < set->viewH; ++y)
            for (int x = 0; x < set->viewW; ++x)
                out.rgb[size_t(y) * out.width + x] =
                    palette[background
                                ->pixels[size_t(y * background->h / set->viewH) * background->w +
                                         x * background->w / set->viewW]];
        camera = Camera{p.x * 256 + 128 - fix14(set->cameraSetback * cosine(degree)),
                        p.z * 256 + 128 - fix14(set->cameraSetback * sine(degree)),
                        set->eyeHeight,
                        degree,
                        set->viewW / 2,
                        set->viewH / 2,
                        set->viewW,
                        set->viewH,
                        128 - set->cameraSetback};
        if (auto motion = host.motionCamera(); motion && !viewpoint) {
            camera->x = motion->x;
            camera->y = motion->y;
            camera->degree = motion->degree;
        }
    }
    std::vector<Draw> world, screen;
    const auto gather = [&](const auto &entities, bool actor) {
        for (const auto &[name, e] : entities) {
            if (!e.visible)
                continue;
            const auto *resource = host.resourceFor(e, actor);
            if (!resource)
                throw std::runtime_error("visible entity resource missing: " + name);
            const auto *entry = entryFor(*resource, e);
            if (!entry)
                throw std::runtime_error("visible entity template missing: " + name);
            const auto *state = entry->state(e.pose);
            if (!state && !e.pose.empty())
                throw std::runtime_error("visible entity pose missing: " + name + "/" + e.pose);
            if (!state && !entry->states.empty())
                state = &entry->states.front();
            if (!state)
                continue;
            const bool inWorld = e.is3d;
            if (inWorld && (!showSet || lower(e.set) != lower(set->name)))
                continue;
            if (!inWorld && !showStage)
                continue;
            const auto *frame =
                actor ? actorFrame(*state, e, inWorld ? &*camera : nullptr) : propFrame(*state, e);
            if (!frame)
                continue;
            if (inWorld) {
                const auto p = project(*camera, e);
                if (p.depth > 0)
                    world.push_back({resource, frame, &e, actor, true, p.x, p.y, p.depth});
            } else
                screen.push_back({resource, frame, &e, actor, false, e.x, e.y, e.distance});
        }
    };
    gather(host.actors(), true);
    gather(host.props(), false);
    std::stable_sort(world.begin(), world.end(),
                     [](const Draw &a, const Draw &b) { return a.depth > b.depth; });
    std::stable_sort(screen.begin(), screen.end(),
                     [](const Draw &a, const Draw &b) { return a.depth > b.depth; });
    for (const auto &d : world)
        drawSprite(out, d, &*camera, &*background, palette, cache, settings);
    for (const auto &d : screen)
        drawSprite(out, d, nullptr, nullptr, palette, cache, settings);
    return out;
}
const GameHit *hitGame(const GameFrame &frame, int x, int y) {
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height)
        return nullptr;
    for (auto it = frame.hits.rbegin(); it != frame.hits.rend(); ++it) {
        if (coversGameHit(*it, x, y))
            return &*it;
    }
    return nullptr;
}
bool coversGameHit(const GameHit &hit, int x, int y) {
    if (x < hit.left || x >= hit.right || y < hit.top || y >= hit.bottom)
        return false;
    return hit.mask.empty() ||
           hit.mask[size_t(y - hit.top) * (hit.right - hit.left) + (x - hit.left)] != 0;
}
} // namespace dust
