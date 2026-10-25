// SPDX-License-Identifier: GPL-3.0-only
#include "sound_window.hpp"
#include "sound_channels.hpp"
#include "sound_mixer.hpp"
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <mutex>
#include <thread>
namespace dust {
struct SoundOutput::Impl {
    PcmMixer mixer;
    mutable std::mutex mutex;
    HWAVEOUT device = nullptr;
    HANDLE ready = nullptr;
    std::atomic<bool> stopping = false;
    std::thread worker;
    // Four 10ms blocks bound gain-change latency without resetting playback.
    struct Block {
        WAVEHDR header{};
        std::array<int16_t, 441> pcm{};
        bool queued = false;
    };
    std::array<Block, 4> blocks;
    std::string failure;
    struct Effect {
        std::shared_ptr<const SoundBank> bank;
        std::string name;
        uint32_t priority = 0;
    };
    std::array<Effect, 2> effects;
    std::shared_ptr<const SoundBank> themeBank;
    std::vector<std::pair<uint64_t, std::string>> themeEnds;
    SoundOutputStats counts;
    mutable uint32_t lastPlayed = 0;
    mutable uint64_t playedEpoch = 0;
    Impl() {
        ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ready)
            throw std::runtime_error("cannot create native audio event");
        WAVEFORMATEX f{};
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = 1;
        f.nSamplesPerSec = PcmMixer::rate;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 2;
        f.nAvgBytesPerSec = PcmMixer::rate * 2;
        try {
            if (waveOutOpen(&device, WAVE_MAPPER, &f, DWORD_PTR(ready), 0, CALLBACK_EVENT))
                throw std::runtime_error("native sound device unavailable");
            const auto result = waveOutSetVolume(device, 0xffffffff);
            if (result && result != MMSYSERR_NOTSUPPORTED)
                throw std::runtime_error("cannot initialize native sound volume");
            for (auto &b : blocks) {
                b.header.lpData = reinterpret_cast<LPSTR>(b.pcm.data());
                b.header.dwBufferLength = DWORD(b.pcm.size() * sizeof(int16_t));
                if (waveOutPrepareHeader(device, &b.header, sizeof(b.header)))
                    throw std::runtime_error("cannot prepare native mixer buffer");
            }
            worker = std::thread([this] { pump(); });
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~Impl() {
        stopping = true;
        SetEvent(ready);
        if (worker.joinable())
            worker.join();
        cleanup();
    }
    void cleanup() {
        if (device) {
            waveOutReset(device);
            for (auto &b : blocks)
                if (b.header.dwFlags & WHDR_PREPARED)
                    waveOutUnprepareHeader(device, &b.header, sizeof(b.header));
            waveOutClose(device);
            device = nullptr;
        }
        if (ready) {
            CloseHandle(ready);
            ready = nullptr;
        }
    }
    void pump() {
        try {
            while (!stopping) {
                {
                    std::lock_guard lock(mutex);
                    for (auto &b : blocks) {
                        if (b.queued && !(b.header.dwFlags & WHDR_DONE))
                            continue;
                        mixer.render(b.pcm);
                        if (waveOutWrite(device, &b.header, sizeof(b.header)))
                            throw std::runtime_error("cannot submit native mixer buffer");
                        b.queued = true;
                    }
                }
                WaitForSingleObject(ready, 20);
            }
        } catch (const std::exception &e) {
            std::lock_guard lock(mutex);
            failure = e.what();
        }
    }
    uint64_t played() const {
        MMTIME t{};
        t.wType = TIME_SAMPLES;
        if (waveOutGetPosition(device, &t, sizeof(t)) != MMSYSERR_NOERROR ||
            t.wType != TIME_SAMPLES)
            return playedEpoch + lastPlayed;
        if (t.u.sample < lastPlayed)
            playedEpoch += uint64_t(1) << 32;
        lastPlayed = t.u.sample;
        return playedEpoch + lastPlayed;
    }
    void check() const {
        if (!failure.empty())
            throw std::runtime_error(failure);
    }
    bool playing(size_t channel) const {
        return mixer.channels[channel].playing(played());
    }
    int currentEffect() const {
        return (playing(1) ? effects[0].priority : 0) > (playing(2) ? effects[1].priority : 0) ? 0
                                                                                               : 1;
    }
};
SoundOutput::SoundOutput() : impl_(std::make_unique<Impl>()) {}
SoundOutput::~SoundOutput() = default;
void SoundOutput::setMasterGain(uint8_t value) {
    std::lock_guard lock(impl_->mutex);
    impl_->mixer.master = value;
}
void SoundOutput::setGameVolume(uint8_t value) {
    std::lock_guard lock(impl_->mutex);
    impl_->mixer.game = value;
}
void SoundOutput::playTheme(std::shared_ptr<const SoundBank> bank, uint8_t volume) {
    if (!bank)
        throw std::runtime_error("theme is not in an open sound bank");
    auto audio = bank->themeAudio();
    std::vector<std::pair<uint64_t, std::string>> ends;
    uint64_t end = 0;
    for (auto ref : bank->playlist) {
        auto part = bank->decode(ref);
        end += (uint64_t(part.pcm.size()) * audio.rate + part.rate - 1) / part.rate;
        ends.emplace_back(end, bank->record(ref).name);
    }
    std::lock_guard lock(impl_->mutex);
    impl_->check();
    impl_->counts.submittedSamples += audio.pcm.size();
    impl_->mixer.channels[0].play(std::move(audio), true, volume);
    impl_->themeBank = std::move(bank);
    impl_->themeEnds = std::move(ends);
    ++impl_->counts.themesStarted;
}
void SoundOutput::playSound(const SoundSelection &s, uint8_t volume, bool loop, uint32_t priority,
                            bool multiple) {
    if (!s.bank)
        throw std::runtime_error("sound is not in an open bank");
    if (!priority)
        priority = s.container;
    std::lock_guard lock(impl_->mutex);
    impl_->check();
    const int slot = soundChannel(priority, impl_->playing(1) ? impl_->effects[0].priority : 0,
                                  impl_->playing(2) ? impl_->effects[1].priority : 0, multiple);
    if (slot < 0)
        return;
    auto audio = s.bank->decode(s.container);
    impl_->counts.submittedSamples += audio.pcm.size();
    impl_->mixer.channels[slot + 1].play(std::move(audio), loop, volume);
    impl_->effects[slot] = {s.bank, s.bank->record(s.container).name, priority};
    ++impl_->counts.soundsStarted;
}
void SoundOutput::playVoice(Audio audio, uint8_t volume) {
    std::lock_guard lock(impl_->mutex);
    impl_->check();
    impl_->counts.submittedSamples += audio.pcm.size();
    impl_->mixer.channels[3].play(std::move(audio), false, volume);
    ++impl_->counts.voicesStarted;
}
void SoundOutput::setThemeVolume(uint8_t volume) {
    std::lock_guard lock(impl_->mutex);
    impl_->mixer.channels[0].gain = volume;
}
void SoundOutput::haltTheme() {
    std::lock_guard lock(impl_->mutex);
    impl_->mixer.channels[0].stop();
    impl_->themeBank.reset();
}
void SoundOutput::haltSound() {
    std::lock_guard lock(impl_->mutex);
    for (int i = 0; i < 2; ++i) {
        impl_->mixer.channels[i + 1].stop();
        impl_->effects[i].bank.reset();
    }
}
void SoundOutput::haltVoice() {
    std::lock_guard lock(impl_->mutex);
    impl_->mixer.channels[3].stop();
}
void SoundOutput::stopBank(const SoundBank &bank) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->themeBank.get() == &bank) {
        impl_->mixer.channels[0].stop();
        impl_->themeBank.reset();
    }
    for (int i = 0; i < 2; ++i)
        if (impl_->effects[i].bank.get() == &bank) {
            impl_->mixer.channels[i + 1].stop();
            impl_->effects[i].bank.reset();
        }
}
void SoundOutput::stopAll() {
    haltTheme();
    haltSound();
    haltVoice();
}
bool SoundOutput::themePlaying() const {
    std::lock_guard lock(impl_->mutex);
    impl_->check();
    return impl_->playing(0);
}
bool SoundOutput::soundPlaying() const {
    std::lock_guard lock(impl_->mutex);
    impl_->check();
    return impl_->playing(1) || impl_->playing(2);
}
bool SoundOutput::voicePlaying() const {
    std::lock_guard lock(impl_->mutex);
    impl_->check();
    return impl_->playing(3);
}
SoundOutputStats SoundOutput::stats() const {
    std::lock_guard lock(impl_->mutex);
    auto r = impl_->counts;
    r.observedPlayedSamples = impl_->played();
    r.themeGain = impl_->mixer.channels[0].gain;
    r.voiceGain = impl_->mixer.channels[3].gain;
    r.channelPeaks = impl_->mixer.peaks;
    return r;
}
std::string SoundOutput::currentTheme(bool chunk) const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->playing(0) || !impl_->themeBank)
        return "none";
    if (!chunk)
        return impl_->themeBank->refName;
    const auto &channel = impl_->mixer.channels[0];
    const auto played = impl_->played();
    const auto at = (played > channel.startFrame
                         ? (played - channel.startFrame) * channel.audio.rate / PcmMixer::rate
                         : 0) %
                    channel.audio.pcm.size();
    for (const auto &[end, name] : impl_->themeEnds)
        if (at < end)
            return name;
    return "none";
}
std::string SoundOutput::currentSound() const {
    std::lock_guard lock(impl_->mutex);
    const int slot = impl_->currentEffect();
    return impl_->playing(slot + 1) ? impl_->effects[slot].name : "none";
}
void SoundOutput::setSoundVolume(uint8_t value) {
    std::lock_guard lock(impl_->mutex);
    impl_->mixer.channels[impl_->currentEffect() + 1].gain = value;
}
void SoundOutput::setSoundLoop(bool value) {
    std::lock_guard lock(impl_->mutex);
    impl_->mixer.channels[impl_->currentEffect() + 1].loop(value, impl_->played());
}
} // namespace dust
