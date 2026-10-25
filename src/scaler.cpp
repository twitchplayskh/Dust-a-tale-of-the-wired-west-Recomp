// SPDX-License-Identifier: GPL-3.0-only
// C++ adaptation of dreamREfactory commit93d7e949 runtime/sprite-scaler.ts.
#include "scaler.hpp"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
namespace dust {
namespace {
struct Maps {
    std::vector<int> forward, reverse;
};
Maps columnMaps(int source, int dest) {
    Maps out{std::vector<int>(size_t(source) + 1), std::vector<int>(size_t(dest) + 1)};
    int s = 0, d = 0;
    if (source <= dest) {
        int error = -dest;
        for (;;) {
            if (error > 0) {
                ++s;
                error -= 2 * dest;
                out.forward[s] = d + 1;
            }
            out.reverse[++d] = s;
            if (d == dest)
                break;
            error += 2 * source;
        }
        out.forward[source] = dest;
    } else {
        int error = -source;
        for (;;) {
            if (error > 0) {
                ++d;
                error -= 2 * source;
                out.reverse[d] = s + 1;
            }
            if (++s == source)
                break;
            error += 2 * dest;
            out.forward[s] = d;
        }
        out.reverse[dest] = source;
        out.forward[source] = dest;
    }
    return out;
}
void resample(const Bytes &src, int at, int count, int output,
              const std::function<void(int, uint8_t, int)> &put) {
    if (count == output) {
        for (int i = 0; i < count; ++i)
            put(i, src.at(size_t(at + i)), at + i);
        return;
    }
    if (output < count) {
        int q = count / output, pos = q / 2, error = (q + 1) * output - count;
        for (int i = 0; i < output; ++i) {
            if (error < 0) {
                put(i, src.at(size_t(at + pos + 1)), at + pos + 1);
                pos += q + 1;
                error += 2 * ((q + 1) * output - count);
            } else {
                put(i, src.at(size_t(at + pos)), at + pos);
                pos += q;
                error += 2 * (q * output - count);
            }
        }
    } else {
        int q = output / count, error = 2 * (q * count - output), o = 0;
        for (int i = 0; i < count; ++i) {
            int repeat = q;
            if (output % count && error < 0) {
                ++repeat;
                error += 2 * ((q + 1) * count - output);
            } else
                error += 2 * (q * count - output);
            for (int j = 0; j < repeat; ++j)
                put(o++, src.at(size_t(at + i)), at + i);
        }
    }
}
struct Span {
    int first, last, left, right;
};
struct Edge {
    int current = 0, start = 0, end = 0, error = 0, twiceDelta = 0, twiceDuration = 0, lifetime = 0;
};
struct Edges {
    std::vector<Edge> items;
    int count = 0;
    explicit Edges(int capacity) : items(capacity) {}
    int value(int v) const {
        return v >= 0 ? v : items.at(size_t(-v - 1)).current;
    }
    int make(int start, int end, int duration, int lifetime) {
        start = value(start);
        int index = 0;
        while (index < count && items[index].lifetime)
            ++index;
        if (index == count)
            ++count;
        if (size_t(index) >= items.size())
            throw std::runtime_error("sprite edge capacity exceeded");
        int twiceDelta = 2 * std::abs(end - start);
        items[index] = {start,      start,        end,     twiceDelta - duration,
                        twiceDelta, 2 * duration, lifetime};
        return -index - 1;
    }
    void expire() {
        for (int i = 0; i < count; ++i)
            if (items[i].lifetime)
                --items[i].lifetime;
        while (count && !items[count - 1].lifetime)
            --count;
    }
    void advance() {
        for (int i = 0; i < count; ++i) {
            auto &e = items[i];
            if (!e.lifetime)
                continue;
            while (e.error >= 0) {
                e.current += e.start < e.end ? 1 : -1;
                e.error -= e.twiceDuration;
            }
            e.error += e.twiceDelta;
        }
    }
};
struct Row {
    std::vector<Span> spans;
    int repeat = 0;
};
int candidate(const Row &row, const Span &span, bool left, int limit,
              const std::vector<int> &forward, int &index, int &endpoint) {
    int best = 8;
    for (size_t i = 0; i < row.spans.size(); ++i) {
        const auto &future = row.spans[i];
        if (future.first > span.last || span.first > future.last)
            continue;
        int difference = left ? span.first - future.first : span.last - future.last;
        difference = std::clamp(difference, -limit, limit);
        int source = (left ? span.first : span.last) - difference;
        if (std::abs(difference) < best) {
            index = int(i);
            endpoint = left ? forward.at(source) : forward.at(source + 1) - 1;
            best = std::abs(difference);
            if (!best)
                return -1;
        }
    }
    return best < 8 ? 1 : 0;
}
void connect(std::vector<Row> &rows, int current, int sourceEnd, const std::vector<int> &forward,
             Edges &edges) {
    edges.expire();
    auto &row = rows[current];
    int index = 0, endpoint = 0;
    for (size_t i = 0; i < row.spans.size(); ++i) {
        auto &span = row.spans[i];
        std::vector<Span *> leftRefs{&span}, rightRefs{&span};
        bool leftDone = false, rightDone = false;
        int duration = row.repeat, lifetime = 1;
        for (int future = current + 1; future < std::min(sourceEnd, current + 8); ++future) {
            if (leftDone && rightDone)
                break;
            if (!leftDone) {
                int limit = i == 0 ? 7 : span.first - row.spans[i - 1].last;
                int status = candidate(rows[future], span, true, limit, forward, index, endpoint);
                if (status < 0)
                    leftRefs.push_back(&rows[future].spans[index]);
                else {
                    if (status > 0) {
                        int ref = edges.make(span.left, endpoint, duration, lifetime);
                        for (auto *s : leftRefs)
                            s->left = ref;
                    }
                    leftDone = true;
                }
            }
            if (!rightDone) {
                int limit = i == row.spans.size() - 1 ? 7 : row.spans[i + 1].first - span.last;
                int status = candidate(rows[future], span, false, limit, forward, index, endpoint);
                if (status < 0)
                    rightRefs.push_back(&rows[future].spans[index]);
                else {
                    if (status > 0) {
                        int ref = edges.make(span.right, endpoint, duration, lifetime);
                        for (auto *s : rightRefs)
                            s->right = ref;
                    }
                    rightDone = true;
                }
            }
            duration += rows[future].repeat;
            ++lifetime;
        }
    }
}
} // namespace
void drawScaledSprite(const Sprite &f, int ax, int ay, int dw, int dh, bool flip,
                      const ScaleTarget &target) {
    int w = f.w, h = f.h;
    if (dw <= 0 || dh <= 0 || w < 1 || h < 1)
        return;
    if (w > 512 || h > 4096 || dw > 16384 || dh > 16384 || f.pixels.size() != size_t(w) * h ||
        f.mask.size() != f.pixels.size())
        throw std::runtime_error("invalid sprite scaler dimensions");
    int left = ax - (flip ? w - f.ax : f.ax) * dw / w, top = ay - f.ay * dh / h;
    if (top >= target.bottom || left >= target.right || top + dh <= target.top ||
        left + dw <= target.left)
        return;
    int first = 0, sourceHeight = h, destHeight = dh, screenY = top;
    if (top < target.top) {
        first = (target.top - top) * h / dh;
        sourceHeight -= first;
        destHeight -= target.top - top;
        screenY = target.top;
    }
    if (top + dh > target.bottom) {
        int excess = top + dh - target.bottom;
        destHeight -= excess;
        sourceHeight -= excess * h / dh;
    }
    if (sourceHeight <= 0 || destHeight <= 0)
        return;
    int clipLeft = std::max(0, target.left - left), clipRight = std::min(dw, target.right - left);
    auto maps = columnMaps(w, dw);
    auto &forward = maps.forward;
    auto &reverse = maps.reverse;
    bool large = sourceHeight < destHeight / 2;
    std::vector<Row> rows(h);
    std::vector<int> selected(destHeight);
    int used = 0;
    if (large) {
        int error = -destHeight;
        for (int sy = first; sy < first + sourceHeight; ++sy) {
            int repeat = 0;
            do {
                ++repeat;
                error += sourceHeight;
            } while (error < 0);
            rows[sy].repeat = repeat;
            error -= destHeight;
            for (int j = 0; j < repeat && used < destHeight; ++j)
                selected[used++] = sy;
        }
    } else if (sourceHeight >= destHeight) {
        int error = 2 * destHeight - sourceHeight, sy = first;
        while (used < destHeight) {
            while (error < 0) {
                ++sy;
                error += 2 * destHeight;
            }
            selected[used++] = sy++;
            error -= 2 * (sourceHeight - destHeight);
        }
    } else {
        int error = 3 * sourceHeight - 2 * destHeight, sy = first;
        while (used < destHeight) {
            int repeat = 1;
            while (error < 0) {
                ++repeat;
                error += 2 * sourceHeight;
            }
            for (int j = 0; j < repeat && used < destHeight; ++j)
                selected[used++] = sy;
            ++sy;
            error -= 2 * (destHeight - sourceHeight);
        }
    }
    bool enlarged = large || sourceHeight < destHeight;
    for (int sy = first; sy < first + sourceHeight; ++sy) {
        auto &spans = rows[sy].spans;
        int base = sy * w;
        for (int x = 0; x < w;) {
            if (!f.mask.at(base + (flip ? w - 1 - x : x))) {
                ++x;
                continue;
            }
            int a = x++;
            while (x < w && f.mask.at(base + (flip ? w - 1 - x : x)))
                ++x;
            int b = x - 1;
            spans.push_back({a, b, forward[a], enlarged ? forward[b + 1] - 1 : forward[b]});
        }
    }
    Edges edges(w * 8 * 2);
    Bytes colors(w);
    std::vector<int> allowed;
    int lastSource = -1;
    for (int row = 0; row < destHeight; ++row) {
        int sy = selected[row], y = screenY + row;
        if (large && sy != lastSource) {
            connect(rows, sy, first + sourceHeight, forward, edges);
            lastSource = sy;
        }
        int base = sy * w;
        for (int x = 0; x < w; ++x)
            colors[x] = f.pixels.at(base + (flip ? w - 1 - x : x));
        allowed.clear();
        for (int x = clipLeft; x < clipRight;) {
            if (target.drawable && !target.drawable(left + x, y)) {
                ++x;
                continue;
            }
            int a = x++;
            while (x < clipRight && (!target.drawable || target.drawable(left + x, y)))
                ++x;
            allowed.push_back(a);
            allowed.push_back(x - 1);
        }
        const auto &spans = rows.at(sy).spans;
        size_t si = 0, mi = 0;
        int bias = 0;
        while (si < spans.size() && mi < allowed.size()) {
            const auto &span = spans[si];
            int d0 = edges.value(span.left), d1 = edges.value(span.right), a0 = allowed[mi],
                a1 = allowed[mi + 1];
            int lo = std::max(span.first, reverse.at(a0) + bias),
                hi = std::min(span.last, (dw < w ? reverse.at(a1 + 1) - 1 : reverse.at(a1)) + bias);
            int dlo = std::max(d0, a0), dhi = std::min(d1, a1);
            if (lo <= hi && dlo <= dhi)
                resample(colors, lo, hi - lo + 1, dhi - dlo + 1, [&](int i, uint8_t c, int column) {
                    target.plot(left + dlo + i, y, c);
                    if (target.plotSource)
                        target.plotSource(left + dlo + i, y,
                                          size_t(base) + (flip ? f.w - 1 - column : column));
                });
            if (a1 <= d1) {
                mi += 2;
                bias = large && a1 == d1 ? ((sy - first) % 8) * 1024 : 0;
            }
            if (d1 <= a1)
                ++si;
        }
        if (large)
            edges.advance();
    }
}
bool scaledSpriteCovers(const Sprite &f, int left, int top, int width, int height, int x, int y) {
    if (width <= 0 || height <= 0 || x < left || y < top || x >= left + width || y >= top + height)
        return false;
    int sx = (x - left) * f.w / width, sy = (y - top) * f.h / height;
    return f.mask.at(size_t(sy) * f.w + std::max(0, sx - 1)) != 0;
}
} // namespace dust
