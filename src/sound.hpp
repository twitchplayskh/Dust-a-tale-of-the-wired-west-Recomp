// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "dust.hpp"
#include <memory>
namespace dust {
struct SoundChunk {
    std::string name;
    uint32_t container = 0;
    bool loop = false;
};
struct SoundBank {
    File file;
    std::filesystem::path path;
    std::string refName;
    size_t oneshots = 0;
    std::vector<SoundChunk> chunks;
    // Actual container locations in authored order, including repetitions.
    std::vector<uint32_t> playlist;
    std::vector<std::string> warnings;
    static SoundBank open(const std::filesystem::path &);
    static SoundBank parse(File, std::filesystem::path sourcePath = {});
    Audio decode(uint32_t container) const;
    // The authored playlist joined at the highest source rate, using linear
    // resampling when its chunks differ. Playback loops this complete cycle.
    Audio themeAudio() const;
    // Record numbers are 1-based, and duplicate identifiers remain distinct.
    const SoundChunk &record(size_t oneBased) const;
    std::optional<uint32_t> find(const std::string &name) const;
};
struct SoundSelection {
    std::shared_ptr<const SoundBank> bank;
    uint32_t container = 0;
};
class SoundLibrary {
  public:
    std::shared_ptr<const SoundBank> open(const std::filesystem::path &);
    bool close(const std::string &fileOrRefName);
    std::shared_ptr<const SoundBank> bank(const std::string &fileOrRefName) const;
    std::shared_ptr<const SoundBank> theme(const std::string &refName = {}) const;
    std::optional<SoundSelection> sound(const std::string &identifier) const;
    const std::vector<std::shared_ptr<const SoundBank>> &banks() const {
        return banks_;
    }

  private:
    std::vector<std::shared_ptr<const SoundBank>> banks_;
};
} // namespace dust
