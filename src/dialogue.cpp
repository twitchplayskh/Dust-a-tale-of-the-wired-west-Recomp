// SPDX-License-Identifier: GPL-3.0-only
#include "dialogue.hpp"
#include <bit>
namespace dust {
DreamRandom::DreamRandom(uint32_t seed) {
    table[0] = seed;
    for (size_t i = 1; i < table.size(); ++i)
        table[i] = (table[i - 1] * 31u + 1u) & 0x7fffffffu;
}
uint32_t DreamRandom::next() {
    cursor = (cursor + 1) % table.size();
    uint32_t sum = table[(cursor + 54) % 55] + table[(cursor + 23) % 55];
    table[cursor] = sum & 0x7fffffffu;
    return sum;
}
int32_t DreamRandom::scaled(int32_t range) {
    uint32_t draw = std::min(next() & 32767u, 32766u);
    auto product = std::bit_cast<int32_t>(draw * uint32_t(range));
    return product / 32767 + 1;
}
void PuppetEventClock::reset(int32_t timeout, uint32_t now) {
    counter = last = now;
    deadline = timeout < 0 ? INT32_MAX : std::bit_cast<int32_t>(now + uint32_t(timeout));
}
bool PuppetEventClock::observe(uint32_t now) {
    if (now != last) {
        ++counter;
        last = now;
    }
    return std::bit_cast<int32_t>(counter) > deadline;
}
namespace {
struct Cancelled {};
void arguments(const std::vector<ScriptValue> &a, size_t count) {
    if (a.size() != count)
        throw ScriptError(14, "invalid dialogue argument count");
}
void type(const ScriptValue &v, ScriptValue::Type t) {
    if (v.type != t)
        throw ScriptError(14, "invalid dialogue argument type");
}
} // namespace
DialogueSession::DialogueSession(ScriptVM &state, SpeechLookup known,
                                 std::array<PuppetIdle, 4> idles, uint32_t randomSeed,
                                 SpeechLookup playerVoice)
    : vm(state), playerLookup(std::move(playerVoice)), random(randomSeed) {
    for (size_t i = 0; i < idles.size(); ++i)
        idleChannels[i] = {idles[i], !idles[i].speech.empty() && (!known || known(idles[i].speech)),
                           0, 0};
    vm.bind(20001, [this](const auto &a, bool) {
        arguments(a, 1);
        type(a[0], ScriptValue::Type::Integer);
        std::lock_guard lock(mutex);
        return ScriptValue::integer(random.scaled(a[0].number));
    });
    vm.bind(16049, [this](const auto &a, bool wants) {
        arguments(a, wants ? 1 : 2);
        for (const auto &v : a)
            type(v, ScriptValue::Type::Integer);
        if (a[0].number < 1 || a[0].number > 8)
            throw ScriptError(14, "invalid puppetparam slot");
        std::lock_guard lock(mutex);
        if (!wants)
            puppetParams[size_t(a[0].number - 1)] = uint16_t(a[1].number);
        return ScriptValue::integer(std::bit_cast<int16_t>(puppetParams[size_t(a[0].number - 1)]));
    });
    vm.bind(12043, [this, known](const auto &a, bool) {
        checkPuppet();
        arguments(a, 1);
        type(a[0], ScriptValue::Type::String);
        if (known && !known(a[0].text)) {
            Request text{Request::Kind::Text, 0, a[0].text, {}};
            text.delayMillis = uint32_t((60 + (a[0].text.size() >> 1)) * 50 / 3);
            wait(std::move(text));
        } else {
            {
                std::lock_guard lock(mutex);
                if (replayRecords.size() < 3)
                    replayRecords.push_back(a[0].text);
                if (interrupted) {
                    ++suppressed;
                    return ScriptValue::integer(0);
                }
            }
            if (wait({Request::Kind::Speech, 0, a[0].text, {}}) == -1) {
                std::lock_guard lock(mutex);
                interrupted = true;
            }
        }
        return ScriptValue::integer(0);
    });
    vm.bind(12041, [this](const auto &a, bool) {
        checkPuppet();
        arguments(a, 0);
        choiceCount = 0;
        return ScriptValue::integer(0);
    });
    vm.bind(12044, [this](const auto &a, bool) {
        checkPuppet();
        arguments(a, 2);
        type(a[0], ScriptValue::Type::String);
        type(a[1], ScriptValue::Type::Integer);
        if (choiceCount >= 5)
            throw ScriptError(0x2e, "dialogue choice capacity exceeded");
        choiceSlots[choiceCount++] = {a[0].text, a[1].number};
        return ScriptValue::integer(0);
    });
    vm.bind(12083, [this](const auto &a, bool) {
        checkPuppet();
        arguments(a, 0);
        std::lock_guard lock(mutex);
        if (choiceCount >= 2)
            for (size_t i = 0; i < choiceCount * 5; ++i) {
                const auto first = size_t(random.scaled(int32_t(choiceCount)) - 1);
                const auto second = size_t(random.scaled(int32_t(choiceCount)) - 1);
                std::swap(choiceSlots[first], choiceSlots[second]);
            }
        return ScriptValue::integer(0);
    });
    vm.bind(20107, [this](const auto &a, bool) {
        checkPuppet();
        arguments(a, 0);
        return ScriptValue::integer(int32_t(choiceCount));
    });
    vm.bind(20028, [this](const auto &a, bool) {
        checkPuppet();
        arguments(a, 1);
        type(a[0], ScriptValue::Type::Integer);
        {
            std::lock_guard lock(mutex);
            interrupted = false;
        }
        Request event{
            Request::Kind::Choice, 0, {}, {choiceSlots.begin(), choiceSlots.begin() + choiceCount}};
        event.waitTicks = a[0].number;
        return ScriptValue::integer(wait(std::move(event)));
    });
}
DialogueSession::~DialogueSession() {
    cancel();
    for (auto opcode :
         std::array<uint16_t, 8>{12043, 12041, 12044, 20028, 20001, 16049, 20107, 12083})
        vm.unbind(opcode);
}
void DialogueSession::setPuppetPresence(std::function<bool()> predicate) {
    std::lock_guard lock(mutex);
    if (!done)
        throw std::runtime_error("cannot change puppet presence during an active script");
    puppetPresence = std::move(predicate);
}
void DialogueSession::checkPuppet() const {
    if (puppetPresence && !puppetPresence())
        throw ScriptError(0x2d, "no puppet open");
}
std::vector<uint32_t> DialogueSession::checkpoint() const {
    std::lock_guard lock(mutex);
    std::vector<uint32_t> values(puppetParams.begin(), puppetParams.end());
    values.insert(values.end(), random.table.begin(), random.table.end());
    values.push_back(uint32_t(random.cursor));
    return values;
}
void DialogueSession::restoreCheckpoint(const std::vector<uint32_t> &values) {
    if (values.size() != 64 || values.back() >= 55 ||
        std::any_of(values.begin(), values.begin() + 8, [](uint32_t v) { return v > 65535; }))
        throw std::runtime_error("invalid dialogue checkpoint");
    std::lock_guard lock(mutex);
    for (size_t i = 0; i < 8; ++i)
        puppetParams[i] = uint16_t(values[i]);
    std::copy_n(values.begin() + 8, 55, random.table.begin());
    random.cursor = values.back();
}
int16_t DialogueSession::parameter(size_t slot) const {
    std::lock_guard lock(mutex);
    if (slot < 1 || slot > 8)
        throw ScriptError(14, "invalid puppet parameter");
    return std::bit_cast<int16_t>(puppetParams[slot - 1]);
}
void DialogueSession::setParameter(size_t slot, int32_t value) {
    std::lock_guard lock(mutex);
    if (slot < 1 || slot > 8)
        throw ScriptError(14, "invalid puppet parameter");
    puppetParams[slot - 1] = uint16_t(value);
}
bool DialogueSession::textLength(uint64_t serial, size_t firstLength) {
    std::lock_guard lock(mutex);
    if (!pending || pending->serial != serial || pending->kind != Request::Kind::Text ||
        firstLength > pending->speech.size())
        return false;
    pending->delayMillis = uint32_t((60 + (firstLength >> 1)) * 50 / 3);
    if (clockStarted)
        textDeadline = textStartedMillis + pending->delayMillis;
    return true;
}
void DialogueSession::loadPuppet(std::array<PuppetIdle, 4> idles, SpeechLookup known,
                                 SpeechLookup player) {
    std::lock_guard lock(mutex);
    if (pending)
        throw std::runtime_error("cannot replace a puppet with an unanswered dialogue request");
    playerLookup = std::move(player);
    choiceCount = 0;
    previous = {};
    replayRecords.clear();
    activeReplay.clear();
    replayIndex = idleCursor = 0;
    suspendedChoice.reset();
    interrupted = false;
    clockStarted = false;
    idleDisabledForWait = false;
    stats = {};
    suppressed = 0;
    for (size_t i = 0; i < idles.size(); ++i)
        idleChannels[i] = {idles[i], !idles[i].speech.empty() && (!known || known(idles[i].speech)),
                           0, 0};
}
int32_t DialogueSession::wait(Request event) {
    std::unique_lock lock(mutex);
    if (stopped)
        throw Cancelled{};
    event.serial = ++nextSerial;
    clockStarted = false;
    idleCursor = 0;
    pending = std::move(event);
    wake.wait(lock, [&] { return stopped || !pending; });
    if (stopped)
        throw Cancelled{};
    return reply;
}
void DialogueSession::start(const ScriptProgram &program, const std::string &handler,
                            const std::vector<ScriptValue> &args, const std::string &me,
                            const std::string &target) {
    start([this, &program, handler, args, me, target] {
        if (!vm.run(program, handler, args, me, target).handled)
            throw std::runtime_error("script handler not found");
    });
}
void DialogueSession::start(std::function<void()> task) {
    cancel();
    {
        std::lock_guard lock(mutex);
        stopped = false;
        done = false;
        failure.clear();
        suppressed = 0;
        stats = {};
    }
    worker = std::thread([this, task = std::move(task)] {
        std::string message;
        try {
            task();
        } catch (const Cancelled &) {
        } catch (const std::exception &e) {
            message = e.what();
        }
        std::lock_guard lock(mutex);
        failure = std::move(message);
        done = true;
    });
}
std::optional<DialogueSession::Request> DialogueSession::request() const {
    std::lock_guard lock(mutex);
    return pending;
}
bool DialogueSession::answer(uint64_t serial, int32_t value) {
    std::lock_guard lock(mutex);
    if (!pending || pending->serial != serial)
        return false;
    if (pending->kind == Request::Kind::Text ||
        (pending->kind == Request::Kind::Speech && value != 0))
        return false;
    if (pending->kind == Request::Kind::Choice && value != -1 &&
        std::none_of(pending->choices.begin(), pending->choices.end(),
                     [&](const auto &c) { return c.id == value; }))
        return false;
    if (pending->kind == Request::Kind::Speech && pending->origin != Request::Origin::Script) {
        finishAuxiliary(false);
        return true;
    }
    int selected = -1;
    if (pending->kind == Request::Kind::Choice && value != -1) {
        auto it = std::find_if(pending->choices.begin(), pending->choices.end(),
                               [&](const auto &c) { return c.id == value; });
        selected = int(it - pending->choices.begin());
    }
    if (selected >= 0)
        select(size_t(selected));
    else
        resolve(value);
    return true;
}
bool DialogueSession::choose(uint64_t serial, size_t row) {
    std::lock_guard lock(mutex);
    if (!pending || pending->serial != serial || pending->kind != Request::Kind::Choice ||
        row >= pending->choices.size())
        return false;
    select(row);
    return true;
}
void DialogueSession::select(size_t row) {
    if (playerLookup && playerLookup(pending->choices[row].text)) {
        playerRow = int(row);
        playerValue = pending->choices[row].id;
        auto text = pending->choices[row].text;
        suspendedChoice = pending;
        auxiliary(text, Request::Origin::Player);
    } else
        resolve(pending->choices[row].id, int(row));
}
void DialogueSession::resolve(int32_t value, int selected) {
    if (pending && pending->kind == Request::Kind::Choice) {
        previous = {pending->choices, selected};
        replayRecords.clear();
    }
    reply = value;
    pending.reset();
    wake.notify_one();
}
bool DialogueSession::interrupt(uint64_t serial) {
    std::lock_guard lock(mutex);
    if (!pending || pending->serial != serial || pending->kind != Request::Kind::Speech)
        return false;
    if (pending->origin != Request::Origin::Script) {
        finishAuxiliary(true);
        return true;
    }
    resolve(-1);
    return true;
}
void DialogueSession::advance(uint32_t ticks, uint32_t milliseconds) {
    std::lock_guard lock(mutex);
    if (!pending || pending->kind == Request::Kind::Speech)
        return;
    if (!clockStarted) {
        eventClock.reset(pending->waitTicks, ticks);
        textStartedMillis = milliseconds;
        textDeadline = textStartedMillis + pending->delayMillis;
        clockStarted = true;
        if (pending->kind == Request::Kind::Choice) {
            idleDisabledForWait = puppetParams[7] != 0;
            for (auto &channel : idleChannels) {
                channel.interval = idleInterval(channel.rule); // draw even for absent records
                channel.last = ticks;
            }
        }
    }
    if (pending->kind == Request::Kind::Choice) {
        for (; idleCursor < idleChannels.size(); ++idleCursor) {
            auto &channel = idleChannels[idleCursor];
            auto elapsed = std::bit_cast<int32_t>(eventClock.current() - channel.last);
            if (!idleDisabledForWait && channel.available && elapsed >= channel.interval) {
                channel.interval = idleInterval(channel.rule);
                channel.last = eventClock.current();
                ++idleCursor;
                suspendedChoice = pending;
                auxiliary(channel.rule.speech, Request::Origin::Idle);
                ++stats.idles;
                return;
            }
        }
        idleCursor = 0;
        if (eventClock.observe(ticks))
            resolve(-2);
    } else if (milliseconds >= textDeadline)
        resolve(0);
}
int32_t DialogueSession::idleInterval(const PuppetIdle &rule) {
    auto range = std::bit_cast<int32_t>(uint32_t(rule.maximum) - uint32_t(rule.minimum));
    return std::bit_cast<int32_t>(uint32_t(rule.minimum) + uint32_t(random.scaled(range)));
}
void DialogueSession::auxiliary(const std::string &speech, Request::Origin origin) {
    Request request{Request::Kind::Speech, ++nextSerial, speech, {}};
    request.origin = origin;
    if (origin == Request::Origin::Replay || origin == Request::Origin::ReplayPlayer) {
        request.choices = previous.choices;
        request.selected = previous.selected;
        if (origin == Request::Origin::Replay)
            ++stats.replayLines;
    } else if (suspendedChoice) {
        request.choices = suspendedChoice->choices;
        if (origin == Request::Origin::Player)
            request.selected = playerRow;
    }
    if (origin == Request::Origin::Player || origin == Request::Origin::ReplayPlayer)
        ++stats.playerLines;
    pending = std::move(request);
}
bool DialogueSession::replay(uint64_t serial) {
    std::lock_guard lock(mutex);
    if (!pending || pending->serial != serial || pending->kind != Request::Kind::Choice ||
        replayRecords.empty())
        return false;
    suspendedChoice = pending;
    activeReplay = replayRecords;
    replayIndex = 0;
    interrupted = false;
    ++stats.replays;
    if (previous.selected >= 0 && size_t(previous.selected) < choiceSlots.size() && playerLookup &&
        playerLookup(choiceSlots[size_t(previous.selected)].text))
        auxiliary(choiceSlots[size_t(previous.selected)].text, Request::Origin::ReplayPlayer);
    else
        auxiliary(activeReplay[0], Request::Origin::Replay);
    return true;
}
void DialogueSession::finishAuxiliary(bool wasInterrupted) {
    auto origin = pending->origin;
    if (origin == Request::Origin::ReplayPlayer) {
        auxiliary(activeReplay[0], Request::Origin::Replay);
        return;
    }
    if (origin == Request::Origin::Player) {
        pending = std::move(suspendedChoice);
        suspendedChoice.reset();
        resolve(playerValue, playerRow);
        return;
    }
    if (origin == Request::Origin::Replay && !wasInterrupted &&
        ++replayIndex < activeReplay.size()) {
        auxiliary(activeReplay[replayIndex], origin);
        return;
    }
    pending = std::move(suspendedChoice);
    suspendedChoice.reset();
    activeReplay.clear();
    pending->serial = ++nextSerial;
    if (origin == Request::Origin::Replay)
        interrupted = false;
    else if (wasInterrupted) {
        interrupted = true;
        resolve(-1);
    }
}
size_t DialogueSession::replaySize() const {
    std::lock_guard lock(mutex);
    return replayRecords.size();
}
DialogueSession::ChoiceMemory DialogueSession::previousChoice() const {
    std::lock_guard lock(mutex);
    return previous;
}
DialogueSession::Statistics DialogueSession::statistics() const {
    std::lock_guard lock(mutex);
    return stats;
}
size_t DialogueSession::suppressedSpeech() const {
    std::lock_guard lock(mutex);
    return suppressed;
}
bool DialogueSession::finished() const {
    std::lock_guard lock(mutex);
    return done;
}
std::string DialogueSession::error() const {
    std::lock_guard lock(mutex);
    return failure;
}
void DialogueSession::cancel() {
    {
        std::lock_guard lock(mutex);
        stopped = true;
        pending.reset();
        suspendedChoice.reset();
        activeReplay.clear();
        wake.notify_one();
    }
    if (worker.joinable())
        worker.join();
}
} // namespace dust
