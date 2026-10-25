// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "vm.hpp"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace dust {
class DreamRandom {
  public:
    explicit DreamRandom(uint32_t seed = 0);
    uint32_t next();
    int32_t scaled(int32_t range);

  private:
    friend class DialogueSession;
    std::array<uint32_t, 55> table{};
    size_t cursor = 0;
};
// Original choice wait counts observed clock changes, not elapsed tick deltas.
class PuppetEventClock {
  public:
    void reset(int32_t timeout, uint32_t now);
    bool observe(uint32_t now);
    uint32_t current() const {
        return counter;
    }

  private:
    uint32_t counter = 0, last = 0;
    int32_t deadline = INT32_MAX;
};
// A host continuation: the VM stack stays on the worker while the UI owns audio/input.
// Keep VM/program alive. Access VM only after finished()/cancel(), or while the
// sole host thread holds an unanswered request (the worker is parked).
class DialogueSession {
  public:
    struct Choice {
        std::string text;
        int32_t id;
    };
    struct Request {
        enum class Kind { Speech, Choice, Text } kind;
        uint64_t serial;
        std::string speech;
        std::vector<Choice> choices;
        int32_t waitTicks = -1;
        uint32_t delayMillis = 0;
        enum class Origin { Script, Idle, Replay, Player, ReplayPlayer } origin = Origin::Script;
        int selected = -1;
    };
    struct ChoiceMemory {
        std::vector<Choice> choices;
        int selected = -1;
    };
    struct Statistics {
        size_t idles = 0, replays = 0, replayLines = 0, playerLines = 0;
    };
    using SpeechLookup = std::function<bool(const std::string &)>;
    explicit DialogueSession(ScriptVM &, SpeechLookup = {}, std::array<PuppetIdle, 4> idles = {},
                             uint32_t randomSeed = 0, SpeechLookup playerLookup = {});
    ~DialogueSession();
    // A session represents one loaded puppet. Choices/interruption survive handler calls.
    void start(const ScriptProgram &, const std::string &handler,
               const std::vector<ScriptValue> &args = {}, const std::string &me = "",
               const std::string &target = "");
    // Runs host context setup and routed VM work on the session's single worker.
    void start(std::function<void()> task);
    // Called by the host while opening/closing a puppet on the script thread.
    // Keep the shared game's RNG, but reset conversation-local state.
    void loadPuppet(std::array<PuppetIdle, 4>, SpeechLookup known = {}, SpeechLookup player = {});
    // Optional resource precondition for integrated hosts. Configure while idle;
    // isolated VM/dialogue models can remain independent of a file loader.
    void setPuppetPresence(std::function<bool()>);
    int16_t parameter(size_t slot) const;
    std::vector<uint32_t> checkpoint() const;
    void restoreCheckpoint(const std::vector<uint32_t> &);
    void setParameter(size_t slot, int32_t value);
    // The original subtitle wrapper mutates its Pascal length before computing
    // the literal-text pause. The text host supplies that measured first length.
    bool textLength(uint64_t serial, size_t firstLength);
    std::optional<Request> request() const;
    bool answer(uint64_t serial, int32_t value = 0);
    bool choose(uint64_t serial, size_t row);
    bool interrupt(uint64_t serial);
    bool replay(uint64_t choiceSerial);
    size_t replaySize() const;
    ChoiceMemory previousChoice() const;
    Statistics statistics() const;
    // Call after processing input. Clocks are injected for deterministic tests.
    void advance(uint32_t ticks, uint32_t milliseconds);
    size_t suppressedSpeech() const;
    bool finished() const;
    std::string error() const;
    void cancel();

  private:
    ScriptVM &vm;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::optional<Request> pending;
    std::optional<Request> suspendedChoice;
    std::array<Choice, 5> choiceSlots;
    size_t choiceCount = 0;
    SpeechLookup playerLookup;
    std::function<bool()> puppetPresence;
    void checkPuppet() const;
    int playerRow = -1;
    int32_t playerValue = 0;
    ChoiceMemory previous;
    std::vector<std::string> replayRecords, activeReplay;
    size_t replayIndex = 0, idleCursor = 0;
    struct IdleChannel {
        PuppetIdle rule;
        bool available;
        int32_t interval;
        uint32_t last;
    };
    std::array<IdleChannel, 4> idleChannels;
    DreamRandom random;
    Statistics stats;
    uint64_t nextSerial = 0;
    bool stopped = true, done = true;
    bool interrupted = false, clockStarted = false;
    std::array<uint16_t, 8> puppetParams{0, 128, 250, 251, 888, 12, 0, 0};
    bool idleDisabledForWait = false;
    size_t suppressed = 0;
    PuppetEventClock eventClock;
    uint32_t textDeadline = 0;
    uint32_t textStartedMillis = 0;
    int32_t reply = 0;
    std::string failure;
    int32_t wait(Request);
    // These helpers require the mutex.
    void resolve(int32_t value, int selected = -1);
    int32_t idleInterval(const PuppetIdle &);
    void auxiliary(const std::string &, Request::Origin);
    void finishAuxiliary(bool wasInterrupted);
    void select(size_t row);
};
} // namespace dust
