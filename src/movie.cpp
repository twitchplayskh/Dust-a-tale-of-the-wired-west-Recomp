// SPDX-License-Identifier: GPL-3.0-only
// Version-1 layout and transitions informed by dreamREfactory (GPL-3.0).
// Addresses in that reference are not asserted for this installation's DF.EXE.
#include "movie.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
namespace dust {
namespace {
void require(bool yes, const char *why) {
    if (!yes)
        throw std::runtime_error(why);
}
int32_t signed32(View b, size_t o) {
    return static_cast<int32_t>(u32(b, o));
}
} // namespace
bool MovieRect::empty() const {
    return bottom <= top || right <= left;
}
Movie Movie::open(const std::filesystem::path &path) {
    Movie m;
    m.file = File::open(path);
    std::set<size_t> seen;
    size_t bias = 0;
    for (;;) {
        require(seen.insert(bias).second, "cyclic movie segment chain");
        auto h = m.file.at(bias);
        require(u32(h, 2) == 1, "unsupported movie header version");
        MovieSegment s;
        s.header = bias;
        s.height = i16(h, 0x22);
        s.width = i16(h, 0x24);
        require(s.width > 0 && s.width <= 640 && s.height > 0 && s.height <= 480,
                "invalid movie dimensions");
        require(i16(h, 0x2a) == 0 && i16(h, 0x2c) == 0, "unsupported movie screen origin");
        s.rate = i16(h, 0x26);
        if (!s.rate)
            s.rate = 3;
        require(s.rate > 0, "negative movie rate");
        s.actionFrame1 = i16(h, 0x2e);
        s.actionFrame2 = i16(h, 0x30);
        for (size_t p = 0; p < 256; ++p)
            for (size_t c = 0; c < 3; ++c)
                s.palette[p][c] = u16(h, 0x3e + p * 8 + 2 + c * 2);
        s.palette[0] = {0, 0, 0};
        s.palette[255] = {65535, 65535, 65535};
        int count = i16(h, 0x18);
        require(count > 0 && count <= 4096, "invalid movie frame count");
        for (int i = 0; i < count; ++i) {
            size_t o = 0x8c2 + size_t(i) * 80;
            u32(h, o + 76); // Require the whole record, including unused tail.
            MovieFrame f;
            const int hotspots = i16(h, o);
            require(hotspots >= 0 && hotspots <= 4096, "invalid movie hotspot count");
            size_t hotspot = u32(h, o + 0x24);
            for (int j = 0; j < hotspots; ++j) {
                MovieHotspot item;
                item.action = i16(h, hotspot);
                require(item.action >= 1 && item.action <= 5, "unsupported movie hotspot action");
                const size_t stride = item.action == 1 || item.action == 5 ? 14
                                      : item.action == 2                   ? 16
                                      : item.action == 3                   ? 46
                                                                           : 48;
                i16(h, hotspot + stride - 2);
                item.rect = {i16(h, hotspot + 2), i16(h, hotspot + 4), i16(h, hotspot + 6),
                             i16(h, hotspot + 8)};
                require(!item.rect.empty(), "empty movie hotspot rectangle");
                const auto sound = signed32(h, hotspot + 10);
                if (sound) {
                    const auto magnitude = sound < 0 ? -int64_t(sound) : int64_t(sound);
                    require(bias + uint64_t(magnitude) < m.file.containers.size(),
                            "movie hotspot sound out of range");
                    item.sound = uint32_t(bias + magnitude);
                    m.file.at(item.sound);
                }
                if (item.action == 2 || item.action == 4)
                    item.target = i16(h, hotspot + 14);
                if (item.action == 3 || item.action == 4) {
                    item.chainTo = pstr(h, hotspot + (item.action == 4 ? 16 : 14), 31);
                    require(!item.chainTo.empty(), "empty hotspot movie chain target");
                }
                f.hotspots.push_back(item);
                hotspot += stride;
            }
            f.holdTicks = signed32(h, o + 2);
            f.effect = i16(h, o + 6);
            require((f.effect >= 0 && f.effect <= 12) || (f.effect >= 16 && f.effect <= 18),
                    "unsupported movie transition effect");
            f.action = i16(h, o + 0x16);
            f.target = i16(h, o + 0x18);
            require(f.action >= 1 && f.action <= 5, "unsupported movie frame action");
            f.flags = u16(h, o + 0x1a);
            require((f.flags & ~0x1f) == 0, "unknown movie frame flags");
            int ref = i16(h, o + 0x1c);
            require(ref > 0, "invalid movie picture reference");
            f.picture = uint32_t(bias + ref);
            auto picture = m.file.at(f.picture);
            require(i16(picture, 0) == s.height && i16(picture, 2) >= s.width &&
                        i16(picture, 2) <= ((s.width + 3) & ~3),
                    "movie picture dimensions differ from segment");
            auto sound = signed32(h, o + 0x20);
            if (sound) {
                auto magnitude = sound < 0 ? -int64_t(sound) : int64_t(sound);
                require(magnitude < int64_t(m.file.containers.size()) &&
                            bias + uint64_t(magnitude) < m.file.containers.size(),
                        "movie sound reference out of range");
                f.sound = uint32_t(bias + magnitude);
                m.file.at(f.sound);
            }
            f.dirty = {i16(h, o + 0x28), i16(h, o + 0x2a), i16(h, o + 0x2c), i16(h, o + 0x2e)};
            if (f.action == 3 || f.action == 4) {
                f.chainTo = pstr(h, o + 0x30, 31);
                require(!f.chainTo.empty(), "empty movie chain target");
            }
            s.frames.push_back(std::move(f));
        }
        int bank = i16(h, 0x1a), bed = i16(h, 0x1c), length = i16(h, 0x34);
        require(bank >= 0 && bed >= 0, "negative movie sound bank");
        s.replacesBed = bed > 0;
        if (bed) {
            require(length >= 0 && length <= 64, "movie bed playlist outside header");
            for (int i = 0; i < length; ++i) {
                int entry = i16(h, 0x83e + size_t(i) * 2);
                require(entry > 0 && entry <= bed, "movie bed entry outside bank");
                auto container = uint32_t(bias + bank + entry);
                m.file.at(container);
                s.bed.push_back(container);
            }
            auto loop = signed32(h, 0x8be);
            s.bedLoopFrom = loop >= 0 && size_t(loop) < s.bed.size() ? size_t(loop) : 0;
        }
        int next = i16(h, 0x36);
        require(next >= 0, "negative next movie segment");
        m.segments.push_back(std::move(s));
        if (!next)
            break;
        bias = size_t(next);
    }
    return m;
}
Audio Movie::sound(uint32_t container) const {
    return audio(file.at(container));
}
MoviePlayer::MoviePlayer(const Movie &m) : movie_(m) {
    require(!m.segments.empty(), "movie has no segments");
}
int MoviePlayer::width() const {
    return movie_.segments[segment_].width;
}
int MoviePlayer::height() const {
    return movie_.segments[segment_].height;
}
void MoviePlayer::start(uint64_t tick, size_t segment, size_t frame) {
    require(segment < movie_.segments.size(), "movie resume segment out of range");
    require(frame < movie_.segments[segment].frames.size(), "movie resume frame out of range");
    segment_ = segment;
    frame_ = 0;
    done_ = false;
    return_ = false;
    call_ = false;
    returnFrame_ = 0;
    actionFrames_ = 0;
    chain_.clear();
    events_.clear();
    lastTick_ = tick;
    enterSegment(tick, frame);
}
void MoviePlayer::enterSegment(uint64_t tick, size_t initialFrame) {
    auto &s = movie_.segments[segment_];
    frame_ = 0;
    paletteUnset_ = true;
    buffer_ = {};
    screen_.assign(size_t(s.width) * s.height, 0);
    // Reconstruct delta-picture history silently when returning into a movie.
    // Do not replay earlier voices or mark unvisited action frames as entered.
    for (size_t i = 0; i < initialFrame; ++i) {
        frame_ = i;
        const auto &f = s.frames[i];
        if (!f.dirty.empty())
            buffer_.decode(movie_.file.at(f.picture));
        entered_ = tick;
        effectApplied_ = 0;
        paintEffect(tick + uint64_t(std::max(s.rate, f.holdTicks)));
    }
    frame_ = initialFrame;
    if (s.replacesBed)
        events_.push_back({MovieAudioEvent::Kind::BedStart, s.bed, s.bedLoopFrom});
    enterFrame(tick);
}
void MoviePlayer::enterFrame(uint64_t tick) {
    auto &s = movie_.segments[segment_];
    if (int(frame_) == s.actionFrame1)
        actionFrames_ |= 1;
    if (int(frame_) == s.actionFrame2)
        actionFrames_ |= 2;
    auto &f = s.frames[frame_];
    entered_ = tick;
    effectApplied_ = 0;
    clicked_.reset();
    if (!f.dirty.empty()) {
        buffer_.decode(movie_.file.at(f.picture));
    }
    if (f.effect <= 16 && paletteUnset_) {
        palette_ = s.palette;
        paletteUnset_ = false;
    }
    if (f.sound)
        events_.push_back({MovieAudioEvent::Kind::Voice, {f.sound}, 0});
    render(tick);
}
void MoviePlayer::render(uint64_t tick) {
    paintEffect(tick);
    auto &s = movie_.segments[segment_];
    auto &f = s.frames[frame_];
    if (f.effect == 17 || f.effect == 18) {
        uint64_t n = uint64_t(std::max(s.rate, f.holdTicks));
        auto step = std::min(n - 1, tick - entered_);
        auto denominator = std::max(uint64_t(1), n - 1);
        step = std::max(uint64_t(1), step);
        for (size_t p = 1; p < 255; ++p)
            for (size_t c = 0; c < 3; ++c) {
                auto original = uint64_t(s.palette[p][c]);
                auto fade = step >= denominator ? original : original * step / denominator;
                palette_[p][c] = uint16_t(f.effect == 18 ? fade : original - fade);
            }
        palette_[0] = {0, 0, 0};
        palette_[255] = {65535, 65535, 65535};
    }
    rgb_.resize(screen_.size());
    for (size_t i = 0; i < screen_.size(); ++i) {
        auto &p = palette_[screen_[i]];
        rgb_[i] = uint32_t(p[0] >> 8) << 16 | uint32_t(p[1] >> 8) << 8 | (p[2] >> 8);
    }
}
void MoviePlayer::paintEffect(uint64_t tick) {
    const auto &s = movie_.segments[segment_];
    const auto &f = s.frames[frame_];
    if (f.dirty.empty())
        return;
    // Rectangle copies and tick order follow ALT31's 0x4041c0 dispatcher and
    // the pinned GPL mov-v1-effects.ts. Clip source/destination independently.
    auto copy = [&](MovieRect src, MovieRect dst) {
        src.top = std::max(0, src.top);
        src.left = std::max(0, src.left);
        src.bottom = std::min(s.height, src.bottom);
        src.right = std::min(s.width, src.right);
        dst.top = std::max(0, dst.top);
        dst.left = std::max(0, dst.left);
        dst.bottom = std::min(s.height, dst.bottom);
        dst.right = std::min(s.width, dst.right);
        if (src.empty() || dst.empty())
            return;
        const int width = std::min(dst.right - dst.left, s.width - src.left);
        const int height = std::min(dst.bottom - dst.top, s.height - src.top);
        for (int y = 0; y < height; ++y)
            std::copy_n(buffer_.pixels.begin() + size_t(src.top + y) * buffer_.w + src.left, width,
                        screen_.begin() + size_t(dst.top + y) * s.width + dst.left);
    };
    auto same = [&](MovieRect r) { copy(r, r); };
    const int n = std::max(s.rate, f.holdTicks);
    if (f.effect >= 16 || tick - entered_ >= uint64_t(n)) {
        if (effectApplied_ <= uint64_t(n))
            same(f.dirty);
        effectApplied_ = uint64_t(n) + 1;
        return;
    }
    const auto &r = f.dirty;
    const int t = r.top, l = r.left, b = r.bottom, right = r.right, w = right - l, h = b - t;
    const int dx = w / (2 * int64_t(n) + 2) + 1, dy = h / (2 * int64_t(n) + 2) + 1;
    const int sx = w / n + 1, sy = h / n + 1, cx = (l + right) / 2, cy = (t + b) / 2;
    for (; effectApplied_ <= tick - entered_; ++effectApplied_) {
        const int i = int(effectApplied_);
        switch (f.effect) {
        case 0:
            same({t, l + i * dx, b, l + (i + 1) * dx});
            same({t, right - (i + 1) * dx, b, right - i * dx});
            break;
        case 1:
            same({t, cx - (i + 1) * dx, b, cx - i * dx});
            same({t, cx + i * dx, b, cx + (i + 1) * dx});
            break;
        case 2:
            same({t + i * dy, l, t + (i + 1) * dy, right});
            same({b - (i + 1) * dy, l, b - i * dy, right});
            same({t, right - (i + 1) * dx, b, right - i * dx});
            same({t, l + i * dx, b, l + (i + 1) * dx});
            break;
        case 3:
            same({cy - (i + 1) * dy, cx - (i + 1) * dx, cy + (i + 1) * dy, cx + (i + 1) * dx});
            break;
        case 4:
            copy({b - (i + 1) * sy, l, b, right}, {t, l, t + (i + 1) * sy, right});
            break;
        case 5:
            copy({t, l, t + (i + 1) * sy, right}, {b - (i + 1) * sy, l, b, right});
            break;
        case 6:
            copy({t, right - (i + 1) * sx, b, right}, {t, l, b, l + (i + 1) * sx});
            break;
        case 7:
            copy({t, l, b, l + (i + 1) * sx}, {t, right - (i + 1) * sx, b, right});
            break;
        case 8:
            same({t + i * sy, l, t + (i + 1) * sy, right});
            break;
        case 9:
            same({b - (i + 1) * sy, l, b - i * sy, right});
            break;
        case 10:
            same({t, l + i * sx, b, l + (i + 1) * sx});
            break;
        case 11:
            same({t, right - (i + 1) * sx, b, right - i * sx});
            break;
        case 12:
            for (int y = 0; y < h; y += n + 1)
                same({t + y, l, t + y + i + 1, right});
            break;
        }
    }
}
void MoviePlayer::finish() {
    done_ = true;
    events_.push_back({MovieAudioEvent::Kind::Stop, {}, 0});
}
void MoviePlayer::skip() {
    if (!done_) {
        chain_.clear();
        call_ = return_ = false;
        finish();
    }
}
const std::vector<MovieHotspot> &MoviePlayer::hotspots() const {
    return movie_.segments[segment_].frames[frame_].hotspots;
}
const MovieHotspot *MoviePlayer::exitHotspot() const {
    const auto &frames = movie_.segments[segment_].frames;
    const auto distance = [&](const MovieHotspot &start) {
        if (start.action == 1 || start.action == 5)
            return 0;
        if (start.action != 2)
            return int(frames.size()) + 1;
        std::deque<std::pair<size_t, int>> pending;
        std::vector<bool> seen(frames.size());
        pending.emplace_back(std::clamp(start.target, 0, int(frames.size()) - 1), 1);
        while (!pending.empty()) {
            const auto [index, steps] = pending.front();
            pending.pop_front();
            if (seen[index])
                continue;
            seen[index] = true;
            const auto &frame = frames[index];
            const auto visit = [&](int action, int target) {
                if (action == 2)
                    pending.emplace_back(std::clamp(target, 0, int(frames.size()) - 1), steps + 1);
                return action == 1 || action == 5;
            };
            if (!(frame.flags & 16))
                for (const auto &region : frame.hotspots)
                    if (visit(region.action, region.target))
                        return steps;
            if ((frame.flags & (4 | 16)) || frame.hotspots.empty()) {
                if (frame.flags & 16) {
                    if (index + 1 < frames.size())
                        pending.emplace_back(index + 1, steps + 1);
                    else
                        return steps;
                } else if (visit(frame.action, frame.target))
                    return steps;
            }
        }
        return int(frames.size()) + 1;
    };
    const MovieHotspot *best = nullptr;
    int shortest = int(frames.size()) + 2;
    for (const auto &region : hotspots()) {
        const int steps = distance(region);
        if (steps < shortest) {
            shortest = steps;
            best = &region;
        }
    }
    return best;
}
bool MoviePlayer::waitsForClick() const {
    const auto &f = movie_.segments[segment_].frames[frame_];
    return !done_ && !f.hotspots.empty() && (f.flags & (4 | 16)) == 0;
}
bool MoviePlayer::click(int x, int y, uint64_t tick) {
    require(tick >= lastTick_, "movie click clock moved backwards");
    lastTick_ = tick;
    if (done_ || clicked_)
        return false;
    const auto &f = movie_.segments[segment_].frames[frame_];
    if (f.flags & 16)
        return false;
    for (const auto &hotspot : f.hotspots) {
        const auto &r = hotspot.rect;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
            clicked_ = hotspot;
            if (hotspot.sound)
                events_.push_back({MovieAudioEvent::Kind::Voice, {hotspot.sound}, 0});
            return true;
        }
    }
    return false;
}
bool MoviePlayer::advance(uint64_t tick, bool voicePlaying) {
    require(tick >= lastTick_, "movie clock moved backwards");
    lastTick_ = tick;
    if (done_)
        return false;
    render(tick);
    auto &s = movie_.segments[segment_];
    auto &f = s.frames[frame_];
    if (tick - entered_ < uint64_t(std::max(s.rate, f.holdTicks)) ||
        ((f.flags & 1) && voicePlaying))
        return true;
    if (clicked_) {
        const auto action = *clicked_;
        clicked_.reset();
        if (action.action == 1)
            finish();
        else if (action.action == 5) {
            return_ = true;
            finish();
        } else if (action.action == 3 || action.action == 4) {
            call_ = action.action == 4;
            returnFrame_ = std::clamp(action.target, 0, int(s.frames.size()) - 1);
            chain_ = action.chainTo;
            finish();
        } else {
            const auto target = size_t(std::clamp(action.target, 0, int(s.frames.size()) - 1));
            if (target != frame_) {
                frame_ = target;
                enterFrame(tick);
            }
        }
        return true;
    }
    if (waitsForClick())
        return true;
    if (f.flags & 16) {
        if (frame_ + 1 < s.frames.size()) {
            ++frame_;
            enterFrame(tick);
            return true;
        }
    } else if (f.action == 2) {
        frame_ = size_t(std::clamp(f.target, 0, int(s.frames.size()) - 1));
        enterFrame(tick);
        return true;
    } else if (f.action == 5) {
        return_ = true;
        finish();
        return true;
    } else if (f.action == 3 || f.action == 4) {
        call_ = f.action == 4;
        returnFrame_ = std::clamp(f.target, 0, int(s.frames.size()) - 1);
        chain_ = f.chainTo;
        finish();
        return true;
    }
    if (frame_ + 1 == s.frames.size() && segment_ + 1 < movie_.segments.size()) {
        ++segment_;
        enterSegment(tick);
    } else
        finish();
    return true;
}
std::vector<MovieAudioEvent> MoviePlayer::drainAudioEvents() {
    auto out = std::move(events_);
    events_.clear();
    return out;
}
std::optional<MovieDestination> MovieNavigation::next(const std::filesystem::path &path,
                                                      const MoviePlayer &player) {
    require(player.finished(), "movie navigation before completion");
    if (player.returnsToCaller()) {
        if (callers_.empty())
            return {};
        auto destination = callers_.back();
        callers_.pop_back();
        return destination;
    }
    if (player.chainTo().empty())
        return {};
    const std::filesystem::path leaf(player.chainTo());
    auto extension = leaf.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    require(!leaf.has_parent_path() && !leaf.has_root_name() && !leaf.is_absolute() &&
                extension == ".mov",
            "invalid chained movie name");
    if (player.callsMovie()) {
        require(callers_.size() < 5, "movie call stack exceeds five entries");
        callers_.push_back({path, player.segmentIndex(), size_t(player.returnFrame())});
    }
    return MovieDestination{path.parent_path() / leaf};
}
} // namespace dust
