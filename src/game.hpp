// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "contexts.hpp"
#include "vm.hpp"
#include "sound.hpp"
#include <map>
#include <memory>
#include <set>

namespace dust {
class CheckpointArchive;
// The host owns game state; all mutations occur on the script thread. A media
// callback is synchronous so the original interpreter cannot outrun playback.
class GameHost {
  public:
    struct Media {
        enum class Kind {
            Movie,
            Fade,
            Black,
            Theme,
            Sound,
            Delay,
            Changed,
            Motion,
            Query,
            StopBank,
            SoundVolume,
            SoundLoop,
            Cursor,
            Effect,
            ScriptText,
            GameVolume,
            FlushInput,
            Dialog,
            Quit,
            Voice,
            ThemeVolume,
            PuppetBase,
            Palette
        } kind;
        std::filesystem::path path;
        std::string name;
        int ticks = 0;
        bool reveal = false;
        std::shared_ptr<const SoundBank> bank;
        uint32_t container = 0;
        int volume = 255;
        bool loop = false;
        std::shared_ptr<std::optional<ScriptValue>> reply;
        int effect = 24014;
        uint32_t priority = 0;
        bool multiple = false;
    };
    using MediaHandler = std::function<void(const Media &)>;
    struct Entity {
        std::string name, group, owner = "none", set = "none", star = "custom", pose;
        int x = 0, y = 0, z = 0, degree = 0, scale = 1000, speed = 1, turn = 1;
        int value = 0, zclip = 0, hitW = 0, hitH = 0, poseStep = 0, distance = 0;
        bool visible = false, is3d = true, starPending = false;
    };
    explicit GameHost(std::filesystem::path original, MediaHandler);
    ScriptVM vm;
    void boot();
    void key(const std::string &, bool repeat = false);
    void control(const std::string &);
    // Native checkpoints capture exploration state, never an active VM continuation.
    void saveGame(const std::filesystem::path &);
    void loadGame(const std::filesystem::path &);
    void setSessionCheckpointHandlers(std::function<std::vector<uint32_t>()> capture,
                                      std::function<void(const std::vector<uint32_t> &)> restore) {
        captureSession = std::move(capture);
        restoreSession = std::move(restore);
    }
    struct TextLabel {
        std::string text;
        int x, y, font, selector;
    };
    const std::vector<TextLabel> &textLabels() const;
    void actorEvent(const std::string &, const std::string &handler = "mousedown");
    void mouse(int x, int y, std::string hitKind, std::string hitName);
    void pointer(int x, int y, std::string hitKind, std::string hitName);
    void idle();
    void setPuppetHandler(std::function<void(const Puppet *)> handler) {
        puppetChanged = std::move(handler);
    }
    const SceneSet *sceneSet() const {
        return set.get();
    }
    Standpoint standpoint() const {
        return position;
    }
    const FrameBuffer *motionFrame() const {
        return motionImage ? &*motionImage : nullptr;
    }
    struct CameraPose {
        int x, y, degree;
    };
    std::optional<CameraPose> motionCamera() const {
        return movingCamera;
    }
    const ResourceContexts *stageResource() const {
        return stage;
    }
    const std::string &flatName() const {
        return flat;
    }
    const Puppet *currentPuppet() const {
        return puppet.get();
    }
    const std::map<std::string, Entity> &actors() const {
        return actorState;
    }
    const std::map<std::string, Entity> &props() const {
        return propState;
    }
    const ResourceContexts *resourceFor(const Entity &, bool actor) const;
    std::filesystem::path resolve(const std::string &) const;
    const std::vector<std::string> &trace() const {
        return events;
    }
    uint64_t frame() const {
        return frameNumber;
    }
    bool setVisible() const {
        return settings.at(16007).number != 0;
    }
    bool stageVisible() const {
        return settings.at(16008).number != 0;
    }
    bool puppetVisible() const {
        auto it = settings.find(16050);
        return puppet && it != settings.end() && it->second.number != 0;
    }
    bool puppetGrab() const {
        auto it = settings.find(12074);
        return it != settings.end() && it->second.number != 0;
    }
    void applyPalette(std::array<uint32_t, 256> &palette) const {
        for (int i = 0; i < 256; ++i) {
            const auto it = settings.find(30000 + i);
            if (it != settings.end())
                palette[i] = uint32_t(it->second.number);
        }
    }

