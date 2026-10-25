// SPDX-License-Identifier: GPL-3.0-only
// Native port informed by pinned dreamREfactory image.ts and set-v1.ts.
#include "dust.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dust {
namespace {
void need(bool ok, const char *reason) {
    if (!ok)
        throw std::runtime_error(reason);
}
struct FrameReader {
    View bytes;
    size_t pos = 4;
    uint8_t byte() {
        need(pos < bytes.size(), "truncated frame stream");
        return bytes[pos++];
    }
    uint8_t peek(size_t at) const {
        // The original bit window speculatively reads beyond its consumed bits.
        // Zero padding is permitted only for this bounded lookahead, not consumption.
        need(at <= bytes.size() + 3, "frame bit window outside input");
        return at < bytes.size() ? bytes[at] : 0;
    }
};
void packed(FrameReader &r, Bytes &out, size_t at, size_t count, int mode, int rowBack) {
    size_t written = 0;
    int back = 1;
    if (mode == 0)
        out[at + written++] = r.byte();
    else
        back = rowBack;
    uint32_t flags = uint32_t(r.peek(r.pos)) << 24 | uint32_t(r.peek(r.pos + 1)) << 16 |
                     uint32_t(r.peek(r.pos + 2)) << 8 | r.peek(r.pos + 3);
    r.pos += 2;
    int bits = 16;
    for (; written < count; ++written) {
        int first = 0;
        uint32_t mask = 0x80000000;
        for (int i = 15; i >= 0; --i, mask >>= 1)
            if (flags & mask) {
                first = i;
                break;
            }
        int64_t source = int64_t(at + written) - back;
        need(source >= 0 && uint64_t(source) < out.size(), "invalid packed predictor");
        int prediction = out[size_t(source)];
        if (first == 15) {
            out[at + written] = uint8_t(prediction);
            --bits;
            flags <<= 1;
        } else if (first < 8) {
            out[at + written] = uint8_t(prediction + ((flags >> 16) & 255));
            bits -= 16;
            flags <<= 16;
        } else {
            int diff = 15 - first;
            out[at + written] =
                uint8_t(prediction + ((flags & (uint32_t(1) << (first + 15))) ? diff : -diff));
            bits -= diff + 2;
            flags <<= diff + 2;
        }
        if (bits < 0) {
            int shift = -bits;
            flags >>= shift;
            r.pos += 2;
            flags |= uint32_t(r.peek(r.pos)) << 8 | r.peek(r.pos + 1);
            flags <<= shift;
            bits += 16;
        }
    }
    if (bits >= 8)
        --r.pos;
    need(r.pos <= r.bytes.size(), "truncated consumed bit stream");
}
int bearing(int facing) {
    switch (facing) {
    case 1:
        return 192;
    case 2:
        return 64;
    case 3:
        return 0;
    case 4:
        return 128;
    default:
        throw std::runtime_error("invalid facing");
    }
}
} // namespace
void FrameBuffer::decode(View bytes) {
    int height = i16(bytes, 0), width = i16(bytes, 2);
    need(width > 0 && height > 0 && width <= 4096 && height <= 4096, "invalid frame dimensions");
    size_t total = size_t(width) * height;
    if (pixels.size() < total)
        pixels.resize(total);
    if (depth.size() < total)
        depth.resize(total);
    w = width;
    h = height;
    hasDepth = false;
    FrameReader r{bytes};
    size_t at = 0;
    int back = 0;
    auto copy = [&](size_t count, int distance, bool tile) {
        int64_t src = int64_t(at) - distance;
        need(src >= 0 && uint64_t(src) + count <= pixels.size() && at + count <= total,
             "frame copy outside output");
        if (tile && distance > 0 && size_t(distance) < count)
            for (size_t k = 0; k < count; ++k)
                pixels[at + k] = pixels[size_t(src) + k];
        else
            std::memmove(pixels.data() + at, pixels.data() + size_t(src), count);
    };
    for (int row = 0; row < height; ++row) {
        size_t written = 0;
        int mode = r.byte() >> 2;
        need(mode >= 1 && mode <= 18, "unsupported frame row mode");
        if (mode == 1) {
            need(at + width <= total, "literal row outside output");
            for (int x = 0; x < width; ++x)
                pixels[at++] = r.byte();
            written = width;
        }
        if (mode <= 5)
            back = width * (6 - mode);
        else if (mode <= 9)
            back = width * (5 - mode);
        else if (mode == 10) {
            written = width;
            at += width;
        } else {
            back = width * ((mode <= 14 ? 15 : 14) - mode);
            copy(width, back, false);
            written = width;
            at += width;
        }
        while (written < size_t(width)) {
            int flag = r.byte(), run = flag & 7;
            size_t count = flag >> 3;
            if (!count)
                count = 32 + r.byte();
            need(count <= size_t(width) - written && at + count <= total, "frame run overruns row");
            switch (run) {
            case 2:
                break;
            case 3:
                copy(count, back, true);
                break;
            case 4:
                need(at > 0, "repeat before first pixel");
                std::fill_n(pixels.begin() + at, count, pixels[at - 1]);
                break;
            case 5:
                for (size_t k = 0; k < count; ++k)
                    pixels[at + k] = r.byte();
                break;
            case 6: {
                auto value = r.byte();
                std::fill_n(pixels.begin() + at, count, value);
                break;
            }
            case 7: {
                auto lo = r.byte();
                int distance = lo | (int(r.byte()) << 8);
                need(distance > 0, "zero frame backreference");
                copy(count, distance, true);
                break;
            }
            default:
                packed(r, pixels, at, count, run, back);
                break;
            }
            written += count;
            at += count;
        }
    }
    need(at == total, "frame output length mismatch");
    if (r.pos < bytes.size()) {
        size_t table = r.pos, z = 0;
        need(size_t(height) * 2 <= bytes.size() - table, "truncated depth table");
        for (int y = 0; y < height; ++y) {
            size_t p = table + u16(bytes, table + size_t(y) * 2);
            need(p < bytes.size(), "invalid depth row offset");
            size_t runs = bytes[p++];
            need(runs * 2 <= bytes.size() - p, "truncated depth runs");
            for (size_t i = 0; i < runs; ++i) {
                size_t n = bytes[p++];
                uint8_t v = bytes[p++];
                need(n <= total - z, "depth output overflow");
                std::fill_n(depth.begin() + z, n, v);
                z += n;
            }
        }
        need(z == total, "depth output length mismatch");
        hasDepth = true;
    }
}
SceneSet SceneSet::open(const std::filesystem::path &path) {
    SceneSet s;
    s.file = File::open(path);
    auto h = s.file.at(0);
    need(u32(h, 2) == 1, "only v1 SET supported");
    s.gridW = i16(h, 0x26);
    s.gridH = i16(h, 0x28);
    s.viewW = i16(h, 0x2a);
    s.viewH = i16(h, 0x2c);
    s.cameraSetback = i16(h, 0x18);
    s.eyeHeight = i16(h, 0x1a);
    need(s.gridW > 0 && s.gridH > 0 && s.gridW <= 256 && s.gridH <= 256, "invalid scene grid");
    need(s.viewW == 512 && s.viewH == 264, "unsupported scene viewport");
    s.initial = {i16(h, 0x30), i16(h, 0x32), i16(h, 0x34)};
    s.mainScript = u32(h, 0x1b78);
    s.name = pstr(h, 0x1b7c, 20);
    s.file.at(s.mainScript);
    // ALT31 0x41a5c0 indexes the 32 DWORD floor-script register directly.
    for (size_t floor = 0; floor < s.floorScripts.size(); ++floor) {
        s.floorScripts[floor] = u32(h, 0x1b8c + floor * 4);
        s.file.at(s.floorScripts[floor]);
    }
    // The v1 register has 50-byte records with a second star nested at +26.
    // Leroy's initial town.leroy1 is a secondary, not a primary table entry.
    auto starData = s.file.at(u32(h, 0x22));
    size_t starCount = u32(starData, 0x18);
    need(starData.size() >= 28 && starCount <= (starData.size() - 28) / 50,
         "truncated scene star register");
    const auto validStarName = [](const std::string &name) {
        return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '_';
        });
    };
    const auto readStar = [&](size_t at, size_t limit, bool secondary) {
        // Secondary tails also hold opaque data. Recognize only a bounded
        // identifier and nonzero position, matching the pinned v1 reader.
        if (starData[at + 8] > limit || starData[at + 8] == 0) {
            need(secondary, "invalid primary scene star name");
            return;
        }
        auto name = pstr(starData, at + 8, limit);
        SceneStar star{name, i16(starData, at + 2), i16(starData, at + 4), i16(starData, at + 6),
                       i16(starData, at)};
        if (secondary && (!validStarName(name) || (!star.x && !star.y && !star.z)))
            return;
        s.stars.push_back(std::move(star));
    };
    for (size_t i = 0; i < starCount; ++i) {
        size_t at = 28 + i * 50;
        readStar(at, 17, false);
        readStar(at + 26, 15, true);
    }
    for (size_t clut = 0; clut < 3; ++clut) {
        auto &pal = s.palettes[clut];
        for (size_t i = 0; i < 256; ++i) {
            size_t o = 0x50 + clut * 0x910 + i * 8;
            pal[i] = uint32_t(u16(h, o + 2) >> 8) << 16 | uint32_t(u16(h, o + 4) >> 8) << 8 |
                     (u16(h, o + 6) >> 8);
        }
        pal[0] = 0;
        pal[255] = 0xffffff;
    }
    // Exact ALT31 DF.EXE: count @1c0c, physical records @1c10, stride 32.
    // 0x419780 uses script +28; 0x41bfb0 compares row/column +0/+2.
    size_t sceneCount = u32(h, 0x1c0c);
    need(sceneCount == size_t(s.gridW) * s.gridH, "scene count differs from grid");
    need(h.size() >= 0x1c10 && sceneCount <= (h.size() - 0x1c10) / 32, "truncated scene table");
    for (size_t i = 0; i < sceneCount; ++i) {
        size_t b = 0x1c10 + i * 32;
        SceneCell c{i16(h, b + 2),  i16(h, b),          pstr(h, b + 12, 15),
                    u32(h, b + 28), i16(h, b + 8) != 0, i16(h, b + 10)};
        need(c.floorIndex >= 0 && c.floorIndex < 32, "scene floor index outside register");
        need(c.x >= 0 && c.x < s.gridW && c.z >= 0 && c.z < s.gridH, "scene cell outside grid");
        s.file.at(c.script);
        s.cells.push_back(c);
    }
    auto reg = s.file.at(u32(h, 0x1e));
    size_t count = u32(h, 8);
    need(count > 0 && count == reg.size() / 28 && reg.size() % 28 == 0,
         "transition register count mismatch");
    for (size_t i = 0; i < count; ++i) {
        size_t o = i * 28;
        for (size_t k = 0; k < 6; ++k)
            if (i16(reg, o + k * 2) != i16(reg, o + 12 + k * 2)) {
                ++s.coordinateCopyMismatches;
                break;
            }
        SceneTransition t{{i16(reg, o), i16(reg, o + 2), i16(reg, o + 4)},
                          {i16(reg, o + 6), i16(reg, o + 8), i16(reg, o + 10)},
                          u32(reg, o + 24),
                          {}};
        bearing(t.from.facing);
        bearing(t.to.facing);
        s.transitions.push_back(t);
    }
    auto real = [&](uint64_t ref) {
        return ref < s.file.containers.size() && !s.file.containers[size_t(ref)].gap &&
               s.file.containers[size_t(ref)].data.size() >= 8;
    };
    for (auto &t : s.transitions) {
        uint64_t next = std::numeric_limits<uint64_t>::max();
        for (auto &other : s.transitions)
            if (other.firstFrame > t.firstFrame)
                next = std::min(next, uint64_t(other.firstFrame));
        for (uint64_t ref = t.firstFrame;
             ref < std::min(next, uint64_t(t.firstFrame) + 5) && real(ref); ++ref)
            t.frames.push_back(uint32_t(ref));
        need(!t.frames.empty(), "transition has no frames");
        uint64_t still = uint64_t(t.firstFrame) + 5;
        if (still < next && t.frames.size() == 5 && real(still)) {
            auto b = s.file.at(size_t(still));
            if (i16(b, 0) == s.viewH && i16(b, 2) == s.viewW && b.size() >= size_t(4 + s.viewH))
                t.departureStill = int64_t(still);
        }
    }
    need(std::any_of(s.transitions.begin(), s.transitions.end(),
                     [&](auto &t) { return t.from == s.initial; }),
         "default standpoint has no transition");
    return s;
}
const SceneStar *SceneSet::findStar(const std::string &query) const {
    const auto same = [&](const std::string &candidate) {
        return candidate.size() == query.size() &&
               std::equal(candidate.begin(), candidate.end(), query.begin(),
                          [](unsigned char a, unsigned char b) {
                              return std::tolower(a) == std::tolower(b);
                          });
    };
    for (const auto &star : stars)
        if (same(star.name))
            return &star;
    return nullptr;
}
FrameBuffer SceneSet::standing(Standpoint p) const {
    for (auto &t : transitions)
        if (t.from == p && t.departureStill >= 0) {
            FrameBuffer fb;
            fb.decode(file.at(size_t(t.departureStill)));
            return fb;
        }
    for (auto &t : transitions)
        if (t.to == p) {
            FrameBuffer fb;
            for (auto ref : t.frames)
                fb.decode(file.at(ref));
            return fb;
        }
    throw std::runtime_error("no standing view for standpoint");
}
std::vector<uint32_t> SceneSet::render(const FrameBuffer &fb, size_t clut,
                                       const Puppet *grabPalette) const {
    need(clut < palettes.size() && fb.w > 0 && fb.h > 0, "invalid scene render input");
    std::vector<uint32_t> out(512 * 384, 0);
    for (int y = 0; y < viewH; ++y)
        for (int x = 0; x < viewW; ++x) {
            auto index = fb.pixels[size_t(y * fb.h / viewH) * fb.w + x * fb.w / viewW];
            out[size_t(y) * 512 + x] =
                (grabPalette && index >= 128) ? grabPalette->palette[index] : palettes[clut][index];
        }
    return out;
}
const SceneTransition *SceneSet::move(Standpoint p, int action) const {
    need(action >= -1 && action <= 1, "invalid movement action");
    for (auto &t : transitions)
        if (t.from == p) {
            bool turn = t.to.x == p.x && t.to.z == p.z;
            if (action == 0 && !turn && t.to.facing == p.facing)
                return &t;
            int delta = (bearing(t.to.facing) - bearing(p.facing) + 256) % 256;
            if (turn && delta == (action > 0 ? 64 : 192) && action != 0)
                return &t;
        }
    return nullptr;
}
} // namespace dust
