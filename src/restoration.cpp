// SPDX-License-Identifier: GPL-3.0-only
#include "restoration.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <stdexcept>
#include <unordered_map>
namespace dust {
namespace {
constexpr uint32_t algorithmRevision = 4;
struct Color {
    float y, cb, cr;
};
Color color(uint32_t rgb) {
    const float r = float((rgb >> 16) & 255), g = float((rgb >> 8) & 255), b = float(rgb & 255);
    const float y = .299f * r + .587f * g + .114f * b;
    return {y, (b - y) * .564f, (r - y) * .713f};
}
uint32_t pack(Color c, uint32_t alpha) {
    const float r = c.y + c.cr / .713f, b = c.y + c.cb / .564f;
    const float g = (c.y - .299f * r - .114f * b) / .587f;
    const auto channel = [](float n) { return uint32_t(std::clamp(std::lround(n), 0l, 255l)); };
    return alpha | (channel(r) << 16) | (channel(g) << 8) | channel(b);
}
RestorationSettings normalize(RestorationSettings s) {
    s.deblock = std::clamp(s.deblock, 0, 100);
    s.chroma = std::clamp(s.chroma, 0, 100);
    s.temporalStrength = std::clamp(s.temporalStrength, 0, 50);
    s.detail = std::clamp(s.detail, 0, 35); // Deliberately cap sharpening strength.
    return s;
}
float ramp(float low, float high, float value) {
    const float t = std::clamp((value - low) / (high - low), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
// Separable rolling sums: linear time regardless of radius. Channel0 is masked
// sample count, so neither guidance nor coefficient averaging reads transparency.
template <size_t N>
std::vector<std::array<float, N>> boxSum(const std::vector<std::array<float, N>> &input, int w,
                                         int h, int radius) {
    std::vector<std::array<float, N>> horizontal(input.size()), output(input.size());
    for (int y = 0; y < h; ++y) {
        std::array<double, N> sum{};
        for (int x = 0; x <= std::min(radius, w - 1); ++x)
            for (size_t c = 0; c < N; ++c)
                sum[c] += input[size_t(y) * w + x][c];
        for (int x = 0; x < w; ++x) {
            for (size_t c = 0; c < N; ++c)
                horizontal[size_t(y) * w + x][c] = float(sum[c]);
            for (size_t c = 0; c < N; ++c) {
                if (x >= radius)
                    sum[c] -= input[size_t(y) * w + x - radius][c];
                if (x + radius + 1 < w)
                    sum[c] += input[size_t(y) * w + x + radius + 1][c];
            }
        }
    }
    std::vector<std::array<double, N>> columns(size_t(w), std::array<double, N>{});
    for (int y = 0; y <= std::min(radius, h - 1); ++y)
        for (int x = 0; x < w; ++x)
            for (size_t c = 0; c < N; ++c)
                columns[x][c] += horizontal[size_t(y) * w + x][c];
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            for (size_t c = 0; c < N; ++c)
                output[size_t(y) * w + x][c] = float(columns[x][c]);
            for (size_t c = 0; c < N; ++c) {
                if (y >= radius)
                    columns[x][c] -= horizontal[size_t(y - radius) * w + x][c];
                if (y + radius + 1 < h)
                    columns[x][c] += horizontal[size_t(y + radius + 1) * w + x][c];
            }
        }
    return output;
}
struct Analysis {
    float texture = 0, damage = 0, colourEdge = 0, lumaEdge = 0;
    float cb = 0, cr = 0, coverage = 1;
    float coarseCb = 0, coarseCr = 0, scaleDamage = 0;
};
std::vector<Analysis> analyze(const std::vector<Color> &source, std::span<const uint8_t> mask,
                              int w, int h, bool multiscale) {
    const auto opaque = [&](size_t i) { return mask.empty() || mask[i] != 0; };
    std::vector<std::array<float, 9>> input(source.size());
    for (size_t i = 0; i < source.size(); ++i)
        if (opaque(i)) {
            const auto c = source[i];
            input[i] = {1,           c.y,         c.y * c.y,  c.cb,      c.cr,
                        c.cb * c.cb, c.cr * c.cr, c.y * c.cb, c.y * c.cr};
        }
    const auto moments = boxSum(input, w, h, 2);
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    std::vector<std::array<float, 9>> coarse;
    if (multiscale) {
        std::vector<std::array<float, 9>> half(size_t(cw) * ch);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (size_t c = 0; c < 9; ++c)
                    half[size_t(y / 2) * cw + x / 2][c] += input[size_t(y) * w + x][c];
        coarse = boxSum(half, cw, ch, 2);
    }
    std::vector<std::array<float, 5>> coefficients(source.size());
    std::vector<Analysis> result(source.size());
    for (size_t i = 0; i < source.size(); ++i)
        if (opaque(i)) {
            const auto &m = moments[i];
            const float inverse = 1.f / std::max(1.f, m[0]);
            const float y = m[1] * inverse, cb = m[3] * inverse, cr = m[4] * inverse;
            const float variance = std::max(0.f, m[2] * inverse - y * y);
            const float acb = (m[7] * inverse - y * cb) / (variance + 36.f);
            const float acr = (m[8] * inverse - y * cr) / (variance + 36.f);
            coefficients[i] = {1, acb, cb - acb * y, acr, cr - acr * y};
            result[i].texture = ramp(6.f, 26.f, std::sqrt(variance));
            const float chromaVariance =
                std::max(0.f, m[5] * inverse - cb * cb) + std::max(0.f, m[6] * inverse - cr * cr);
            const float covCb = m[7] * inverse - y * cb, covCr = m[8] * inverse - y * cr;
            const float unexplained =
                std::max(0.f, chromaVariance - (covCb * covCb + covCr * covCr) / (variance + 36.f));
            result[i].damage = ramp(.8f, 7.f, std::sqrt(multiscale ? unexplained : chromaVariance));
            if (multiscale) {
                const int px = int(i % w), py = int(i / w);
                const float fx = (px - .5f) * .5f, fy = (py - .5f) * .5f;
                const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
                const float tx = fx - x0, ty = fy - y0;
                std::array<float, 9> scale{};
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const auto &cell = coarse[size_t(std::clamp(y0 + dy, 0, ch - 1)) * cw +
                                                  std::clamp(x0 + dx, 0, cw - 1)];
                        const float weight =
                            (dx ? tx : 1 - tx) * (dy ? ty : 1 - ty) / std::max(1.f, cell[0]);
                        for (size_t c = 1; c < 9; ++c)
                            scale[c] += cell[c] * weight;
                    }
                const float vy = std::max(0.f, scale[2] - scale[1] * scale[1]);
                const float cy = scale[7] - scale[1] * scale[3],
                            ry = scale[8] - scale[1] * scale[4];
                const float residual =
                    std::max(0.f, scale[5] - scale[3] * scale[3] + scale[6] - scale[4] * scale[4] -
                                      (cy * cy + ry * ry) / (vy + 36));
                result[i].coarseCb = scale[3] + cy / (vy + 36) * (source[i].y - scale[1]);
                result[i].coarseCr = scale[4] + ry / (vy + 36) * (source[i].y - scale[1]);
                result[i].scaleDamage =
                    ramp(2.f, 9.f, std::sqrt(residual)) * (1 - ramp(4.f, 14.f, std::sqrt(vy)));
            }
        }
    // Keep a one-pixel authored border exact and taper the next pixel. This
    // protects small eye/hair cutouts and reduces joins between animated layers.
    std::vector<uint8_t> distance(source.size(), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            if (!opaque(i))
                continue;
            int d = std::min({3, x + 1, y + 1, w - x, h - y});
            if (x)
                d = std::min(d, int(distance[i - 1]) + 1);
            if (y)
                d = std::min(d, int(distance[i - w]) + 1);
            distance[i] = uint8_t(d);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            const size_t i = size_t(y) * w + x;
            if (!opaque(i))
                continue;
            int d = distance[i];
            if (x + 1 < w)
                d = std::min(d, int(distance[i + 1]) + 1);
            if (y + 1 < h)
                d = std::min(d, int(distance[i + w]) + 1);
            distance[i] = uint8_t(d);
            result[i].coverage = ramp(1.f, 3.f, float(d));
        }
    const auto averaged = boxSum(coefficients, w, h, 2);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            if (!opaque(i))
                continue;
            if (result[i].damage == 0 && result[i].texture == 0) {
                result[i].cb = source[i].cb;
                result[i].cr = source[i].cr;
                continue;
            }
            const auto &m = averaged[i];
            const float inverse = 1.f / std::max(1.f, m[0]);
            result[i].cb = (m[1] * source[i].y + m[2]) * inverse;
            result[i].cr = (m[3] * source[i].y + m[4]) * inverse;
            const auto sample = [&](int dx, int dy) {
                const size_t at =
                    size_t(std::clamp(y + dy, 0, h - 1)) * w + std::clamp(x + dx, 0, w - 1);
                return opaque(at) ? source[at] : source[i];
            };
            float colourEdge = 0, lumaEdge = 0;
            for (const auto direction :
                 std::array<std::array<int, 2>, 4>{{{1, 0}, {0, 1}, {1, 1}, {1, -1}}}) {
                auto a = sample(direction[0], direction[1]),
                     b = sample(2 * direction[0], 2 * direction[1]);
                auto c = sample(-direction[0], -direction[1]),
                     d = sample(-2 * direction[0], -2 * direction[1]);
                colourEdge = std::max(colourEdge, .5f * (std::abs(a.cb + b.cb - c.cb - d.cb) +
                                                         std::abs(a.cr + b.cr - c.cr - d.cr)));
                lumaEdge = std::max(lumaEdge, .5f * std::abs(a.y + b.y - c.y - d.y));
            }
            result[i].colourEdge = ramp(8.f, 28.f, colourEdge);
            result[i].lumaEdge = ramp(4.f, 24.f, lumaEdge);
        }
    return result;
}
std::shared_ptr<const RestoredFrame> restore(int w, int h, std::span<const uint32_t> source,
                                             std::span<const uint8_t> mask,
                                             RestorationSettings settings) {
    auto out = std::make_shared<RestoredFrame>();
    out->width = w;
    out->height = h;
    out->rgb.assign(source.begin(), source.end());
    if (!settings.enabled || (!settings.deblock && !settings.chroma && !settings.detail))
        return out;
    const auto opaque = [&](size_t i) { return mask.empty() || mask[i] != 0; };
    std::vector<Color> original(source.size()), clean(source.size());
    for (size_t i = 0; i < source.size(); ++i)
        original[i] = color(source[i]);
    clean = original;
    const auto analysis = analyze(original, mask, w, h, settings.multiscale);
    // Reduce only weak, flat-sided discontinuities on an 8-pixel block grid.
    // Natural high-contrast edges and ramps fail these tests and remain intact.
    if (settings.deblock) {
        std::vector<float> correction(source.size(), 0);
        const auto boundary = [&](size_t p0, size_t p1, size_t q0, size_t q1) {
            if (!opaque(p0) || !opaque(p1) || !opaque(q0) || !opaque(q1))
                return;
            const float delta = original[q0].y - original[p0].y;
            const float slope = std::max(std::abs(original[p0].y - original[p1].y),
                                         std::abs(original[q0].y - original[q1].y));
            if (std::abs(delta) < 1.5f || std::abs(delta) > 24 || slope > 6 ||
                std::abs(delta) <= 2 * slope + 1)
                return;
            // Do not erase an isoluminant colour boundary either.
            if (std::abs(original[q0].cb - original[p0].cb) +
                    std::abs(original[q0].cr - original[p0].cr) >
                20)
                return;
            const float evidence =
                (1 - ramp(2.f, 6.f, slope)) * (1 - ramp(14.f, 24.f, std::abs(delta)));
            const float protection =
                1 - .75f * std::max(analysis[p0].texture, analysis[q0].texture);
            const float amount = std::clamp(delta * .30f, -3.f, 3.f) * settings.deblock / 100.f *
                                 evidence * protection *
                                 std::min(analysis[p0].coverage, analysis[q0].coverage);
            correction[p0] += amount;
            correction[q0] -= amount;
        };
        for (int y = 0; y < h; ++y)
            for (int x = 8; x + 1 < w; x += 8) {
                const size_t q = size_t(y) * w + x;
                boundary(q - 1, q - 2, q, q + 1);
            }
        for (int y = 8; y + 1 < h; y += 8)
            for (int x = 0; x < w; ++x) {
                const size_t q = size_t(y) * w + x;
                boundary(q - w, q - 2 * w, q, q + w);
            }
        for (size_t i = 0; i < clean.size(); ++i)
            clean[i].y += std::clamp(correction[i], -3.f, 3.f);
    }
    // Palette quantization leaves isolated luminance speckles, not just block seams.
    // A masked median removes weak isolated outliers; a strict range gate protects
    // eyes, hair and silhouette edges. Skip the border to retain authored ramps.
    if (settings.deblock) {
        const auto blocked = clean;
        for (int y = 1; y + 1 < h; ++y)
            for (int x = 1; x + 1 < w; ++x) {
                const size_t at = size_t(y) * w + x;
                if (!opaque(at))
                    continue;
                std::array<float, 9> values{};
                size_t count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const size_t i = size_t(y + dy) * w + x + dx;
                        if (opaque(i))
                            values[count++] = blocked[i].y;
                    }
                if (count < 5)
                    continue;
                std::sort(values.begin(), values.begin() + count);
                const float difference = values[count / 2] - blocked[at].y;
                const float outlier = 1 - ramp(16.f, 28.f, std::abs(difference));
                const float protection = 1 - .90f * analysis[at].texture;
                clean[at].y += difference * .55f * settings.deblock / 100.f * outlier * protection *
                               analysis[at].coverage;
            }
    }
    const auto base = clean;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t at = size_t(y) * w + x;
            if (!opaque(at))
                continue;
            const auto center = base[at];
            const auto &guide = analysis[at];
            if (guide.coverage == 0)
                continue;
            if (guide.damage == 0 && guide.texture == 0 && center.y == original[at].y)
                continue;
            float cb = 2 * center.cb, cr = 2 * center.cr, cw = 2;
            float lowpass = 2 * center.y, yw = 2, lo = original[at].y, hi = lo;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((!dx && !dy) || x + dx < 0 || x + dx >= w || y + dy < 0 || y + dy >= h)
                        continue;
                    const size_t i = size_t(y + dy) * w + x + dx;
                    if (!opaque(i))
                        continue; // Never sample outside the authored silhouette.
                    lo = std::min(lo, original[i].y);
                    hi = std::max(hi, original[i].y);
                    const auto c = base[i];
                    const float edge = std::max(0.f, 1 - std::abs(c.y - center.y) / 40.f);
                    const float spatial = 1.f / float(1 + dx * dx + dy * dy);
                    const float weight = edge * edge * spatial;
                    lowpass += c.y * weight;
                    yw += weight;
                    const float difference =
                        std::abs(c.cb - center.cb) + std::abs(c.cr - center.cr);
                    const float chromaWeight = weight * std::max(0.f, 1 - difference / 96.f);
                    cb += c.cb * chromaWeight;
                    cr += c.cr * chromaWeight;
                    cw += chromaWeight;
                }
            auto result = center;
            // Retain Revised Balanced's bilateral estimate, blended with a masked
            // luminance-guided local linear fit. Smooth confidence gates avoid
            // per-frame mode changes, while coherent colour edges veto mixing.
            const float blend =
                std::clamp(settings.chroma / 100.f * (1.4f * guide.damage) *
                               (1 - .85f * guide.texture) * (1 - guide.colourEdge) * guide.coverage,
                           0.f, .85f);
            float targetCb = .35f * (cb / cw) + .65f * guide.cb;
            float targetCr = .35f * (cr / cw) + .65f * guide.cr;
            const float scaleBlend =
                .35f * guide.scaleDamage * (1 - guide.texture) * (1 - guide.colourEdge);
            targetCb += (guide.coarseCb - targetCb) * scaleBlend;
            targetCr += (guide.coarseCr - targetCr) * scaleBlend;
            result.cb += (targetCb - center.cb) * blend;
            result.cr += (targetCr - center.cr) * blend;
            const float detail = center.y - lowpass / yw;
            if (std::abs(detail) > .7f) {
                const float adjustment =
                    std::clamp(detail * settings.detail / 100.f * guide.texture * guide.lumaEdge *
                                   guide.coverage,
                               -1.25f, 1.25f);
                result.y = std::clamp(center.y + adjustment, lo, hi);
            }
            out->rgb[at] = pack(result, source[at] & 0xff000000);
        }
    return out;
}
struct Key {
    std::string asset;
    uint32_t container, revision = algorithmRevision;
    int width, height;
    uint64_t source, neighborhood;
    RestorationSettings settings;
    bool operator==(const Key &) const = default;
};
struct Hash {
    size_t operator()(const Key &k) const {
        return std::hash<std::string>{}(k.asset) ^ size_t(k.source) ^ (size_t(k.container) << 1) ^
               (size_t(k.settings.deblock) << 16) ^ (size_t(k.settings.chroma) << 24) ^
               (size_t(k.settings.detail) << 8) ^ size_t(k.neighborhood) ^
               (size_t(k.settings.temporalStrength) << 32) ^ (size_t(k.settings.multiscale) << 6) ^
               (size_t(k.settings.temporal) << 7);
    }
};
uint64_t fingerprint(std::span<const uint32_t> pixels, std::span<const uint8_t> mask) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto pixel : pixels)
        for (int shift : {0, 8, 16, 24})
            hash = (hash ^ uint8_t(pixel >> shift)) * 1099511628211ull;
    for (const auto byte : mask)
        hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}
} // namespace
struct RestorationCache::Impl {
    struct Entry {
        Key key;
        std::shared_ptr<const RestoredFrame> frame;
        size_t bytes;
    };
    std::list<Entry> lru;
    std::unordered_map<Key, std::list<Entry>::iterator, Hash> index;
    RestorationStats stats;
    void trim(size_t incoming = 0) {
        while (!lru.empty() && (incoming > stats.budget || stats.bytes > stats.budget - incoming)) {
            stats.bytes -= lru.back().bytes;
            index.erase(lru.back().key);
            lru.pop_back();
            ++stats.evictions;
        }
        stats.entries = index.size();
    }
};
RestorationCache::RestorationCache(size_t budget) : impl(std::make_unique<Impl>()) {
    impl->stats.budget = budget;
}
RestorationCache::~RestorationCache() = default;
std::shared_ptr<const RestoredFrame>
RestorationCache::get(const std::string &asset, uint32_t container, int w, int h,
                      std::span<const uint32_t> source, std::span<const uint8_t> mask,
                      RestorationSettings settings,
                      std::span<const RestorationNeighbor> neighbors) {
    if (w < 1 || h < 1 || w > 4096 || h > 4096 || source.size() != size_t(w) * h ||
        (!mask.empty() && mask.size() != source.size()))
        throw std::runtime_error("invalid restoration frame");
    settings = normalize(settings);
    uint64_t neighborhood = 0;
    if (settings.temporal && neighbors.size() == 2) {
        for (const auto &n : neighbors) {
            if (n.offsetX < -4096 || n.offsetX > 4096 || n.offsetY < -4096 || n.offsetY > 4096 ||
                n.width < 1 || n.height < 1 || n.width > 4096 || n.height > 4096 ||
                n.rgb.size() != size_t(n.width) * n.height ||
                (!n.mask.empty() && n.mask.size() != n.rgb.size()))
                throw std::runtime_error("invalid temporal neighbor");
            neighborhood = (neighborhood ^ fingerprint(n.rgb, n.mask)) * 1099511628211ull;
            for (int v : {int(n.identity), n.width, n.height, n.offsetX, n.offsetY, n.distance,
                          int(n.continuous)})
                neighborhood = (neighborhood ^ uint32_t(v)) * 1099511628211ull;
        }
    }
    Key key{asset,        container, algorithmRevision, w, h, fingerprint(source, mask),
            neighborhood, settings};
    if (const auto it = impl->index.find(key); it != impl->index.end()) {
        ++impl->stats.hits;
        impl->lru.splice(impl->lru.begin(), impl->lru, it->second);
        return it->second->frame;
    }
    ++impl->stats.misses;
    const bool hasTemporal = settings.temporal && neighbors.size() == 2 &&
                             neighbors[0].identity != container &&
                             neighbors[1].identity != container;
    std::shared_ptr<const RestoredFrame> frame;
    if (hasTemporal) {
        auto spatialSettings = settings;
        spatialSettings.temporal = false;
        frame = get(asset, container, w, h, source, mask, spatialSettings);
    } else
        frame = restore(w, h, source, mask, settings);
    if (hasTemporal) {
        auto temporal = std::make_shared<RestoredFrame>(*frame);
        restoreTemporal(*temporal, source, mask, neighbors, settings);
        frame = temporal;
        if (frame->temporalPixels) {
            ++impl->stats.temporalFrames;
            impl->stats.temporalPixels += frame->temporalPixels;
        }
    }
    const size_t bytes = frame->rgb.capacity() * sizeof(uint32_t) + sizeof(Impl::Entry) +
                         2 * (sizeof(Key) + key.asset.capacity()) + sizeof(RestoredFrame) + 64;
    if (bytes <= impl->stats.budget) {
        impl->trim(bytes);
        impl->lru.push_front({std::move(key), frame, bytes});
        impl->index.emplace(impl->lru.front().key, impl->lru.begin());
        impl->stats.bytes += bytes;
        impl->stats.entries = impl->index.size();
    }
    return frame;
}
void RestorationCache::setBudget(size_t budget) {
    impl->stats.budget = budget;
    impl->trim();
}
void RestorationCache::clear() {
    impl->index.clear();
    impl->lru.clear();
    impl->stats.bytes = impl->stats.entries = 0;
}
RestorationStats RestorationCache::stats() const {
    return impl->stats;
}
} // namespace dust
