// SPDX-License-Identifier: GPL-3.0-only
// DF1 SND layout informed by pinned dreamREfactory df/snd.ts (GPL-3.0).
#include "sound.hpp"
#include <algorithm>
#include <stdexcept>
namespace dust {
namespace {
std::string fold(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c + ('a' - 'A'));
    return s;
}
void require(bool yes, const char *why) {
    if (!yes)
        throw std::runtime_error(why);
}
} // namespace
SoundBank SoundBank::open(const std::filesystem::path &path) {
    return parse(File::open(path), path);
}
SoundBank SoundBank::parse(File file, std::filesystem::path path) {
    SoundBank bank;
    bank.path = std::move(path);
    bank.file = std::move(file);
    auto h = bank.file.at(0);
    require(u32(h, 2) == 1, "unsupported sound bank version");
    int ones = i16(h, 0x18), loops = i16(h, 0x1a), order = i16(h, 0x1c);
    require(ones >= 0 && loops >= 0 && size_t(ones + loops) + 1 == bank.file.containers.size(),
            "sound bank split does not account for containers");
    require(order >= 0 && order <= 64, "sound bank playlist exceeds header");
    bank.refName = pstr(h, 158, 31);
    if (bank.refName.empty())
        bank.warnings.push_back("unnamed bank");
    bank.oneshots = size_t(ones);
    for (size_t i = 0; i < size_t(ones + loops); ++i) {
        auto name = pstr(h, 186 + i * 24, 23);
        require(!name.empty(), "empty sound identifier");
        bank.file.at(i + 1);
        bank.chunks.push_back({name, uint32_t(i + 1), i >= bank.oneshots});
    }
    for (int i = 0; i < order; ++i) {
        int loop = i16(h, 0x1e + size_t(i) * 2);
        if (loop >= 1 && loop <= loops)
            bank.playlist.push_back(uint32_t(ones + loop));
        else
            bank.warnings.push_back("playlist entry outside loop bank (skipped)");
    }
    return bank;
}
Audio SoundBank::decode(uint32_t container) const {
    require(container > 0 && container <= chunks.size(), "sound index outside bank");
    return audio(file.at(container));
}
Audio SoundBank::themeAudio() const {
    require(!playlist.empty(), "sound bank has no theme playlist");
    std::vector<Audio> parts;
    uint32_t rate = 0;
    for (auto ref : playlist) {
        parts.push_back(decode(ref));
        rate = std::max(rate, parts.back().rate);
    }
    Audio out;
    out.rate = rate;
    for (auto &part : parts) {
        require(part.rate > 0 && !part.pcm.empty(), "invalid theme chunk");
        uint64_t count = (uint64_t(part.pcm.size()) * rate + part.rate - 1) / part.rate;
        require(count <= 100000000 && out.pcm.size() <= 100000000 - count,
                "theme playlist exceeds native sample budget");
        for (uint64_t i = 0; i < count; ++i) {
            auto source = i * part.rate;
            auto index = size_t(source / rate);
            auto fraction = int64_t(source % rate);
            int64_t a = part.pcm[std::min(index, part.pcm.size() - 1)],
                    b = part.pcm[std::min(index + 1, part.pcm.size() - 1)];
            out.pcm.push_back(int16_t(a + (b - a) * fraction / rate));
        }
    }
    return out;
}
const SoundChunk &SoundBank::record(size_t oneBased) const {
    require(oneBased > 0 && oneBased <= chunks.size(), "sound record outside bank");
    return chunks[oneBased - 1];
}
std::optional<uint32_t> SoundBank::find(const std::string &name) const {
    // ALT31 0x40f210 scans records forward; duplicate names select the first.
    auto key = fold(name);
    for (size_t i = 0; i < oneshots; ++i)
        if (fold(chunks[i].name) == key)
            return chunks[i].container;
    return {};
}
std::shared_ptr<const SoundBank> SoundLibrary::open(const std::filesystem::path &path) {
    auto key = fold(path.filename().string());
    for (auto &b : banks_)
        if (fold(b->path.filename().string()) == key)
            return b;
    auto bank = std::make_shared<SoundBank>(SoundBank::open(path));
    banks_.push_back(bank);
    return bank;
}
bool SoundLibrary::close(const std::string &name) {
    auto key = fold(name);
    for (auto it = banks_.begin(); it != banks_.end(); ++it)
        if (fold((*it)->refName) == key) {
            banks_.erase(it);
            return true;
        }
    return false;
}
std::shared_ptr<const SoundBank> SoundLibrary::theme(const std::string &name) const {
    if (!name.empty()) {
        for (const auto &b : banks_)
            if (fold(b->refName) == fold(name))
                return b->playlist.empty() ? nullptr : b;
        return nullptr;
    }
    for (auto &b : banks_)
        if (!b->playlist.empty())
            return b;
    return {};
}
std::shared_ptr<const SoundBank> SoundLibrary::bank(const std::string &name) const {
    auto key = fold(name);
    for (auto &b : banks_)
        if (fold(b->path.filename().string()) == key)
            return b;
    for (auto &b : banks_)
        if (fold(b->refName) == key)
            return b;
    return {};
}
std::optional<SoundSelection> SoundLibrary::sound(const std::string &identifier) const {
    const auto key = identifier;
    for (auto &b : banks_)
        if (auto container = b->find(key))
            return SoundSelection{b, *container};
    return {};
}
} // namespace dust
