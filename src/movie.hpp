// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "dust.hpp"
#include <deque>
namespace dust {
struct MovieRect {
    int top, left, bottom, right;
    bool empty() const;
};
struct MovieHotspot {
    MovieRect rect{};
    int action = 1, target = 0;
    uint32_t sound = 0;
    std::string chainTo;
};
struct MovieFrame {
    uint32_t picture = 0, sound = 0;
    int32_t holdTicks = 0;
    int effect = 16, action = 1, target = 0, flags = 0;
    MovieRect dirty{};
    std::string chainTo;
    std::vector<MovieHotspot> hotspots;
};
struct MovieSegment {
    size_t header = 0;
    int width = 0, height = 0, rate = 0, actionFrame1 = -1, actionFrame2 = -1;
    std::array<std::array<uint16_t, 3>, 256> palette{};
    std::vector<MovieFrame> frames;
    std::vector<uint32_t> bed;
    size_t bedLoopFrom = 0;
    bool replacesBed = false;
};
struct Movie {
    File file;
    std::vector<MovieSegment> segments;
    static Movie open(const std::filesystem::path &);
    Audio sound(uint32_t container) const;
};
struct MovieAudioEvent {
    enum class Kind { Voice, BedStart, Stop } kind;
    // BedStart provides the segment playlist and loop point; Voice one container.
    std::vector<uint32_t> containers;
    size_t loopFrom = 0;
};
// Pure playback state: the UI owns clocks and audio devices. Time is elapsed
// 60-Hz ticks since start. At most one authored frame advances per update.
class MoviePlayer {
  public:
    explicit MoviePlayer(const Movie &);
    void start(uint64_t tick = 0, size_t segment = 0, size_t frame = 0);
    bool advance(uint64_t tick, bool voicePlaying);
    void skip();
    bool click(int x, int y, uint64_t tick);
    bool waitsForClick() const;
    bool returnsToCaller() const {
        return return_;
    }
    bool callsMovie() const {
        return call_;
    }
    int returnFrame() const {
        return returnFrame_;
    }
    const std::vector<MovieHotspot> &hotspots() const;
    // Diagnostic navigation: prefer the shortest authored route to an exit.
    const MovieHotspot *exitHotspot() const;
    bool finished() const {
        return done_;
    }
    uint32_t actionFrames() const {
        return actionFrames_;
    }
    size_t segmentIndex() const {
        return segment_;
    }
    size_t frameIndex() const {
        return frame_;
    }
    const std::string &chainTo() const {
        return chain_;
    }
    const std::vector<uint32_t> &rgb() const {
        return rgb_;
    }
    int width() const;
    int height() const;
    std::vector<MovieAudioEvent> drainAudioEvents();

  private:
    const Movie &movie_;
    size_t segment_ = 0, frame_ = 0;
    bool done_ = true, paletteUnset_ = true, return_ = false, call_ = false;
    int returnFrame_ = 0;
    uint32_t actionFrames_ = 0;
    uint64_t entered_ = 0, lastTick_ = 0;
    uint64_t effectApplied_ = 0;
    FrameBuffer buffer_;
    Bytes screen_;
    std::array<std::array<uint16_t, 3>, 256> palette_{};
    std::vector<uint32_t> rgb_;
    std::vector<MovieAudioEvent> events_;
    std::string chain_;
    std::optional<MovieHotspot> clicked_;
    void enterSegment(uint64_t, size_t initialFrame = 0);
    void enterFrame(uint64_t);
    void render(uint64_t);
    void paintEffect(uint64_t);
    void finish();
};
struct MovieDestination {
    std::filesystem::path path;
    size_t segment = 0, frame = 0;
};
// DF1 movie calls retain a five-deep (movie, segment, return-frame) stack.
// Shared by the native window and deterministic gameplay probe.
class MovieNavigation {
  public:
    std::optional<MovieDestination> next(const std::filesystem::path &, const MoviePlayer &);

  private:
    std::vector<MovieDestination> callers_;
};
} // namespace dust
