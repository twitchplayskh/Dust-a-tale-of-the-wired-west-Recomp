// SPDX-License-Identifier: GPL-3.0-only
#include "dialogue.hpp"
#include "game_render.hpp"
#include "movie_window.hpp"
#include "sound_window.hpp"
#include "cursor_window.hpp"
#include "window_canvas.hpp"
#include "display_settings.hpp"
#include "controls.hpp"
#include "gpu_presenter.hpp"
#include "visual_effect.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <windowsx.h>
#include <commdlg.h>
using namespace dust;
namespace {
bool integerScaling = false;
constexpr WPARAM testInput = WPARAM(1) << 31;
std::wstring wide(const std::string &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(1252, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(n, L' ');
    MultiByteToWideChar(1252, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}
RECT gameViewport(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const auto v = scaledViewport(client.right, client.bottom, 512, 384, integerScaling);
    return {v.x, v.y, v.x + v.width, v.y + v.height};
}
POINT clientPoint(HWND window, int x, int y) {
    const auto r = gameViewport(window);
    return {r.left + LONG((int64_t(2 * x + 1) * (r.right - r.left)) / 1024),
            r.top + LONG((int64_t(2 * y + 1) * (r.bottom - r.top)) / 768)};
}
struct Application {
    // Preserve a press against the frame the player saw while the script worker
    // finishes its current update. Never enter the VM from two UI events at once.
    struct BufferedClick {
        int x, y;
        std::string kind, name, flat;
        Standpoint position;
    };
    std::optional<BufferedClick> bufferedClick;
    std::string worldFlat;
    Standpoint worldPosition{};
    bool idleTask = false;
    bool mouseIdleTest = false, mouseIdleInjected = false;
    bool mouseIdleCancelDone = false, mouseIdleCancelling = false, mouseIdleSized = false;
    int mouseIdleStage = 0;
    size_t bufferedMousePresses = 0, dispatchedMousePresses = 0;
    KeyInput input;
    GpuPresenter gpu;
    RestorationCache restorationCache;
    std::vector<uint32_t> gpuOverlay;
    size_t gpuPaints = 0;
    bool capturedGpu = false;
    bool gpuFailed = false;
    int gameVolume = 9;
    std::string scriptVoice;
    bool controlsTest = false;
    bool guideTest = false;
    bool storyDialogueTest = false;
    std::vector<int> storyChoices;
    size_t storyChoiceIndex = 0;
    bool storyTimeoutPending = false, storyTimeoutResuming = false;
    int storySteps = 0;
    std::string storyAction, storyExpected;
    bool storyGiving = false;
    POINT storyDrop{};
    std::string storyPicker;
    int storyPickerStep = 0;
    int storyPickerPolls = 0;
    bool fistfightTest = false, fistfightWin = false;
    int fistfightSteps = 0, fistfightPunches = 0;
    bool inventoryInfoTest = false, inventoryInfoMovieSeen = false, inventoryInfoPages = false;
    bool inventoryInfoChapters = false;
    int inventoryInfoChapter = 0;
    uint64_t inventoryInfoContentsHash = 0;
    std::vector<uint32_t> inventoryInfoFirstPage;
    std::vector<uint64_t> inventoryInfoPageHashes;
    int inventoryInfoPage = 0;
    int inventoryInfoStage = 0, inventoryInfoDegree = 0;
    std::string inventoryInfoMovie, inventoryInfoItem;
    bool handoffTest = false;
    bool puppetFramePresented = false;
    size_t puppetUpdatePaints = 0, hiddenPuppetPaints = 0;
    bool itemDragTest = false;
    bool checkpointTest = false;
    bool checkpointKeyPending = false;
    std::string checkpointExpectedSet;
    std::string checkpointClickProp, checkpointExpectedPose;
    bool deathRestoreTest = false;
    bool deathNewTest = false;
    int deathRestoreStage = 0;
    std::filesystem::path deathRestorePath;
    Standpoint deathRestorePosition{};
    bool blackjackTest = false;
    bool checkersTest = false;
    int checkersStage = 0;
    std::string checkersBoard;
    bool endingTest = false;
    bool duelTest = false;
    int blackjackStage = 0;
    int checkpointIdles = 0;
    bool dogGiftTest = false;
    POINT dogDrop{};
    int guideStage = 0;
    int guideItemDepth = 0;
    size_t pointerQueries = 0, guidePointerStart = 0;
    int controlsStage = 0;
    size_t inventoryLabelPaints = 0, keyLabelPaints = 0;
    DisplaySettings preferences;
    RECT windowedRect{};
    HWND window = nullptr;
    std::unique_ptr<GameHost> game;
    std::unique_ptr<DialogueSession> dialogue;
    SoundOutput sound;
    std::unique_ptr<CursorOutput> cursors;
    HCURSOR currentCursor = nullptr;
    POINT pointerClient{-1, -1};
    bool trackingPointer = false;
    bool buttonDown = false;
    std::vector<uint32_t> effectTarget;
    std::vector<uint32_t> effectFrom;
    int effectStep = 0;
    size_t effectFrames = 0;
    bool mapTest = false, mapPresented = false;
    int mapStage = 0;
    size_t mapPaints = 0;
    bool helpTest = false, helpCompleted = false;
    int helpStage = 0;
    size_t helpClicks = 0, helpPaints = 0;
    size_t heldQueries = 0, heldPointerQueries = 0, heldPaints = 0;
    bool helpBevel = false, capturedHeld = false;
    std::optional<std::chrono::steady_clock::time_point> testMoveAt, testReleaseAt;
    bool hoverPrepared = false;
    int hoverUpdates = 0;
    GameFrame world;
    std::vector<uint32_t> pixels = std::vector<uint32_t>(512 * 384, 0xff000000);
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<GameHost::Media> media;
    bool stopped = false, booting = true, taskActive = false, failed = false, inTimer = false;
    uint64_t mediaSerial = 0, handledSerial = 0, activeSerial = 0;
    std::optional<GameHost::Media> timed;
    std::chrono::steady_clock::time_point timedStart, voiceStart, lastIdle;
    std::vector<uint32_t> fadeSource;
    std::optional<DialogueSession::Request> request;
    DialogueSession::ChoiceMemory visibleChoices;
    std::optional<std::pair<uint64_t, size_t>> pressedChoice;
    bool pressedChoiceInside = false;
    bool dialogueDisplayTest = false;
    std::optional<std::chrono::steady_clock::time_point> firstChoiceAt;
    size_t verifiedChoicePaints = 0;
    bool capturedWaitingChoice = false;
    int replyCancelStage = 0;
    std::optional<size_t> line;
    size_t poseTick = 0;
    uint32_t textStart = 0;
    float brightness = 0;
    bool smoke = false, fullIntro = false, testChoicePosted = false, capturedSpeech = false,
         mouseChoice = false, replayTest = false, replayPosted = false;
    size_t replayLines = 0, replayPaints = 0;
    bool captionForLine = false;
    std::array<uint32_t, 256> puppetPalette{};
    uint32_t choiceColour = 0xfcfcfc;
    bool ravenFont = false;
    HDC canvasDC = nullptr;
    HBITMAP canvasBitmap = nullptr;
    HGDIOBJ canvasPrevious = nullptr;
    uint32_t *canvasData = nullptr;
    WindowCanvas presentation;
    bool choicePresented = false, capturedChoice = false, capturedCaption = false;
    int smokeState = 0;
    size_t paints = 0, movies = 0, speech = 0, choices = 0, motionFrames = 0, mouseInputs = 0,
           keyInputs = 0, choicePaints = 0, captionPaints = 0;
    std::string failure;
    std::ofstream log;
    std::filesystem::path snapshot;
    explicit Application(std::filesystem::path root, std::filesystem::path report, bool test,
                         bool full, bool choiceMouse, bool replay, bool map, bool help)
        : smoke(test), fullIntro(full), mouseChoice(choiceMouse), replayTest(replay), mapTest(map),
          helpTest(help) {
        if (!report.parent_path().empty())
            std::filesystem::create_directories(report.parent_path());
        log.open(report);
        if (!log)
            throw std::runtime_error("cannot create game report");
        snapshot = report;
        snapshot.replace_extension("ppm");
        game = std::make_unique<GameHost>(std::move(root), [this](const GameHost::Media &m) {
            std::unique_lock lock(mutex);
            if (stopped)
                throw std::runtime_error("game window closed");
            media = m;
            const auto serial = ++mediaSerial;
            wake.wait(lock, [&] { return stopped || handledSerial >= serial; });
            if (stopped)
                throw std::runtime_error("game window closed");
        });
        dialogue = std::make_unique<DialogueSession>(game->vm, [this](const std::string &s) {
            auto *p = game->currentPuppet();
            return p && p->findSpeech(s).has_value();
        });
        dialogue->setPuppetPresence([this] { return game->currentPuppet() != nullptr; });
        game->setSessionCheckpointHandlers(
            [this] { return dialogue->checkpoint(); },
            [this](const auto &values) { dialogue->restoreCheckpoint(values); });
        game->setPuppetHandler([this](const Puppet *p) {
            dialogue->loadPuppet(
                p ? p->idleSlots : std::array<PuppetIdle, 4>{},
                [p](const std::string &id) { return p && p->findSpeech(id).has_value(); },
                [p](const std::string &id) {
                    return p && p->playerVoiceEnabled && p->findPlayerReply(id).has_value();
                });
        });
        cursors = std::make_unique<CursorOutput>(game->resolve("df.exe"));
        currentCursor = cursors->get("arrow");
        if (smoke) {
            for (const auto *name : {"arrow", "fist", "goleft", "goright", "gostrait", "hand",
                                     "sight", "touch", "watch"}) {
                ICONINFO info{};
                if (!GetIconInfo(cursors->get(name), &info))
                    throw std::runtime_error("cannot inspect original cursor");
                log << "cursor resource " << name << " hotspot=" << info.xHotspot << ","
                    << info.yHotspot << "\n";
                if (info.hbmMask)
                    DeleteObject(info.hbmMask);
                if (info.hbmColor)
                    DeleteObject(info.hbmColor);
            }
        }
    }
    ~Application() {
        stop();
        if (canvasDC && canvasPrevious)
            SelectObject(canvasDC, canvasPrevious);
        if (canvasBitmap)
            DeleteObject(canvasBitmap);
        if (canvasDC)
            DeleteDC(canvasDC);
    }
    void stop() {
        {
            std::lock_guard lock(mutex);
            stopped = true;
            wake.notify_all();
        }
        if (dialogue)
            dialogue->cancel();
        sound.stopAll();
    }
    void start(std::function<void()> task, bool idle = false) {
        if (taskActive || failed)
            return;
        taskActive = true;
        idleTask = idle;
        dialogue->start(std::move(task));
    }
    void acknowledge(uint64_t serial) {
        std::lock_guard lock(mutex);
        handledSerial = std::max(handledSerial, serial);
        media.reset();
        wake.notify_all();
    }
    void error(const std::string &s) {
        failed = true;
        failure = s;
        log << "ERROR " << s << "\n";
        log.flush();
        SetWindowTextW(window, (L"Dust — " + wide(s)).c_str());
        if (smoke)
            PostMessageW(window, WM_CLOSE, 0, 0);
        else
            MessageBoxW(window, wide(s).c_str(), L"Dust", MB_OK | MB_ICONERROR);
    }
    void captureWorld() {
        world = renderGame(*game, &restorationCache, preferences.restoration);
        worldFlat = game->flatName();
        worldPosition = game->standpoint();
        pixels = world.rgb;
        puppetFramePresented = false;
        if (helpTest) {
            const auto bevel = game->props().find("butbevel");
            helpBevel = bevel != game->props().end() && bevel->second.visible;
        }
        // forceupdate must preserve the character and reply panel as well as
        // the cached text while the puppet is visible.
        if (game->puppetVisible() && line)
            redrawPuppet();
    }
    void postTarget(const std::string &kind, const std::string &name) {
        captureWorld();
        for (int y = 0; y < 384; ++y)
            for (int x = 0; x < 512; ++x) {
                const auto *hit = hitGame(world, x, y);
                if (hit && hit->kind == kind && hit->name == name) {
                    const auto p = clientPoint(window, x, y);
                    const auto viewport = gameViewport(window);
                    const int pickedX =
                        int(int64_t(p.x - viewport.left) * 512 / (viewport.right - viewport.left));
                    const int pickedY =
                        int(int64_t(p.y - viewport.top) * 384 / (viewport.bottom - viewport.top));
                    const auto *picked = hitGame(world, pickedX, pickedY);
                    if (!picked || picked->kind != kind || picked->name != name)
                        continue; // A source edge pixel may round outside the target when resized.
                    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON | testInput,
                                 MAKELPARAM(p.x, p.y));
                    return;
                }
            }
        throw std::runtime_error("test target missing: " + kind + ":" + name);
    }
    bool playerSpeech() const {
        return request && (request->origin == DialogueSession::Request::Origin::Player ||
                           request->origin == DialogueSession::Request::Origin::ReplayPlayer);
    }
    std::pair<std::string, std::string> targetAt(int x, int y) const {
        if (x < 0 || x >= 512 || y < 0 || y >= 384)
            return {"none", "none"};
        if (const auto *h = hitGame(world, x, y))
            return {h->kind, h->name};
        if (y >= 264)
            return {"flat", game->flatName()};
        if (const auto *set = game->sceneSet()) {
            const auto p = game->standpoint();
            for (const auto &cell : set->cells)
                if (cell.x == p.x && cell.z == p.z)
                    return {"scene", cell.name};
        }
        return {"scene", "none"};
    }
    void idleAtPointer() {
        const auto r = gameViewport(window);
        int x = -1, y = -1;
        if (pointerClient.x >= r.left && pointerClient.x < r.right && pointerClient.y >= r.top &&
            pointerClient.y < r.bottom) {
            x = int(int64_t(pointerClient.x - r.left) * 512 / (r.right - r.left));
            y = int(int64_t(pointerClient.y - r.top) * 384 / (r.bottom - r.top));
        }
        auto [kind, name] = targetAt(x, y);
        if (smoke)
            log << "hover snapshot " << kind << " " << name << " " << x << "," << y << "\n";
        start(
            [this, x, y, kind, name] {
                game->pointer(x, y, kind, name);
                game->idle();
            },
            true);
    }
    void redrawPuppet() {
        const auto *p = game->currentPuppet();
        if (!p || !line)
            return;
        puppetPalette = p->palette;
        int first = dialogue->parameter(1), last = dialogue->parameter(2);
        if (first < 0 || last < first || last > 256)
            throw std::runtime_error("invalid puppet palette range");
        if (auto *set = game->sceneSet(); set && game->puppetGrab())
            std::copy(set->palettes[0].begin() + first, set->palettes[0].begin() + last,
                      puppetPalette.begin() + first);
        int colour = dialogue->parameter(3);
        if (colour < 0 || colour > 255)
            throw std::runtime_error("invalid puppet choice colour");
        choiceColour = puppetPalette[size_t(colour)];
        // Both original font setters write port+0x38 (verified assembly).
        // Slot6 consequently wins; port+0x3c remains its initialized12px size.
        ravenFont = dialogue->parameter(6) == 16;
        if (!game->puppetVisible()) {
            captureWorld();
            return;
        }
        auto backdrop = world.rgb;
        if (auto *set = game->sceneSet(); set && game->puppetGrab()) {
            auto fb = set->standing(game->standpoint());
            if (backdrop.size() != 512 * 384)
                backdrop.assign(512 * 384, 0xff000000);
            for (int y = 0; y < set->viewH; ++y)
                for (int x = 0; x < set->viewW; ++x)
                    backdrop[size_t(y) * 512 + x] =
                        puppetPalette[fb.pixels[size_t(y * fb.h / set->viewH) * fb.w +
                                                x * fb.w / set->viewW]];
        }
        pixels = p->render(*line, poseTick, backdrop, game->puppetGrab(), puppetPalette,
                           &restorationCache, preferences.restoration);
        puppetFramePresented = true;
    }
    void barnDoorStep(int mode) {
        ++effectStep;
        composeEffect(mode, 512, 384, effectStep, effectFrom, effectTarget, pixels);
        ++effectFrames;
    }
    void presentMedia(const GameHost::Media &m, uint64_t serial) {
        using K = GameHost::Media::Kind;
        // A cancelled gift can reopen inventory in the same conversation.
        // Each original selector entry needs fresh native item/OK clicks.
        if (storyDialogueTest && !storyPicker.empty() && game->flatName() != "avatar")
            storyPickerStep = 0;
        log << "media " << int(m.kind) << " " << m.name << "\n";
        switch (m.kind) {
        case K::Movie: {
            gpu.reset(); // Release the swap chain before the embedded GDI film renderer takes over.
            if (booting && preferences.skipIntro &&
                (m.name == "intro.mov" || m.name == "intro2.mov" || m.name == "intro3.mov")) {
                log << "startup film skipped by launcher setting: " << m.name << "\n";
                break;
            }
            sound.haltVoice();
            MovieWindowOptions options{window, true, smoke && !fullIntro ? size_t(20) : 0};
            options.backdrop = pixels;
            options.backdropWidth = 512;
            options.backdropHeight = 384;
            // Match the current room/puppet fade rather than exposing a bright
            // cached image beneath a transition that started from darkness.
            for (auto &c : options.backdrop)
                c = uint32_t(((c >> 16) & 255) * brightness) << 16 |
                    uint32_t(((c >> 8) & 255) * brightness) << 8 | uint32_t((c & 255) * brightness);
            if (inventoryInfoTest) {
                if (CompareStringOrdinal(wide(m.name).c_str(), -1, wide(inventoryInfoMovie).c_str(),
                                         -1, TRUE) != CSTR_EQUAL)
                    throw std::runtime_error("inventory Info selected unexpected film: " + m.name);
                inventoryInfoMovieSeen = true;
                options.testFrameLimit = 0;
                options.testLiveHotspots = true;
                options.testPreferExitHotspot = true;
            }
            options.integerScaling = integerScaling;
            if (duelTest && m.name == "kiddie.mov") {
                options.testFrameLimit = 0;
                options.testLiveHotspots = true;
            }
            options.masterGain =
                uint8_t(unsigned(volumeGain(preferences.volume)) * unsigned(gameVolume) / 9);
            if (smoke && m.name == "help.mov") {
                options.testWaitingCapture = snapshot;
                options.testWaitingCapture.replace_filename(snapshot.stem().string() + "-help.ppm");
            }
            const auto gameProcedure = GetWindowLongPtrW(window, GWLP_WNDPROC);
            auto result = playMovieWindow(m.path, options);
            *m.reply = ScriptValue::integer(int32_t(result.actionFrames));
            if (!result.embedded || !IsWindow(window) ||
                GetWindowLongPtrW(window, GWLP_WNDPROC) != gameProcedure)
                throw std::runtime_error("movie did not preserve the main game window");
            if (smoke && !result.cancelled && !result.paints)
                throw std::runtime_error("movie never painted the main game window");
            movies += result.movies;
            log << "movie frames=" << result.frames << " played=" << result.observedPlayedSamples
                << " skipped=" << result.skipped << " embedded=" << result.embedded
                << " paints=" << result.paints << "\n";
            log << "movie hotspot clicks=" << result.hotspotClicks
                << " waiting paints=" << result.waitingPaints << "\n";
            if (m.name == "help.mov") {
                helpClicks += result.hotspotClicks;
                helpPaints += result.waitingPaints;
                helpCompleted = result.completed && !result.skipped;
            }
            if (result.cancelled) {
                PostMessageW(window, WM_CLOSE, 0, 0);
                return;
            }
            brightness = 0;
            break;
        }
        case K::Black:
            brightness = 0;
            break;
        case K::Palette:
            brightness = 1.0f;
            captureWorld();
            break;
        case K::Fade:
            if (m.reveal) {
                if (game->currentPuppet()) {
                    if (!line && !game->currentPuppet()->lines.empty())
                        line = 0;
                    redrawPuppet();
                } else
                    captureWorld();
            }
            timed = m;
            activeSerial = serial;
            timedStart = std::chrono::steady_clock::now();
            fadeSource = pixels;
            return;
        case K::Motion:
            ++motionFrames;
            [[fallthrough]];
        case K::Delay:
            captureWorld();
            timed = m;
            activeSerial = serial;
            timedStart = std::chrono::steady_clock::now();
            return;
        case K::Changed:
            if (m.name == "puppet") {
                visibleChoices = {};
                line = 0;
                poseTick = 0;
                redrawPuppet();
            } else {
                if (!game->currentPuppet())
                    visibleChoices = {};
                if (!game->currentPuppet())
                    line.reset();
                captureWorld();
            }
            break;
        case K::PuppetBase:
            if (const auto *puppet = game->currentPuppet()) {
                const auto pose =
                    m.name.empty() ? std::optional<size_t>(0) : puppet->findSpeech(m.name);
                if (pose && *pose < puppet->lines.size()) {
                    line = pose;
                    poseTick = 0;
                    redrawPuppet();
                }
            }
            break;
        case K::Theme:
            if (m.name == "stop")
                sound.haltTheme();
            else
                sound.playTheme(m.bank, uint8_t(m.volume));
            break;
        case K::ThemeVolume:
            if (sound.currentTheme() == m.name)
                sound.setThemeVolume(uint8_t(m.volume));
            break;
        case K::Sound:
            if (m.name == "stop")
                sound.haltSound();
            else
                sound.playSound({m.bank, m.container}, uint8_t(m.volume), m.loop, m.priority,
                                m.multiple);
            break;
        case K::Voice:
            scriptVoice = m.name;
            if (m.name == "stop")
                sound.haltVoice();
            else
                sound.playVoice(m.bank->decode(m.container));
            break;
        case K::StopBank:
            sound.stopBank(*m.bank);
            break;
        case K::SoundVolume:
            if (sound.currentSound() == m.name)
                sound.setSoundVolume(uint8_t(m.volume));
            break;
        case K::SoundLoop:
            if (sound.currentSound() == m.name)
                sound.setSoundLoop(m.loop);
            break;
        case K::Query:
            if (storyDialogueTest && !storyPicker.empty()) {
                if (++storyPickerPolls == 100)
                    log << "native picker flat=" << game->flatName() << " step=" << storyPickerStep
                        << " hand=" << game->vm.global("handitem").text << " held=" << buttonDown
                        << '\n';
                if (storyPickerPolls > 4000)
                    throw std::runtime_error(
                        "native story inventory picker did not finish: flat=" + game->flatName() +
                        " step=" + std::to_string(storyPickerStep) +
                        " hand=" + game->vm.global("handitem").text);
            }
            if (storyDialogueTest && !storyPicker.empty() && game->flatName() == "avatar" &&
                m.name == "button" && !buttonDown && !testReleaseAt) {
                if (storyPickerStep == 0) {
                    storyPickerStep = 1;
                    postTarget("prop", storyPicker);
                } else if (storyPickerStep == 1 &&
                           game->vm.global("handitem").text == storyPicker) {
                    storyPickerStep = 2;
                    postTarget("button", "ok");
                } else if (storyPickerStep == 1) {
                    // The original selector flushes pending presses on entry.
                    // Retry through mouse input if that consumed the first one.
                    postTarget("prop", storyPicker);
                } else if (storyPickerStep == 2) {
                    postTarget("button", "ok");
                }
            }
            if (m.name == "save-path" || m.name == "load-path") {
                std::filesystem::path selected;
                if (deathRestoreTest && m.name == "load-path") {
                    selected = deathRestorePath;
                } else if (controlsTest) {
                    selected = snapshot;
                    selected.replace_extension("dustsave");
                } else if (!smoke) {
                    const auto folder = defaultSettingsPath().parent_path() / L"saves";
                    std::filesystem::create_directories(folder);
                    wchar_t name[32768] = L"Dust.dustsave";
                    OPENFILENAMEW dialog{sizeof(dialog)};
                    dialog.hwndOwner = window;
                    dialog.lpstrFilter = L"Dust native saves (*.dustsave)\0*.dustsave\0\0";
                    dialog.lpstrFile = name;
                    dialog.nMaxFile = 32768;
                    dialog.lpstrInitialDir = folder.c_str();
                    dialog.lpstrDefExt = L"dustsave";
                    dialog.Flags =
                        OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                        (m.name == "save-path" ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
                    if (m.name == "save-path" ? GetSaveFileNameW(&dialog)
                                              : GetOpenFileNameW(&dialog))
                        selected = name;
                }
                const auto utf8 = selected.u8string();
                *m.reply = ScriptValue::string(std::string(utf8.begin(), utf8.end()));
            } else if (m.name == "sound-done" || m.name == "voice-done") {
                *m.reply = ScriptValue::boolean(m.name == "sound-done" ? !sound.soundPlaying()
                                                                       : !sound.voicePlaying());
            } else if (m.name == "modifier-shift" || m.name == "modifier-control") {
                const int key = m.name == "modifier-shift" ? VK_SHIFT : VK_CONTROL;
                *m.reply = ScriptValue::boolean(!smoke && (GetKeyState(key) & 0x8000));
            } else if (m.name == "button" || m.name == "stilldown") {
                *m.reply = ScriptValue::boolean(buttonDown);
                if (buttonDown)
                    ++heldQueries;
            } else if (m.name == "pointer") {
                ++pointerQueries;
                if (buttonDown)
                    ++heldPointerQueries;
                const auto r = gameViewport(window);
                const int x =
                    r.right > r.left
                        ? int(int64_t(pointerClient.x - r.left) * 512 / (r.right - r.left))
                        : -1;
                const int y = r.bottom > r.top
                                  ? int(int64_t(pointerClient.y - r.top) * 384 / (r.bottom - r.top))
                                  : -1;
                *m.reply =
                    ScriptValue::integer(int32_t((uint32_t(uint16_t(x)) << 16) | uint16_t(y)));
            } else if (m.name == "theme-bank" || m.name == "theme-chunk")
                *m.reply = ScriptValue::string(sound.currentTheme(m.name == "theme-chunk"));
            else if (m.name == "sound")
                *m.reply = ScriptValue::string(sound.currentSound());
            else if (m.name == "voice")
                *m.reply = ScriptValue::string(!sound.voicePlaying() ? "none"
                                               : request             ? request->speech
                                                                     : scriptVoice);
            else
                throw std::runtime_error("unsupported device query");
            break;
        case K::Cursor:
            currentCursor = cursors->get(m.name);
            {
                POINT p{};
                GetCursorPos(&p);
                ScreenToClient(window, &p);
                const auto r = gameViewport(window);
                if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom)
                    SetCursor(currentCursor);
            }
            break;
        case K::ScriptText:
            captureWorld();
            break;
        case K::GameVolume:
            gameVolume = m.ticks;
            sound.setGameVolume(uint8_t(m.ticks * 255 / 9));
            break;
        case K::FlushInput: {
            input.flush();
            bufferedClick.reset();
            MSG pending{};
            while (PeekMessageW(&pending, window, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE)) {
            }
            while (PeekMessageW(&pending, window, WM_CHAR, WM_CHAR, PM_REMOVE)) {
            }
            while (PeekMessageW(&pending, window, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_REMOVE)) {
            }
            break;
        }
        case K::Dialog:
            if ((controlsTest || deathRestoreTest) && m.name.starts_with("Could not "))
                throw std::runtime_error(m.name);
            *m.reply = ScriptValue::boolean(
                smoke ? false
                      : MessageBoxW(window, wide(m.name).c_str(), L"Dust",
                                    m.reveal ? MB_YESNO | MB_ICONQUESTION : MB_OK) == IDYES);
            break;
        case K::Quit:
            acknowledge(serial);
            PostMessageW(window, WM_CLOSE, 0, 0);
            return;
        case K::Effect: {
            auto before = std::move(pixels);
            if (game->currentPuppet())
                redrawPuppet();
            else
                captureWorld();
            if (m.effect == 24014)
                break;
            effectTarget = std::move(pixels);
            effectFrom = std::move(before);
            pixels = effectFrom;
            effectStep = 0;
            barnDoorStep(m.effect);
            timed = m;
            activeSerial = serial;
            timedStart = std::chrono::steady_clock::now();
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
        }
        InvalidateRect(window, nullptr, FALSE);
        acknowledge(serial);
    }
    void update() {
        if (inTimer || failed || stopped)
            return;
        inTimer = true;
        try {
            auto now = std::chrono::steady_clock::now();
            if (testMoveAt && now >= *testMoveAt) {
                const auto p = clientPoint(window, 0, 0);
                PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON | testInput, MAKELPARAM(p.x, p.y));
                testMoveAt.reset();
            }
            if (testReleaseAt && now >= *testReleaseAt) {
                PostMessageW(window, WM_LBUTTONUP, testInput,
                             MAKELPARAM(pointerClient.x, pointerClient.y));
                testReleaseAt.reset();
            }
            if (timed) {
                double ticks = std::chrono::duration<double>(now - timedStart).count() * 60;
                double end = timed->ticks;
                if (timed->kind == GameHost::Media::Kind::Effect) {
                    const int passes = effectPasses(timed->effect, 384);
                    const int delay =
                        std::max(1, timed->ticks / (timed->effect == 24009 ? 384 : passes));
                    const int desired = std::min(passes, 1 + int(ticks / delay));
                    while (effectStep < desired)
                        barnDoorStep(timed->effect);
                    end = delay * passes;
                    if (ticks >= end && timed->effect >= 24005 && timed->effect != 24009)
                        pixels = effectTarget;
                }
                if (timed->kind == GameHost::Media::Kind::Fade)
                    brightness = timed->reveal
                                     ? float(std::min(1., ticks / std::max(1, timed->ticks)))
                                     : float(std::max(0., 1. - ticks / std::max(1, timed->ticks)));
                InvalidateRect(window, nullptr, FALSE);
                if (ticks >= end) {
                    timed.reset();
                    acknowledge(activeSerial);
                }
            }
            if (!timed) {
                std::optional<GameHost::Media> pending;
                uint64_t serial = 0;
                {
                    std::lock_guard lock(mutex);
                    if (media) {
                        pending = media;
                        serial = mediaSerial;
                    }
                }
                if (pending)
                    presentMedia(*pending, serial);
            }
            if (mouseIdleTest && idleTask && timed && timed->kind == GameHost::Media::Kind::Delay &&
                timed->name == "update" && !mouseIdleInjected) {
                static const char *buttons[] = {"horn",   "east", "subtitles", "ok",
                                                "avatar", "ok",   "map",       "untitled"};
                mouseIdleInjected = true;
                postTarget(mouseIdleStage == 4 ? "prop" : "button", buttons[mouseIdleStage]);
            }
            if (auto next = dialogue->request();
                next && (!request || next->serial != request->serial)) {
                bool resumeTimedTest = false;
                // Idle/replay speech republishes the suspended menu with a new
                // serial. A timeout fixture must keep waiting for that menu.
                if (storyDialogueTest && storyTimeoutPending) {
                    if (next->kind == DialogueSession::Request::Kind::Speech &&
                        next->origin != DialogueSession::Request::Origin::Script)
                        storyTimeoutResuming = true;
                    else if (next->kind == DialogueSession::Request::Kind::Choice &&
                             storyTimeoutResuming) {
                        resumeTimedTest = true;
                        storyTimeoutResuming = false;
                        log << "native resumed timed choice after auxiliary speech\n";
                    } else {
                        storyTimeoutPending = storyTimeoutResuming = false;
                    }
                }
                request = next;
                input.flush();
                bufferedClick.reset();
                if (next->kind == DialogueSession::Request::Kind::Choice ||
                    !next->choices.empty()) {
                    visibleChoices = {next->choices, next->selected};
                    if (!firstChoiceAt)
                        firstChoiceAt = now;
                }
                pressedChoice.reset();
                captionForLine = dialogue->parameter(7) != 0;
                voiceStart = now;
                textStart = GetTickCount();
                testChoicePosted = resumeTimedTest;
                choicePresented = false;
                if (next->kind == DialogueSession::Request::Kind::Speech) {
                    const auto *p = game->currentPuppet();
                    if (!p)
                        throw std::runtime_error("speech requested without an open puppet");
                    if (playerSpeech()) {
                        const auto reply = p->findPlayerReply(next->speech);
                        if (!reply)
                            throw std::runtime_error("missing player reply " + next->speech);
                        sound.playVoice(audio(p->file.at(p->replies[*reply].audio)));
                    } else {
                        poseTick = 0;
                        line = p->findSpeech(next->speech);
                        if (!line)
                            throw std::runtime_error("missing speech " + next->speech);
                        sound.playVoice(audio(p->file.at(p->lines[*line].audio)));
                    }
                    if (smoke) {
                        const auto levels = sound.stats();
                        log << "audio speech mix theme=" << int(levels.themeGain)
                            << " voice=" << int(levels.voiceGain) << '\n';
                    }
                    if (next->origin == DialogueSession::Request::Origin::Replay)
                        ++replayLines;
                    ++speech;
                    log << "speech " << next->speech << "\n";
                }
                if (next->kind == DialogueSession::Request::Kind::Choice) {
                    ++choices;
                    log << "reply table serial=" << next->serial;
                    for (const auto &choice : next->choices)
                        log << " [" << choice.id << ":" << choice.text << "]";
                    log << "\n";
                }
                redrawPuppet();
                InvalidateRect(window, nullptr, FALSE);
            }
            if (request) {
                if (dialogueDisplayTest && !visibleChoices.choices.empty())
                    InvalidateRect(window, nullptr, FALSE);
                if (dialogueDisplayTest && !replyCancelStage && firstChoiceAt &&
                    now - *firstChoiceAt >= std::chrono::seconds(2) &&
                    request->kind == DialogueSession::Request::Kind::Choice) {
                    const auto serial = request->serial;
                    const auto p = clientPoint(window, 40, 276);
                    replyCancelStage = 1;
                    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON | testInput,
                                 MAKELPARAM(p.x, p.y));
                    UpdateWindow(window);
                    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON | testInput, MAKELPARAM(0, 0));
                    SendMessageW(window, WM_LBUTTONUP, testInput, MAKELPARAM(0, 0));
                    if (!request || request->serial != serial || pressedChoice)
                        throw std::runtime_error("dragging off a reply selected it");
                    replyCancelStage = 2;
                    log << "PASS reply press/drag/release cancellation\n";
                }
                if (request->kind == DialogueSession::Request::Kind::Speech) {
                    const auto *p = game->currentPuppet();
                    auto elapsed = std::chrono::duration<double>(now - voiceStart).count();
                    // Original animation index = elapsed 60 Hz ticks / 2.
                    if (!playerSpeech()) {
                        auto track = p->track(*line);
                        poseTick =
                            track.empty() ? 0 : std::min(track.size() - 1, size_t(elapsed * 30));
                    }
                    redrawPuppet();
                    if (smoke && !capturedSpeech && elapsed >= 0.6) {
                        auto path = snapshot;
                        path.replace_filename(snapshot.stem().string() + "-speech.ppm");
                        ppm(path, pixels, 512, 384);
                        capturedSpeech = true;
                        PostMessageW(window, WM_KEYDOWN, 'T', 0);
                    }
                    if ((handoffTest || dogGiftTest || blackjackTest || endingTest || duelTest ||
                         storyDialogueTest) &&
                        elapsed >= 0.2)
                        sound.haltVoice(); // Shortened speech only in this rendering fixture.
                    if (!sound.voicePlaying()) {
                        dialogue->answer(request->serial);
                        request.reset();
                    }
                    InvalidateRect(window, nullptr, FALSE);
                } else if (smoke && choicePresented && !testChoicePosted &&
                           (!dialogueDisplayTest ||
                            (firstChoiceAt && now - *firstChoiceAt >= std::chrono::seconds(12))) &&
                           request->kind == DialogueSession::Request::Kind::Choice) {
                    if (replayTest && choices == 2 && !replayPosted) {
                        testChoicePosted = true;
                        replayPosted = true;
                        PostMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
                        inTimer = false;
                        return;
                    }
                    size_t row = 0;
                    if (handoffTest) {
                        const auto before = pixels;
                        captureWorld();
                        if (!puppetFramePresented || pixels != before)
                            throw std::runtime_error(
                                "world refresh replaced the conversation composite");
                        ++puppetUpdatePaints;
                    }
                    bool found = false;
                    const int guideChoices[] = {103, 102, 101};
                    const int bets[] = {101, 105, 111, 112};
                    if (storyDialogueTest && storyChoiceIndex >= storyChoices.size())
                        throw std::runtime_error(
                            "native story fixture exhausted its authored replies");
                    const int desired =
                        storyDialogueTest ? storyChoices[storyChoiceIndex++]
                        : duelTest        ? 101
                        : endingTest      ? request->choices.front().id
                        : blackjackTest && choices >= 1 && choices <= 4 ? bets[choices - 1]
                        : dogGiftTest                                   ? 102
                        : guideTest && choices >= 1 && choices <= 3     ? guideChoices[choices - 1]
                        : replayTest && choices == 1                    ? 102
                                                                        : 104;
                    for (size_t i = 0; i < request->choices.size(); ++i)
                        if (request->choices[i].id == desired) {
                            row = i;
                            found = true;
                        }
                    if (!found && desired != -1 && desired != -3)
                        throw std::runtime_error("smoke expected original choice " +
                                                 std::to_string(desired));
                    testChoicePosted = true;
                    if (desired == -3) {
                        if (request->waitTicks < 0)
                            throw std::runtime_error(
                                "native fixture cannot time out untimed choice");
                        storyTimeoutPending = true;
                        log << "native waiting for actual choice timeout ticks="
                            << request->waitTicks << '\n';
                    } else if (desired == -1)
                        PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
                    else if (mouseChoice || dialogueDisplayTest || storyDialogueTest) {
                        const auto p = clientPoint(window, 40, 264 + 24 * int(row) + 12);
                        PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON | testInput,
                                     MAKELPARAM(p.x, p.y));
                    } else
                        PostMessageW(window, WM_KEYDOWN, endingTest ? VK_ESCAPE : WPARAM('1' + row),
                                     0);
                }
                if (!pressedChoice)
                    dialogue->advance(uint32_t(GetTickCount64() * 3 / 50), GetTickCount());
                if (request && request->kind == DialogueSession::Request::Kind::Text) {
                    auto current = dialogue->request();
                    if (!current || current->serial != request->serial)
                        request.reset();
                }
            }
            if (taskActive && dialogue->finished()) {
                taskActive = false;
                idleTask = false;
                if (!dialogue->error().empty())
                    throw std::runtime_error(dialogue->error());
                booting = false;
                captureWorld();
                brightness = 1;
                line.reset();
                request.reset();
                visibleChoices = {};
                log << "event returned frame=" << game->frame() << "\n";
                log.flush();
                InvalidateRect(window, nullptr, FALSE);
            }
            if (!taskActive && !booting) {
                if (bufferedClick) {
                    auto click = std::move(*bufferedClick);
                    bufferedClick.reset();
                    const bool worldTarget =
                        click.flat == "mainpanel" && click.y < 264 && click.kind != "button";
                    if (click.flat == worldFlat &&
                        (!worldTarget || click.position == worldPosition)) {
                        ++dispatchedMousePresses;
                        if (mouseIdleTest)
                            log << "dispatch buffered " << click.kind << ":" << click.name << " "
                                << click.x << "," << click.y << "\n";
                        start([this, click] {
                            game->mouse(click.x, click.y, click.kind, click.name);
                        });
                    }
                } else if (checkpointTest) {
                    if (!checkpointClickProp.empty() && checkpointIdles == 0) {
                        checkpointIdles = 1;
                        postTarget("prop", checkpointClickProp);
                    } else if (checkpointKeyPending) {
                        checkpointKeyPending = false;
                        PostMessageW(window, WM_KEYDOWN, VK_UP, 0);
                    } else if (checkpointIdles++ < 12 ||
                               (!checkpointClickProp.empty() && checkpointIdles < 120 &&
                                game->props().at(checkpointClickProp).pose !=
                                    checkpointExpectedPose)) {
                        idleAtPointer();
                    } else {
                        if (!checkpointExpectedSet.empty() &&
                            (!game->sceneSet() || game->sceneSet()->name != checkpointExpectedSet))
                            throw std::runtime_error("native checkpoint key did not reach " +
                                                     checkpointExpectedSet);
                        if (!checkpointClickProp.empty() &&
                            game->props().at(checkpointClickProp).pose != checkpointExpectedPose)
                            throw std::runtime_error(
                                "native prop click did not reach expected pose " +
                                checkpointExpectedPose);
                        if (!canvasData)
                            throw std::runtime_error("checkpoint did not paint a native canvas");
                        GdiFlush();
                        ppm(snapshot, std::vector<uint32_t>(canvasData, canvasData + 512 * 384),
                            512, 384);
                        log << "PASS native checkpoint display set="
                            << (game->sceneSet() ? game->sceneSet()->name : "none")
                            << " flat=" << game->flatName()
                            << " day=" << game->vm.global("day").number << "\n";
                        if (!checkpointClickProp.empty())
                            log << "PASS native prop click " << checkpointClickProp
                                << " pose=" << checkpointExpectedPose << "\n";
                        log.flush();
                        PostMessageW(window, WM_CLOSE, 0, 0);
                    }
                } else if (deathRestoreTest) {
                    if (deathRestoreStage == 0) {
                        if (game->sceneSet() || game->flatName() != "death")
                            throw std::runtime_error(
                                "native failure checkpoint did not open death screen");
                        ++deathRestoreStage;
                        postTarget("button", deathNewTest ? "new" : "open");
                    } else if (deathRestoreStage == 1) {
                        if (!game->sceneSet() || game->flatName() != "mainpanel")
                            throw std::runtime_error(
                                "native death-screen action did not restore exploration");
                        if (deathNewTest &&
                            (game->vm.global("day").number != 1 ||
                             game->vm.global("clock").number != 3 ||
                             game->vm.global("playercash").number != 5 ||
                             game->actors().size() != 54 || game->props().size() != 73 ||
                             game->props().at("gun").owner != "none"))
                            throw std::runtime_error(
                                "native New did not reset the authored game state");
                        deathRestorePosition = game->standpoint();
                        ++deathRestoreStage;
                        PostMessageW(window, WM_KEYDOWN, VK_LEFT, 0);
                    } else {
                        if (game->standpoint() == deathRestorePosition)
                            throw std::runtime_error(
                                "native input did not resume after death-screen restore");
                        log << (deathNewTest
                                    ? "PASS native death-screen New/reset/resumed movement\n"
                                    : "PASS native death-screen Open/checkpoint restore/resumed "
                                      "movement\n");
                        log.flush();
                        PostMessageW(window, WM_CLOSE, 0, 0);
                    }
                } else if (mouseIdleTest) {
                    testMouseIdle();
                } else if (guideTest) {
                    testGuide();
                } else if (controlsTest && controlsStage < 15) {
                    testControls();
                } else if (auto queued = input.next()) {
                    dispatchKey(queued->key, queued->repeat);
                } else if (smoke && smokeState < 2) {
                    PostMessageW(window, WM_KEYDOWN, smokeState == 0 ? VK_LEFT : VK_RIGHT, 0);
                    ++smokeState;
                } else if (smoke && smokeState == 2) {
                    if (helpTest && helpStage == 0) {
                        helpStage = 1;
                        postTarget("prop", "helpbut");
                        inTimer = false;
                        return;
                    }
                    if (helpTest && helpStage == 1) {
                        if (helpCompleted || movies != (preferences.skipIntro ? 0u : 2u))
                            throw std::runtime_error("dragging off Help did not cancel its button");
                        helpStage = 2;
                        postTarget("prop", "helpbut");
                        inTimer = false;
                        return;
                    }
                    if (helpTest && (!helpCompleted || helpClicks != 1 || helpPaints == 0))
                        throw std::runtime_error(
                            "help did not return through its authored Exit hotspot");
                    if (helpTest && (!heldQueries || !heldPointerQueries || !heldPaints))
                        throw std::runtime_error(
                            "held Help button did not track pointer and paint its bevel");
                    if (mapTest && mapStage < 2) {
                        if (mapStage == 0) {
                            mapStage = 1;
                            postTarget("button", "map");
                        } else if (mapPresented) {
                            if (game->flatName() != "map" || game->setVisible())
                                throw std::runtime_error("map did not suspend the set");
                            mapStage = 2;
                            postTarget("button", "untitled");
                        }
                        inTimer = false;
                        return;
                    }
                    if (mapTest && (game->flatName() != "mainpanel" || !game->setVisible() ||
                                    effectFrames != 16))
                        throw std::runtime_error(
                            "map did not restore exploration through both authored effects");
                    if (mouseChoice && !hoverPrepared) {
                        // Exercise both letterboxing and real hit routing in the UI fixture.
                        SetWindowPos(window, nullptr, 0, 0, 1100, 650, SWP_NOMOVE | SWP_NOZORDER);
                        const auto r = gameViewport(window);
                        if (r.left <= 0 || (r.right - r.left) * 3 != (r.bottom - r.top) * 4)
                            throw std::runtime_error("resized game viewport lost its aspect ratio");
                        const auto inputs = mouseInputs;
                        click(0, int(r.top));
                        if (mouseInputs != inputs)
                            throw std::runtime_error("letterbox click reached game");
                        log << "resized viewport=" << r.left << "," << r.top << "," << r.right
                            << "," << r.bottom << "\n";
                    }
                    captureWorld();
                    ppm(snapshot, pixels, 512, 384);
                    const auto it =
                        std::find_if(world.hits.begin(), world.hits.end(), [](const GameHit &h) {
                            return h.kind == "actor" && h.name == "leroy";
                        });
                    if (it == world.hits.end())
                        throw std::runtime_error("Leroy is absent from the initial hit map");
                    int x = -1, y = -1;
                    for (int yy = it->top; yy < it->bottom && x < 0; ++yy)
                        for (int xx = it->left; xx < it->right; ++xx) {
                            const auto *hit = hitGame(world, xx, yy);
                            if (hit && hit->name == "leroy") {
                                x = xx;
                                y = yy;
                                break;
                            }
                        }
                    if (x < 0)
                        throw std::runtime_error("no opaque Leroy input pixel");
                    const auto p = clientPoint(window, x, y);
                    if (mouseChoice && !hoverPrepared) {
                        hoverPrepared = true;
                        PostMessageW(window, WM_MOUSEMOVE, testInput, MAKELPARAM(p.x, p.y));
                        inTimer = false;
                        return;
                    }
                    // Original BOOTFILE checks the pointer every fourth idle.
                    if (mouseChoice && hoverUpdates < 4) {
                        ++hoverUpdates;
                        idleAtPointer();
                        inTimer = false;
                        return;
                    }
                    if (mouseChoice && currentCursor != cursors->get("touch"))
                        throw std::runtime_error(
                            "original actor hover did not select touch cursor");
                    ++smokeState;
                    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON | testInput,
                                 MAKELPARAM(p.x, p.y));
                } else if (smoke && smokeState >= 3) {
                    if (game->currentPuppet() || game->vm.global("leroyphase").number != 1)
                        throw std::runtime_error("Leroy did not return to exploration with phase1");
                    if (game->actors().at("leroy").star != "town.leroy1") {
                        if (smokeState++ > 400)
                            throw std::runtime_error("Leroy did not finish his return walk");
                        start([this] { game->idle(); });
                        inTimer = false;
                        return;
                    }
                    if (motionFrames != 10 || mouseInputs < (mouseChoice ? 2u : 1u) ||
                        keyInputs < (mouseChoice ? 2u : 3u) || (fullIntro && movies != 3) ||
                        !capturedChoice || !capturedCaption)
                        throw std::runtime_error("native movement/input coverage incomplete");
                    if (replayTest && (!replayPosted || replayLines != 2 || replayPaints == 0))
                        throw std::runtime_error(
                            "native replay did not preserve the saved choice table");
                    log << "PASS startup/movement/mouse/dialogue/return paints=" << paints
                        << " movies=" << movies << " speech=" << speech << " choices=" << choices
                        << " motion=" << motionFrames << " mouse=" << mouseInputs
                        << " keys=" << keyInputs << "\n";
                    log << "choice paints=" << choicePaints << " caption paints=" << captionPaints
                        << "\n";
                    if (dialogueDisplayTest && (verifiedChoicePaints < 20 ||
                                                replyCancelStage != 2 || !capturedWaitingChoice))
                        throw std::runtime_error("reply labels did not survive the choice wait");
                    log << "verified reply label paints=" << verifiedChoicePaints << "\n";
                    if (mapTest && (!mapPresented || mapPaints == 0 || effectFrames != 16))
                        throw std::runtime_error("map presentation coverage incomplete");
                    log << "map paints=" << mapPaints << " barndoor steps=" << effectFrames << "\n";
                    log << "help completed=" << helpCompleted << " hotspot clicks=" << helpClicks
                        << " waiting paints=" << helpPaints << "\n";
                    log << "held queries=" << heldQueries
                        << " pointer queries=" << heldPointerQueries
                        << " held paints=" << heldPaints << "\n";
                    const auto audioStats = sound.stats();
                    if (!audioStats.observedPlayedSamples || audioStats.voicesStarted < speech)
                        throw std::runtime_error("native game audio coverage incomplete");
                    log << "replay lines=" << replayLines << " replay paints=" << replayPaints
                        << " voice starts=" << audioStats.voicesStarted
                        << " played samples=" << audioStats.observedPlayedSamples << "\n";
                    log.flush();
                    ++smokeState;
                    PostMessageW(window, WM_CLOSE, 0, 0);
                } else if (now - lastIdle >= std::chrono::milliseconds(50)) {
                    lastIdle = now;
                    idleAtPointer();
                }
            }
        } catch (const std::exception &e) {
            error(e.what());
        }
        inTimer = false;
    }
    void key(WPARAM k, bool repeat = false) {
        input.down(unsigned(k));
        if (k == VK_F11) {
            if (!repeat)
                toggleFullscreen();
            return;
        }
        ++keyInputs;
        pressedChoice.reset();
        if (request) {
            if (repeat)
                return;
            if (request->kind == DialogueSession::Request::Kind::Speech && k == 'T') {
                captionForLine = true;
                InvalidateRect(window, nullptr, FALSE);
            } else if (request->kind == DialogueSession::Request::Kind::Choice && k == VK_RETURN)
                dialogue->replay(request->serial);
            else if (request->kind == DialogueSession::Request::Kind::Choice && k >= '1' &&
                     k <= '5') {
                size_t row = k - '1';
                if (row < request->choices.size()) {
                    visibleChoices.selected = int(row);
                    dialogue->choose(request->serial, row);
                    request.reset();
                }
            } else if (request->kind == DialogueSession::Request::Kind::Choice && k == VK_ESCAPE) {
                if (dialogue->answer(request->serial, -1))
                    request.reset();
            } else if (request->kind == DialogueSession::Request::Kind::Speech &&
                       (k == VK_SPACE || k == VK_ESCAPE)) {
                sound.haltVoice();
                dialogue->interrupt(request->serial);
                request.reset();
            }
            return;
        }
        if (booting)
            return;
        if (taskActive) {
            input.defer(unsigned(k), repeat);
            return;
        }
        dispatchKey(unsigned(k), repeat);
    }
    void dispatchKey(unsigned k, bool repeat) {
        if (repeat && (k == VK_ESCAPE || k == VK_F1))
            return;
        std::string action;
        if (k == VK_ESCAPE)
            action = "close";
        else if (k == VK_F1)
            action = "help";
        if (!action.empty()) {
            start([this, action] { game->control(action); });
            return;
        }
        auto name = gameKeyName(k);
        if (!name.empty())
            start([this, name, repeat] { game->key(name, repeat); });
    }
    void testControls() {
        auto key = [&](unsigned value) { PostMessageW(window, WM_KEYDOWN, value, 0); };
        switch (controlsStage) {
        case 0:
            ++controlsStage;
            key('I');
            break;
        case 1:
            if (game->flatName() != "avatar")
                throw std::runtime_error("inventory shortcut did not open avatar");
            if (!inventoryLabelPaints) {
                idleAtPointer();
                break;
            }
            ++controlsStage;
            key(VK_ESCAPE);
            break;
        case 2:
            if (game->flatName() != "mainpanel" || !game->setVisible())
                throw std::runtime_error("inventory close did not restore exploration");
            ++controlsStage;
            key(VK_ESCAPE);
            break;
        case 3:
            if (game->flatName() != "score")
                throw std::runtime_error("Escape did not open options");
            if (!keyLabelPaints) {
                idleAtPointer();
                break;
            }
            ++controlsStage;
            key('J');
            break;
        case 4:
            if (game->vm.global("keynorth").text != "j")
                throw std::runtime_error("options did not rebind forward");
            ++controlsStage;
            postTarget("button", "subtitles");
            break;
        case 5:
            if (dialogue->parameter(7) != 1)
                throw std::runtime_error("subtitle checkbox did not enable captions");
            ++controlsStage;
            postTarget("button", "subtitles");
            break;
        case 6:
            if (dialogue->parameter(7) != 0)
                throw std::runtime_error("subtitle checkbox did not disable captions");
            ++controlsStage;
            postTarget("prop", "slider");
            break;
        case 7:
            if (gameVolume != 0 || !heldPointerQueries)
                throw std::runtime_error("dragging volume slider did not reach mute");
            ++controlsStage;
            key(VK_ESCAPE);
            break;
        case 8:
            if (game->flatName() != "mainpanel" || !game->setVisible())
                throw std::runtime_error("options close did not restore exploration");
            ++controlsStage;
            key('M');
            break;
        case 9:
            if (game->flatName() != "map" || game->setVisible())
                throw std::runtime_error("map shortcut failed");
            ++controlsStage;
            key(VK_ESCAPE);
            break;
        case 10:
            ++controlsStage;
            key(VK_ESCAPE);
            break;
        case 11:
            ++controlsStage;
            postTarget("button", "save");
            break;
        case 12: {
            auto saved = snapshot;
            saved.replace_extension("dustsave");
            if (game->flatName() != "score" || !std::filesystem::exists(saved))
                throw std::runtime_error("original Save button did not write a checkpoint");
            ++controlsStage;
            key('K');
            break;
        }
        case 13:
            if (game->vm.global("keynorth").text != "k")
                throw std::runtime_error("checkpoint fixture did not change key binding");
            ++controlsStage;
            postTarget("button", "open");
            break;
        case 14:
            if (game->flatName() != "mainpanel" || !game->setVisible() ||
                game->vm.global("keynorth").text != "j")
                throw std::runtime_error(
                    "original Open button did not restore exploration and binding");
            ++controlsStage;
            log << "PASS native inventory/options/key rebinding/subtitles/volume drag/map "
                << "inventory label paints=" << inventoryLabelPaints
                << " key label paints=" << keyLabelPaints << " save/open restored binding=j\n";
            // Restore audible playback for the existing conversation regression.
            sound.setGameVolume(255);
            gameVolume = 9;
            break;
        }
    }
    void testMouseIdle() {
        static const char *before[] = {"mainpanel", "score",     "score", "score",    "mainpanel",
                                       "avatar",    "mainpanel", "map",   "mainpanel"};
        if (mouseIdleInjected) {
            if (mouseIdleCancelling) {
                if (game->flatName() != "score" || bufferedClick)
                    throw std::runtime_error("queued button release outside did not cancel");
                mouseIdleCancelling = false;
                mouseIdleCancelDone = true;
            } else {
                ++mouseIdleStage;
                if (game->flatName() != before[mouseIdleStage])
                    throw std::runtime_error(
                        "mouse press during idle did not activate menu stage " +
                        std::to_string(mouseIdleStage));
                if (mouseIdleStage == 2 && game->vm.global("seldir").text != "east")
                    throw std::runtime_error("idle-time direction selection click was lost");
                if (mouseIdleStage == 3 && dialogue->parameter(7) != (preferences.captions ? 0 : 1))
                    throw std::runtime_error("idle-time subtitle click was lost");
            }
            mouseIdleInjected = false;
        }
        if (mouseIdleStage == 8) {
            if (bufferedMousePresses != 9 || dispatchedMousePresses != 8 || !mouseIdleCancelDone ||
                keyInputs)
                throw std::runtime_error("idle-time mouse coverage incomplete");
            log << "PASS mouse during idle: options/direction/subtitles/inventory/map and drag "
                   "cancellation; buffered="
                << bufferedMousePresses << " dispatched=" << dispatchedMousePresses
                << " keys=" << keyInputs << "\n";
            log.flush();
            // This fixture has no conversation; its GPU output is captured separately below.
            PostMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        if (!mouseIdleSized) {
            SetWindowPos(window, nullptr, 0, 0, 1100, 650, SWP_NOMOVE | SWP_NOZORDER);
            mouseIdleSized = true;
        }
        idleAtPointer();
    }
    void testGuide() {
        if (storyDialogueTest) {
            if (!storySteps++) {
                if (storyAction.starts_with("actor:"))
                    postTarget("actor", storyAction.substr(6));
                else if (storyAction == "key:uparrow")
                    PostMessageW(window, WM_KEYDOWN, VK_UP, 0);
                else if (storyAction.starts_with("give:")) {
                    const auto colon = storyAction.find(':', 5);
                    if (colon == std::string::npos)
                        throw std::runtime_error("native gift requires give:ITEM:ACTOR");
                    const auto item = storyAction.substr(5, colon - 5),
                               actor = storyAction.substr(colon + 1);
                    if (game->vm.global("handitem").text != item)
                        throw std::runtime_error("native gift item is not selected");
                    captureWorld();
                    std::optional<POINT> drop;
                    int nearest = 512 * 512 + 384 * 384;
                    for (const auto &region : world.hits) {
                        if (region.kind != "actor" || region.name != actor)
                            continue;
                        const int cx = (region.left + region.right) / 2,
                                  cy = (region.top + region.bottom) / 2;
                        for (int y = std::max(0, region.top); y < std::min(264, region.bottom); ++y)
                            for (int x = std::max(0, region.left); x < std::min(512, region.right);
                                 ++x) {
                                const int distance = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                                const auto *hit = hitGame(world, x, y);
                                if (distance < nearest && hit && hit->kind == "actor" &&
                                    hit->name == actor) {
                                    nearest = distance;
                                    drop = clientPoint(window, x, y);
                                }
                            }
                    }
                    if (!drop)
                        throw std::runtime_error("native gift actor is not visible");
                    storyDrop = *drop;
                    storyGiving = true;
                    postTarget("prop", item);
                } else if (storyAction.starts_with("point:")) {
                    auto coords = storyAction.substr(6);
                    std::replace(coords.begin(), coords.end(), ':', ' ');
                    int x = -1, y = -1;
                    std::istringstream input(coords);
                    if (!(input >> x >> y) || x < 0 || x >= 512 || y < 0 || y >= 384)
                        throw std::runtime_error("invalid native story point");
                    const auto p = clientPoint(window, x, y);
                    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON | testInput,
                                 MAKELPARAM(p.x, p.y));
                } else
                    throw std::runtime_error("native story action requires actor:NAME, point:X:Y, "
                                             "give:ITEM:ACTOR or key:uparrow");
                return;
            }
            if (storyChoiceIndex < storyChoices.size()) {
                if (storySteps > 1000)
                    throw std::runtime_error("native story replies were not offered");
                idleAtPointer();
                return;
            }
            std::istringstream expected(storyExpected);
            std::string check;
            while (std::getline(expected, check, ',')) {
                const auto equal = check.find('=');
                if (equal == std::string::npos)
                    throw std::runtime_error("native story expectation requires NAME=VALUE");
                const auto name = check.substr(0, equal), value = check.substr(equal + 1);
                std::string actual;
                if (name == "flat")
                    actual = game->flatName();
                else if (name.starts_with("owner."))
                    actual = game->props().at(name.substr(6)).owner;
                else if (name.starts_with("propdegree."))
                    actual = std::to_string(game->props().at(name.substr(11)).degree);
                else if (name.starts_with("actorowner."))
                    actual = game->actors().at(name.substr(11)).owner;
                else if (name.starts_with("actorvisible."))
                    actual = game->actors().at(name.substr(13)).visible ? "1" : "0";
                else {
                    const auto global = game->vm.global(name);
                    actual = global.type == ScriptValue::Type::String
                                 ? global.text
                                 : std::to_string(global.number);
                }
                if (actual != value)
                    throw std::runtime_error("native story expectation " + check + " was " +
                                             actual);
                log << "PASS native story expected " << check << '\n';
            }
            captureWorld();
            ppm(snapshot, world.rgb, 512, 384);
            log << "PASS native story dialogue replies=" << storyChoiceIndex
                << " action=" << storyAction << "\n";
            log.flush();
            PostMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        if (fistfightTest) {
            if (++fistfightSteps > 4000)
                throw std::runtime_error("native fistfight fixture exceeded its input budget");
            if (game->flatName() == "mainpanel") {
                if (game->actors().at("dell").owner != (fistfightWin ? "lostfight" : "wonfight") ||
                    game->props().size() != 73 || !game->setVisible() ||
                    game->vm.global("lockevents").number || (fistfightWin && !fistfightPunches))
                    throw std::runtime_error("native fistfight outcome/cleanup mismatch");
                captureWorld();
                ppm(snapshot, world.rgb, 512, 384);
                log << "PASS native fistfight " << (fistfightWin ? "win" : "loss")
                    << " punches=" << fistfightPunches << " and exploration return\n";
                log.flush();
                PostMessageW(window, WM_CLOSE, 0, 0);
                return;
            }
            if (game->flatName() != "flat 0" || !game->props().contains("fists"))
                throw std::runtime_error(
                    "native fistfight fixture requires an earned fight checkpoint");
            if (fistfightSteps == 1) {
                captureWorld();
                auto picture = snapshot;
                picture.replace_filename(snapshot.stem().string() + "-fight.ppm");
                ppm(picture, world.rgb, 512, 384);
            }
            if (fistfightWin && !game->vm.global("fightover").number &&
                game->props().at("fists").pose == "rest") {
                const auto p = clientPoint(window, 230, 132);
                ++fistfightPunches;
                PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON | testInput, MAKELPARAM(p.x, p.y));
            } else
                idleAtPointer();
            return;
        }
        if (inventoryInfoTest) {
            const bool historyBook = inventoryInfoItem == "history";
            const bool fullBook = historyBook || inventoryInfoItem == "yunnibook";
            const int backX = historyBook ? 50 : fullBook ? 210 : 100;
            const auto firstFlat = historyBook ? "flat 1" : "flat 0";
            const auto nextFlat = historyBook ? "flat 2" : "flat 1";
            const auto pageClick = [&](int x, int y) {
                const auto p = clientPoint(window, x, y);
                PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON | testInput, MAKELPARAM(p.x, p.y));
            };
            const auto verifyPage = [&](bool remember, bool compare = true) {
                if (game->flatName() != "flat " + std::to_string(inventoryInfoPage))
                    throw std::runtime_error("native book navigation reached the wrong page");
                captureWorld();
                InvalidateRect(window, nullptr, FALSE);
                UpdateWindow(window);
                GdiFlush();
                uint64_t hash = 14695981039346656037ull;
                size_t lit = 0;
                for (size_t i = 0; i < 512 * 384; ++i) {
                    const auto pixel = canvasData[i] & 0xffffff;
                    if (pixel != (world.rgb[i] & 0xffffff))
                        throw std::runtime_error("native book canvas differs from composed page");
                    lit += pixel != 0;
                    hash = (hash ^ pixel) * 1099511628211ull;
                }
                if (lit < 128)
                    throw std::runtime_error("native book page is blank");
                if (remember) {
                    inventoryInfoPageHashes.push_back(hash);
                    auto page = snapshot;
                    page.replace_filename(snapshot.stem().string() + "-page-" +
                                          std::to_string(inventoryInfoPage) + ".ppm");
                    ppm(page, {canvasData, 512 * 384}, 512, 384);
                } else if (compare && hash != inventoryInfoPageHashes.at(size_t(inventoryInfoPage)))
                    throw std::runtime_error("native book page changed after reverse navigation");
                log << "book page=" << inventoryInfoPage << " hash=" << hash << " lit=" << lit
                    << " direction=" << (remember ? "forward" : "back") << '\n';
                return hash;
            };
            log << "inventory Info test stage=" << inventoryInfoStage
                << " flat=" << game->flatName() << " movies=" << movies << "\n";
            log.flush();
            switch (inventoryInfoStage++) {
            case 0:
                inventoryInfoItem = game->vm.global("handitem").text;
                if (inventoryInfoItem.empty() || game->flatName() != "mainpanel")
                    throw std::runtime_error("inventory Info fixture requires a held item");
                inventoryInfoDegree = game->props().at(inventoryInfoItem).degree;
                PostMessageW(window, WM_KEYDOWN, 'I', 0);
                break;
            case 1:
                if (game->flatName() != "avatar")
                    throw std::runtime_error("native inventory did not open for Info");
                postTarget("button", "info");
                break;
            case 2:
                if (movies != (inventoryInfoMovie == "none" ? 0u : 1u) ||
                    (inventoryInfoMovie != "none" && !inventoryInfoMovieSeen))
                    throw std::runtime_error("native Info film count or identity mismatch");
                if (inventoryInfoPages) {
                    if (game->flatName() != firstFlat)
                        throw std::runtime_error("native inventory book did not open");
                    captureWorld();
                    inventoryInfoFirstPage = world.rgb;
                    {
                        auto page = snapshot;
                        page.replace_filename(snapshot.stem().string() + "-firstpage.ppm");
                        ppm(page, world.rgb, 512, 384);
                    }
                    pageClick(historyBook ? 450 : 300, historyBook ? 350 : 150);
                    break;
                }
                inventoryInfoStage = 6;
                if (game->flatName() != "avatar")
                    throw std::runtime_error("native Info film did not return to inventory");
                postTarget("button", "ok");
                break;
            case 3:
                captureWorld();
                if (game->flatName() != nextFlat || world.rgb == inventoryInfoFirstPage)
                    throw std::runtime_error("native inventory book did not show its next page");
                {
                    auto page = snapshot;
                    page.replace_filename(snapshot.stem().string() + "-nextpage.ppm");
                    ppm(page, world.rgb, 512, 384);
                }
                if (inventoryInfoChapters) {
                    if (!historyBook)
                        throw std::runtime_error("chapter fixture requires the history book");
                    inventoryInfoPage = 2;
                    inventoryInfoContentsHash = verifyPage(false, false);
                    inventoryInfoStage = 15;
                    postTarget("button", "ch1");
                    break;
                }
                pageClick(backX, historyBook ? 350 : 150);
                break;
            case 4:
                captureWorld();
                if (game->flatName() != firstFlat || world.rgb != inventoryInfoFirstPage)
                    throw std::runtime_error(
                        "native inventory book did not restore its first page");
                if (!fullBook) {
                    pageClick(5, 5);
                    break;
                }
                inventoryInfoStage = 7;
                inventoryInfoPage = 0;
                pageClick(backX, historyBook ? 350 : 150);
                break;
            case 5:
                if (game->flatName() != "avatar")
                    throw std::runtime_error("native book close did not restore inventory");
                postTarget("button", "ok");
                break;
            case 6:
                if (game->flatName() != "mainpanel" || !game->setVisible() ||
                    game->vm.global("handitem").text != inventoryInfoItem ||
                    game->props().at(inventoryInfoItem).degree != inventoryInfoDegree ||
                    game->props().at(inventoryInfoItem).owner != "stranger")
                    throw std::runtime_error("native Info lost the item or exploration state");
                ppm(snapshot, world.rgb, 512, 384);
                log << "PASS native inventory Info " << inventoryInfoItem
                    << " degree=" << inventoryInfoDegree << " movie=" << inventoryInfoMovie
                    << (inventoryInfoPages ? " pages forward/back/close" : "")
                    << " and exploration return\n";
                if (!inventoryInfoPageHashes.empty())
                    log << "PASS native book all " << inventoryInfoPageHashes.size()
                        << " pages forward/back, endpoint limits and stable painted pixels\n";
                log.flush();
                PostMessageW(window, WM_CLOSE, 0, 0);
                break;
            case 7:
                verifyPage(true);
                if (inventoryInfoPage < (historyBook ? 49 : 20)) {
                    ++inventoryInfoPage;
                    inventoryInfoStage = 7;
                }
                pageClick(historyBook ? 450 : 300, historyBook ? 350 : 150);
                break;
            case 8:
                verifyPage(false); // A further Next click must stay on the last page.
                --inventoryInfoPage;
                pageClick(backX, historyBook ? 350 : 150);
                break;
            case 9:
                verifyPage(false);
                if (inventoryInfoPage > 0) {
                    --inventoryInfoPage;
                    inventoryInfoStage = 9;
                }
                pageClick(backX, historyBook ? 350 : 150);
                break;
            case 10:
                verifyPage(false); // A further Back click must stay on page zero.
                if (!historyBook) {
                    inventoryInfoPage = 11;
                    pageClick(100, 10); // Authored bookmark tab jumps to the fold-out page.
                    break;
                }
                inventoryInfoStage = 5;
                pageClick(5, 5);
                break;
            case 11:
                verifyPage(false);
                if (game->props().at("yunnibord").degree != 2)
                    throw std::runtime_error("native Yunni bookmark did not open");
                pageClick(100, 150);
                break;
            case 12:
                if (game->props().at("yunnibord").degree != 1 ||
                    verifyPage(false, false) == inventoryInfoPageHashes.at(11))
                    throw std::runtime_error("native Yunni bookmark did not fold");
                pageClick(20, 100);
                break;
            case 13:
                verifyPage(false);
                if (game->props().at("yunnibord").degree != 2)
                    throw std::runtime_error("native Yunni bookmark did not unfold");
                --inventoryInfoPage;
                pageClick(210, 150);
                break;
            case 14:
                verifyPage(false);
                if (game->props().at("yunnibord").degree != 0)
                    throw std::runtime_error("native Yunni bookmark survived leaving its page");
                log << "PASS native Yunni bookmark jump/fold/unfold/page-turn\n";
                inventoryInfoStage = 5;
                pageClick(5, 5);
                break;
            case 15: {
                // Destinations from the ten authored HIST.FLT button handlers.
                constexpr int destinations[] = {3, 8, 12, 18, 21, 24, 31, 35, 38, 44};
                inventoryInfoPage = destinations[inventoryInfoChapter];
                const auto hash = verifyPage(false, false);
                if (game->props().at("histbord").degree != 2)
                    throw std::runtime_error("native history chapter lost its contents tab");
                log << "book chapter=" << inventoryInfoChapter + 1 << " page=" << inventoryInfoPage
                    << " hash=" << hash << '\n';
                pageClick(170, 10);
                break;
            }
            case 16:
                inventoryInfoPage = 2;
                if (verifyPage(false, false) != inventoryInfoContentsHash ||
                    game->props().at("histbord").degree != 1)
                    throw std::runtime_error("native chapter tab did not restore contents pixels");
                if (++inventoryInfoChapter < 10) {
                    inventoryInfoStage = 15;
                    postTarget("button", "ch" + std::to_string(inventoryInfoChapter + 1));
                } else {
                    log << "PASS native history all 10 chapter links and contents returns\n";
                    inventoryInfoStage = 5;
                    pageClick(5, 5);
                }
                break;
            default:
                throw std::runtime_error(
                    "inventory Info fixture advanced without its input completing");
            }
            return;
        }
        if (checkersTest) {
            if (checkersStage == 0) {
                if (game->flatName() != "flat 0" || game->vm.global("mecount").number != 12)
                    throw std::runtime_error(
                        "native checkers fixture did not load its starting board");
                checkersBoard = game->vm.global("mainboard").text;
                checkersStage = 1;
                postTarget("prop", "me1");
            } else if (checkersStage == 1) {
                if (game->vm.global("mainboard").text == checkersBoard)
                    throw std::runtime_error("native checker drag did not update the board");
                if (!game->vm.global("playerturn").number) {
                    idleAtPointer();
                    return;
                }
                std::istringstream before(checkersBoard), after(game->vm.global("mainboard").text);
                bool opponentMoved = false;
                for (int i = 0; i < 64; ++i) {
                    int a = 0, b = 0;
                    before >> a;
                    after >> b;
                    opponentMoved |= (a > 0 || b > 0) && a != b;
                }
                if (!opponentMoved)
                    throw std::runtime_error("native checkers opponent did not move");
                log << "PASS native checker drag and original-script opponent reply\n";
                checkersStage = 2;
                postTarget("button", "exit");
            } else {
                if (game->flatName() != "mainpanel" || game->props().size() != 73 ||
                    game->vm.global("score").text != "abort")
                    throw std::runtime_error("native checkers abort did not restore exploration");
                log << "PASS native checkers exit/cleanup/store return\n";
                log.flush();
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            return;
        }
        if (duelTest) {
            if (game->vm.global("clock").number == 3) {
                if (choices != 5 || movies < 6)
                    throw std::runtime_error("native duel missed its authored dialogue/movies");
                auto saved = snapshot;
                saved.replace_extension("dustsave");
                game->saveGame(saved);
                log << "PASS native Kid duel: five choices, timed movie clicks, action frame and "
                       "night transition\n";
                log.flush();
                PostMessageW(window, WM_CLOSE, 0, 0);
            } else if (!guideStage++)
                PostMessageW(window, WM_KEYDOWN, VK_UP, 0);
            else
                throw std::runtime_error("native duel did not advance the clock");
            return;
        }
        if (endingTest) {
            if (game->vm.global("phase").number == 99) {
                auto saved = snapshot;
                saved.replace_extension("dustsave");
                game->saveGame(saved);
                log << "PASS native Day 5 movement/finale/dialogue/ending phase=99 choices="
                    << choices << " movies=" << movies << "\n";
                log.flush();
                PostMessageW(window, WM_CLOSE, 0, 0);
            } else {
                if (!game->sceneSet() || game->sceneSet()->name != "town" ||
                    game->standpoint().x != 6 || game->standpoint().facing != 2 ||
                    game->standpoint().z >= 13)
                    throw std::runtime_error("ending fixture left the authored final street");
                PostMessageW(window, WM_KEYDOWN, VK_UP, 0);
            }
            return;
        }
        if (blackjackTest) {
            if (blackjackStage == 0) {
                ++blackjackStage;
                postTarget("prop", "blackjack");
            } else if (blackjackStage == 1) {
                if (game->flatName() != "flat 2" || game->vm.global("playercount").number != 2)
                    throw std::runtime_error("native blackjack did not deal the initial hand");
                ++blackjackStage;
                auto picture = snapshot;
                picture.replace_filename(snapshot.stem().string() + "-table.ppm");
                GdiFlush();
                ppm(picture, std::vector<uint32_t>(canvasData, canvasData + 512 * 384), 512, 384);
                postTarget("button", "stay");
            } else if (game->flatName() != "mainpanel") {
                idleAtPointer();
            } else {
                if (game->props().size() != 73 || choices < 3 || movies)
                    throw std::runtime_error("native blackjack did not release its stage and shop");
                log << "PASS native blackjack greeting/bet/deal/stay/verdict/quit/return cash="
                    << game->vm.global("playercash").number << "\n";
                log.flush();
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            return;
        }
        if (dogGiftTest) {
            if (guideStage < 19) {
                const unsigned keys[] = {VK_RIGHT, VK_RIGHT, VK_UP, VK_UP};
                PostMessageW(window, WM_KEYDOWN, keys[guideStage++ - 15], 0);
                return;
            }
            if (guideStage == 19) {
                captureWorld();
                bool found = false;
                for (int y = 0; y < 264 && !found; ++y)
                    for (int x = 0; x < 512; ++x) {
                        const auto *hit = hitGame(world, x, y);
                        if (hit && hit->kind == "actor" && hit->name == "dog") {
                            dogDrop = clientPoint(window, x, y);
                            found = true;
                            break;
                        }
                    }
                if (!found)
                    throw std::runtime_error("dog gift target is not visible");
                ++guideStage;
                postTarget("prop", "bone");
                return;
            }
            if (game->actors().at("dog").visible || game->props().at("bone").owner != "none" ||
                game->props().at("ring").owner != "stranger" ||
                game->vm.global("helpphase").number != 3 || choices != 1 || movies != 1)
                throw std::runtime_error("native bone gift/ring progression failed");
            auto checkpoint = snapshot;
            checkpoint.replace_extension("dustsave");
            start([this, checkpoint] { game->saveGame(checkpoint); });
            log << "PASS native bone drag/dog movie/Help/ring checkpoint\n";
            log.flush();
            PostMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        if (guideStage < 9) {
            const unsigned keys[] = {VK_UP, VK_UP,    VK_UP, VK_UP, VK_RIGHT,
                                     VK_UP, VK_RIGHT, VK_UP, VK_UP};
            PostMessageW(window, WM_KEYDOWN, keys[guideStage++], 0);
            return;
        }
        switch (guideStage) {
        case 9:
            if (choices != 3 || !game->props().at("bone").visible)
                throw std::runtime_error("guide choices did not spawn the bone");
            captureWorld();
            if (handoffTest && (puppetUpdatePaints != 3 || !hiddenPuppetPaints))
                throw std::runtime_error("handoff rendering fixture coverage incomplete");
            ppm(snapshot, world.rgb, 512, 384);
            if (handoffTest)
                log << "PASS handoff composite refreshes=" << puppetUpdatePaints
                    << " hidden/closed puppet paints=" << hiddenPuppetPaints << "\n";
            log << "bone spawned xyz=" << game->props().at("bone").x << ","
                << game->props().at("bone").y << "," << game->props().at("bone").z
                << " set=" << game->props().at("bone").set
                << " view=" << game->props().at("bone").pose << "\n";
            log.flush();
            ++guideStage;
            postTarget("prop", "bone");
            break;
        case 10:
            if (game->props().at("bone").owner != "stranger" ||
                game->vm.global("handitem").text != "bone")
                throw std::runtime_error("bone pickup did not update inventory and hand item");
            ++guideStage;
            PostMessageW(window, WM_KEYDOWN, 'I', 0);
            break;
        case 11:
            if (game->flatName() != "avatar" || game->setVisible())
                throw std::runtime_error("bone inventory did not open");
            ++guideStage;
            postTarget("button", "info");
            break;
        case 12:
            if (game->flatName() != "avatar" || movies != (preferences.skipIntro ? 2u : 4u))
                throw std::runtime_error("bone Info did not play its original movie and return");
            ++guideStage;
            PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
            break;
        case 13:
            if (game->flatName() != "mainpanel" || !game->setVisible() ||
                game->vm.global("handitem").text != "bone" ||
                game->props().at("bone").pose != "large")
                throw std::runtime_error("inventory did not restore the selected bone");
            guideItemDepth = game->props().at("bone").distance;
            guidePointerStart = pointerQueries;
            ++guideStage;
            postTarget("prop", "bone");
            break;
        case 14:
            if (game->props().at("bone").x != 316 || game->props().at("bone").y != 320 ||
                game->props().at("bone").distance != guideItemDepth ||
                game->vm.global("handitem").text != "bone" || pointerQueries <= guidePointerStart)
                throw std::runtime_error(
                    "held item drag did not restore the hand item: xy=" +
                    std::to_string(game->props().at("bone").x) + "," +
                    std::to_string(game->props().at("bone").y) +
                    " depth=" + std::to_string(game->props().at("bone").distance) + " expected=" +
                    std::to_string(guideItemDepth) + " hand=" + game->vm.global("handitem").text +
                    " pointer queries=" + std::to_string(pointerQueries - guidePointerStart));
            ppm(snapshot, world.rgb, 512, 384);
            log << (itemDragTest
                        ? "PASS native checkpoint/item drag/release "
                        : "PASS native dog gate/guide choices/bone pickup/inventory/info/return ")
                << "choices=" << choices << " movies=" << movies
                << " handitem=bone drag/release=1\n";
            log.flush();
            ++guideStage;
            PostMessageW(window, WM_CLOSE, 0, 0);
            break;
        }
    }
    void toggleFullscreen() {
        if (!preferences.fullscreen) {
            GetWindowRect(window, &windowedRect);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                         monitor.rcMonitor.right - monitor.rcMonitor.left,
                         monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_FRAMECHANGED);
        } else {
            SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPos(window, nullptr, windowedRect.left, windowedRect.top,
                         windowedRect.right - windowedRect.left,
                         windowedRect.bottom - windowedRect.top, SWP_FRAMECHANGED | SWP_NOZORDER);
        }
        preferences.fullscreen = !preferences.fullscreen;
    }
    void click(int x, int y) {
        const auto r = gameViewport(window);
        if (x < r.left || x >= r.right || y < r.top || y >= r.bottom)
            return;
        ++mouseInputs;
        x = int(int64_t(x - r.left) * 512 / (r.right - r.left));
        y = int(int64_t(y - r.top) * 384 / (r.bottom - r.top));
        if (request) {
            if (request->kind == DialogueSession::Request::Kind::Choice) {
                if (y < 264) {
                    dialogue->replay(request->serial);
                    return;
                }
                int row = (y - 264) / 24;
                if (y >= 264 && row >= 0 && size_t(row) < request->choices.size()) {
                    pressedChoice = {{request->serial, size_t(row)}};
                    log << "reply press serial=" << request->serial << " row=" << row << "\n";
                    pressedChoiceInside = true;
                    InvalidateRect(window, nullptr, FALSE);
                }
            } else {
                sound.haltVoice();
                dialogue->interrupt(request->serial);
                request.reset();
            }
            return;
        }
        if (booting)
            return;
        const auto [kind, name] = targetAt(x, y);
        if (taskActive) {
            if (mouseIdleTest && !idleTask)
                throw std::runtime_error("mouse fixture did not inject during idle");
            if (!bufferedClick) {
                bufferedClick = BufferedClick{x, y, kind, name, worldFlat, worldPosition};
                ++bufferedMousePresses;
                if (mouseIdleTest)
                    log << "buffer during idle " << kind << ":" << name << " " << x << "," << y
                        << "\n";
            }
            return;
        }
        start([this, x, y, kind, name] { game->mouse(x, y, kind, name); });
    }
    void releaseWorldClick(int x, int y) {
        pointerClient = {x, y};
        if (!bufferedClick)
            return;
        const auto r = gameViewport(window);
        if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) {
            bufferedClick.reset();
            return;
        }
        x = int(int64_t(x - r.left) * 512 / (r.right - r.left));
        y = int(int64_t(y - r.top) * 384 / (r.bottom - r.top));
        const auto [kind, name] = targetAt(x, y);
        const auto &click = *bufferedClick;
        const bool screenTarget =
            click.flat != "mainpanel" || click.y >= 264 || click.kind == "button";
        if (screenTarget && (kind != click.kind || name != click.name))
            bufferedClick.reset(); // A button released outside its target is cancelled.
    }
    std::optional<size_t> replyAtClient(int x, int y) const {
        if (!request || request->kind != DialogueSession::Request::Kind::Choice)
            return {};
        const auto r = gameViewport(window);
        if (x < r.left || x >= r.right || y < r.top || y >= r.bottom)
            return {};
        const int logicalY = int(int64_t(y - r.top) * 384 / (r.bottom - r.top));
        if (logicalY < 264 || size_t((logicalY - 264) / 24) >= request->choices.size())
            return {};
        return size_t((logicalY - 264) / 24);
    }
    void moveReply(int x, int y) {
        if (pressedChoice) {
            pressedChoiceInside = replyAtClient(x, y) == pressedChoice->second;
            InvalidateRect(window, nullptr, FALSE);
        }
    }
    void releaseReply(int x, int y) {
        if (!pressedChoice)
            return;
        const auto [serial, row] = *pressedChoice;
        pressedChoice.reset();
        if (request && request->serial == serial && replyAtClient(x, y) == row) {
            log << "reply release serial=" << serial << " row=" << row
                << " id=" << request->choices[row].id << "\n";
            visibleChoices.selected = int(row);
            dialogue->choose(serial, row);
            request.reset();
        }
        InvalidateRect(window, nullptr, FALSE);
    }
    void paint() {
        PAINTSTRUCT ps;
        HDC screen = BeginPaint(window, &ps);
        RECT client;
        GetClientRect(window, &client);
        if (client.right <= 0 || client.bottom <= 0) {
            EndPaint(window, &ps);
            return;
        }
        RECT r{0, 0, 512, 384};
        auto output = pixels;
        if (brightness < 1)
            for (auto &c : output) {
                unsigned red = unsigned(((c >> 16) & 255) * brightness),
                         green = unsigned(((c >> 8) & 255) * brightness),
                         blue = unsigned((c & 255) * brightness);
                c = 0xff000000 | (red << 16) | (green << 8) | blue;
            }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 512;
        info.bmiHeader.biHeight = -384;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (!canvasDC) {
            canvasDC = CreateCompatibleDC(screen);
            canvasBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                            reinterpret_cast<void **>(&canvasData), nullptr, 0);
            if (!canvasDC || !canvasBitmap || !canvasData) {
                EndPaint(window, &ps);
                error("cannot create game drawing surface");
                return;
            }
            canvasPrevious = SelectObject(canvasDC, canvasBitmap);
        }
        // Finish queued GDI reads/drawing before writing this shared DIB's memory.
        GdiFlush();
        std::copy(output.begin(), output.end(), canvasData);
        HDC dc = canvasDC;
        if (!game->puppetVisible()) {
            for (const auto &label : world.labels) {
                auto font = CreateFontW(-12, 0, 0, 0, label.selector == 16 ? FW_NORMAL : FW_BOLD,
                                        FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_TT_ONLY_PRECIS,
                                        CLIP_CHARACTER_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH,
                                        label.selector == 16 ? L"Raven Digital" : L"Arial");
                auto previous = SelectObject(dc, font);
                SetTextColor(dc, RGB(0, 0, 0));
                SetBkMode(dc, TRANSPARENT);
                SetTextAlign(dc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
                auto text = wide(label.text);
                TextOutW(dc, label.x, label.y, text.c_str(), int(text.size()));
                SelectObject(dc, previous);
                DeleteObject(font);
            }
        }
        auto draw = [&](const std::string &text, RECT area, int baseline, uint32_t colour) {
            auto w = wide(text);
            SetTextColor(dc, RGB((colour >> 16) & 255, (colour >> 8) & 255, colour & 255));
            SetBkMode(dc, TRANSPARENT);
            SetTextAlign(dc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
            ExtTextOutW(dc, 8 * r.right / 512, baseline * r.bottom / 384, ETO_CLIPPED, &area,
                        w.data(), UINT(w.size()), nullptr);
        };
        HFONT font = CreateFontW(-std::max(1, int(12 * r.bottom / 384)), 0, 0, 0,
                                 ravenFont ? FW_NORMAL : FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
                                 OUT_TT_ONLY_PRECIS, CLIP_CHARACTER_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH, ravenFont ? L"Raven Digital" : L"Arial");
        auto old = SelectObject(dc, font);
        if (game->puppetVisible() && puppetFramePresented && brightness > 0.9f) {
            if (!visibleChoices.choices.empty()) {
                for (size_t i = 0; i < visibleChoices.choices.size(); ++i) {
                    // ALT31 0x409e00: each band row is [264+i*24,288+i*24).
                    RECT row{8 * r.right / 512, (264 + int(i) * 24) * r.bottom / 384, r.right,
                             (288 + int(i) * 24) * r.bottom / 384};
                    draw(visibleChoices.choices[i].text, row, 280 + int(i) * 24, choiceColour);
                }
                if (request && request->kind == DialogueSession::Request::Kind::Choice)
                    choicePresented = true;
                else if (request && request->origin == DialogueSession::Request::Origin::Replay)
                    ++replayPaints;
                if (visibleChoices.selected >= 0 &&
                    size_t(visibleChoices.selected) < visibleChoices.choices.size()) {
                    const int colour = dialogue->parameter(4);
                    if (colour >= 0 && colour < 256) {
                        const auto rgb = puppetPalette[size_t(colour)];
                        const auto pen = CreatePen(
                            PS_SOLID, 3, RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255));
                        const auto oldPen = SelectObject(dc, pen);
                        const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                        const int top = 264 + visibleChoices.selected * 24;
                        Rectangle(dc, 1, top + 1, 511, top + 23);
                        SelectObject(dc, oldBrush);
                        SelectObject(dc, oldPen);
                        DeleteObject(pen);
                    }
                }
                if (pressedChoice && pressedChoiceInside) {
                    auto pen = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
                    auto oldPen = SelectObject(dc, pen);
                    auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                    auto oldMode = SetROP2(dc, R2_NOT);
                    const int top = 264 + int(pressedChoice->second) * 24;
                    Rectangle(dc, 1, top + 1, 511, top + 23);
                    SetROP2(dc, oldMode);
                    SelectObject(dc, oldBrush);
                    SelectObject(dc, oldPen);
                    DeleteObject(pen);
                }
                if (smoke && request &&
                    request->origin == DialogueSession::Request::Origin::Replay &&
                    replayPaints == 1) {
                    GdiFlush();
                    auto path = snapshot;
                    path.replace_filename(snapshot.stem().string() + "-replay.ppm");
                    ppm(path, {canvasData, 512 * 384}, 512, 384);
                }
                ++choicePaints;
                if (dialogueDisplayTest) {
                    GdiFlush();
                    for (size_t row = 0; row < visibleChoices.choices.size(); ++row) {
                        if (visibleChoices.choices[row].text.empty())
                            continue;
                        size_t ink = 0;
                        for (int y = 266 + int(row) * 24; y < 286 + int(row) * 24; ++y)
                            for (int x = 8; x < 504; ++x) {
                                const auto index = size_t(y) * 512 + x;
                                if ((canvasData[index] & 0xffffff) != (output[index] & 0xffffff))
                                    ++ink;
                            }
                        if (ink < 8) {
                            SelectObject(dc, old);
                            DeleteObject(font);
                            EndPaint(window, &ps);
                            error("reply row rendered without readable label pixels");
                            return;
                        }
                    }
                    ++verifiedChoicePaints;
                    if (!capturedWaitingChoice && firstChoiceAt &&
                        std::chrono::steady_clock::now() - *firstChoiceAt >=
                            std::chrono::seconds(8)) {
                        auto path = snapshot;
                        path.replace_filename(snapshot.stem().string() + "-waiting.ppm");
                        ppm(path, {canvasData, 512 * 384}, 512, 384);
                        capturedWaitingChoice = true;
                    }
                }
                if (smoke && !capturedChoice) {
                    GdiFlush();
                    auto path = snapshot;
                    path.replace_filename(snapshot.stem().string() + "-choice.ppm");
                    ppm(path, {canvasData, 512 * 384}, 512, 384);
                    capturedChoice = true;
                }
            }
            if (request &&
                (request->kind == DialogueSession::Request::Kind::Text ||
                 (request->kind == DialogueSession::Request::Kind::Speech && captionForLine))) {
                std::string caption = request->speech;
                if (request->kind == DialogueSession::Request::Kind::Speech &&
                    game->currentPuppet() && line && !playerSpeech())
                    caption = game->currentPuppet()->lines[*line].text;
                // 0x40a6f0 clears a white plate and draws black at baselines240/256.
                RECT band{0, 224 * r.bottom / 384, r.right, 264 * r.bottom / 384};
                FillRect(dc, &band, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                auto width = [&](const std::string &s) {
                    auto w = wide(s);
                    SIZE size{};
                    GetTextExtentPoint32W(dc, w.data(), int(w.size()), &size);
                    return size.cx * 512 / std::max(1L, r.right);
                };
                size_t firstLength = caption.size();
                if (width(caption) < 496)
                    draw(caption, band, 240, 0);
                else {
                    for (size_t i = caption.size(); i > 0; --i)
                        if (caption[i - 1] == ' ') {
                            firstLength = i - 1;
                            if (width(caption.substr(0, i - 1)) < 496) {
                                draw(caption.substr(0, i - 1), band, 240, 0);
                                draw(caption.substr(i), band, 256, 0);
                                break;
                            }
                        }
                }
                if (request->kind == DialogueSession::Request::Kind::Text)
                    dialogue->textLength(request->serial, firstLength);
                ++captionPaints;
                if (smoke && !capturedCaption) {
                    GdiFlush();
                    auto path = snapshot;
                    path.replace_filename(snapshot.stem().string() + "-caption.ppm");
                    ppm(path, {canvasData, 512 * 384}, 512, 384);
                    capturedCaption = true;
                }
            }
        }
        SelectObject(dc, old);
        DeleteObject(font);
        if (handoffTest && !game->puppetVisible() && !visibleChoices.choices.empty()) {
            GdiFlush();
            for (size_t i = 264 * 512; i < output.size(); ++i)
                if ((canvasData[i] & 0xffffff) != (output[i] & 0xffffff)) {
                    EndPaint(window, &ps);
                    error("hidden conversation left reply text on the world frame");
                    return;
                }
            ++hiddenPuppetPaints;
        }
        HDC composed;
        try {
            composed = presentation.prepare(screen, client.right, client.bottom);
        } catch (const std::exception &e) {
            EndPaint(window, &ps);
            error(e.what());
            return;
        }
        SetStretchBltMode(composed, COLORONCOLOR);
        FillRect(composed, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const auto viewport = gameViewport(window);
        if (viewport.right > viewport.left && viewport.bottom > viewport.top)
            StretchBlt(composed, viewport.left, viewport.top, viewport.right - viewport.left,
                       viewport.bottom - viewport.top, canvasDC, 0, 0, 512, 384, SRCCOPY);
        if (preferences.smoothScaling) {
            GdiFlush();
            gpuOverlay.resize(output.size());
            for (size_t i = 0; i < output.size(); ++i)
                gpuOverlay[i] = (canvasData[i] & 0xffffff) == (output[i] & 0xffffff)
                                    ? 0
                                    : canvasData[i] | 0xff000000;
            try {
                const bool capture = smoke && !capturedGpu &&
                                     ((checkpointTest && brightness > 0.9f) ||
                                      (puppetFramePresented && choicePresented) ||
                                      (mouseIdleTest && worldFlat == "score" &&
                                       world.labels.size() == 3 && brightness > 0.9f));
                gpu.present(window, output, gpuOverlay, 512, 384, viewport, client.right,
                            client.bottom, capture);
                ++gpuPaints;
                if (capture) {
                    int w = 0, h = 0;
                    const auto frame = gpu.readback(w, h);
                    auto path = snapshot;
                    path.replace_filename(snapshot.stem().string() + "-gpu.ppm");
                    ppm(path, frame, w, h);
                    capturedGpu = true;
                    log << "PASS GPU live artwork capture=" << w << "x" << h << "\n";
                }
            } catch (const std::exception &e) {
                gpuFailed = true;
                preferences.smoothScaling = false;
                gpu.reset();
                log << "GPU presentation unavailable; cached restoration continues through GDI: "
                    << e.what() << "\n";
                log.flush();
                BitBlt(screen, 0, 0, client.right, client.bottom, composed, 0, 0, SRCCOPY);
            }
        } else
            BitBlt(screen, 0, 0, client.right, client.bottom, composed, 0, 0, SRCCOPY);
        if (controlsTest && !world.labels.empty() &&
            (game->flatName() != "score" || world.labels.size() == 3) &&
            (game->flatName() == "avatar" || game->flatName() == "score")) {
            GdiFlush();
            for (const auto &label : world.labels) {
                size_t ink = 0;
                for (int y = std::max(0, label.y - 12); y < std::min(384, label.y); ++y)
                    for (int x = std::max(0, label.x);
                         x < std::min(512, label.x + int(label.text.size()) * 12); ++x)
                        if ((canvasData[y * 512 + x] & 0xffffff) == 0 &&
                            (pixels[y * 512 + x] & 0xffffff) != 0)
                            ++ink;
                if (!ink)
                    throw std::runtime_error("menu label did not paint: " + label.text);
            }
            auto &count = game->flatName() == "avatar" ? inventoryLabelPaints : keyLabelPaints;
            if (!count++) {
                auto path = snapshot;
                path.replace_filename(snapshot.stem().string() + "-" + game->flatName() + ".ppm");
                ppm(path, {canvasData, 512 * 384}, 512, 384);
            }
        }
        if (mapTest && !taskActive && game->flatName() == "map") {
            ++mapPaints;
            if (!mapPresented) {
                GdiFlush();
                auto path = snapshot;
                path.replace_filename(snapshot.stem().string() + "-map.ppm");
                ppm(path, {canvasData, 512 * 384}, 512, 384);
            }
            mapPresented = true;
        }
        if (helpTest && helpBevel && buttonDown) {
            ++heldPaints;
            if (!capturedHeld) {
                GdiFlush();
                auto path = snapshot;
                path.replace_filename(snapshot.stem().string() + "-pressed.ppm");
                ppm(path, {canvasData, 512 * 384}, 512, 384);
                capturedHeld = true;
            }
        }
        ++paints;
        EndPaint(window, &ps);
    }
};
Application *app = nullptr;
LRESULT CALLBACK procedure(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE && app && !app->window)
        app->window = h;
    switch (m) {
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1L << 29))) {
            if (!(l & (1L << 30)))
                app->toggleFullscreen();
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        if (!app->preferences.fullscreen) {
            auto r = reinterpret_cast<RECT *>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER);
        }
        return 0;
    }
    case WM_TIMER:
        app->update();
        return 0;
    case WM_PAINT:
        app->paint();
        return 0;
    case WM_SIZE:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT p{};
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            const auto r = gameViewport(h);
            SetCursor(p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom
                          ? app->currentCursor
                          : LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (app->smoke && l != 0 && l != (1L << 30))
            return 0;
        app->key(w, (l & (1L << 30)) != 0);
        return 0;
    case WM_KEYUP:
        app->input.up(unsigned(w));
        return 0;
    case WM_KILLFOCUS:
        app->input.reset();
        app->bufferedClick.reset();
        app->pointerClient = {-1, -1};
        app->pressedChoice.reset();
        app->buttonDown = false;
        if (GetCapture() == h)
            ReleaseCapture();
        return 0;
    case WM_LBUTTONDOWN:
        if (app->smoke && !(w & testInput))
            return 0;
        app->buttonDown = true;
        app->pointerClient = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        SetCapture(h);
        app->click(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (app->smoke) {
            if (app->storyGiving) {
                app->storyGiving = false;
                const auto p = app->storyDrop;
                PostMessageW(h, WM_MOUSEMOVE, MK_LBUTTON | testInput, MAKELPARAM(p.x, p.y));
                app->testReleaseAt =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(220);
                return 0;
            }
            if (app->storyDialogueTest && !app->storyPicker.empty() &&
                app->game->flatName() == "avatar") {
                app->testReleaseAt =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
                return 0;
            }
            if (app->checkersTest && app->checkersStage == 1) {
                const auto p = clientPoint(h, 182, 147);
                PostMessageW(h, WM_MOUSEMOVE, MK_LBUTTON | testInput, MAKELPARAM(p.x, p.y));
                app->testReleaseAt =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(220);
                return 0;
            }
            if (app->dogGiftTest && app->guideStage == 20) {
                const auto p = app->dogDrop;
                PostMessageW(h, WM_MOUSEMOVE, MK_LBUTTON | testInput, MAKELPARAM(p.x, p.y));
                app->testReleaseAt =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(220);
                return 0;
            }
            if (app->mouseIdleTest && app->mouseIdleStage == 3 && !app->mouseIdleCancelDone) {
                app->mouseIdleCancelling = true;
                const auto p = clientPoint(h, 0, 0);
                PostMessageW(h, WM_MOUSEMOVE, MK_LBUTTON | testInput, MAKELPARAM(p.x, p.y));
                PostMessageW(h, WM_LBUTTONUP, testInput, MAKELPARAM(p.x, p.y));
                return 0;
            }
            if (app->guideTest && app->guideStage == 14) {
                const auto p = clientPoint(h, 200, 370);
                PostMessageW(h, WM_MOUSEMOVE, MK_LBUTTON | testInput, MAKELPARAM(p.x, p.y));
                app->testReleaseAt =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(220);
                return 0;
            }
            if (app->controlsTest && app->controlsStage == 7) {
                const auto p = clientPoint(h, 117, 325);
                PostMessageW(h, WM_MOUSEMOVE, MK_LBUTTON | testInput, MAKELPARAM(p.x, p.y));
                app->testReleaseAt =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(220);
                return 0;
            }
            if (app->dialogueDisplayTest && app->replyCancelStage == 1)
                return 0; // This fixture sends its own drag and button release.
            if (app->helpTest && (app->helpStage == 1 || app->helpStage == 2) &&
                app->smokeState == 2) {
                const auto now = std::chrono::steady_clock::now();
                app->testReleaseAt = now + std::chrono::milliseconds(220);
                if (app->helpStage == 1)
                    app->testMoveAt = now + std::chrono::milliseconds(90);
            } else
                PostMessageW(h, WM_LBUTTONUP, testInput, l);
        }
        return 0;
    case WM_LBUTTONUP:
        if (app->smoke && !(w & testInput))
            return 0;
        app->buttonDown = false;
        app->releaseWorldClick(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        app->releaseReply(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (GetCapture() == h)
            ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if (app->buttonDown)
            app->bufferedClick.reset();
        app->buttonDown = false;
        app->pressedChoice.reset();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (app->smoke && !(w & testInput))
            return 0;
        app->pointerClient = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        app->moveReply(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        // Posted smoke messages do not move the physical OS pointer. Tracking
        // that physical pointer would manufacture a leave for the fixture.
        if (!app->smoke && !app->trackingPointer) {
            TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, h, 0};
            app->trackingPointer = TrackMouseEvent(&track) != FALSE;
        }
        return 0;
    case WM_MOUSELEAVE:
        app->pointerClient = {-1, -1};
        app->trackingPointer = false;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        KillTimer(h, 1);
        app->stop();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc < 2)
        return 2;
    try {
        std::filesystem::path report = argc > 2 ? argv[2] : L"game-window.log";
        bool full = argc > 3 && std::wstring(argv[3]) == L"--full-startup-smoke";
        bool mouse = argc > 3 && std::wstring(argv[3]) == L"--mouse-choice-smoke";
        bool replay = argc > 3 && std::wstring(argv[3]) == L"--replay-smoke";
        bool map = argc > 3 && std::wstring(argv[3]) == L"--map-smoke";
        bool help = argc > 3 && std::wstring(argv[3]) == L"--help-smoke";
        bool display = argc > 3 && std::wstring(argv[3]) == L"--dialogue-display-smoke";
        bool controls = argc > 3 && std::wstring(argv[3]) == L"--controls-smoke";
        bool guide = argc > 3 && std::wstring(argv[3]) == L"--guide-smoke";
        bool handoff = argc > 3 && std::wstring(argv[3]) == L"--guide-handoff-smoke";
        bool itemDrag = argc > 3 && std::wstring(argv[3]) == L"--item-drag-smoke";
        bool dogGift = argc > 3 && std::wstring(argv[3]) == L"--dog-gift-smoke";
        bool checkpointKey = argc > 3 && std::wstring(argv[3]) == L"--checkpoint-key-smoke";
        bool checkpointProp = argc > 3 && std::wstring(argv[3]) == L"--checkpoint-prop-smoke";
        if (checkpointProp && argc < 7)
            throw std::runtime_error(
                "--checkpoint-prop-smoke requires checkpoint, prop and expected pose");
        if (checkpointKey && argc < 6)
            throw std::runtime_error("--checkpoint-key-smoke requires checkpoint and expected set");
        bool checkpoint = checkpointKey || checkpointProp ||
                          (argc > 3 && std::wstring(argv[3]) == L"--checkpoint-smoke");
        bool deathNew = argc > 3 && std::wstring(argv[3]) == L"--death-new-smoke";
        bool deathRestore =
            deathNew || (argc > 3 && std::wstring(argv[3]) == L"--death-restore-smoke");
        if (deathRestore && argc < (deathNew ? 5 : 6))
            throw std::runtime_error(
                "--death-restore-smoke requires death and recovery checkpoints");
        bool blackjack = argc > 3 && std::wstring(argv[3]) == L"--blackjack-smoke";
        bool storyDialogue = argc > 3 && std::wstring(argv[3]) == L"--story-dialogue-smoke";
        if (storyDialogue && argc < 8)
            throw std::runtime_error(
                "--story-dialogue-smoke requires checkpoint, action, reply CSV and expected state");
        bool fistfight = argc > 3 && std::wstring(argv[3]) == L"--fistfight-smoke";
        if (fistfight &&
            (argc < 6 || (std::wstring(argv[5]) != L"win" && std::wstring(argv[5]) != L"loss")))
            throw std::runtime_error("--fistfight-smoke requires checkpoint and win|loss");
        bool inventoryChapters = argc > 3 && std::wstring(argv[3]) == L"--inventory-chapters-smoke";
        bool inventoryPages =
            inventoryChapters || (argc > 3 && std::wstring(argv[3]) == L"--inventory-pages-smoke");
        bool inventoryInfo =
            inventoryPages || (argc > 3 && std::wstring(argv[3]) == L"--inventory-info-smoke");
        if (inventoryInfo && argc < 6)
            throw std::runtime_error(
                "--inventory-info-smoke requires checkpoint and expected movie");
        bool checkers = argc > 3 && std::wstring(argv[3]) == L"--checkers-smoke";
        bool ending = argc > 3 && std::wstring(argv[3]) == L"--ending-smoke";
        bool duel = argc > 3 && std::wstring(argv[3]) == L"--duel-smoke";
        bool mouseIdle = argc > 3 && std::wstring(argv[3]) == L"--mouse-idle-smoke";
        if ((itemDrag || dogGift || checkpoint || blackjack || checkers || ending || duel) &&
            argc < 5)
            throw std::runtime_error("--item-drag-smoke requires a native checkpoint");
        bool smoke = full || mouse || replay || map || help || display || controls || guide ||
                     handoff || itemDrag || mouseIdle || dogGift || checkpoint || blackjack ||
                     checkers || ending || duel || deathRestore || inventoryInfo || fistfight ||
                     storyDialogue || (argc > 3 && std::wstring(argv[3]) == L"--smoke");
        DisplaySettings preferences;
        bool configured = false;
        preferences.integerScaling = false; // Preserve direct probe/legacy CLI behavior.
        for (int i = 3; i < argc; ++i)
            if (std::wstring(argv[i]) == L"--settings") {
                if (++i >= argc)
                    throw std::runtime_error("--settings requires a file");
                preferences = loadSettings(std::filesystem::absolute(argv[i]));
                configured = true;
            }
        integerScaling = preferences.integerScaling;
        Application state(argv[1], report, smoke, full, mouse, replay, map, help);
        state.dialogueDisplayTest = display;
        state.controlsTest = controls;
        state.mouseIdleTest = mouseIdle;
        state.guideTest = guide || handoff || itemDrag || dogGift || blackjack || checkers ||
                          ending || duel || inventoryInfo || fistfight || storyDialogue;
        state.storyDialogueTest = storyDialogue;
        if (storyDialogue) {
            state.storyAction = std::filesystem::path(argv[5]).string();
            state.storyExpected = std::filesystem::path(argv[7]).string();
            if (argc > 8)
                state.storyPicker = std::filesystem::path(argv[8]).string();
            std::istringstream replies(std::filesystem::path(argv[6]).string());
            std::string reply;
            while (std::getline(replies, reply, ','))
                if (reply != "none")
                    state.storyChoices.push_back(std::stoi(reply));
            if (state.storyChoices.empty() && std::filesystem::path(argv[6]).string() != "none")
                throw std::runtime_error("native story fixture requires replies");
        }
        state.fistfightTest = fistfight;
        state.fistfightWin = fistfight && std::wstring(argv[5]) == L"win";
        state.inventoryInfoTest = inventoryInfo;
        state.inventoryInfoPages = inventoryPages;
        state.inventoryInfoChapters = inventoryChapters;
        if (inventoryInfo)
            state.inventoryInfoMovie = std::filesystem::path(argv[5]).string();
        state.handoffTest = handoff;
        state.itemDragTest = itemDrag;
        state.checkpointTest = checkpoint;
        state.checkpointKeyPending = checkpointKey;
        if (checkpointKey)
            state.checkpointExpectedSet = std::filesystem::path(argv[5]).string();
        if (checkpointProp) {
            state.checkpointClickProp = std::filesystem::path(argv[5]).string();
            state.checkpointExpectedPose = std::filesystem::path(argv[6]).string();
        }
        state.deathRestoreTest = deathRestore;
        state.deathNewTest = deathNew;
        if (deathRestore && !deathNew)
            state.deathRestorePath = std::filesystem::absolute(argv[5]);
        state.blackjackTest = blackjack;
        state.checkersTest = checkers;
        state.endingTest = ending;
        state.duelTest = duel;
        state.dogGiftTest = dogGift;
        if (dogGift)
            state.guideStage = 15;
        if (itemDrag)
            state.guideStage = 13;
        state.preferences = preferences;
        state.restorationCache.setBudget(size_t(preferences.restorationCacheMiB) * 1024 * 1024);
        state.dialogue->setParameter(7, preferences.captions ? 1 : 0);
        state.sound.setMasterGain(volumeGain(preferences.volume));
        state.log << "launcher settings integer=" << integerScaling
                  << " smoothGPU=" << preferences.smoothScaling
                  << " restoration=" << preferences.restoration.enabled
                  << " restorationAlgorithm=multiscale-temporal-v4"
                  << " deblock=" << preferences.restoration.deblock
                  << " chroma=" << preferences.restoration.chroma
                  << " detail=" << preferences.restoration.detail
                  << " multiscale=" << preferences.restoration.multiscale
                  << " temporal=" << preferences.restoration.temporal
                  << " temporalStrength=" << preferences.restoration.temporalStrength
                  << " volume=" << preferences.volume << " captions=" << preferences.captions
                  << " skipIntro=" << preferences.skipIntro << "\n";
        app = &state;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW cls{};
        cls.lpfnWndProc = procedure;
        cls.hInstance = instance;
        cls.lpszClassName = L"DustNativeGame";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&cls);
        RECT rect{0, 0, preferences.width, preferences.height};
        AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
        POINT pointer{};
        GetCursorPos(&pointer);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST), &monitor);
        const int width =
            std::min(int(rect.right - rect.left), int(monitor.rcWork.right - monitor.rcWork.left));
        const int height =
            std::min(int(rect.bottom - rect.top), int(monitor.rcWork.bottom - monitor.rcWork.top));
        const int left =
            monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
        const int top =
            monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
        state.window = CreateWindowExW(0, cls.lpszClassName,
                                       smoke ? L"Dust — automated verification"
                                             : L"Dust: A Tale of the Wired West",
                                       WS_OVERLAPPEDWINDOW | WS_VISIBLE, left, top, width, height,
                                       nullptr, nullptr, instance, nullptr);
        if (!state.window)
            throw std::runtime_error("cannot create game window");
        if (preferences.fullscreen) {
            state.preferences.fullscreen = false;
            state.toggleFullscreen();
        }
        if (smoke && configured) {
            RECT before{}, after{};
            GetWindowRect(state.window, &before);
            SendMessageW(state.window, WM_SYSKEYDOWN, VK_RETURN, 1L << 29);
            SendMessageW(state.window, WM_SYSKEYDOWN, VK_RETURN, 1L << 29);
            GetWindowRect(state.window, &after);
            if (!EqualRect(&before, &after))
                throw std::runtime_error("fullscreen toggle changed restored bounds");
            state.log << "PASS fullscreen/windowed/restore toggle\n";
        }
        const auto viewport = gameViewport(state.window);
        state.log << "game viewport=" << viewport.left << "," << viewport.top << ".."
                  << viewport.right << "," << viewport.bottom << "\n";
        SetTimer(state.window, 1, 16, nullptr);
        state.start([&state, itemDrag, dogGift, checkpoint, blackjack, checkers, ending, duel,
                     deathRestore, inventoryInfo, fistfight, storyDialogue, argv] {
            if (itemDrag || dogGift || checkpoint || blackjack || checkers || ending || duel ||
                deathRestore || inventoryInfo || fistfight || storyDialogue)
                state.game->loadGame(argv[4]);
            else
                state.game->boot();
        });
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const auto restored = state.restorationCache.stats();
        state.log << "Restoration cache hits=" << restored.hits << " misses=" << restored.misses
                  << " entries=" << restored.entries << " bytes=" << restored.bytes
                  << " budget=" << restored.budget << " temporalFrames=" << restored.temporalFrames
                  << " temporalPixels=" << restored.temporalPixels << "\n";
        state.log << "GPU restored artwork paints=" << state.gpuPaints << "\n";
        state.log << "mouse input presses=" << state.mouseInputs
                  << " buffered=" << state.bufferedMousePresses
                  << " dispatched=" << state.dispatchedMousePresses << "\n";
        state.log.flush();
        if (smoke && (state.gpuFailed ||
                      (preferences.smoothScaling && (!state.gpuPaints || !state.capturedGpu))))
            return 1;
        return state.failed ? 1 : 0;
    } catch (const std::exception &e) {
        MessageBoxW(nullptr, wide(e.what()).c_str(), L"Dust", MB_OK | MB_ICONERROR);
        return 1;
    }
}
