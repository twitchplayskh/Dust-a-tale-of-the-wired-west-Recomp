// SPDX-License-Identifier: GPL-3.0-only
#include "game.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include <set>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
namespace dust {
// Versioned native format. It deliberately does not emulate the original heap dump.
class CheckpointArchive {
  public:
    bool reading = false;
    uint32_t version = 3;
    Bytes bytes;
    size_t offset = 0;
    void operator()(uint32_t &v) {
        if (reading) {
            if (offset + 4 > bytes.size())
                throw std::runtime_error("truncated checkpoint");
            v = 0;
            for (unsigned n = 0; n < 4; ++n)
                v |= uint32_t(bytes[offset++]) << (n * 8);
        } else {
            for (unsigned n = 0; n < 4; ++n)
                bytes.push_back(uint8_t(v >> (n * 8)));
        }
    }
    void operator()(int &v) {
        uint32_t raw = uint32_t(v);
        (*this)(raw);
        if (reading)
            v = std::bit_cast<int32_t>(raw);
    }
    void operator()(uint64_t &v) {
        uint32_t lo = uint32_t(v), hi = uint32_t(v >> 32);
        (*this)(lo);
        (*this)(hi);
        if (reading)
            v = (uint64_t(hi) << 32) | lo;
    }
    void operator()(bool &v) {
        uint32_t raw = v;
        (*this)(raw);
        if (raw > 1)
            throw std::runtime_error("invalid checkpoint flag");
        v = raw != 0;
    }
    void operator()(std::string &v) {
        uint32_t count = uint32_t(v.size());
        (*this)(count);
        if (count > 4096)
            throw std::runtime_error("checkpoint string too long");
        if (reading) {
            if (offset + count > bytes.size())
                throw std::runtime_error("truncated checkpoint text");
            v.assign(reinterpret_cast<const char *>(bytes.data() + offset), count);
            offset += count;
        } else
            bytes.insert(bytes.end(), v.begin(), v.end());
    }
    void operator()(ScriptValue &v) {
        int type = int(v.type);
        (*this)(type);
        if (type < 0 || type > 2)
            throw std::runtime_error("invalid checkpoint value type");
        v.type = ScriptValue::Type(type);
        (*this)(v.number);
        (*this)(v.text);
    }
    template <class T, class U> void operator()(std::pair<T, U> &v) {
        (*this)(v.first);
        (*this)(v.second);
    }
    template <class T> void operator()(std::vector<T> &v) {
        uint32_t count = uint32_t(v.size());
        (*this)(count);
        if (count > 8192)
            throw std::runtime_error("checkpoint table too large");
        if (reading)
            v.resize(count);
        for (auto &item : v)
            (*this)(item);
    }
    template <class T, size_t N> void operator()(std::array<T, N> &v) {
        for (auto &item : v)
            (*this)(item);
    }
    template <class K, class V> void operator()(std::map<K, V> &v) {
        uint32_t count = uint32_t(v.size());
        (*this)(count);
        if (count > 8192)
            throw std::runtime_error("checkpoint table too large");
        if (reading) {
            v.clear();
            for (uint32_t n = 0; n < count; ++n) {
                K key{};
                V value{};
                (*this)(key);
                (*this)(value);
                if (!v.emplace(std::move(key), std::move(value)).second)
                    throw std::runtime_error("duplicate checkpoint key");
            }
        } else
            for (auto &[key, value] : v) {
                auto writableKey = key;
                (*this)(writableKey);
                (*this)(value);
            }
    }
    void operator()(GameHost::Entity &e) {
        (*this)(e.name);
        (*this)(e.group);
        (*this)(e.owner);
        (*this)(e.set);
        (*this)(e.star);
        (*this)(e.pose);
        for (auto *v : {&e.x, &e.y, &e.z, &e.degree, &e.scale, &e.speed, &e.turn, &e.value,
                        &e.zclip, &e.hitW, &e.hitH, &e.poseStep, &e.distance})
            (*this)(*v);
        (*this)(e.visible);
        (*this)(e.is3d);
        (*this)(e.starPending);
    }
};
namespace {
uint32_t checksum(const Bytes &bytes) {
    uint32_t result = 2166136261u;
    for (auto value : bytes)
        result = (result ^ value) * 16777619u;
    return result;
}
} // namespace
void GameHost::checkpointState(CheckpointArchive &a) {
    auto descriptors = resourceFiles;
    a(descriptors);
    if (a.reading)
        for (const auto &[file, kind] : descriptors) {
            if (kind < 0 || kind > int(ContextKind::Shop))
                throw std::runtime_error("invalid resource kind");
            loadResource(file, ContextKind(kind), false);
        }
    a(setFile);
    if (a.reading && !setFile.empty()) {
        set = std::make_unique<SceneSet>(SceneSet::open(resolve(setFile)));
        setContext = context(set->name, set->file.at(set->mainScript), nullptr);
    }
    a(position.x);
    a(position.z);
    a(position.facing);
    a(flat);
    a(cursorName);
    a(frameNumber);
    auto globalValues = vm.globals();
    std::map<std::string, ScriptValue> globals(globalValues.begin(), globalValues.end());
    a(globals);
    if (a.reading)
        vm.replaceGlobals({globals.begin(), globals.end()});
    a(settings);
    a(paths);
    if (a.version >= 2)
        a(knownStars);
    else if (a.reading) {
        // Version 1 was released for the opening town slice. Recover its town
        // markers so those existing checkpoints can continue into interiors.
        const auto town = SceneSet::open(resolve("nite.set"));
        for (const auto &star : town.stars)
            knownStars[star.name] = {star.x, star.y, star.z};
        if (set)
            for (const auto &star : set->stars)
                knownStars[star.name] = {star.x, star.y, star.z};
    }
    a(lastResult);
    a(actorState);
    a(propState);
    a(actorOrder);
    a(propOrder);
    a(soundVolumes);
    a(soundLoops);
    a(sessionCheckpoint);
    std::vector<std::string> banks;
    if (!a.reading)
        for (const auto &bank : soundLibrary.banks())
            banks.push_back(bank->path.filename().string());
    a(banks);
    if (a.reading)
        for (const auto &bank : banks)
            soundLibrary.open(resolve(bank));
    uint32_t count = uint32_t(loops.size());
    a(count);
    if (count > 8192)
        throw std::runtime_error("too many checkpoint timers");
    if (a.reading)
        loops.resize(count);
    for (auto &l : loops) {
        a(l.kind);
        a(l.name);
        a(l.handler);
        a(l.interval);
        a(l.due);
        a(l.paused);
        if (l.interval < 1)
            throw std::runtime_error("invalid checkpoint timer");
    }
    count = uint32_t(walks.size());
    a(count);
    if (count > 8192)
        throw std::runtime_error("too many checkpoint walks");
    if (a.reading)
        walks.resize(count);
    for (auto &w : walks) {
        a(w.name);
        a(w.dest);
        for (auto *v : {&w.x, &w.y, &w.z, &w.degree, &w.sx, &w.sy, &w.sz, &w.distance, &w.progress})
            a(*v);
        a(w.turning);
        a(w.paused);
        a(w.facingPending);
        if (w.distance < 1 || w.progress < 0 || w.progress > w.distance)
            throw std::runtime_error("invalid checkpoint walk");
    }
    if (a.version >= 3) {
        a(playerHitbox);
        for (auto &slot : balls) {
            bool present = slot.has_value();
            a(present);
            if (present) {
                if (a.reading)
                    slot.emplace();
                auto &ball = *slot;
                a(ball.name);
                for (auto *value :
                     {&ball.pause, &ball.vx, &ball.vy, &ball.vz, &ball.gravity, &ball.bounce,
                      &ball.frames, &ball.degree, &ball.x, &ball.y, &ball.z, &ball.tx, &ball.ty})
                    a(*value);
                if (a.reading && !propState.contains(ball.name))
                    throw std::runtime_error("checkpoint ball prop is missing");
            } else
                slot.reset();
        }
    }
    // Labels are cached output, but cash/key labels must survive a menu save too.
    count = uint32_t(flatLabels.size());
    a(count);
    if (count > 8192)
        throw std::runtime_error("too many checkpoint label sets");
    if (a.reading)
        flatLabels.clear();
    auto labels = [&](std::string &name, std::vector<TextLabel> &items) {
        a(name);
        uint32_t n = uint32_t(items.size());
        a(n);
        if (n > 8192)
            throw std::runtime_error("too many checkpoint labels");
        if (a.reading)
            items.resize(n);
        for (auto &l : items) {
            a(l.text);
            a(l.x);
            a(l.y);
            a(l.font);
            a(l.selector);
        }
    };
    if (a.reading) {
        for (uint32_t i = 0; i < count; ++i) {
            std::string name;
            std::vector<TextLabel> items;
            labels(name, items);
            if (!flatLabels.emplace(name, std::move(items)).second)
                throw std::runtime_error("duplicate checkpoint labels");
        }
    } else
        for (auto &[key, items] : flatLabels) {
            auto name = key;
            labels(name, items);
        }
    if (a.reading) {
        if (flat != "none" && !flats.contains(flat))
            throw std::runtime_error("checkpoint flat is missing");
        const std::set<std::string> cursors{"arrow", "fist",  "goleft", "goright", "gostrait",
                                            "hand",  "sight", "touch",  "watch"};
        if (!cursors.contains(cursorName))
            throw std::runtime_error("checkpoint cursor is invalid");
        if (set && (!currentCell() || position.facing < 1 || position.facing > 4))
            throw std::runtime_error("checkpoint position is invalid");
        for (int op : {16007, 16008, 16022, 16033})
            if (!settings.contains(op))
                throw std::runtime_error("checkpoint setting is missing");
        for (const auto &[key, value] : settings)
            if (key >= 30000 && key < 30256 &&
                (value.type != ScriptValue::Type::Integer || value.number < 0 ||
                 value.number > 0xffffff))
                throw std::runtime_error("checkpoint palette color is invalid");
        if (settings.at(16022).number < 1 || settings.at(16022).number > 1000 ||
            settings.at(16033).number < 0 || settings.at(16033).number > 9)
            throw std::runtime_error("checkpoint timing or volume is invalid");
        auto entities = [&](auto &states, auto &order, auto &contexts, bool actor) {
            std::set<std::string> seen;
            for (const auto &name : order)
                if (!states.contains(name) || !seen.insert(name).second)
                    throw std::runtime_error("checkpoint entity order is invalid");
            if (seen.size() != states.size())
                throw std::runtime_error("checkpoint entity order is incomplete");
            for (const auto &[name, e] : states) {
                const auto *r = resourceFor(e, actor);
                const auto split = e.group.find(':');
                const auto original = split == std::string::npos ? "" : e.group.substr(split + 1);
                if (name != e.name || !r || !r->find(original) || !contexts.contains(original) ||
                    (!e.pose.empty() && !r->find(original)->state(e.pose)) || e.poseStep < 0 ||
                    e.scale < 0 || e.scale > 32767)
                    throw std::runtime_error("checkpoint entity is invalid");
                contexts[name] = contexts.at(original);
            }
        };
        entities(actorState, actorOrder, actorContexts, true);
        entities(propState, propOrder, propContexts, false);
        for (const auto &w : walks)
            if (!actorState.contains(w.name))
                throw std::runtime_error("checkpoint walk actor is missing");
        for (const auto &l : loops)
            if (!(l.kind == "actor" && actorContexts.contains(l.name)) &&
                !(l.kind == "prop" && propContexts.contains(l.name)) &&
                !(l.kind == "flat" && flats.contains(l.name)) &&
                !(l.kind == "scene" && set &&
                  std::any_of(set->cells.begin(), set->cells.end(), [&](const auto &cell) {
                      auto name = cell.name;
                      for (auto &ch : name)
                          ch = char(std::tolower(static_cast<unsigned char>(ch)));
                      return name == l.name;
                  })))
                throw std::runtime_error("checkpoint timer recipient is missing: " + l.kind + ":" +
                                         l.name);
        const auto theme = settings.find(20079);
        if (theme != settings.end() && theme->second.text != "none" &&
            !soundLibrary.theme(theme->second.text))
            throw std::runtime_error("checkpoint theme is missing");
    }
}
void GameHost::saveGame(const std::filesystem::path &path) {
    if (puppet || motionImage || movingCamera)
        throw std::runtime_error("finish the current conversation or movement before saving");
    auto folded = [](std::wstring text) {
        for (auto &ch : text)
            ch = wchar_t(std::towlower(ch));
        return text;
    };
    const auto source = folded(std::filesystem::canonical(root).wstring()) + L"\\";
    const auto target =
        folded((std::filesystem::canonical(std::filesystem::absolute(path).parent_path()) /
                path.filename())
                   .wstring());
    if (target.starts_with(source))
        throw std::runtime_error("save outside the read-only original game folder");
    if (folded(path.extension().wstring()) != L".dustsave")
        throw std::runtime_error("native saves require the .dustsave extension");
    if (captureSession)
        sessionCheckpoint = captureSession();
    CheckpointArchive a;
    checkpointState(a);
    if (a.bytes.size() > 16 * 1024 * 1024)
        throw std::runtime_error("checkpoint is too large");
    CheckpointArchive header;
    uint32_t magic = 0x56535544, version = a.version, size = uint32_t(a.bytes.size()),
             hash = checksum(a.bytes);
    header(magic);
    header(version);
    header(size);
    header(hash);
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(header.bytes.data()), header.bytes.size());
        stream.write(reinterpret_cast<const char *>(a.bytes.data()), a.bytes.size());
        stream.flush();
        if (!stream)
            throw std::runtime_error("could not write checkpoint");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("could not replace checkpoint file");
    }
}
void GameHost::loadGame(const std::filesystem::path &path) {
    if (puppet || motionImage || movingCamera)
        throw std::runtime_error("finish the current conversation or movement before opening");
    const auto size = std::filesystem::file_size(path);
    if (size < 16 || size > 16 * 1024 * 1024)
        throw std::runtime_error("invalid checkpoint size");
    CheckpointArchive a;
    a.reading = true;
    a.bytes.resize(size);
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char *>(a.bytes.data()), size);
    if (!stream)
        throw std::runtime_error("could not read checkpoint");
    uint32_t magic = 0, version = 0, payload = 0, hash = 0;
    a(magic);
    a(version);
    a(payload);
    a(hash);
    if (magic != 0x56535544 || version < 1 || version > 3 || payload != size - 16)
        throw std::runtime_error("unsupported checkpoint format");
    a.bytes.erase(a.bytes.begin(), a.bytes.begin() + 16);
    a.offset = 0;
    a.version = version;
    if (checksum(a.bytes) != hash)
        throw std::runtime_error("checkpoint checksum failed");
    GameHost candidate(root, [](const Media &) {});
    candidate.checkpointState(a);
    if (a.offset != a.bytes.size())
        throw std::runtime_error("unexpected checkpoint data");
    if (restoreSession)
        restoreSession(candidate.sessionCheckpoint);
    // Retain old contexts until the original Open-button call unwinds.
    for (auto &c : contexts)
        retiredContexts.push_back(std::move(c));
    for (auto &r : resources)
        retiredResources.push_back(std::move(r));
    contexts = std::move(candidate.contexts);
    resources = std::move(candidate.resources);
    vm.replaceGlobals(candidate.vm.globals());
    bootContext = candidate.bootContext;
    stageContext = candidate.stageContext;
    setContext = candidate.setContext;
    stage = candidate.stage;
    casts = std::move(candidate.casts);
    shops = std::move(candidate.shops);
    actorContexts = std::move(candidate.actorContexts);
    propContexts = std::move(candidate.propContexts);
    flats = std::move(candidate.flats);
    sceneContexts = std::move(candidate.sceneContexts);
    floorContexts = std::move(candidate.floorContexts);
    actorState = std::move(candidate.actorState);
    propState = std::move(candidate.propState);
    actorOrder = std::move(candidate.actorOrder);
    propOrder = std::move(candidate.propOrder);
    resourceFiles = std::move(candidate.resourceFiles);
    setFile = std::move(candidate.setFile);
    set = std::move(candidate.set);
    position = candidate.position;
    flat = candidate.flat;
    cursorName = candidate.cursorName;
    settings = std::move(candidate.settings);
    paths = std::move(candidate.paths);
    knownStars = std::move(candidate.knownStars);
    lastResult = candidate.lastResult;
    loops = std::move(candidate.loops);
    walks = std::move(candidate.walks);
    balls = std::move(candidate.balls);
    playerHitbox = candidate.playerHitbox;
    soundLibrary = std::move(candidate.soundLibrary);
    soundVolumes = std::move(candidate.soundVolumes);
    soundLoops = std::move(candidate.soundLoops);
    flatLabels = std::move(candidate.flatLabels);
    repaintedTextProps.clear();
    sessionCheckpoint = std::move(candidate.sessionCheckpoint);
    frameNumber = candidate.frameNumber;
    ++worldGeneration;
    media({Media::Kind::Sound, {}, "stop"});
    media({Media::Kind::Voice, {}, "stop"});
    media({Media::Kind::Theme, {}, "stop"});
    const auto theme = settings.find(20079);
    if (theme != settings.end() && theme->second.text != "none") {
        auto bank = soundLibrary.theme(theme->second.text);
        Media request{Media::Kind::Theme, bank->path, theme->second.text};
        request.bank = bank;
        auto first = bank->record(bank->playlist.front()).name;
        for (auto &ch : first)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        request.volume = soundVolumes.contains(first) ? soundVolumes.at(first) : 255;
        media(request);
    }
    media({Media::Kind::GameVolume, {}, {}, settings.at(16033).number});
    media({Media::Kind::Cursor, {}, cursorName});
    media({Media::Kind::FlushInput});
    media({Media::Kind::Changed, {}, "set"});
    if (stack.empty()) {
        retiredContexts.clear();
        retiredResources.clear();
    }
}
} // namespace dust
