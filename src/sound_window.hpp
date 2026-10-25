// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "sound.hpp"
#include <memory>
#include <array>
namespace dust {
struct SoundOutputStats {
    size_t themesStarted = 0, soundsStarted = 0, voicesStarted = 0;
    uint64_t submittedSamples = 0, observedPlayedSamples = 0;
    uint8_t themeGain = 255, voiceGain = 255;
    std::array<int, 4> channelPeaks{};
};
// Nonblocking waveOut submission; all methods are called on the owning UI
// thread. A short-buffer PCM mixer isolates music/effect/voice gains on one
// native device, including drivers sharing per-handle session volume.
class SoundOutput {
  public:
    SoundOutput();
    ~SoundOutput();
    SoundOutput(const SoundOutput &) = delete;
    SoundOutput &operator=(const SoundOutput &) = delete;
    void setMasterGain(uint8_t);
    void setGameVolume(uint8_t);
    void playTheme(std::shared_ptr<const SoundBank>, uint8_t volume = 255);
    void playSound(const SoundSelection &, uint8_t volume = 255, bool loop = false,
                   uint32_t priority = 0, bool multiple = false);
    void playVoice(Audio, uint8_t volume = 255);
    void setThemeVolume(uint8_t);
    void haltTheme();
    void haltSound();
    void haltVoice();
    void stopBank(const SoundBank &);
    void stopAll();
    bool themePlaying() const;
    bool soundPlaying() const;
    bool voicePlaying() const;
    std::string currentTheme(bool chunk = false) const;
    std::string currentSound() const;
    void setSoundVolume(uint8_t);
    void setSoundLoop(bool);
    SoundOutputStats stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dust
