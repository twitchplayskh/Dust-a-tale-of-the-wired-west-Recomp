// SPDX-License-Identifier: GPL-3.0-only
// Format/codec port informed by dreamREfactory (GPL-3.0). See THIRD_PARTY.md.
#include "dust.hpp"
#include "vm.hpp"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
namespace dust {
static void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
static View slice(View b, size_t p, size_t n) {
    check(p <= b.size() && n <= b.size() - p, "byte range outside resource");
    return b.subspan(p, n);
}
uint16_t u16(View b, size_t p) {
    auto v = slice(b, p, 2);
    return uint16_t(v[0] | uint16_t(v[1]) << 8);
}
int16_t i16(View b, size_t p) {
    return static_cast<int16_t>(u16(b, p));
}
uint32_t u32(View b, size_t p) {
    auto v = slice(b, p, 4);
    return uint32_t(v[0]) | uint32_t(v[1]) << 8 | uint32_t(v[2]) << 16 | uint32_t(v[3]) << 24;
}
std::string pstr(View b, size_t p, size_t max) {
    auto n = slice(b, p, 1)[0];
    check(n <= max, "Pascal string exceeds field");
    auto v = slice(b, p + 1, n);
    return {v.begin(), v.end()};
}
File File::parse(View b) {
    check(b.size() >= 1024, "short container header");
    check(u32(b, 0) == 0x10000, "unsupported container file version");
    check(u32(b, 4) == b.size(), "declared file size mismatch");
    uint32_t count = u32(b, 20);
    // Actual position entries identify gaps. MAYOR.PUP type=2, gapWhere=313 has
    // a real referenced record at 312 and gaps at 4 and 313; no inferred adjacent gap.
    check(count <= (b.size() - 1024) / 4, "position table outside file");
    File out;
    out.containers.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t p = u32(b, 1024 + size_t(i) * 4);
        bool dummy = p <= 1024;
        if (dummy) {
            out.containers.push_back({int32_t(i), true, {}});
            continue;
        }
        check(p >= 1024 + size_t(count) * 4, "record overlaps table");
        auto n = u32(b, size_t(p) + 4);
        auto v = slice(b, size_t(p) + 8, n);
        out.containers.push_back({int32_t(u32(b, p)), false, {v.begin(), v.end()}});
    }
    return out;
}
File File::open(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    check(bool(f), "cannot open resource");
    Bytes b((std::istreambuf_iterator<char>(f)), {});
    return parse(b);
}
View File::at(size_t n) const {
    check(n < containers.size() && !containers[n].gap, "invalid container reference");
    return containers[n].data;
}
Sprite Sprite::decode(View b) {
    Sprite s;
    s.h = i16(b, 0);
    s.w = i16(b, 2);
    s.ay = i16(b, 4);
    s.ax = i16(b, 6);
    check(s.w > 0 && s.h > 0 && s.w <= 4096 && s.h <= 4096, "invalid sprite dimensions");
    size_t n = size_t(s.w) * s.h;
    s.pixels.resize(n);
    s.mask.resize(n);
    size_t p = 8;
    for (int y = 0; y < s.h; ++y) {
        size_t end = p + 2 + u16(b, p);
        p += 2;
        check(end <= b.size(), "truncated sprite row");
        size_t x = 0;
        while (p < end) {
            uint8_t flag = b[p++];
            size_t count = flag >> 2;
            int kind = flag & 3;
            check(count > 0 && x + count <= size_t(s.w), "invalid sprite run");
            size_t dst = size_t(y) * s.w + x;
            if (kind == 3) {
                check(count <= end - p, "truncated literal");
                for (size_t k = 0; k < count; ++k) {
                    s.pixels[dst + k] = b[p++];
                    s.mask[dst + k] = 1;
                }
            } else if (kind == 2) {
                check(p < end, "truncated repeat");
                uint8_t v = b[p++];
                for (size_t k = 0; k < count; ++k) {
                    s.pixels[dst + k] = v;
                    s.mask[dst + k] = 1;
                }
            } else if (kind == 0) {
                check(y > 0, "sprite copy before first row");
                for (size_t k = 0; k < count; ++k) {
                    s.pixels[dst + k] = s.pixels[dst + k - s.w];
                    s.mask[dst + k] = s.mask[dst + k - s.w];
                }
            }
            x += count;
        }
        check(p == end && x == size_t(s.w), "sprite row width mismatch");
    }
    check(p == b.size(), "sprite trailing bytes");
    return s;
}
Puppet Puppet::open(const std::filesystem::path &path) {
    Puppet p;
    p.file = File::open(path);
    auto directory = p.file.at(2);
    int scriptCount = i16(directory, 22);
    check(scriptCount >= 0 && directory.size() >= 24 &&
              size_t(scriptCount) <= (directory.size() - 24) / 40,
          "invalid puppet script directory");
    for (int i = 0; i < scriptCount; ++i) {
        size_t at = 24 + size_t(i) * 40;
        auto ref = u32(directory, at);
        p.file.at(ref);
        p.scripts.push_back({pstr(directory, at + 8, 31), ref});
    }
    auto h = p.file.at(0);
    check(u32(h, 0) == 0x10000, "only DreamFactory 1 puppets supported");
    p.refName = pstr(h, 0x85e, 15);
    for (size_t i = 0; i < 256; ++i) {
        size_t o = 58 + i * 8;
        p.palette[i] = uint32_t(slice(h, o + 3, 1)[0]) << 16 |
                       uint32_t(slice(h, o + 5, 1)[0]) << 8 | slice(h, o + 7, 1)[0];
    }
    p.palette[0] = 0;
    p.palette[255] = 0xffffff;
    p.band = u32(h, 2138);
    p.playerVoiceEnabled = i16(h, 0x20) != 0;
    for (size_t i = 0; i < 4; ++i)
        p.idleSlots[i] = {"idle " + std::to_string(i + 1), int32_t(u32(h, 0x83a + i * 4)),
                          int32_t(u32(h, 0x84a + i * 4))};
    int count = i16(h, 2158);
    check(count >= 0 && size_t(count) <= (h.size() - 2160) / 312, "invalid dialogue table");
    for (int i = 0; i < count; ++i) {
        size_t o = 2160 + size_t(i) * 312;
        Line l{pstr(h, o + 280, 31), pstr(h, o + 24, 255), u32(h, o + 8), u32(h, o + 12),
               i16(h, o)};
        check(l.stance == 0, "unexpected v1 stance");
        p.lines.push_back(l);
    }
    auto replies = p.file.at(1);
    int replyCount = i16(replies, 22);
    check(replyCount >= 0 && size_t(replyCount) <= (replies.size() - 24) / 296,
          "invalid player reply directory");
    for (int i = 0; i < replyCount; ++i) {
        size_t o = 24 + size_t(i) * 296;
        PlayerReply reply{u32(replies, o), u32(replies, o + 4), pstr(replies, o + 8, 255),
                          pstr(replies, o + 264, 31)};
        p.file.at(reply.audio);
        p.replies.push_back(std::move(reply));
    }
    auto st = p.file.at(3);
    check(st.size() == 2904, "invalid v1 stance size");
    for (size_t i = 0; i < 11; ++i) {
        size_t o = 22 + i * 262;
        int n = i16(st, o);
        check(n >= 0 && n <= 32, "invalid layer count");
        auto &l = p.layers[i];
        l.y = i16(st, o + 2);
        l.x = i16(st, o + 4);
        for (int k = 0; k < n; ++k) {
            uint32_t ref = u32(st, o + 6 + size_t(k) * 4);
            p.file.at(ref);
            l.frames.push_back(ref);
        }
    }
    return p;
}
std::vector<Tick> Puppet::track(size_t line) const {
    check(line < lines.size(), "invalid dialogue line");
    if (!lines[line].track)
        return {};
    auto b = file.at(lines[line].track);
    check(b.size() % 82 == 0, "invalid puppet track length");
    std::vector<Tick> out(b.size() / 82);
    for (size_t i = 0; i < out.size(); ++i)
        for (size_t l = 0; l < 11; ++l) {
            size_t o = i * 82 + 16 + l * 6;
            out[i][l] = {i16(b, o), i16(b, o + 2), i16(b, o + 4)};
        }
    return out;
}
View Puppet::handlerScript(const std::string &handler) const {
    std::vector<uint32_t> refs;
    auto table = file.at(2);
    int count = i16(table, 22);
    check(count >= 0 && size_t(count) <= (table.size() - 24) / 40,
          "invalid puppet script directory");
    for (int i = 0; i < count; ++i)
        refs.push_back(u32(table, 24 + size_t(i) * 40));
    auto lower = [](std::string s) {
        for (char &c : s)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return s;
    };
    for (auto ref : refs) {
        auto tokens = script(file.at(ref));
        bool has = false;
        std::string actualName;
        for (size_t i = 0; i + 1 < tokens.size(); ++i)
            if (tokens[i].cmd == 4001 && lower(tokens[i + 1].text) == lower(handler)) {
                has = true;
                actualName = tokens[i + 1].text;
            }
        if (!has)
            continue;
        return file.at(ref);
    }
    throw std::runtime_error("puppet handler not found");
}
std::optional<size_t> Puppet::findSpeech(const std::string &name) const {
    if (name.empty())
        return {};
    auto lower = [](std::string s) {
        for (auto &c : s)
            if (c >= 'A' && c <= 'Z')
                c = char(c + ('a' - 'A'));
        return s;
    };
    auto wanted = lower(name);
    for (size_t i = 0; i < lines.size(); ++i)
        if (lower(lines[i].id) == wanted || lower(lines[i].text) == wanted)
            return i;
    return {};
}
std::optional<size_t> Puppet::findPlayerReply(const std::string &name) const {
    if (name.empty())
        return {};
    auto fold = [](std::string s) {
        for (auto &c : s)
            if (c >= 'A' && c <= 'Z')
                c = char(c + 'a' - 'A');
        return s;
    };
    auto wanted = fold(name);
    for (size_t i = 0; i < replies.size(); ++i)
        if (fold(replies[i].alias) == wanted || fold(replies[i].text) == wanted)
            return i;
    return {};
}
std::vector<size_t> Puppet::sequence(const std::string &handler) const {
    std::vector<size_t> result;
    runScript(handlerScript(handler), handler, [&](const std::string &id) {
        auto found = findSpeech(id);
        check(found.has_value(), "script line identifier/text not found in puppet");
        result.push_back(*found);
    });
    return result;
}
std::vector<uint32_t> Puppet::render(size_t line, size_t tick, std::span<const uint32_t> backdrop,
                                     bool grab, std::span<const uint32_t> renderPalette,
                                     RestorationCache *cache, RestorationSettings settings) const {
    std::span<const uint32_t> colours =
        renderPalette.empty() ? std::span<const uint32_t>(palette) : renderPalette;
    check(colours.size() == 256, "invalid puppet render palette");
    std::vector<uint32_t> out(512 * 384, 0x202020);
    if (!backdrop.empty()) {
        check(backdrop.size() == out.size(), "invalid puppet backdrop");
        std::copy(backdrop.begin(), backdrop.end(), out.begin());
    }
    auto tr = track(line);
    Tick pose{};
    for (size_t l = 0; l < 11; ++l)
        pose[l] = {layers[l].frames.empty() ? -1 : 0, layers[l].y, layers[l].x};
    if (!tr.empty())
        pose = tr[std::min(tick, tr.size() - 1)];
    auto draw = [&](uint32_t ref, int x, int y, int clip, int layer) {
        auto s = Sprite::decode(file.at(ref));
        std::shared_ptr<const RestoredFrame> cleaned;
        if (cache && settings.enabled && clip == 264) {
            std::vector<uint32_t> source(s.pixels.size());
            for (size_t i = 0; i < source.size(); ++i)
                source[i] = colours[s.pixels[i]];
            std::array<Sprite, 2> adjacent{};
            std::array<std::vector<uint32_t>, 2> adjacentRgb;
            std::array<RestorationNeighbor, 2> neighbors{};
            size_t neighborCount = 0;
            if (settings.temporal && layer >= 0 && !tr.empty()) {
                const size_t at = std::min(tick, tr.size() - 1);
                size_t first = at, last = at;
                const int current = pose[size_t(layer)].frame;
                while (first > 0 && at - first < 7 && tr[first - 1][size_t(layer)].frame == current)
                    --first;
                while (last + 1 < tr.size() && last - at < 7 &&
                       tr[last + 1][size_t(layer)].frame == current)
                    ++last;
                if (first > 0 && last + 1 < tr.size() && last - first < 6) {
                    const std::array<size_t, 2> ticks{first - 1, last + 1};
                    bool valid = true;
                    for (size_t k = 0; k < 2; ++k) {
                        const auto p = tr[ticks[k]][size_t(layer)];
                        if (p.frame < 0 || size_t(p.frame) >= layers[size_t(layer)].frames.size()) {
                            valid = false;
                            break;
                        }
                        const auto otherRef = layers[size_t(layer)].frames[size_t(p.frame)];
                        if (otherRef == ref || (k && otherRef == neighbors[0].identity)) {
                            valid = false;
                            break;
                        }
                        adjacent[k] = Sprite::decode(file.at(otherRef));
                        const auto &other = adjacent[k];
                        adjacentRgb[k].resize(other.pixels.size());
                        for (size_t i = 0; i < other.pixels.size(); ++i)
                            adjacentRgb[k][i] = colours[other.pixels[i]];
                        const int ox = other.ax - s.ax, oy = other.ay - s.ay;
                        neighbors[k] = {otherRef,
                                        other.w,
                                        other.h,
                                        ox,
                                        oy,
                                        int((last - first + 2) / 2),
                                        adjacentRgb[k],
                                        other.mask,
                                        std::abs(x - p.x) <= 32 && std::abs(y - p.y) <= 32};
                    }
                    if (valid)
                        neighborCount = 2;
                }
            }
            const auto identity =
                "puppet:" + refName +
                (neighborCount ? ":line=" + std::to_string(line) + ":layer=" + std::to_string(layer)
                               : "");
            cleaned =
                cache->get(identity, ref, s.w, s.h, source, s.mask, settings,
                           std::span<const RestorationNeighbor>(neighbors.data(), neighborCount));
        }
        for (int sy = 0; sy < s.h; ++sy)
            for (int sx = 0; sx < s.w; ++sx) {
                int dx = x - s.ax + sx, dy = y - s.ay + sy;
                size_t k = size_t(sy) * s.w + sx;
                if (dx >= 0 && dx < 512 && dy >= 0 && dy < clip && s.mask[k])
                    out[size_t(dy) * 512 + dx] = cleaned ? cleaned->rgb[k] : colours[s.pixels[k]];
            }
    };
    // Exact ALT31 0x409920 draws only 0<=frame<count. Invalid non-background
    // layers do not draw; invalid layer0 clears the port. 0x409710 substitutes
    // the SET background for layer0 when puppetgrab's WORD is nonzero.
    for (size_t l = 0; l < 11; ++l) {
        if (grab && l == 0)
            continue;
        if (pose[l].frame >= 0 && size_t(pose[l].frame) < layers[l].frames.size())
            draw(layers[l].frames[size_t(pose[l].frame)], pose[l].x, pose[l].y, 264, int(l));
        else if (l == 0)
            std::fill(out.begin(), out.end(), colours[0]);
    }
    if (band)
        draw(band, 256, 324, 384, -1);
    return out;
}
std::vector<Token> script(View b) {
    std::vector<Token> out;
    for (size_t p = 0;; p += 8) {
        slice(b, p, 8);
        uint16_t cmd = u16(b, p);
        if (!cmd)
            break;
        Token t{cmd, u32(b, p + 2), {}};
        check(u16(b, p + 6) == 0, "nonzero script padding");
        if (cmd == 3 || cmd == 5) {
            check(t.info <= b.size() - p, "script string offset");
            t.text = pstr(b, p + t.info, 255);
        }
        out.push_back(t);
    }
    return out;
}
void runScript(View b, const std::string &name,
               const std::function<void(const std::string &)> &speak) {
    ScriptProgram program(b);
    ScriptVM vm;
    vm.bind(12043, [&](const std::vector<ScriptValue> &args, bool) {
        if (args.size() != 1 || args[0].type != ScriptValue::Type::String)
            throw ScriptError(0xe, "puppetspeak requires one string");
        speak(args[0].text);
        return ScriptValue::integer(0);
    });
    auto result = vm.run(program, name);
    check(result.handled, "script handler not found");
}
Audio audio(View b) {
    check(u32(b, 0) == 0x10000, "unsupported sound version");
    auto codec = u16(b, 26);
    Audio a{u32(b, 28), {}};
    uint32_t bytes = u32(b, 36);
    size_t p = u32(b, 44);
    check(a.rate >= 4000 && a.rate <= 192000 && bytes > 0 && bytes <= 64 * 1024 * 1024 && p >= 48 &&
              p < b.size(),
          "invalid audio header");
    check(codec == 1 || codec == 2, "unsupported sound codec");
    if (codec == 2) {
        check(bytes % 2 == 0, "odd 16-bit audio length");
        size_t n = bytes / 2;
        auto v = slice(b, p, n);
        int current = 0;
        a.pcm.reserve(n);
        for (auto x : v) {
            int low = x & 127;
            int signed7 = low < 64 ? low : low - 128;
            current = (x & 128) ? signed7 * 512 : int(int16_t(current + signed7 * 32));
            a.pcm.push_back(int16_t(current));
        }
    } else {
        a.pcm.reserve(bytes);
        auto next = [&]() { return slice(b, p++, 1)[0]; };
        int prev = int8_t(next());
        auto push = [&](int v) {
            if (a.pcm.size() < bytes)
                a.pcm.push_back(int16_t((int(uint8_t(v + 64)) - 128) * 256));
        };
        push(prev);
        while (a.pcm.size() < bytes) {
            int x = next();
            if (!(x & 128)) {
                prev = x;
                push(prev);
            } else if (!(x & 64)) {
                int n = (x & 63) + 1;
                for (int i = 0; i < n; ++i) {
                    int pair = next();
                    int hi = pair >> 4;
                    hi = hi < 8 ? hi : hi - 16;
                    int lo = pair & 15;
                    lo = lo < 8 ? lo : lo - 16;
                    int step = int8_t(prev + hi);
                    int idx = int8_t(step + lo);
                    push(step);
                    push(idx);
                    prev = idx;
                }
            } else {
                int n = (x & 63) + 1;
                for (int i = 0; i < n; ++i)
                    push(prev);
            }
        }
    }
    return a;
}
Bytes wave(const Audio &a) {
    Bytes b;
    auto put16 = [&](uint16_t n) {
        b.push_back(uint8_t(n));
        b.push_back(uint8_t(n >> 8));
    };
    auto put32 = [&](uint32_t n) {
        put16(uint16_t(n));
        put16(uint16_t(n >> 16));
    };
    auto tag = [&](const char *s) {
        for (int i = 0; i < 4; ++i)
            b.push_back(uint8_t(s[i]));
    };
    check(a.pcm.size() < UINT32_MAX / 2 - 44, "audio too large");
    uint32_t bytes = uint32_t(a.pcm.size() * 2);
    tag("RIFF");
    put32(bytes + 36);
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(1);
    put16(1);
    put32(a.rate);
    put32(a.rate * 2);
    put16(2);
    put16(16);
    tag("data");
    put32(bytes);
    for (auto s : a.pcm)
        put16(uint16_t(s));
    return b;
}
void ppm(const std::filesystem::path &path, std::span<const uint32_t> v, int w, int h) {
    check(v.size() == size_t(w) * h, "image size mismatch");
    std::ofstream f(path, std::ios::binary);
    check(bool(f), "cannot write image");
    f << "P6\n" << w << " " << h << "\n255\n";
    for (auto c : v) {
        f.put(char(c >> 16));
        f.put(char(c >> 8));
        f.put(char(c));
    }
    check(bool(f), "image write failed");
}
} // namespace dust
