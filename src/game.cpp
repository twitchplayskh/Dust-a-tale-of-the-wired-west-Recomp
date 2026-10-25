// SPDX-License-Identifier: GPL-3.0-only
// Lifecycle and primitive layouts cross-checked against the pinned GPL
// dreamREfactory reference; first-slice script evidence is retained in reports.
#include "game.hpp"
#include "game_render.hpp"
#include "checkers.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace dust {
namespace {
using V = ScriptValue;
std::string low(std::string s) {
    for (auto &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
int number(const std::vector<V> &a, size_t i) {
    if (i >= a.size() || a[i].type == V::Type::String)
        throw ScriptError(4, "expected numeric host argument");
    return a[i].number;
}
std::string str(const std::vector<V> &a, size_t i) {
    if (i >= a.size() || a[i].type != V::Type::String)
        throw ScriptError(4, "expected string host argument");
    return low(a[i].text);
}
std::string stem(const std::string &s) {
    return low(std::filesystem::path(s).stem().string());
}
int32_t point(int x, int y) {
    return int32_t((uint32_t(uint16_t(x)) << 16) | uint16_t(y));
}
int px(int32_t p) {
    return int16_t(uint32_t(p) >> 16);
}
int py(int32_t p) {
    return int16_t(p);
}
int bearing(int dx, int dy) {
    dx = int16_t(dx);
    dy = int16_t(dy);
    if (!dx && !dy)
        return 0;
    const int ax = std::abs(dx), ay = std::abs(dy);
    if (!dy)
        return dx > 0 ? 0 : 128;
    if (!dx)
        return dy > 0 ? 64 : 192;
    int small = std::min(ax, ay), large = std::max(ax, ay);
    int octant =
        int(std::floor(std::atan(double((small << 9) / large) / 512) * 128 / 3.141592653589793));
    int angle = ax >= ay ? octant : 64 - octant;
    if (dx < 0)
        angle = 128 - angle;
    if (dy < 0)
        angle = 256 - angle;
    return angle & 255;
}
int stepDegree(int cur, int target, int step) {
    if (target > cur) {
        if (cur - target + 255 > target - cur)
            return std::min(target, cur + step);
        int next = cur - step;
        if (next >= 0)
            return next;
        return std::max(target, next + 255);
    }
    if (target < cur) {
        if (target - cur + 255 > cur - target)
            return std::max(target, cur - step);
        int next = cur + step;
        if (next < 255)
            return next;
        return std::min(target, next - 255);
    }
    return cur;
}
int direction(const std::string &d) {
    if (d == "north")
        return 1;
    if (d == "south")
        return 2;
    if (d == "east")
        return 3;
    if (d == "west")
        return 4;
    throw ScriptError(10, "unknown compass direction: " + d);
}
int degrees(int f) {
    constexpr int v[] = {0, 192, 64, 0, 128};
    return v[f];
}
std::string dirName(int f) {
    constexpr const char *v[] = {"", "north", "south", "east", "west"};
    return v[f];
}
} // namespace
GameHost::GameHost(std::filesystem::path original, MediaHandler handler)
    : root(std::move(original)), media(std::move(handler)) {
    if (!media)
        throw std::runtime_error("game host requires a media consumer");
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
        if (entry.is_regular_file()) {
            const auto name = low(entry.path().filename().string());
            // Installed ALT31 owns BOOTFILE; resources otherwise have unique leaf names.
            if (!files.contains(name) ||
                low(entry.path().parent_path().filename().string()) == "alt31")
                files[name] = entry.path();
        }
    paths[1] = "host:";
    settings[16022] = V::integer(3);
    settings[16007] = V::boolean(1);
    settings[16008] = V::boolean(1);
    settings[16035] = V::integer(0);
    settings[16037] = V::boolean(0);
    settings[16045] = V::boolean(0);
    settings[16033] = V::integer(9);
    auto bootFile = File::open(resolve("bootfile"));
    bootContext = context("boot", bootFile.at(1), nullptr);
    bind();
}
std::filesystem::path GameHost::resolve(const std::string &name) const {
    auto n = low(name);
    if (auto at = n.find_last_of(':'); at != std::string::npos)
        n = n.substr(at + 1);
    if (n.find('/') != std::string::npos || n.find('\\') != std::string::npos)
        throw std::runtime_error("resource must be a leaf filename");
    auto found = files.find(n);
    if (found == files.end())
        throw std::runtime_error("missing game resource: " + n);
    return found->second;
}
GameHost::Context *GameHost::context(std::string name, View bytes, Context *parent) {
    auto c = std::make_unique<Context>();
    c->name = std::move(name);
    c->program = std::make_shared<ScriptProgram>(bytes);
    c->parent = parent;
    auto *p = c.get();
    contexts.push_back(std::move(c));
    return p;
}
ScriptResult GameHost::call(Context *ctx, const std::string &handler, const std::vector<V> &args,
                            const std::string &me, const std::string &target, bool value,
                            bool fallback) {
    // A load can replace the resource graph inside the original Open handler.
    // Release that old graph only after every active script call has unwound.
    auto cleanup =
        std::unique_ptr<GameHost, std::function<void(GameHost *)>>(this, [this](GameHost *) {
            if (stack.empty()) {
                retiredContexts.clear();
                retiredResources.clear();
            }
        });
    std::string currentMe = me;
    ScriptResult last;
    while (ctx) {
        events.push_back(ctx->name + ":" + handler + " me=" + currentMe + " target=" + target);
        stack.push_back(ctx);
        ScriptResult result;
        try {
            result = vm.dispatch(*ctx->program, handler, args, currentMe, target, value);
        } catch (...) {
            stack.pop_back();
            throw;
        }
        stack.pop_back();
        last = result;
        if (result.handled && !result.passed)
            return result;
        if (!fallback)
            return result;
        ctx = ctx->parent;
        if (ctx)
            currentMe = ctx->name;
    }
    if (last.handled)
        return last;
    // Exact ALT31 0x41dc00 event table. Absent engine lifecycle/input events
    // are optional; ordinary authored routines remain error4.
    static const std::vector<std::string> optional = {
        "opencast",  "openactor",  "closecast", "closeactor", "openflat",   "openstage",
        "closeflat", "closestage", "endwalk",   "endturn",    "endball",    "endloop",
        "openshop",  "openprop",   "closeshop", "closeprop",  "openpuppet", "closepuppet",
        "menustate", "boot",       "idle",      "menuselect", "keydown",    "keyrepeat",
        "mousedown", "openset",    "openfloor", "openscene",  "closeset",   "closefloor",
        "closescene"};
    if (std::find(optional.begin(), optional.end(), low(handler)) != optional.end())
        return {};
    throw ScriptError(4, "script handler not found: " + handler + " target=" + target);
}
ScriptResult GameHost::route(const ScriptRoute &r) {
    if (r.handlerOpcode)
        throw ScriptError(1, "engine-coded deferred handler unsupported: " +
                                 std::to_string(r.handlerOpcode));
    if (!r.opcode) {
        if (stack.empty())
            return {};
        auto *parent = stack.back()->parent;
        return call(parent, r.handler, r.args, parent ? parent->name : r.me, r.target,
                    r.wantsValue);
    }
    Context *to = nullptr;
    std::string who = low(r.recipient);
    auto lookup = [&](const auto &map) -> Context * {
        auto it = map.find(who);
        return it == map.end() ? nullptr : it->second;
    };
    switch (r.opcode) {
    case 12081:
    case 20095:
        to = bootContext;
        who = "boot";
        break;
    case 12070:
    case 20094:
        to = stageContext;
        who = stage ? stage->name : "none";
        break;
    case 12067:
    case 20091:
        to = setContext;
        who = set ? set->name : "none";
        break;
    case 12016:
    case 20084:
        to = lookup(actorContexts);
        break;
    case 12055:
    case 20088:
        to = lookup(propContexts);
        break;
    case 12048:
    case 20087:
        who = stem(who);
        for (const auto &resource : resources)
            if (resource->kind == ContextKind::Cast && stem(resource->name) == who)
                who = resource->sourceName;
        to = lookup(casts);
        break;
    case 12059:
    case 20089:
        who = stem(who);
        for (const auto &resource : resources)
            if (resource->kind == ContextKind::Shop && stem(resource->name) == who)
                who = resource->sourceName;
        to = lookup(shops);
        break;
    case 12069:
    case 20093:
        to = lookup(flats);
        break;
    case 12068:
    case 20092:
        if (stage)
            if (const auto *flatEntry = stage->find(who))
                for (const auto &button : flatEntry->regions)
                    if (low(button.name) == low(r.secondaryRecipient)) {
                        auto parent = flats.find(who);
                        if (parent == flats.end())
                            break;
                        who = low(button.name);
                        to = context(who, stage->file.at(button.scriptRef), parent->second);
                        break;
                    }
        break;
    case 12045:
    case 20086:
        if (!puppet)
            throw ScriptError(0x2d, "no puppet open");
        to = lookup(puppetContexts);
        break;
    case 12034:
    case 20085:
        if (set)
            for (const auto &c : set->cells)
                if (low(c.name) == who) {
                    to = sceneContext(c);
                    break;
                }
        break;
    case 12066:
    case 20090: {
        int index = std::stoi(who);
        if (!set || index < 0 || index >= 32)
            throw ScriptError(10, "floor recipient out of range");
        who = std::to_string(index + 1);
        to = context(who, set->file.at(set->floorScripts[index]), setContext);
        break;
    }
    default:
        throw ScriptError(1, "unimplemented message route " + std::to_string(r.opcode));
    }
    if (!to)
        throw ScriptError(1, "message recipient not loaded: " + who);
    return call(to, r.handler, r.args, who, who, r.wantsValue);
}
void GameHost::loadResource(const std::string &filename, ContextKind kind, bool initialize) {
    const auto key = stem(filename);
    if (kind == ContextKind::Stage && stage)
        throw ScriptError(11, "a stage is already open");
    if (kind == ContextKind::Cast && casts.contains(key))
        return;
    if (kind == ContextKind::Shop && shops.contains(key))
        return;
    auto resource =
        std::make_unique<ResourceContexts>(ResourceContexts::open(resolve(filename), kind));
    auto *data = resource.get();
    data->sourceName = key;
    resourceFiles.emplace_back(filename, int(kind));
    resources.push_back(std::move(resource));
    auto *main = context(key, data->main(), nullptr);
    auto &table = kind == ContextKind::Cast   ? actorContexts
                  : kind == ContextKind::Shop ? propContexts
                                              : flats;
    if (kind == ContextKind::Cast)
        casts[key] = main;
    else if (kind == ContextKind::Shop)
        shops[key] = main;
    else {
        stage = data;
        stageContext = main;
        flats.clear();
        flat = "none";
    }
    std::vector<std::string> names;
    for (const auto &entry : data->entries) {
        const auto n = low(entry.name);
        names.push_back(n);
        table[n] = context(n, data->file.at(entry.scriptRef), main);
        if (kind != ContextKind::Stage) {
            Entity e;
            e.name = n;
            e.group = key + ":" + n;
            auto &states = kind == ContextKind::Cast ? actorState : propState;
            auto &order = kind == ContextKind::Cast ? actorOrder : propOrder;
            states[n] = e;
            order.push_back(n);
        }
    }
    if (!initialize)
        return;
    if (kind == ContextKind::Cast)
        call(main, "opencast", {}, key, key);
    if (kind != ContextKind::Stage)
        for (const auto &n : names)
            call(table.at(n), kind == ContextKind::Cast ? "openactor" : "openprop", {}, n, n);
    if (kind == ContextKind::Shop)
        call(main, "openshop", {}, key, key);
    if (kind == ContextKind::Stage) {
        // A newly opened stage installs its own palette. In particular the
        // Yunni book fades out the old inventory, then opens its pages without
        // an explicit blacktoscreen: the old all-black CLUT cannot survive.
        for (int i = 0; i < 256; ++i)
            settings.erase(30000 + i);
        if (!names.empty())
            flat = names.front();
        call(main, "openstage", {}, key, key);
        // ALT31 00411860/00411d40: the first flat is already current during
        // openstage. Its openflat follows only if that callback kept the stage
        // and flat selected when it entered.
        if (stage == data && !names.empty() && flat == names.front()) {
            call(flats.at(flat), "openflat", {}, flat, flat);
        }
    }
}
void GameHost::closeStage() {
    if (!stage)
        throw ScriptError(11, "stage is not open");
    auto *oldStage = stage;
    auto *oldContext = stageContext;
    call(oldContext, "closestage", {}, oldContext->name, oldContext->name);
    if (stage != oldStage)
        return;
    if (flat != "none")
        call(flats.at(flat), "closeflat", {}, flat, flat);
    if (stage != oldStage)
        return;
    std::erase_if(loops, [](const auto &loop) { return loop.kind == "flat"; });
    flatLabels.clear();
    repaintedTextProps.clear();
    flats.clear();
    flat = "none";
    stage = nullptr;
    stageContext = nullptr;
    std::erase_if(resourceFiles,
                  [](const auto &file) { return file.second == int(ContextKind::Stage); });
    for (auto it = contexts.begin(); it != contexts.end();) {
        auto *parent = it->get();
        while (parent && parent != oldContext)
            parent = parent->parent;
        if (parent) {
            retiredContexts.push_back(std::move(*it));
            it = contexts.erase(it);
        } else
            ++it;
    }
    for (auto it = resources.begin(); it != resources.end(); ++it)
        if (it->get() == oldStage) {
            retiredResources.push_back(std::move(*it));
            resources.erase(it);
            break;
        }
    media({Media::Kind::Changed, {}, "set"});
}
void GameHost::closeEntityResource(const std::string &filename, ContextKind kind) {
    const bool actor = kind == ContextKind::Cast;
    auto &mains = actor ? casts : shops;
    auto &order = actor ? actorOrder : propOrder;
    auto &states = actor ? actorState : propState;
    auto &scripts = actor ? actorContexts : propContexts;
    auto key = stem(filename);
    for (const auto &resource : resources)
        if (resource->kind == kind && stem(resource->name) == key)
            key = resource->sourceName;
    const auto found = mains.find(key);
    if (found == mains.end())
        throw ScriptError(10, "entity resource is not open: " + key);
    auto *main = found->second;
    call(main, actor ? "closecast" : "closeshop", {}, key, key);
    std::vector<std::string> owned;
    for (const auto &name : order)
        if (states.at(name).group.starts_with(key + ":"))
            owned.push_back(name);
    for (const auto &name : owned)
        call(scripts.at(name), actor ? "closeactor" : "closeprop", {}, name, name);
    std::erase_if(loops, [&](const auto &loop) {
        return loop.kind == (actor ? "actor" : "prop") &&
               std::find(owned.begin(), owned.end(), loop.name) != owned.end();
    });
    if (actor)
        std::erase_if(walks, [&](const auto &walk) {
            return std::find(owned.begin(), owned.end(), walk.name) != owned.end();
        });
    else
        for (auto &ball : balls)
            if (ball && std::find(owned.begin(), owned.end(), ball->name) != owned.end())
                ball.reset();
    for (const auto &name : owned) {
        states.erase(name);
        scripts.erase(name);
        std::erase(order, name);
    }
    mains.erase(key);
    std::erase_if(resourceFiles, [&](const auto &file) {
        return file.second == int(kind) && stem(file.first) == key;
    });
    for (auto it = contexts.begin(); it != contexts.end();) {
        auto *parent = it->get();
        while (parent && parent != main)
            parent = parent->parent;
        if (parent) {
            retiredContexts.push_back(std::move(*it));
            it = contexts.erase(it);
        } else
            ++it;
    }
    for (auto it = resources.begin(); it != resources.end(); ++it)
        if ((*it)->kind == kind && (*it)->sourceName == key) {
            retiredResources.push_back(std::move(*it));
            resources.erase(it);
            break;
        }
    media({Media::Kind::Changed, {}, "set"});
}
GameHost::Entity &GameHost::entity(const std::string &name, bool actor) {
    auto &states = actor ? actorState : propState;
    auto it = states.find(low(name));
    if (it == states.end())
        throw ScriptError(10, "unknown " + std::string(actor ? "actor: " : "prop: ") + name);
    return it->second;
}
const ResourceContexts *GameHost::resourceFor(const Entity &e, bool actor) const {
    const auto key = e.group.substr(0, e.group.find(':'));
    for (const auto &r : resources)
        if (r->kind == (actor ? ContextKind::Cast : ContextKind::Shop) && r->sourceName == key)
            return r.get();
    return nullptr;
}
const SceneCell *GameHost::currentCell() const {
    if (set)
        for (const auto &c : set->cells)
            if (c.x == position.x && c.z == position.z)
                return &c;
    return nullptr;
}
GameHost::Context *GameHost::floorContext(int index) {
    if (auto it = floorContexts.find(index); it != floorContexts.end())
        return it->second;
    auto *c = context(std::to_string(index), set->file.at(set->floorScripts.at(index)), setContext);
    floorContexts[index] = c;
    return c;
}
GameHost::Context *GameHost::sceneContext(const SceneCell &cell) {
    const auto key = low(cell.name);
    if (auto it = sceneContexts.find(key); it != sceneContexts.end())
        return it->second;
    auto *c = context(key, set->file.at(cell.script), floorContext(cell.floorIndex));
    sceneContexts[key] = c;
    return c;
}
void GameHost::openSet(const std::string &filename) {
    if (set)
        throw ScriptError(0x28, "a set is already open");
    set = std::make_unique<SceneSet>(SceneSet::open(resolve(filename)));
    // DF1 4195f0 initializes both the set-open and set-visible words to one.
    settings[16007] = V::boolean(true);
    for (const auto &star : set->stars)
        knownStars[low(star.name)] = {star.x, star.y, star.z};
    setFile = filename;
    position = set->initial;
    sceneContexts.clear();
    floorContexts.clear();
    const auto generation = ++worldGeneration;
    const auto initial = position;
    setContext = context(low(set->name), set->file.at(set->mainScript), nullptr);
    for (auto *states : {&actorState, &propState})
        for (auto &[name, e] : *states)
            if (e.starPending) {
                if (const auto *star = set->findStar(e.star)) {
                    e.x = star->x;
                    e.y = star->y;
                    e.z = star->z;
                    e.starPending = false;
                    e.is3d = true;
                }
            }
    call(setContext, "openset", {}, set->name, set->name);
    if (!set || worldGeneration != generation || position != initial)
        return;
    if (const auto *cell = currentCell()) {
        const auto copy = *cell;
        auto *floor = floorContext(copy.floorIndex);
        call(floor, "openfloor", {}, floor->name, floor->name);
        if (!set || worldGeneration != generation || position != initial)
            return;
        call(sceneContext(copy), "openscene", {}, low(copy.name), low(copy.name));
    }
}
void GameHost::closeSet() {
    if (!set)
        return;
    const auto generation = worldGeneration;
    auto initial = position;
    auto cell = currentCell();
    std::optional<SceneCell> copy;
    if (cell)
        copy = *cell;
    call(setContext, "closeset", {}, set->name, set->name);
    if (!set || worldGeneration != generation || position != initial)
        return;
    if (copy) {
        auto *floor = floorContext(copy->floorIndex);
        call(floor, "closefloor", {}, floor->name, floor->name);
        if (!set || worldGeneration != generation || position != initial)
            return;
        call(sceneContext(*copy), "closescene", {}, low(copy->name), low(copy->name));
    }
    if (worldGeneration == generation) {
        // Scene names are local to a set. A loop scheduled from a previous
        // cell must not resolve against an unrelated cell in the next set.
        std::erase_if(loops, [](const Loop &l) { return l.kind == "scene"; });
        set.reset();
        setFile.clear();
        setContext = nullptr;
        sceneContexts.clear();
        floorContexts.clear();
        ++worldGeneration;
    }
}
void GameHost::changeScene(const std::string &name) {
    if (!set)
        throw ScriptError(10, "no set open");
    const auto wanted = low(name);
    for (const auto &c : set->cells)
        if (low(c.name) == wanted) {
            const auto dest = c;
            if (dest.x == position.x && dest.z == position.z)
                return;
            const auto generation = worldGeneration;
            const auto from = position;
            if (const auto *old = currentCell())
                call(sceneContext(*old), "closescene", {}, low(old->name), low(old->name));
            if (!set || worldGeneration != generation || position != from)
                return;
            position.x = dest.x;
            position.z = dest.z;
            call(sceneContext(dest), "openscene", {}, wanted, wanted);
            return;
        }
    int action = wanted == "left" ? -1 : wanted == "right" ? 1 : 0;
    if (wanted != "left" && wanted != "right" && wanted != "strait" && wanted != "forward")
        throw ScriptError(10, "unknown scene: " + wanted);
    const auto *found = set->move(position, action);
    if (!found)
        return;
    const auto move = *found;
    const auto generation = worldGeneration;
    const auto from = position;
    if (const auto *old = currentCell())
        call(sceneContext(*old), "closescene", {}, low(old->name), low(old->name));
    if (!set || worldGeneration != generation || position != from)
        return;
    motionImage.emplace();
    int delta = (degrees(move.to.facing) - degrees(from.facing) + 256) % 256;
    if (delta > 128)
        delta -= 256;
    for (size_t i = 0; i < move.frames.size(); ++i) {
        motionImage->decode(set->file.at(move.frames[i]));
        const int degree = (degrees(from.facing) + delta * int(i) / 4) & 255;
        const double angle = degree * 6.283185307179586 / 256.;
        movingCamera = CameraPose{from.x * 256 + 128 + (move.to.x - from.x) * 64 * int(i) -
                                      int(std::lround(set->cameraSetback * std::cos(angle))),
                                  from.z * 256 + 128 + (move.to.z - from.z) * 64 * int(i) -
                                      int(std::lround(set->cameraSetback * std::sin(angle))),
                                  degree};
        media({Media::Kind::Motion, {}, wanted, settings.at(16022).number});
        update(false);
        if (!set || worldGeneration != generation) {
            motionImage.reset();
            movingCamera.reset();
            return;
        }
    }
    position = move.to;
    motionImage.reset();
    movingCamera.reset();
    if (const auto *cell = currentCell())
        call(sceneContext(*cell), "openscene", {}, low(cell->name), low(cell->name));
}
void GameHost::boot() {
    call(bootContext, "boot", {}, "boot", "boot");
}
void GameHost::key(const std::string &s, bool repeat) {
    if (!repeat && !puppet && flat == "mainpanel") {
        bool bound = false;
        for (const auto *name : {"keynorth", "keyeast", "keywest"})
            if (vm.hasGlobal(name) && low(vm.global(name).text) == low(s))
                bound = true;
        if (!bound && (low(s) == "m" || low(s) == "i")) {
            control(low(s) == "m" ? "map" : "inventory");
            return;
        }
    }
    call(bootContext, repeat ? "keyrepeat" : "keydown", {V::string(s)}, "boot", "boot");
}
const std::vector<GameHost::TextLabel> &GameHost::textLabels() const {
    static const std::vector<TextLabel> empty;
    const auto found = flatLabels.find(flat);
    return found == flatLabels.end() ? empty : found->second;
}
void GameHost::control(const std::string &requested) {
    if (vm.hasGlobal("lockevents") && vm.global("lockevents").number)
        return;
    if (puppet)
        return;
    auto action = low(requested);
    std::string button;
    if (action == "help") {
        if (flat == "score")
            button = "help";
        else if (propContexts.contains("helpbut")) {
            call(propContexts.at("helpbut"), "mousedown", {V::integer(0)}, "helpbut", "helpbut");
            return;
        }
    } else if (action == "close" || action == "menu") {
        if (flat == "map")
            button = "untitled";
        else if (flat == "avatar" || flat == "score")
            button = "ok";
        else if (flat == "mainpanel")
            button = "horn";
    } else if (flat == "mainpanel") {
        if (action == "inventory")
            button = "self";
        if (action == "map")
            button = "map";
    }
    if (!stage || button.empty())
        return;
    const auto *entry = stage->find(flat);
    if (!entry)
        return;
    for (const auto &region : entry->regions)
        if (low(region.name) == button) {
            auto *recipient = context(button, stage->file.at(region.scriptRef), flats.at(flat));
            call(recipient, "mousedown", {V::integer(0)}, button, button);
            return;
        }
}
void GameHost::actorEvent(const std::string &n, const std::string &handler) {
    auto it = actorContexts.find(low(n));
    if (it == actorContexts.end())
        throw ScriptError(10, "actor event recipient missing");
    call(it->second, handler, {V::integer(0)}, low(n), low(n));
}
void GameHost::idle() {
    call(bootContext, "idle", {}, "boot", "boot");
}
void GameHost::pointer(int x, int y, std::string kind, std::string name) {
    mouseX = x;
    mouseY = y;
    hitKind = std::move(kind);
    hitName = std::move(name);
}
void GameHost::mouse(int x, int y, std::string kind, std::string name) {
    pointer(x, y, std::move(kind), std::move(name));
    call(bootContext, "mousedown", {V::integer(point(x, y))}, "boot", "boot");
}
void GameHost::bind() {
    vm.bindRouter([this](const ScriptRoute &r) { return route(r); });
    const uint16_t ops[] = {
        12004, 12005, 12006, 12009, 12010, 12011, 12012, 12013, 12017, 12018, 12019, 12020, 12021,
        12022, 12028, 12029, 12032, 12033, 12038, 12039, 12042, 12049, 12050, 12051, 12052, 12056,
        12060, 12062, 12072, 12074, 12075, 12076, 16001, 16002, 16003, 16004, 16005, 16006, 16007,
        16008, 16009, 16010, 16011, 16012, 16014, 16015, 16016, 16017, 16018, 16019, 16020, 16021,
        16013, 16022, 16023, 16024, 16025, 16026, 16027, 16028, 16029, 16030, 16031, 16032, 16034,
        16035, 16036, 16037, 16039, 16040, 16042, 16043, 16044, 16045, 16046, 16047, 16048, 16050,
        16051, 16052, 20002, 20003, 20004, 20005, 20006, 20007, 20009, 20010, 20011, 20012, 20013,
        20023, 20024, 20025, 20026, 20029, 20030, 20031, 20032, 20033, 20034, 20035, 20036, 20038,
        20039, 20040, 20041, 20042, 20046, 20047, 20048, 20049, 20053, 20055, 20058, 20059, 20061,
        20062, 20063, 20064, 20067, 20070, 20071, 20073, 20074, 20078, 20079, 20081, 20096, 20099,
        12073, 12080, 16033, 20043, 20060, 12079, 20068, 12071, 12077, 12078, 12026, 12030, 16041,
        20016, 20097, 12061, 12057, 12014, 20054, 20056, 20057, 12001, 16053, 20083, 20108, 12007,
        20101, 20104, 12085, 12086, 12087, 12082, 20077, 12023, 20019, 20020, 20021, 20022, 20100,
        12027, 20098, 12024, 20014, 20015, 20037, 12015, 12031, 12035, 12036, 12046, 12053, 12054,
        12063, 12064};
    for (auto op : ops)
        vm.bind(op, [this, op](const auto &a, bool wants) {
            try {
                return command(op, a, wants);
            } catch (const ScriptError &e) {
                throw ScriptError(e.code, "host opcode " + std::to_string(op) + ": " + e.what());
            } catch (const std::exception &e) {
                throw std::runtime_error("host opcode " + std::to_string(op) + ": " + e.what());
            }
        });
}
std::array<int, 3> GameHost::starPosition(const std::string &name) const {
    auto s = low(name);
    if (s.find(',') != std::string::npos) {
        std::replace(s.begin(), s.end(), ',', ' ');
        std::istringstream in(s);
        std::array<int, 3> p{};
        if (in >> p[0] >> p[1] >> p[2])
            return p;
    }
    if (set)
        if (const auto *star = set->findStar(s))
            return {star->x, star->y, star->z};
    if (const auto found = knownStars.find(s); found != knownStars.end())
        return found->second;
    throw ScriptError(10, "unknown named star: " + name);
}
void GameHost::clearRepaintedText() {
    auto pending = std::move(repaintedTextProps);
    repaintedTextProps.clear();
    auto found = flatLabels.find(flat);
    if (pending.empty() || found == flatLabels.end() || found->second.empty())
        return;
    std::erase_if(pending, [&](const std::string &name) {
        const auto it = propState.find(name);
        return it == propState.end() || !it->second.visible || it->second.is3d;
    });
    if (pending.empty())
        return;
    // Script text is retained separately from the native canvas. A screen prop
    // used to wipe a field must retire the old run as well as repaint its pixels.
    // Use actual opaque sprite coverage, not a guessed font width or a UI name.
    const auto frame = renderGame(*this);
    std::erase_if(found->second, [&](const TextLabel &label) {
        if (label.x < 0 || label.x >= frame.width || label.y <= 0 || label.y > frame.height)
            return false;
        return std::any_of(frame.hits.begin(), frame.hits.end(), [&](const GameHit &hit) {
            return hit.kind == "prop" && pending.contains(hit.name) &&
                   coversGameHit(hit, label.x, label.y - 1);
        });
    });
}
void GameHost::update(bool wait) {
    clearRepaintedText();
    ++frameNumber;
    // DF1 scheduler behavior adapted from the pinned GPL reference. Target-
    // executable equivalence of the complete scheduler remains unverified.
    auto pending = walks;
    for (const auto &snapshot : pending) {
        auto it = std::find_if(walks.begin(), walks.end(),
                               [&](const Walk &w) { return w.name == snapshot.name; });
        if (it == walks.end() || it->paused)
            continue;
        auto &w = *it;
        auto &e = entity(w.name, true);
        const auto name = w.name;
        if (w.facingPending) {
            e.degree = stepDegree(e.degree, w.degree, e.turn) & 255;
            e.x = w.sx;
            e.y = w.sy;
            e.z = w.sz;
            if (e.degree != w.degree)
                continue;
            if (w.turning)
                walks.erase(it);
            else
                w.facingPending = false;
            call(actorContexts.at(name), "endturn", {}, name, name);
            continue;
        }
        w.progress = std::min(w.distance, int(int16_t(w.progress + e.speed)));
        e.x = int16_t(w.sx + int64_t(w.x - w.sx) * w.progress / w.distance);
        e.y = int16_t(w.sy + int64_t(w.y - w.sy) * w.progress / w.distance);
        e.z = int16_t(w.sz + int64_t(w.z - w.sz) * w.progress / w.distance);
        e.degree = w.degree;
        if (w.progress >= w.distance) {
            e.star = w.dest;
            walks.erase(it);
            call(actorContexts.at(name), "endwalk", {}, name, name);
        }
    }
    for (auto &[name, e] : actorState)
        if (e.visible) {
            const auto *resource = resourceFor(e, true);
            if (!resource)
                continue;
            const auto *entry = resource->find(e.group.substr(e.group.find(':') + 1));
            if (!entry)
                continue;
            const auto *pose = entry->state(e.pose);
            if (pose && !pose->playOrder.empty())
                e.poseStep = (e.poseStep + 1) % int(pose->playOrder.size());
        }
    updateBalls();
    auto due = loops;
    for (const auto &l : due)
        if (!l.paused && l.due <= frameNumber) {
            auto found = std::find_if(loops.begin(), loops.end(), [&](const Loop &x) {
                return x.kind == l.kind && x.name == l.name && x.due == l.due;
            });
            if (found == loops.end())
                continue;
            loops.erase(found);
            Context *c = nullptr;
            if (l.kind == "actor")
                c = actorContexts.at(l.name);
            else if (l.kind == "prop")
                c = propContexts.at(l.name);
            else if (l.kind == "flat")
                c = flats.at(l.name);
            else if (l.kind == "scene" && set)
                for (const auto &cell : set->cells)
                    if (low(cell.name) == l.name)
                        c = sceneContext(cell);
            if (c)
                call(c, l.handler, {}, l.name, l.name);
        }
    if (wait)
        media({Media::Kind::Delay, {}, "update", settings.at(16022).number, false});
}
V GameHost::command(uint16_t op, const std::vector<V> &a, bool wants) {
    const auto zero = V::integer(0);
    auto ni = [&](size_t i) { return number(a, i); };
    auto ss = [&](size_t i) { return str(a, i); };
    auto setting = [&]() {
        if (!a.empty())
            settings[op] = a.at(0);
        return settings.at(op);
    };
    auto send = [&](Media::Kind kind, std::string name = {}, int ticks = 0, bool reveal = false,
                    std::filesystem::path path = {}) {
        media({kind, std::move(path), std::move(name), ticks, reveal});
    };
    switch (op) {
    case 12077:
    case 12078: {
        if (a.size() != 1 || a[0].type != V::Type::String)
            throw ScriptError(14, "save/open requires a game identifier");
        Media request{Media::Kind::Query, {}, op == 12077 ? "save-path" : "load-path"};
        request.reply = std::make_shared<std::optional<V>>(V::string(""));
        media(request);
        const auto selected = **request.reply;
        if (selected.type != V::Type::String)
            throw ScriptError(14, "save/open path must be text");
        if (selected.text.empty())
            return V::boolean(false);
        try {
            const auto path =
                std::filesystem::path(std::u8string(selected.text.begin(), selected.text.end()));
            if (op == 12077)
                saveGame(path);
            else
                loadGame(path);
        } catch (const std::exception &e) {
            Media notice{Media::Kind::Dialog,
                         {},
                         std::string("Could not ") + (op == 12077 ? "save: " : "open: ") +
                             e.what()};
            notice.reply = std::make_shared<std::optional<V>>(V::boolean(false));
            media(notice);
            return V::boolean(false);
        }
        return V::boolean(true);
    }
    case 12080: {
        if (a.size() != 4 || a[0].type != V::Type::String || a[1].type != V::Type::Integer ||
            a[2].type != V::Type::Integer || a[3].type != V::Type::Integer)
            throw ScriptError(14, "drawstring requires text, point and two font integers");
        clearRepaintedText();
        auto &labels = flatLabels[flat];
        TextLabel label{a[0].text, px(ni(1)), py(ni(1)), ni(2), ni(3)};
        auto found = std::find_if(labels.begin(), labels.end(), [&](const TextLabel &old) {
            return old.x == label.x && old.y == label.y;
        });
        if (found == labels.end())
            labels.push_back(std::move(label));
        else
            *found = std::move(label);
        send(Media::Kind::ScriptText);
        return zero;
    }
    case 16033:
        if (!a.empty()) {
            if (a.size() != 1 || a[0].type != V::Type::Integer)
                throw ScriptError(14, "wavevolume requires an integer");
            if (ni(0) < 0 || ni(0) > 9)
                throw ScriptError(10, "wavevolume outside 0..9");
            settings[op] = a[0];
            send(Media::Kind::GameVolume, {}, ni(0));
        }
        return settings.at(op);
    case 12073:
        if (!a.empty())
            throw ScriptError(14, "flushevents takes no arguments");
        send(Media::Kind::FlushInput);
        return zero;
    case 12001:
        if (a.size() != 1 || a[0].type != V::Type::String)
            throw ScriptError(14, "message requires text");
        // DF1 425b70 only copies into the diagnostic message buffer (459d00).
        // It does not display a modal dialog, including during combat hits.
        settings[12001] = a[0];
        return zero;
    case 12079:
    case 20068: {
        if (a.size() != 1 || a[0].type != V::Type::String)
            throw ScriptError(14, "dialog requires text");
        Media request{Media::Kind::Dialog, {}, a[0].text};
        request.reveal = op == 20068;
        request.reply = std::make_shared<std::optional<V>>(V::boolean(false));
        media(request);
        return op == 20068 ? **request.reply : zero;
    }
    case 12071:
        if (!a.empty())
            throw ScriptError(14, "quit takes no arguments");
        send(Media::Kind::Quit);
        return zero;
    case 20043: {
        if (a.size() != 3 || a[0].type != V::Type::String || a[1].type != V::Type::String ||
            a[2].type != V::Type::Integer)
            throw ScriptError(14, "pointinbutton requires flat, button and point");
        if (!stage)
            throw ScriptError(11, "pointinbutton requires an open stage");
        const auto *entry = stage->find(ss(0));
        if (!entry)
            throw ScriptError(10, "unknown flat in pointinbutton");
        for (const auto &region : entry->regions)
            if (low(region.name) == ss(1))
                return V::boolean(px(ni(2)) >= region.left && px(ni(2)) < region.right &&
                                  py(ni(2)) >= region.top && py(ni(2)) < region.bottom);
        throw ScriptError(10, "unknown button in pointinbutton");
    }
    case 16010:
        if (!a.empty())
            lastResult = a.at(0);
        return lastResult;
    case 16007:
    case 16008:
    case 16022:
    case 16035:
    case 16037:
    case 16045:
        return setting();
    case 16050:
        if (!puppet)
            throw ScriptError(0x2d, "no puppet open");
        return setting();
    case 16009: {
        int i = ni(0);
        if (a.size() > 1)
            paths[i] = ss(1);
        return V::string(paths[i]);
    }
    case 12074:
        if (a.size() != 1 || a[0].type != V::Type::Boolean)
            throw ScriptError(14, "puppetgrab requires a boolean");
        settings[op] = V::boolean(int16_t(a[0].number));
        return zero;
    case 12039:
        if (cursorName != ss(0)) {
            cursorName = ss(0);
            send(Media::Kind::Cursor, cursorName);
        }
        return zero;
    case 12009:
        if (ni(0) < 24001 || ni(0) > 24014)
            throw ScriptError(14, "visual effect outside 24001..24014");
        settings[op] = V::integer(std::clamp(ni(1), 1, 1000));
        {
            Media effect{Media::Kind::Effect, {}, "visualeffect", settings[op].number};
            effect.effect = ni(0);
            media(effect);
        }
        return zero;
    case 12082: {
        if (a.size() != 5 || a[0].type != V::Type::String || a[1].type != V::Type::String ||
            a[2].type != V::Type::Integer || a[3].type != V::Type::Integer ||
            a[4].type != V::Type::Integer)
            throw ScriptError(14, "mixclut requires palettes, range and amount");
        auto palette = [&](const std::string &name) {
            std::array<uint32_t, 256> colors{};
            if (name == "black")
                return colors;
            if (name == "set" && set)
                return set->palettes[0];
            if (name == "stage" && stage)
                return stage->palette;
            if (name == "current") {
                if (set && setVisible())
                    colors = set->palettes[0];
                else if (stage)
                    colors = stage->palette;
                if (set && setVisible() && stage && stageVisible())
                    std::copy(stage->palette.begin() + 128, stage->palette.end(),
                              colors.begin() + 128);
                applyPalette(colors);
                return colors;
            }
            throw ScriptError(10, "mixclut palette is unavailable: " + name);
        };
        const auto from = palette(ss(0)), to = palette(ss(1));
        const int first = std::clamp(ni(2), 0, 255), last = std::clamp(ni(3), 0, 255),
                  amount = std::clamp(ni(4), 0, 255);
        for (int i = first; i <= last; ++i) {
            uint32_t color = 0;
            for (int shift : {0, 8, 16}) {
                const int x = (from[i] >> shift) & 255, y = (to[i] >> shift) & 255;
                color |= uint32_t(x + (y - x) * amount / 255) << shift;
            }
            settings[30000 + i] = V::integer(int32_t(color));
        }
        send(Media::Kind::Palette);
        return zero;
    }
    case 12038:
        if (ss(0) == "current") {
            send(Media::Kind::Palette);
            return zero;
        }
        for (int i = 0; i < 256; ++i)
            settings.erase(30000 + i);
        if (ss(0) == "black") {
            for (int i = 0; i < 256; ++i)
                settings[30000 + i] = V::integer(0);
            send(Media::Kind::Black);
        } else if (ss(0) == "set" && set) {
            for (int i = 0; i < 256; ++i)
                settings[30000 + i] = V::integer(int32_t(set->palettes[0][i]));
            send(Media::Kind::Palette);
        } else if (ss(0) == "stage" && stage) {
            for (int i = 0; i < 256; ++i)
                settings[30000 + i] = V::integer(int32_t(stage->palette[i]));
            send(Media::Kind::Palette);
        } else
            throw ScriptError(10, "unsupported palette name");
        return zero;
    case 12051:
        send(Media::Kind::Black);
        return zero;
    case 12049:
    case 12050:
        if (op == 12049)
            for (int i = 0; i < 256; ++i)
                settings.erase(30000 + i);
        send(Media::Kind::Fade, ss(0), ni(1), op == 12049);
        if (op == 12050)
            for (int i = 0; i < 256; ++i)
                settings[30000 + i] = V::integer(0);
        return zero;
    case 12004:
        send(Media::Kind::Delay, "delay", ni(0));
        return zero;
    case 20083:
        if (a.size() != 1 || a[0].type != V::Type::Integer || ni(0) < 1 || ni(0) > 2)
            throw ScriptError(14, "actionframe selector must be 1 or 2");
        return V::boolean(settings.contains(op) && (settings.at(op).number & (1 << (ni(0) - 1))));
    case 12017: {
        Media request{Media::Kind::Movie, resolve(ss(0)), ss(0)};
        request.reply = std::make_shared<std::optional<V>>(V::integer(0));
        media(request);
        settings[20083] = **request.reply;
        return zero;
    }
    case 12013:
        loadResource(ss(0), ContextKind::Cast);
        return zero;
    case 12056:
        loadResource(ss(0), ContextKind::Shop);
        return zero;
    case 12014:
    case 12057:
        if (a.size() != 1 || a[0].type != V::Type::String)
            throw ScriptError(14, "closeshopfile requires a shop name");
        closeEntityResource(ss(0), op == 12014 ? ContextKind::Cast : ContextKind::Shop);
        return zero;
    case 12060:
        loadResource(ss(0), ContextKind::Stage);
        return zero;
    case 12061:
        if (!a.empty())
            throw ScriptError(14, "closestagefile takes no arguments");
        closeStage();
        return zero;
    case 12062: {
        if (!stage || stage->entries.empty())
            throw ScriptError(11, "stage is not open");
        if (a.size() != 1)
            throw ScriptError(14, "gotoflat requires one name or index");
        auto *previousStage = stage;
        std::string name;
        if (a[0].type == V::Type::Integer) {
            const auto index = std::clamp(a[0].number, 1, int(stage->entries.size()));
            name = low(stage->entries[size_t(index - 1)].name);
        } else
            name = ss(0);
        auto it = flats.find(name);
        if (it == flats.end())
            throw ScriptError(10, "unknown flat");
        if (flat != "none")
            call(flats.at(flat), "closeflat", {}, flat, flat);
        if (stage != previousStage)
            return zero;
        flat = name;
        call(it->second, "openflat", {}, flat, flat);
        return zero;
    }
    case 12032:
        openSet(ss(0));
        return zero;
    case 12033:
        closeSet();
        return zero;
    case 16053:
        if (!a.empty()) {
            if (a.size() != 1 || a[0].type != V::Type::String)
                throw ScriptError(14, "puppetbase requires one speech identifier");
            if (a[0].text.size() > 31)
                throw ScriptError(26, "puppetbase identifier exceeds 31 bytes");
            settings[op] = V::string(ss(0));
            send(Media::Kind::PuppetBase, ss(0));
        }
        return settings.contains(op) ? settings.at(op) : V::string("");
    case 12018: {
        if (puppet)
            throw ScriptError(0x2d, "puppet already open");
        puppet = std::make_unique<Puppet>(Puppet::open(resolve(ss(0))));
        puppetName = low(puppet->refName);
        puppetContexts.clear();
        settings[16050] = V::boolean(1);
        for (const auto &entry : puppet->scripts) {
            auto name = low(entry.name);
            if (!puppetContexts.contains(name))
                puppetContexts[name] = context(name, puppet->file.at(entry.container), nullptr);
        }
        if (puppetChanged)
            puppetChanged(puppet.get());
        send(Media::Kind::Changed, "puppet");
        return zero;
    }
    case 12042:
        if (puppetChanged)
            puppetChanged(nullptr);
        puppet.reset();
        puppetName = "none";
        puppetContexts.clear();
        send(Media::Kind::Changed, "set");
        return zero;
    case 12019:
        soundLibrary.open(resolve(ss(0)));
        return zero;
    case 12020: {
        std::shared_ptr<const SoundBank> selected;
        for (const auto &bank : soundLibrary.banks())
            if (low(bank->refName) == ss(0)) {
                selected = bank;
                break;
            }
        if (!selected)
            throw ScriptError(10, "track bank is not open: " + ss(0));
        Media request{Media::Kind::StopBank, selected->path, ss(0)};
        request.bank = selected;
        media(request);
        if (const auto theme = settings.find(20079);
            theme != settings.end() && low(theme->second.text) == low(selected->refName))
            settings[20079] = V::string("none");
        soundLibrary.close(ss(0));
        return zero;
    }
    case 12021: {
        auto name = ss(0);
        auto bank = soundLibrary.theme(name);
        if (!bank)
            throw ScriptError(10, "theme bank is not open: " + name);
        Media request{Media::Kind::Theme, bank->path, name};
        request.bank = bank;
        const auto first = low(bank->record(bank->playlist.front()).name);
        request.volume = soundVolumes.contains(first) ? soundVolumes.at(first) : 255;
        media(request);
        settings[20079] = V::string(name);
        return zero;
    }
    case 12028:
        send(Media::Kind::Sound, "stop");
        return zero;
    case 12029:
        send(Media::Kind::Theme, "stop");
        settings[20079] = V::string("none");
        return zero;
    case 12022:
    case 12024: // DF1 dualsound is the singlesound entry, not a new channel rule.
    case 12023: {
        auto selection = soundLibrary.sound(ss(0));
        if (!selection)
            throw ScriptError(10, "sound is not in an open bank: " + ss(0));
        Media request{Media::Kind::Sound, selection->bank->path, ss(0)};
        request.bank = selection->bank;
        request.container = selection->container;
        request.volume = soundVolumes.contains(ss(0)) ? soundVolumes[ss(0)] : 255;
        request.loop = soundLoops[ss(0)];
        const auto &banks = soundLibrary.banks();
        for (size_t i = 0; i < banks.size(); ++i)
            if (banks[i] == selection->bank)
                request.priority = uint32_t(((i + 1) << 16) | selection->container);
        request.multiple = op == 12023;
        media(request);
        return zero;
    }
    case 12026: {
        if (a.size() != 1 || a[0].type != V::Type::String)
            throw ScriptError(14, "voicesound requires a sound name");
        const auto selection = soundLibrary.sound(ss(0));
        if (!selection)
            throw ScriptError(10, "voice is not in an open bank: " + ss(0));
        Media request{Media::Kind::Voice, selection->bank->path, ss(0)};
        request.bank = selection->bank;
        request.container = selection->container;
        media(request);
        return zero;
    }
    case 12030:
        if (!a.empty())
            throw ScriptError(14, "haltvoice takes no arguments");
        send(Media::Kind::Voice, "stop");
        return zero;
    case 16041: {
        if (a.empty() || a.size() > 2 || a[0].type != V::Type::String ||
            (a.size() == 2 && a[1].type != V::Type::Integer))
            throw ScriptError(14, "themevol requires a track name and optional integer level");
        const auto bank = soundLibrary.bank(ss(0));
        if (!bank)
            throw ScriptError(10, "unknown track: " + ss(0));
        // ALT31 0040e9e0 sets every record in the track, including one-shots.
        if (a.size() == 2) {
            const auto level = std::clamp(ni(1), 0, 255);
            for (const auto &chunk : bank->chunks) {
                const auto name = low(chunk.name);
                soundVolumes[name] = level;
                Media update{Media::Kind::SoundVolume, {}, name};
                update.volume = level;
                media(update);
            }
            Media update{Media::Kind::ThemeVolume, bank->path, low(bank->refName)};
            update.volume = level;
            media(update);
        }
        if (bank->oneshots == 0)
            return V::integer(0);
        const auto first = low(bank->chunks.front().name);
        return V::integer(soundVolumes.contains(first) ? soundVolumes.at(first) : 255);
    }
    case 16040: {
        auto name = ss(0);
        if (!soundLibrary.sound(name))
            throw ScriptError(10, "unknown sound: " + name);
        if (a.size() > 1) {
            soundVolumes[name] = std::clamp(ni(1), 0, 255);
            Media request{Media::Kind::SoundVolume, {}, name};
            request.volume = soundVolumes[name];
            media(request);
        }
        return V::integer(soundVolumes.contains(name) ? soundVolumes[name] : 255);
    }
    case 16039: {
        auto name = ss(0);
        if (!soundLibrary.sound(name))
            throw ScriptError(10, "unknown sound: " + name);
        if (a.size() > 1) {
            soundLoops[name] = ni(1) != 0;
            Media request{Media::Kind::SoundLoop, {}, name};
            request.loop = soundLoops[name];
            media(request);
        }
        return V::boolean(soundLoops[name]);
    }
    case 20016:
    case 20097: {
        if (!a.empty())
            throw ScriptError(14, "audio completion query takes no arguments");
        Media request{Media::Kind::Query, {}, op == 20016 ? "sound-done" : "voice-done"};
        request.reply = std::make_shared<std::optional<V>>(V::boolean(true));
        media(request);
        return **request.reply;
    }
    case 20014:
    case 20015: {
        const auto bank = soundLibrary.bank(ss(0));
        if (!bank)
            throw ScriptError(10, "sound bank is not open: " + ss(0));
        if (op == 20014)
            return V::integer(int(bank->oneshots));
        const int index = ni(1);
        if (index < 1 || size_t(index) > bank->oneshots)
            return V::string("");
        return V::string(low(bank->record(size_t(index)).name));
    }
    case 20037:
        if (!stage)
            throw ScriptError(11, "flattoindex requires an open stage");
        for (size_t i = 0; i < stage->entries.size(); ++i)
            if (low(stage->entries[i].name) == ss(0))
                return V::integer(int(i + 1));
        return V::integer(0);
    case 12053:
        throw ScriptError(0x2f, "original script error() — runtime user error");
    // The original retail engine compiled its script editor out. These retain
    // the target validation of its editor hooks, then return without a window.
    case 12031:
        return zero;
    case 12015:
        (void)entity(ss(0), true);
        return zero;
    case 12054:
        (void)entity(ss(0), false);
        return zero;
    case 12035:
        if (!set)
            throw ScriptError(40, "setscript requires an open set");
        return zero;
    case 12036:
        if (!set)
            throw ScriptError(40, "scenescript requires an open set");
        for (const auto &cell : set->cells)
            if (low(cell.name) == ss(0))
                return zero;
        throw ScriptError(10, "scenescript scene is missing");
    case 12063:
        if (!stage)
            throw ScriptError(11, "stagescript requires an open stage");
        return zero;
    case 12064:
        if (!stage)
            throw ScriptError(11, "flatscript requires an open stage");
        if (!stage->find(ss(0)))
            throw ScriptError(10, "flatscript flat is missing");
        return zero;
    case 12046:
        if (!puppet)
            throw ScriptError(0x2d, "puppetscript requires an open puppet");
        if (!puppetContexts.contains(ss(0)))
            throw ScriptError(10, "puppetscript context is missing");
        return zero;
    case 20077:
    case 20078:
    case 20079: {
        Media request{Media::Kind::Query,
                      {},
                      op == 20077   ? "sound"
                      : op == 20078 ? "voice"
                                    : (!a.empty() && ni(0) == 1 ? "theme-chunk" : "theme-bank")};
        request.reply = std::make_shared<std::optional<V>>();
        media(request);
        if (!*request.reply)
            throw std::runtime_error("media consumer did not answer sound query");
        return **request.reply;
    }
    case 20067:
        try {
            (void)resolve(ss(0));
            return V::boolean(1);
        } catch (...) {
            return V::boolean(0);
        }
    case 20058:
        if (!a.empty())
            throw ScriptError(14, "modifier query takes no arguments");
        return V::boolean(false); // Windows DF.EXE has no Option-key mapping.
    case 20059:
    case 20060: {
        if (!a.empty())
            throw ScriptError(14, "modifier query takes no arguments");
        Media request{Media::Kind::Query, {}, op == 20059 ? "modifier-shift" : "modifier-control"};
        request.reply = std::make_shared<std::optional<V>>(V::boolean(false));
        media(request);
        return **request.reply;
    }
    case 20108:
        if (a.size() != 1 || a[0].type != V::Type::Integer)
            throw ScriptError(14, "sqrt requires one integer");
        return V::integer(int(std::sqrt(double(uint32_t(ni(0))))) & 65535);
    case 20055: {
        if (a.size() != 2)
            throw ScriptError(14, "substring requires two strings");
        const auto needle = ss(1);
        const auto at = ss(0).find(needle);
        return V::integer(needle.empty() || at == std::string::npos ? -1 : int(at + 1));
    }
    case 20056:
        if (a.size() != 1)
            throw ScriptError(14, "stringlength requires one string");
        return V::integer(int(ss(0).size()));
    case 20054:
    case 20057: {
        if (a.size() != (op == 20054 ? 3u : 4u) || a[0].type != V::Type::String ||
            a[1].type != V::Type::String || a[2].type != V::Type::Integer ||
            (op == 20057 && a[3].type != V::Type::String))
            throw ScriptError(
                14, "word operation requires text, separator, index and optional replacement");
        auto text = a[0].text;
        const auto separator = a[1].text;
        int word = a[2].number;
        if (separator.empty()) {
            if (word < 1 || size_t(word) > text.size() + (op == 20057 ? 1u : 0u))
                return V::string("");
            if (op == 20054)
                return V::string(text.substr(size_t(word - 1), 1));
            text.insert(size_t(word - 1), a[3].text);
            return V::string(text);
        }
        size_t start = 0;
        const auto folded = low(text), delim = low(separator);
        while (--word > 0) {
            const auto end = folded.find(delim, start);
            if (end == std::string::npos)
                return V::string("");
            start = end + separator.size();
        }
        auto end = folded.find(delim, start);
        if (op == 20054) {
            // DF.EXE 00416650 stops at length-separatorLength, losing an
            // unterminated final tail. Preserve this original DF1 behavior.
            if (end == std::string::npos)
                end = text.size() >= separator.size() ? text.size() - separator.size() : 0;
            return V::string(end >= start ? text.substr(start, end - start) : "");
        }
        if (end == std::string::npos)
            end = text.size();
        text.replace(start, end - start, a[3].text);
        return V::string(text);
    }
    case 12087:
        if (a.empty() || a[0].type != V::Type::String)
            throw ScriptError(14, "shopwarm requires a shop name");
        for (const auto &resource : resources)
            if (resource->kind == ContextKind::Shop && low(resource->name) == ss(0))
                return zero;
        throw ScriptError(10, "shopwarm recipient is missing");
    case 12085:
    case 12086: {
        if (a.empty() || a[0].type != V::Type::String)
            throw ScriptError(14, "warm requires an entity name");
        // Original warm commands load the resolved entity's art into memory.
        // Native resource files are already resident; validate the same recipient.
        const bool actor = op == 12085;
        const auto &e = entity(ss(0), actor);
        if (!resourceFor(e, actor))
            throw ScriptError(10, "warm resource is missing");
        return {};
    }
    case 20025: {
        if (a.empty() || a[0].type != V::Type::String)
            throw ScriptError(14, "stringtonum requires text");
        // DF1 416420 -> 42f3f0 uses sscanf with the original "%ld" format.
        long value = 0;
#ifdef _MSC_VER
        const int scanned = sscanf_s(a[0].text.c_str(), "%ld", &value);
#else
        const int scanned = std::sscanf(a[0].text.c_str(), "%ld", &value);
#endif
        return V::integer(scanned == 1 ? static_cast<int32_t>(value) : 0);
    }
    case 20026: {
        if (ni(0) == 0)
            return V::string("0");
        return V::string(std::to_string(ni(0)));
    }
    case 20002:
        return V::integer(px(ni(0)));
    case 20003:
        return V::integer(py(ni(0)));
    case 20005:
    case 20006:
    case 20007: {
        if (!a.empty())
            throw ScriptError(14, "pointer query takes no arguments");
        Media query{Media::Kind::Query,
                    {},
                    op == 20006   ? "pointer"
                    : op == 20005 ? "button"
                                  : "stilldown"};
        query.reply = std::make_shared<std::optional<V>>(
            op == 20006 ? V::integer(point(mouseX, mouseY)) : V::boolean(0));
        media(query);
        return **query.reply;
    }
    case 20070: {
        if (a.size() != 1 || a[0].type != V::Type::Integer)
            throw ScriptError(14, "hittest requires one integer point");
        const int x = px(a[0].number), y = py(a[0].number);
        const auto frame = renderGame(*this);
        if (const auto *hit = hitGame(frame, x, y)) {
            lastResult = V::string(hit->kind);
            return V::string(hit->name);
        }
        if (set && setVisible() && x >= 0 && x < set->viewW && y >= 0 && y < set->viewH) {
            lastResult = V::string("scene");
            if (const auto *cell = currentCell())
                return V::string(low(cell->name));
        }
        if (stage && stageVisible() && x >= 0 && x < stage->width && y >= 0 && y < stage->height) {
            lastResult = V::string("flat");
            return V::string(flat);
        }
        lastResult = V::string("none");
        return V::string("");
    }
    case 20041:
        return V::boolean(setVisible() && px(ni(0)) >= 0 && px(ni(0)) < 512 && py(ni(0)) >= 0 &&
                          py(ni(0)) < 264);
    case 20042:
        return V::boolean(stageVisible() && px(ni(0)) >= 0 && px(ni(0)) < 512 && py(ni(0)) >= 0 &&
                          py(ni(0)) < 384);
    case 20039:
    case 20040: {
        if (a.size() != 2 || a[0].type != V::Type::String || a[1].type != V::Type::Integer)
            throw ScriptError(14, "pointinactor/pointinprop requires a name and integer point");
        const auto &e = entity(ss(0), op == 20039);
        const auto frame = renderGame(*this);
        const auto kind = op == 20039 ? "actor" : "prop";
        const auto point = a[1].number;
        return V::boolean(
            std::any_of(frame.hits.begin(), frame.hits.end(), [&](const GameHit &hit) {
                return hit.kind == kind && hit.name == e.name &&
                       coversGameHit(hit, px(point), py(point));
            }));
    }
    case 20004:
        return V::integer(point(ni(0), ni(1)));
    case 20061:
    case 20062: {
        double angle = ni(0) * 6.283185307179586 / 256.;
        return V::integer(
            int(std::lround((op == 20061 ? std::cos(angle) : std::sin(angle)) * ni(1))));
    }
    case 20071: {
        int x = px(ni(1)) - px(ni(0)), y = py(ni(1)) - py(ni(0));
        return V::integer(bearing(x, y));
    }
    case 20081: {
        int64_t x = px(ni(1)) - px(ni(0)), y = py(ni(1)) - py(ni(0));
        return V::integer(int(std::sqrt(double(x * x + y * y))));
    }
    case 20073: {
        auto p = starPosition(ss(0));
        int c = ni(1);
        if (c == 4)
            return V::integer(point(p[0], p[1]));
        if (c < 1 || c > 3)
            throw ScriptError(10, "invalid coordinate selector");
        return V::integer(p[c - 1]);
    }
    case 20063:
    case 20064: {
        if (!set)
            throw ScriptError(10, "camera queried without set");
        int degree = movingCamera ? movingCamera->degree : degrees(position.facing);
        double angle = degree * 6.283185307179586 / 256.;
        int bx = int(std::lround(set->cameraSetback * std::cos(angle))),
            by = int(std::lround(set->cameraSetback * std::sin(angle)));
        int x = movingCamera ? movingCamera->x + bx : position.x * 256 + 128,
            y = movingCamera ? movingCamera->y + by : position.z * 256 + 128, c = ni(0);
        if (op == 20063) {
            x -= bx;
            y -= by;
        }
        if (c < 1 || c > 4)
            throw ScriptError(10, "invalid camera coordinate selector");
        return V::integer(c == 4        ? point(x, y)
                          : c == 1      ? x
                          : c == 2      ? y
                          : op == 20063 ? set->eyeHeight
                                        : 0);
    }
    case 20074:
        return V::integer(int32_t(frameNumber));
    case 16029:
        if (!a.empty())
            changeScene(ss(0));
        if (set)
            for (const auto &c : set->cells)
                if (c.x == position.x && c.z == position.z)
                    return V::string(low(c.name));
        return V::string("none");
    case 16011:
        if (!a.empty())
            position.facing = direction(ss(0));
        return V::string(set ? dirName(position.facing) : "none");
    case 16031:
        if (!a.empty())
            throw ScriptError(1, "currentdeg setter is not implemented");
        return V::integer(movingCamera ? movingCamera->degree : set ? degrees(position.facing) : 0);
    case 20053:
        return V::string(set ? low(set->name) : "none");
    case 20048:
        return V::string(stage ? low(stage->name) : "none");
    case 20038:
        return V::string(flat);
    case 20049:
        return V::string(puppetName);
    case 20046:
        if (!puppet)
            throw ScriptError(0x2d, "no puppet open");
        return V::integer(int(puppet->scripts.size()));
    case 20047: {
        if (!puppet)
            throw ScriptError(0x2d, "no puppet open");
        int i = ni(0);
        if (i < 1 || size_t(i) > puppet->scripts.size())
            throw ScriptError(10, "puppet script index out of range");
        return V::string(low(puppet->scripts[i - 1].name));
    }
    case 20012:
        return V::integer(int(actorOrder.size()));
    case 20031:
        return V::integer(int(propOrder.size()));
    case 20013:
    case 20032: {
        const auto &order = op == 20013 ? actorOrder : propOrder;
        int i = ni(0);
        if (i < 1 || size_t(i) > order.size())
            throw ScriptError(10, "entity index out of range");
        const auto &e = entity(order[i - 1], op == 20013);
        const auto *resource = resourceFor(e, op == 20013);
        if (!resource)
            throw ScriptError(10, "entity resource is not open");
        lastResult = V::string(low(resource->name));
        return V::string(order[i - 1]);
    }
    case 20029:
        return V::integer(int(casts.size()));
    case 20033:
        return V::integer(int(shops.size()));
    case 20030:
    case 20034: {
        const auto &table = op == 20030 ? casts : shops;
        auto i = ni(0);
        if (i < 1 || size_t(i) > table.size())
            throw ScriptError(10, "resource index out of range");
        auto it = table.begin();
        std::advance(it, i - 1);
        return V::string(it->first);
    }
    case 20035:
        return V::integer(stage ? int(stage->entries.size()) : 0);
    case 20036: {
        int i = ni(0);
        if (!stage || i < 1 || size_t(i) > stage->entries.size())
            throw ScriptError(10, "flat index out of range");
        return V::string(low(stage->entries[i - 1].name));
    }
    case 20019:
        if (!set)
            throw ScriptError(40, "scene query requires an open set");
        return V::integer(int(set->cells.size()));
    case 20020:
        if (!set)
            throw ScriptError(40, "scene query requires an open set");
        if (ni(0) < 1 || size_t(ni(0)) > set->cells.size())
            throw ScriptError(10, "scene index out of range");
        return V::string(low(set->cells[size_t(ni(0) - 1)].name));
    case 20021:
        if (!set)
            throw ScriptError(40, "scene query requires an open set");
        if (a.size() != 2 || a[0].type != V::Type::Integer || a[1].type != V::Type::Integer)
            throw ScriptError(14, "rowcoltoscene requires integer row and column");
        for (const auto &cell : set->cells)
            if (cell.z == int16_t(ni(0)) && cell.x == int16_t(ni(1)))
                return V::string(low(cell.name));
        return V::string("none");
    case 20022:
    case 20100:
        if (!set)
            throw ScriptError(40, "scene query requires an open set");
        for (const auto &cell : set->cells)
            if (low(cell.name) == ss(0))
                return op == 20100 ? V::boolean(cell.building) : V::integer(cell.floorIndex);
        throw ScriptError(10, "scene query recipient is missing");
    case 20023:
    case 20024:
        if (set)
            for (const auto &c : set->cells)
                if (low(c.name) == ss(0))
                    return V::integer(op == 20023 ? c.z : c.x);
        throw ScriptError(10, "scene not found");
    case 20096:
        if (set)
            for (const auto &c : set->cells)
                if (low(c.name) == ss(0)) {
                    int x = c.x * 256 + 128, y = c.z * 256 + 128, axis = ni(1);
                    return V::integer(axis == 4 ? point(x, y) : axis == 1 ? x : axis == 2 ? y : 0);
                }
        throw ScriptError(10, "scene coordinate recipient missing");
    case 12075:
    case 12076: {
        bool actor = op == 12075;
        auto name = ss(1);
        auto &states = actor ? actorState : propState;
        auto &order = actor ? actorOrder : propOrder;
        if (states.contains(name))
            return zero; // Existing destinations keep their own state and script.
        Entity copy = entity(ss(0), actor);
        copy.name = name;
        states[name] = copy;
        order.push_back(name);
        auto &map = actor ? actorContexts : propContexts;
        map[name] = map.at(ss(0));
        return zero;
    }
    case 12010:
        std::erase_if(walks, [&](const Walk &w) { return ss(0) == "all" || w.name == ss(0); });
        return zero;
    case 12011:
        std::erase_if(loops, [&](const Loop &l) {
            return (ss(0) == "all" || l.kind == ss(0)) && (ss(1) == "all" || l.name == ss(1));
        });
        return zero;
    case 12012:
        if (a.size() != 1 || a[0].type != V::Type::String)
            throw ScriptError(14, "stopball requires one name");
        for (auto &ball : balls)
            if (ball && (ss(0) == "all" || ball->name == ss(0)))
                ball.reset();
        return zero;
    case 12007: {
        if (a.size() != 7 || a[0].type != V::Type::String ||
            std::any_of(a.begin() + 1, a.end(),
                        [](const V &v) { return v.type != V::Type::Integer; }))
            throw ScriptError(14, "makeball requires a prop and six integers");
        const auto &e = entity(ss(0), false);
        for (auto &ball : balls)
            if (ball && ball->name == e.name)
                ball.reset();
        const auto free =
            std::find_if(balls.begin(), balls.end(), [](const auto &b) { return !b; });
        if (free == balls.end())
            throw ScriptError(8, "ball table is full");
        *free = Ball{e.name,
                     0,
                     int16_t(ni(1)),
                     int16_t(ni(2)),
                     int16_t(ni(3)),
                     int16_t(ni(4)),
                     int16_t(ni(5)),
                     int16_t(ni(6)),
                     e.degree,
                     int16_t(e.x),
                     int16_t(e.y),
                     int16_t(e.z),
                     int16_t(e.x) >> 8,
                     int16_t(e.y) >> 8};
        return zero;
    }
    case 12005: {
        auto kind = ss(0), name = ss(1);
        int n = ni(3);
        if (n < 1)
            n = 1;
        std::erase_if(loops, [&](const Loop &l) { return l.kind == kind && l.name == name; });
        loops.push_back({kind, name, ss(2), n, frameNumber + uint64_t(n), false});
        return zero;
    }
    case 20009:
        return V::boolean(std::any_of(walks.begin(), walks.end(),
                                      [&](const Walk &w) { return w.name == ss(0); }));
    case 20010:
        return V::boolean(std::any_of(loops.begin(), loops.end(), [&](const Loop &l) {
            return l.kind == ss(0) && l.name == ss(1);
        }));
    case 20011:
        return V::boolean(std::any_of(balls.begin(), balls.end(),
                                      [&](const auto &b) { return b && b->name == ss(0); }));
    case 20101:
        return V::integer(
            int(std::count_if(balls.begin(), balls.end(), [](const auto &b) { return bool(b); })));
    case 12027:
    case 20098:
        if (a.size() != 4 || ss(0) != "checkmove" || a[1].type != V::Type::String ||
            a[2].type != V::Type::Integer || a[3].type != V::Type::Integer)
            throw ScriptError(14, "unsupported native plugin or invalid checkmove arguments");
        return V::string(checkersMove(a[1].text, ni(2), ni(3)));
    case 20104: {
        int count = ni(0);
        for (const auto &ball : balls)
            if (ball && --count <= 0)
                return V::string(ball->name);
        return V::string("none");
    }
    case 20099:
        for (const auto &w : walks)
            if (w.name == ss(0))
                return V::string(w.dest);
        return V::string("none");
    case 12006: {
        auto name = ss(0), dest = ss(1);
        auto p = starPosition(dest);
        std::erase_if(walks, [&](const Walk &w) { return w.name == name; });
        auto &e = entity(name, true);
        Walk w{name, dest, p[0], p[1], p[2], bearing(p[0] - e.x, p[1] - e.y), false, false};
        w.sx = e.x;
        w.sy = e.y;
        w.sz = e.z;
        int64_t dx = p[0] - e.x, dy = p[1] - e.y, dz = p[2] - e.z;
        w.distance = std::max(1, int(std::sqrt(double(dx * dx + dy * dy + dz * dz))));
        walks.push_back(w);
        e.star = "walktostar";
        return zero;
    }
    case 12072: {
        auto name = ss(0);
        std::erase_if(walks, [&](const Walk &w) { return w.name == name; });
        const auto &e = entity(name, true);
        Walk w{name, e.star, e.x, e.y, e.z, ni(1) & 255, true, false};
        w.sx = e.x;
        w.sy = e.y;
        w.sz = e.z;
        walks.push_back(w);
        return zero;
    }
    case 16046:
        for (auto &l : loops)
            if ((ss(0) == "all" || l.kind == ss(0)) && (ss(1) == "all" || l.name == ss(1)))
                l.paused = ni(2) != 0;
        return zero;
    case 16048:
        for (auto &w : walks)
            if (ss(0) == "all" || w.name == ss(0))
                w.paused = ni(1) != 0;
        return zero;
    case 16047:
        if (a.size() == 2 && a[1].type != V::Type::Boolean)
            throw ScriptError(14, "pauseball requires a boolean flag");
        for (auto &ball : balls)
            if (ball && (ss(0) == "all" || ball->name == ss(0))) {
                if (a.size() == 1)
                    return V::boolean(ball->pause);
                ball->pause =
                    ni(1) ? int16_t(ball->pause + 1) : std::max(0, int(int16_t(ball->pause - 1)));
            }
        return V::boolean(false);
    case 12052:
        update();
        return zero;
    }
    if (op == 16034 && ss(0) == "player") {
        if (a.size() == 2) {
            if (a[1].type != V::Type::Integer || (ni(1) != 1 && ni(1) != 2))
                throw ScriptError(14, "player hitbox selector requires 1 or 2");
            return V::integer(playerHitbox[ni(1) - 1]);
        }
        if (a.size() != 3 || a[1].type != V::Type::Integer || a[2].type != V::Type::Integer)
            throw ScriptError(14, "player hitbox requires two integers");
        playerHitbox = {int16_t(ni(1)), int16_t(ni(2))};
        return zero;
    }
    const bool actor = (op >= 16001 && op <= 16006) || op == 16012 || op == 16014 || op == 16021 ||
                       op == 16023 || op == 16024 || op == 16034 || op == 16036 || op == 16043 ||
                       op == 16044 || op == 16051;
    auto &e = entity(ss(0), actor);
    if (!actor && a.size() > 1 &&
        (op == 16015 || op == 16016 || op == 16017 || op == 16018 || op == 16019 || op == 16020 ||
         op == 16025 || op == 16028 || op == 16013))
        repaintedTextProps.insert(e.name);
    auto integerProperty = [&](int &field) {
        if (a.size() > 1)
            field = ni(1);
        return V::integer(field);
    };
    auto stringProperty = [&](std::string &field) {
        if (a.size() > 1)
            field = ss(1);
        return V::string(field);
    };
    switch (op) {
    case 16001:
    case 16015:
        if (a.size() > 1)
            e.visible = ni(1) != 0;
        return V::boolean(e.visible);
    case 16005:
    case 16019:
        if (a.size() > 1)
            e.is3d = ni(1) != 0;
        return V::boolean(e.is3d);
    case 16002:
    case 16016:
        return integerProperty(e.degree);
    case 16014:
    case 16025:
        if (a.size() > 1)
            e.poseStep = 0;
        return stringProperty(e.pose);
    case 16021:
    case 16027:
        return stringProperty(e.set);
    case 16023:
    case 16026:
        return integerProperty(e.speed);
    case 16024:
    case 16028:
        return integerProperty(e.scale);
    case 16036:
        return integerProperty(e.turn);
    case 16043:
    case 16032:
        return stringProperty(e.owner);
    case 16044:
    case 16042:
        return integerProperty(e.value);
    case 16051:
    case 16052:
        return integerProperty(e.zclip);
    case 16034:
        if (a.size() == 2) {
            if (a[1].type != V::Type::Integer || (ni(1) != 1 && ni(1) != 2))
                throw ScriptError(14, "actorhitbox selector requires 1 or 2");
            return V::integer(ni(1) == 1 ? e.hitW : e.hitH);
        }
        if (a.size() != 3)
            throw ScriptError(2, "actorhitbox arity");
        e.hitW = ni(1);
        e.hitH = ni(2);
        return zero;
    case 16006:
    case 16020:
        if (a.size() > 1) {
            e.star = ss(1);
            e.starPending = false;
            if (!e.star.empty() && e.star != "custom" && e.star != "resume") {
                auto dot = e.star.find('.');
                if (!knownStars.contains(e.star) && dot != std::string::npos &&
                    (!set || e.star.substr(0, dot) != low(set->name)))
                    e.starPending = true;
                else {
                    auto p = starPosition(e.star);
                    e.x = p[0];
                    e.y = p[1];
                    e.z = p[2];
                    e.is3d = true;
                }
            }
        }
        return V::string(e.star);
    case 16003:
    case 16017:
        if (a.size() == 4) {
            e.x = ni(1);
            e.y = ni(2);
            e.z = ni(3);
            e.is3d = true;
            e.starPending = false;
            return zero;
        }
        if (a.size() == 2) {
            int c = ni(1);
            return V::integer(c == 4 ? point(e.x, e.y) : c == 1 ? e.x : c == 2 ? e.y : e.z);
        }
        throw ScriptError(2, "xyz arity");
    case 16004:
    case 16018:
        if (a.size() == 3) {
            e.x = ni(1);
            e.y = ni(2);
            e.is3d = false;
            return zero;
        }
        if (a.size() == 2) {
            if (a[1].type != V::Type::Integer || ni(1) < 1 || ni(1) > 3)
                throw ScriptError(14, "xy selector requires 1, 2 or 3");
            return V::integer(ni(1) == 3 ? point(e.x, e.y) : ni(1) == 1 ? e.x : e.y);
        }
        return V::integer(point(e.x, e.y));
    case 16012:
    case 16013:
        if (op == 16013 && a.size() == 2) {
            if (a[1].type != V::Type::Integer)
                throw ScriptError(14, "propdist requires an integer distance");
            if (!e.is3d)
                e.distance = int16_t(a[1].number);
            return zero;
        }
        if (op == 16013 && !e.is3d)
            return V::integer(e.distance);
        if (!set || !setVisible() || puppet || !e.visible || e.set != low(set->name))
            return V::integer(32000);
        {
            auto frame = renderGame(*this);
            bool drawn = std::any_of(frame.hits.begin(), frame.hits.end(), [&](const GameHit &h) {
                return h.kind == (op == 16012 ? "actor" : "prop") && h.name == e.name;
            });
            if (!drawn)
                return V::integer(32000);
            int degree = movingCamera ? movingCamera->degree : degrees(position.facing);
            double angle = degree * 6.283185307179586 / 256.;
            int x = movingCamera ? movingCamera->x
                                 : position.x * 256 + 128 -
                                       int(std::lround(set->cameraSetback * std::cos(angle)));
            int y = movingCamera ? movingCamera->y
                                 : position.z * 256 + 128 -
                                       int(std::lround(set->cameraSetback * std::sin(angle)));
            int depth = ((e.x - x) * int(std::lround(std::cos(angle) * 16384)) +
                         (e.y - y) * int(std::lround(std::sin(angle) * 16384))) /
                        16384;
            return V::integer(depth);
        }
    default:
        throw ScriptError(1, "unsupported host primitive " + std::to_string(op) +
                                 (wants ? " function" : " statement"));
    }
}
} // namespace dust
