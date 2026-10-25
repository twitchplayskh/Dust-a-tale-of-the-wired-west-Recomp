// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "restoration.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace dust {
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;
uint16_t u16(View, size_t);
int16_t i16(View, size_t);
uint32_t u32(View, size_t);
std::string pstr(View, size_t, size_t);
struct Container {
    int32_t id;
    bool gap;
    Bytes data;
};
struct File {
    std::vector<Container> containers;
    static File parse(View);
    static File open(const std::filesystem::path &);
    View at(size_t) const;
};
struct Sprite {
    int w, h, ax, ay;
    Bytes pixels, mask;
    static Sprite decode(View);
};
struct Layer {
    int x, y;
    std::vector<uint32_t> frames;
};
struct Pose {
    int frame, y, x;
};
using Tick = std::array<Pose, 11>;
struct Line {
    std::string id, text;
    uint32_t audio, track;
    int stance;
};
struct PuppetIdle {
    std::string speech;
    int32_t minimum = 0, maximum = 0;
};
struct PlayerReply {
    uint32_t audio, opaque;
    std::string text, alias;
};
struct PuppetScript {
    std::string name;
    uint32_t container;
};
struct Puppet {
    File file;
    std::string refName;
    std::vector<PuppetScript> scripts;
    std::array<Layer, 11> layers;
    std::array<uint32_t, 256> palette;
    std::vector<Line> lines;
    std::vector<PlayerReply> replies;
    bool playerVoiceEnabled = false;
    std::array<PuppetIdle, 4> idleSlots;
    uint32_t band;
    static Puppet open(const std::filesystem::path &);
    std::vector<Tick> track(size_t line) const;
    std::optional<size_t> findSpeech(const std::string &) const;
    std::optional<size_t> findPlayerReply(const std::string &) const;
    View handlerScript(const std::string &handler) const;
    std::vector<size_t> sequence(const std::string &handler) const;
    std::vector<uint32_t> render(size_t line, size_t tick, std::span<const uint32_t> backdrop = {},
                                 bool grab = false, std::span<const uint32_t> renderPalette = {},
                                 RestorationCache *cache = nullptr,
                                 RestorationSettings settings = {}) const;
};
struct FrameBuffer {
    int w = 0, h = 0;
    Bytes pixels, depth;
    bool hasDepth = false;
    void decode(View);
};
struct Standpoint {
    int x, z, facing;
    bool operator==(const Standpoint &) const = default;
};
struct SceneCell {
    int x, z;
    std::string name;
    uint32_t script;
    bool building;
    int floorIndex = 0; // Raw zero-based floor script slot, scene record WORD +10.
};
struct SceneTransition {
    Standpoint from, to;
    uint32_t firstFrame;
    std::vector<uint32_t> frames;
    int64_t departureStill = -1;
};
struct SceneStar {
    std::string name;
    // World coordinates: x/y are horizontal; z is height (the source stores X,Z,Y).
    int x = 0, y = 0, z = 0, bearing = 0;
};
struct SceneSet {
    File file;
    std::string name;
    int gridW, gridH, viewW, viewH;
    int cameraSetback = 0, eyeHeight = 0;
    uint32_t mainScript;
    std::array<uint32_t, 32> floorScripts{};
    Standpoint initial;
    std::array<std::array<uint32_t, 256>, 3> palettes;
    std::vector<SceneCell> cells;
    std::vector<SceneTransition> transitions;
    std::vector<SceneStar> stars;
    size_t coordinateCopyMismatches = 0;
    static SceneSet open(const std::filesystem::path &);
    const SceneStar *findStar(const std::string &) const;
    FrameBuffer standing(Standpoint) const;
    std::vector<uint32_t> render(const FrameBuffer &, size_t palette = 0,
                                 const Puppet *grabPalette = nullptr) const;
    // Requests only transitions actually present in the authored register.
    const SceneTransition *move(Standpoint, int action) const; // -1 left, 0 forward, 1 right
};
struct Token {
    uint16_t cmd;
    uint32_t info;
    std::string text;
};
std::vector<Token> script(View);
// Speech-only host adapter around the typed VM. A fresh global scope is used
// per call; persistent game sessions use ScriptVM directly (vm.hpp).
void runScript(View, const std::string &, const std::function<void(const std::string &)> &);
struct Audio {
    uint32_t rate;
    std::vector<int16_t> pcm;
};
Audio audio(View);
Bytes wave(const Audio &);
void ppm(const std::filesystem::path &, std::span<const uint32_t>, int, int);
} // namespace dust