  private:
    struct Context {
        std::string name;
        std::shared_ptr<ScriptProgram> program;
        Context *parent = nullptr;
    };
    struct Loop {
        std::string kind, name, handler;
        int interval;
        uint64_t due;
        bool paused = false;
    };
    struct Walk {
        std::string name, dest;
        int x, y, z, degree;
        bool turning = false, paused = false;
        int sx = 0, sy = 0, sz = 0, distance = 1, progress = 0;
        bool facingPending = true;
    };
    struct Ball {
        std::string name;
        int pause = 0, vx = 0, vy = 0, vz = 0, gravity = 0, bounce = 0, frames = 0;
        int degree = 0, x = 0, y = 0, z = 0, tx = 0, ty = 0;
    };
    std::filesystem::path root;
    std::map<std::string, std::filesystem::path> files;
    std::vector<std::unique_ptr<ResourceContexts>> resources;
    std::vector<std::pair<std::string, int>> resourceFiles;
    std::string setFile;
    std::function<std::vector<uint32_t>()> captureSession;
    std::function<void(const std::vector<uint32_t> &)> restoreSession;
    std::vector<uint32_t> sessionCheckpoint;
    std::vector<std::unique_ptr<Context>> contexts;
    std::vector<std::unique_ptr<Context>> retiredContexts;
    std::vector<std::unique_ptr<ResourceContexts>> retiredResources;
    std::map<std::string, Context *> casts, shops, actorContexts, propContexts, flats,
        puppetContexts;
    std::map<std::string, Entity> actorState, propState;
    std::vector<std::string> actorOrder, propOrder;
    std::vector<Context *> stack;
    Context *bootContext = nullptr, *stageContext = nullptr, *setContext = nullptr;
    std::map<std::string, Context *> sceneContexts;
    std::map<int, Context *> floorContexts;
    ResourceContexts *stage = nullptr;
    std::unique_ptr<SceneSet> set;
    std::unique_ptr<Puppet> puppet;
    Standpoint position{};
    std::optional<FrameBuffer> motionImage;
    std::optional<CameraPose> movingCamera;
    uint64_t worldGeneration = 0;
    std::string flat = "none", puppetName = "none", cursorName = "arrow";
    std::map<std::string, std::vector<TextLabel>> flatLabels;
    std::set<std::string> repaintedTextProps;
    std::map<int, ScriptValue> settings;
    ScriptValue lastResult = ScriptValue::integer(0);
    int mouseX = 0, mouseY = 0;
    std::string hitKind = "none", hitName = "none";
    std::map<int, std::string> paths;
    std::map<std::string, std::array<int, 3>> knownStars;
    SoundLibrary soundLibrary;
    std::map<std::string, int> soundVolumes;
    std::map<std::string, bool> soundLoops;
    std::vector<Loop> loops;
    std::vector<Walk> walks;
    std::array<std::optional<Ball>, 16> balls;
    std::array<int, 2> playerHitbox{};
    bool servicingBalls = false;
    std::vector<std::string> events;
    uint64_t frameNumber = 0;
    MediaHandler media;
    std::function<void(const Puppet *)> puppetChanged;
    Context *context(std::string, View, Context *);
    ScriptResult call(Context *, const std::string &, const std::vector<ScriptValue> & = {},
                      const std::string &me = "", const std::string &target = "",
                      bool value = false, bool fallback = true);
    ScriptResult route(const ScriptRoute &);
    void loadResource(const std::string &, ContextKind, bool initialize = true);
    void closeStage();
    void closeEntityResource(const std::string &, ContextKind);
    void checkpointState(CheckpointArchive &);
    void openSet(const std::string &);
    void closeSet();
    Context *sceneContext(const SceneCell &);
    Context *floorContext(int);
    const SceneCell *currentCell() const;
    void changeScene(const std::string &);
    void update(bool wait = true);
    void clearRepaintedText();
    void updateBalls();
    void bind();
    ScriptValue command(uint16_t, const std::vector<ScriptValue> &, bool);
    Entity &entity(const std::string &, bool actor);
    std::array<int, 3> starPosition(const std::string &) const;
};
} // namespace dust
