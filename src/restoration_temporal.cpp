// SPDX-License-Identifier: GPL-3.0-only
#include "restoration.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
namespace dust {
namespace {
struct Ycc {
    float y, cb, cr;
};
Ycc convert(uint32_t c) {
    const float r = float((c >> 16) & 255), g = float((c >> 8) & 255), b = float(c & 255),
                y = .299f * r + .587f * g + .114f * b;
    return {y, (b - y) * .564f, (r - y) * .713f};
}
uint32_t pack(Ycc c, uint32_t alpha) {
    const float r = c.y + c.cr / .713f, b = c.y + c.cb / .564f,
                g = (c.y - .299f * r - .114f * b) / .587f;
    const auto ch = [](float x) { return uint32_t(std::clamp(std::lround(x), 0l, 255l)); };
    return alpha | (ch(r) << 16) | (ch(g) << 8) | ch(b);
}
float gate(float low, float high, float x) {
    float t = std::clamp((x - low) / (high - low), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
struct Plane {
    int w, h;
    std::span<const uint8_t> mask;
    std::vector<Ycc> pixels;
    bool opaque(int x, int y) const {
        return x >= 0 && y >= 0 && x < w && y < h && (mask.empty() || mask[size_t(y) * w + x]);
    }
    bool interior(int x, int y) const {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (!opaque(x + dx, y + dy))
                    return false;
        return true;
    }
    const Ycc &at(int x, int y) const {
        return pixels[size_t(y) * w + x];
    }
};
Plane plane(int w, int h, std::span<const uint32_t> rgb, std::span<const uint8_t> mask) {
    Plane p{w, h, mask, {}};
    p.pixels.reserve(rgb.size());
    for (auto c : rgb)
        p.pixels.push_back(convert(c));
    return p;
}
struct Match {
    int dx = 0, dy = 0;
    float confidence = 0;
};
Match match(const Plane &a, const Plane &b, int x, int y, int offsetX, int offsetY) {
    float best = std::numeric_limits<float>::max(), second = best;
    int bx = 0, by = 0;
    float texture = 0;
    int textureCount = 0;
    for (int yy = y; yy < std::min(y + 8, a.h); yy += 2)
        for (int xx = x; xx < std::min(x + 8, a.w); xx += 2)
            if (a.interior(xx, yy)) {
                texture += std::abs(a.at(xx + 1, yy).y - a.at(xx - 1, yy).y);
                ++textureCount;
            }
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            float cost = 0;
            int valid = 0, total = 0;
            for (int yy = y; yy < std::min(y + 8, a.h); yy += 2)
                for (int xx = x; xx < std::min(x + 8, a.w); xx += 2) {
                    if (!a.interior(xx, yy))
                        continue;
                    ++total;
                    const int nx = xx + offsetX + dx, ny = yy + offsetY + dy;
                    if (!b.interior(nx, ny))
                        continue;
                    cost += std::abs(a.at(xx, yy).y - b.at(nx, ny).y);
                    cost += .35f * std::abs((a.at(xx + 1, yy).y - a.at(xx - 1, yy).y) -
                                            (b.at(nx + 1, ny).y - b.at(nx - 1, ny).y));
                    ++valid;
                }
            if (valid < 6 || valid * 10 < total * 9)
                continue;
            cost = cost / valid + .04f * float(std::abs(dx) + std::abs(dy));
            if (cost < best) {
                second = best;
                best = cost;
                bx = dx;
                by = dy;
            } else
                second = std::min(second, cost);
        }
    if (best >= 6 || !std::isfinite(second))
        return {};
    const float structured = textureCount ? gate(2.f, 12.f, texture / textureCount) : 0;
    // Flat patches have ambiguous motion; only an excellent zero-offset match
    // is safe there. Textured patches require a distinct minimum.
    const float uniqueness = gate(.1f, 1.f, second - best);
    float confidence = (1 - gate(1.5f, 6.f, best)) * ((1 - structured) + structured * uniqueness);
    if (!bx && !by)
        confidence = std::max(confidence, (1 - gate(.75f, 3.f, best)) * (1 - .5f * structured));
    if ((bx || by) && structured < .15f && uniqueness < .7f)
        confidence = 0;
    return {bx, by, confidence};
}
bool sameScene(const Plane &a, const Plane &b, int ox, int oy) {
    double y = 0, c = 0;
    int total = 0, valid = 0;
    for (int yy = 0; yy < a.h; yy += 4)
        for (int xx = 0; xx < a.w; xx += 4) {
            if (!a.opaque(xx, yy))
                continue;
            ++total;
            if (!b.opaque(xx + ox, yy + oy))
                continue;
            auto p = a.at(xx, yy), q = b.at(xx + ox, yy + oy);
            y += std::abs(p.y - q.y);
            c += std::abs(p.cb - q.cb) + std::abs(p.cr - q.cr);
            ++valid;
        }
    return valid >= 8 && valid * 4 >= total * 3 && y / valid < 12 && c / valid < 28;
}
} // namespace
void restoreTemporal(RestoredFrame &out, std::span<const uint32_t> source,
                     std::span<const uint8_t> mask, std::span<const RestorationNeighbor> neighbors,
                     RestorationSettings settings) {
    if (!settings.enabled || !settings.temporal || settings.temporalStrength <= 0 ||
        neighbors.size() != 2)
        return;
    const auto &n0 = neighbors[0], &n1 = neighbors[1];
    if (!n0.continuous || !n1.continuous || n0.identity == n1.identity || n0.distance < 1 ||
        n1.distance < 1 || n0.distance > 3 || n1.distance > 3)
        return;
    const auto samePixels = [](std::span<const uint32_t> a, std::span<const uint32_t> b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
    };
    // Different container IDs can still contain a repeated scan. Such copies
    // are not independent temporal evidence.
    if ((n0.width == n1.width && n0.height == n1.height && samePixels(n0.rgb, n1.rgb)) ||
        (n0.width == out.width && n0.height == out.height && samePixels(n0.rgb, source)) ||
        (n1.width == out.width && n1.height == out.height && samePixels(n1.rgb, source)))
        return;
    const auto current = plane(out.width, out.height, source, mask);
    const std::array<Plane, 2> adjacent{plane(n0.width, n0.height, n0.rgb, n0.mask),
                                        plane(n1.width, n1.height, n1.rgb, n1.mask)};
    for (int k = 0; k < 2; ++k)
        if (!sameScene(current, adjacent[k], neighbors[k].offsetX, neighbors[k].offsetY))
            return;
    const float strength = std::clamp(settings.temporalStrength, 0, 50) / 100.f;
    for (int y = 0; y < out.height; y += 8)
        for (int x = 0; x < out.width; x += 8) {
            std::array<Match, 2> matches{};
            bool accepted = true;
            for (int k = 0; k < 2; ++k) {
                const auto &n = neighbors[k];
                const auto &p = adjacent[k];
                matches[k] = match(current, p, x, y, n.offsetX, n.offsetY);
                const auto &m = matches[k];
                if (m.confidence <= .1f) {
                    accepted = false;
                    break;
                }
                const auto reverse = match(p, current, x + n.offsetX + m.dx, y + n.offsetY + m.dy,
                                           -n.offsetX, -n.offsetY);
                if (reverse.confidence <= .1f || std::abs(m.dx + reverse.dx) > 0 ||
                    std::abs(m.dy + reverse.dy) > 0) {
                    accepted = false;
                    break;
                }
            }
            if (!accepted) {
                ++out.rejectedTiles;
                continue;
            }
            ++out.temporalTiles;
            for (int yy = y; yy < std::min(y + 8, out.height); ++yy)
                for (int xx = x; xx < std::min(x + 8, out.width); ++xx) {
                    if (!current.interior(xx, yy))
                        continue;
                    std::array<Ycc, 2> sample{};
                    bool valid = true;
                    for (int k = 0; k < 2; ++k) {
                        int nx = xx + neighbors[k].offsetX + matches[k].dx,
                            ny = yy + neighbors[k].offsetY + matches[k].dy;
                        if (!adjacent[k].interior(nx, ny)) {
                            valid = false;
                            break;
                        }
                        sample[k] = adjacent[k].at(nx, ny);
                    }
                    if (!valid)
                        continue;
                    const auto c = current.at(xx, yy), a = sample[0], b = sample[1];
                    const float lumaDifference =
                        std::max({std::abs(c.y - a.y), std::abs(c.y - b.y), std::abs(a.y - b.y)});
                    const float chromaAgreement = std::abs(a.cb - b.cb) + std::abs(a.cr - b.cr);
                    const float cb = (a.cb + b.cb) * .5f, cr = (a.cr + b.cr) * .5f;
                    const float residual = std::abs(cb - c.cb) + std::abs(cr - c.cr);
                    float mean = 0, square = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            float v = current.at(xx + dx, yy + dy).y;
                            mean += v;
                            square += v * v;
                        }
                    const float texture =
                        gate(5.f, 20.f, std::sqrt(std::max(0.f, square / 9 - mean * mean / 81)));
                    const float confidence =
                        std::min(matches[0].confidence, matches[1].confidence) *
                        (1 - gate(2.f, 7.f, lumaDifference)) *
                        (1 - gate(5.f, 16.f, chromaAgreement)) * (1 - gate(10.f, 24.f, residual)) *
                        (1 - .9f * texture) * gate(.8f, 3.f, residual);
                    if (confidence < .1f)
                        continue;
                    const size_t i = size_t(yy) * out.width + xx;
                    auto cleaned = convert(out.rgb[i]);
                    // Preserve spatial luminance. Only temporally inconsistent chroma is
                    // blended; original motion, alpha and fine luminance detail remain.
                    const float distance = std::sqrt(float(std::max(n0.distance, n1.distance)));
                    const float deltaCb = (cb - cleaned.cb) * strength * confidence / distance;
                    const float deltaCr = (cr - cleaned.cr) * strength * confidence / distance;
                    // Sub-level corrections can toggle RGB rounding between poses
                    // without a useful restoration benefit. Leave those pixels exact.
                    if (deltaCb * deltaCb + deltaCr * deltaCr < 1.f)
                        continue;
                    cleaned.cb += deltaCb;
                    cleaned.cr += deltaCr;
                    const auto pixel = pack(cleaned, out.rgb[i] & 0xff000000);
                    if (pixel != out.rgb[i]) {
                        out.rgb[i] = pixel;
                        ++out.temporalPixels;
                    }
                }
        }
}
} // namespace dust
