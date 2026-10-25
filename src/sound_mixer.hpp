// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "sound.hpp"
#include <array>
#include <algorithm>
#include <span>
#include <stdexcept>
namespace dust {
// Gains in PCM prevent legacy waveOut session volume coupling the channels.
// Synchronization and native buffers are owned by SoundOutput.
struct PcmMixer {
    static constexpr uint32_t rate = 44100;
    struct Channel {
        Audio audio;
        uint64_t phase = 0, endFrame = 0, startFrame = 0;
        bool active = false, repeat = false, renderedEnd = false, started = false;
        uint8_t gain = 255;
        void play(Audio source, bool loop, uint8_t volume) {
            if (!source.rate || source.pcm.empty())
                throw std::runtime_error("invalid mixer source");
            audio = std::move(source);
            phase = endFrame = 0;
            active = true;
            renderedEnd = false;
            started = false;
            repeat = loop;
            gain = volume;
        }
        void stop() {
            active = false;
            audio = {};
        }
        bool playing(uint64_t played) const {
            return active && (!renderedEnd || played < endFrame);
        }
        void loop(bool value, uint64_t played) {
            if (!playing(played))
                return;
            repeat = value;
            if (value && renderedEnd) {
                renderedEnd = false;
                phase = 0;
            }
        }
    };
    std::array<Channel, 4> channels; // Theme, effects 0/1, voice.
    uint8_t master = 255, game = 255;
    uint64_t submitted = 0;
    std::array<int, 4> peaks{};
    void render(std::span<int16_t> output) {
        peaks = {};
        for (size_t frame = 0; frame < output.size(); ++frame) {
            int mixed = 0;
            for (size_t i = 0; i < channels.size(); ++i) {
                auto &c = channels[i];
                if (!c.active || c.renderedEnd)
                    continue;
                if (!c.started) {
                    c.startFrame = submitted + frame;
                    c.started = true;
                }
                const auto index = size_t(c.phase / rate);
                const auto next = index + 1 < c.audio.pcm.size() ? index + 1 : c.repeat ? 0 : index;
                const int a = c.audio.pcm[index], b = c.audio.pcm[next];
                const auto sample = a + int(int64_t(b - a) * int64_t(c.phase % rate) / rate);
                const int scaled =
                    int(int64_t(sample) * c.gain * master * game / (255ll * 255 * 255));
                peaks[i] = std::max(peaks[i], std::abs(scaled));
                mixed += scaled;
                c.phase += c.audio.rate;
                const auto length = uint64_t(c.audio.pcm.size()) * rate;
                if (c.phase >= length) {
                    if (c.repeat)
                        c.phase %= length;
                    else {
                        c.renderedEnd = true;
                        c.endFrame = submitted + frame + 1;
                    }
                }
            }
            output[frame] = int16_t(std::clamp(mixed, -32768, 32767));
        }
        submitted += output.size();
    }
};
} // namespace dust
