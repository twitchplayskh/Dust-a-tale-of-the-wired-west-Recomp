// SPDX-License-Identifier: GPL-3.0-only
#include "movie_window.hpp"
#include <iostream>
#include "movie.hpp"
#include "window_canvas.hpp"
#include "display_settings.hpp"
#include <mmsystem.h>
#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
namespace dust {
namespace {
constexpr wchar_t movieStateProperty[] = L"DustNativeMovieState";
constexpr UINT_PTR movieTimer = 0xD057;
struct PaintSession {
    HWND window;
    PAINTSTRUCT paint{};
    HDC dc;
    explicit PaintSession(HWND target) : window(target), dc(BeginPaint(target, &paint)) {}
    ~PaintSession() {
        EndPaint(window, &paint);
    }
};
class Channel {
    HWAVEOUT device_ = nullptr;
    std::vector<Audio> sounds_;
    std::vector<WAVEHDR> headers_;

  public:
    ~Channel() {
        stop();
    }
    void stop() {
        if (!device_)
            return;
        waveOutReset(device_);
        for (auto &h : headers_)
            if (h.dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(device_, &h, sizeof(h));
        waveOutClose(device_);
        device_ = nullptr;
        headers_.clear();
        sounds_.clear();
    }
    bool playing() const {
        return device_ && !headers_.empty() && !(headers_.back().dwFlags & WHDR_DONE);
    }
    uint64_t played() const {
        if (!device_)
            return 0;
        MMTIME time{};
        time.wType = TIME_SAMPLES;
        return waveOutGetPosition(device_, &time, sizeof(time)) == MMSYSERR_NOERROR &&
                       time.wType == TIME_SAMPLES
                   ? time.u.sample
                   : 0;
    }
    uint64_t start(const Movie &m, const std::vector<uint32_t> &refs, bool loop, size_t loopFrom,
                   uint8_t gain) {
        stop();
        if (refs.empty())
            return 0;
        for (auto ref : refs)
            sounds_.push_back(m.sound(ref));
        uint32_t rate = 0;
        for (auto &s : sounds_)
            rate = std::max(rate, s.rate);
        uint64_t samples = 0;
        for (auto &s : sounds_) {
            // waveOut's queued buffers share one format. Shipped intro beds mix
            // 11025 and 22050 Hz chunks; preserve their durations by resampling
            // to the highest source rate with linear interpolation.
            if (s.rate != rate) {
                if (!s.rate || s.pcm.empty())
                    throw std::runtime_error("invalid movie bed audio");
                uint64_t count = (uint64_t(s.pcm.size()) * rate + s.rate - 1) / s.rate;
                if (count > std::numeric_limits<DWORD>::max() / 2)
                    throw std::runtime_error("resampled movie bed exceeds device buffer");
                std::vector<int16_t> pcm;
                pcm.reserve(size_t(count));
                for (uint64_t i = 0; i < count; ++i) {
                    auto source = i * s.rate;
                    auto index = size_t(source / rate);
                    auto fraction = int64_t(source % rate);
                    int64_t a = s.pcm[std::min(index, s.pcm.size() - 1)],
                            b = s.pcm[std::min(index + 1, s.pcm.size() - 1)];
                    pcm.push_back(int16_t(a + (b - a) * fraction / rate));
                }
                s.pcm = std::move(pcm);
                s.rate = rate;
            }
            if (s.pcm.size() > std::numeric_limits<DWORD>::max() / sizeof(int16_t))
                throw std::runtime_error("movie audio exceeds device buffer");
            samples += s.pcm.size();
            scalePcm(s.pcm, gain);
        }
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 1;
        fmt.nSamplesPerSec = rate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 2;
        fmt.nAvgBytesPerSec = rate * 2;
        if (waveOutOpen(&device_, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
            throw std::runtime_error("movie audio output device unavailable");
        headers_.resize(sounds_.size());
        for (size_t i = 0; i < sounds_.size(); ++i) {
            auto &h = headers_[i];
            h = {};
            h.lpData = reinterpret_cast<LPSTR>(sounds_[i].pcm.data());
            h.dwBufferLength = DWORD(sounds_[i].pcm.size() * 2);
            if (loop && i == loopFrom) {
                h.dwFlags |= WHDR_BEGINLOOP;
                h.dwLoops = std::numeric_limits<DWORD>::max();
            }
            if (loop && i + 1 == sounds_.size())
                h.dwFlags |= WHDR_ENDLOOP;
            if (waveOutPrepareHeader(device_, &h, sizeof(h)) != MMSYSERR_NOERROR)
                throw std::runtime_error("cannot prepare movie sound");
        }
        for (auto &h : headers_)
            if (waveOutWrite(device_, &h, sizeof(h)) != MMSYSERR_NOERROR)
                throw std::runtime_error("cannot submit movie sound");
        return samples;
    }
};
struct WindowState {
    WNDPROC previousProcedure = nullptr;
    MovieWindowOptions options;
    MovieWindowResult result;
    std::filesystem::path path;
    std::unique_ptr<Movie> movie;
    std::unique_ptr<MoviePlayer> player;
    MovieNavigation navigation;
    Channel voice, bed;
    WindowCanvas canvas;
    std::exception_ptr failure;
    bool running = true;
    std::optional<std::pair<size_t, size_t>> paintedFrame, automaticClick;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    uint64_t now() const {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - epoch)
                            .count()) *
               60 / 1000000;
    }
    void audioEvents() {
        result.actionFrames |= player->actionFrames();
        for (auto &e : player->drainAudioEvents()) {
            if (e.kind == MovieAudioEvent::Kind::Voice) {
                result.observedPlayedSamples += voice.played();
                result.submittedPcmSamples +=
                    voice.start(*movie, e.containers, false, 0, options.masterGain);
                ++result.voiceStarts;
            } else if (e.kind == MovieAudioEvent::Kind::BedStart) {
                result.observedPlayedSamples += bed.played();
                result.submittedPcmSamples +=
                    bed.start(*movie, e.containers, true, e.loopFrom, options.masterGain);
                ++result.bedStarts;
            } else {
                result.observedPlayedSamples += voice.played() + bed.played();
                voice.stop();
                bed.stop();
            }
        }
    }
    void load(const std::filesystem::path &p, size_t segment = 0, size_t frame = 0) {
        if (options.testCallIndex >= 0)
            std::cerr << "movie test load " << p.filename().string() << " frame=" << frame << '\n';
        path = p;
        player.reset();
        movie = std::make_unique<Movie>(Movie::open(path));
        player = std::make_unique<MoviePlayer>(*movie);
        player->start(now(), segment, frame);
        paintedFrame.reset();
        automaticClick.reset();
        ++result.movies;
        ++result.frames;
        audioEvents();
    }
    void stop(bool skip, bool cancel) {
        result.skipped = skip;
        result.cancelled = cancel;
        if (player) {
            player->skip();
            audioEvents();
        }
        running = false;
    }
    void tick(HWND window) {
        auto before = std::pair(player->segmentIndex(), player->frameIndex());
        player->advance(now(), voice.playing());
        audioEvents();
        if (before != std::pair(player->segmentIndex(), player->frameIndex()))
            ++result.frames;
        const auto current = std::pair(player->segmentIndex(), player->frameIndex());
        if (options.testCallIndex >= 0 && result.movies >= 3 && paintedFrame == current) {
            SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0);
            return;
        }
        if ((options.testFrameLimit || options.testLiveHotspots) &&
            (player->waitsForClick() ||
             (options.testLiveHotspots && !player->hotspots().empty())) &&
            paintedFrame == current && automaticClick != current) {
            automaticClick = current;
            if (player->hotspots().empty()) {
                // Click-to-dismiss stills need no authored region. Exercise
                // the same mouse path as normal playback, without front() on
                // an empty region table in automatic native tests.
                SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, 0);
                return;
            }
            const MovieHotspot *selected = &player->hotspots().front();
            if (options.testPreferExitHotspot)
                selected = player->exitHotspot();
            if (options.testCallIndex >= 0 && result.movies == 1) {
                int index = 0;
                for (const auto &candidate : player->hotspots())
                    if (candidate.action == 4 && index++ == options.testCallIndex) {
                        selected = &candidate;
                        break;
                    }
            }
            const auto &hotspot = *selected;
            if (options.testCallIndex >= 0)
                std::cerr << "movie test click action=" << hotspot.action
                          << " frame=" << player->frameIndex() << '\n';
            RECT client{};
            GetClientRect(window, &client);
            const auto v = scaledViewport(client.right, client.bottom, player->width(),
                                          player->height(), options.integerScaling);
            const int dw = v.width, dh = v.height;
            const int left = std::clamp(hotspot.rect.left, 0, player->width());
            const int right = std::clamp(hotspot.rect.right, 0, player->width());
            const int top = std::clamp(hotspot.rect.top, 0, player->height());
            const int bottom = std::clamp(hotspot.rect.bottom, 0, player->height());
            if (left >= right || top >= bottom)
                throw std::runtime_error("test movie hotspot is entirely off screen");
            const auto selectedAt = [&](int x, int y) {
                for (const auto &candidate : player->hotspots()) {
                    const auto &r = candidate.rect;
                    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
                        return &candidate == selected;
                }
                return false;
            };
            std::optional<POINT> point;
            for (int y = top; y < bottom && !point; ++y)
                for (int x = left; x < right; ++x) {
                    const int cx = v.x + (2 * x + 1) * dw / (2 * player->width());
                    const int cy = v.y + (2 * y + 1) * dh / (2 * player->height());
                    if (selectedAt((cx - v.x) * player->width() / dw,
                                   (cy - v.y) * player->height() / dh)) {
                        point = POINT{cx, cy};
                        break;
                    }
                }
            if (!point)
                throw std::runtime_error("test movie hotspot is obscured by earlier regions");
            PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point->x, point->y));
        }
        if (options.testFrameLimit && result.frames >= options.testFrameLimit) {
            // Exercise the same window input route as a player's Space key.
            SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0);
            return;
        }
        if (player->finished()) {
            const auto destination =
                options.followChains ? navigation.next(path, *player) : std::nullopt;
            if (destination) {
                load(destination->path, destination->segment, destination->frame);
            } else {
                result.completed = true;
                running = false;
            }
        }
        InvalidateRect(window, nullptr, FALSE);
    }
};
LRESULT CALLBACK procedure(HWND window, UINT msg, WPARAM w, LPARAM l) {
    auto state = static_cast<WindowState *>(GetPropW(window, movieStateProperty));
    if (msg == WM_NCCREATE) {
        state = static_cast<WindowState *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        if (!SetPropW(window, movieStateProperty, state))
            return FALSE;
    }
    if (!state)
        return DefWindowProcW(window, msg, w, l);
    try {
        switch (msg) {
        case WM_TIMER:
            if (w == movieTimer) {
                state->tick(window);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (w == VK_F11 && state->previousProcedure)
                return CallWindowProcW(state->previousProcedure, window, msg, w, l);
            if (w == VK_ESCAPE || w == VK_SPACE) {
                state->stop(true, false);
                return 0;
            }
            return 0; // Do not send gameplay keys while its movie is active.
        case WM_KEYUP:
        case WM_CHAR:
        case WM_LBUTTONUP:
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            if (state->player->hotspots().empty())
                state->stop(true, false);
            else {
                RECT client{};
                GetClientRect(window, &client);
                const int width = state->player->width(), height = state->player->height();
                const auto v = scaledViewport(client.right, client.bottom, width, height,
                                              state->options.integerScaling);
                const int dw = v.width, dh = v.height;
                const int x = GET_X_LPARAM(l) - v.x, y = GET_Y_LPARAM(l) - v.y;
                if (dw > 0 && dh > 0 && x >= 0 && x < dw && y >= 0 && y < dh &&
                    state->player->click(x * width / dw, y * height / dh, state->now())) {
                    ++state->result.hotspotClicks;
                    state->audioEvents();
                }
            }
            return 0;
        case WM_CLOSE:
            state->stop(false, true);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PaintSession paint(window);
            RECT client{};
            GetClientRect(window, &client);
            if (client.right <= 0 || client.bottom <= 0)
                return 0;
            auto dc = state->canvas.prepare(paint.dc, client.right, client.bottom);
            // Compose letterboxing and the complete frame off screen. Clearing
            // the visible DC first exposes a black flash between every frame.
            FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            if (state->player) {
                int width = state->player->width(), height = state->player->height();
                const auto v = scaledViewport(client.right, client.bottom, width, height,
                                              state->options.integerScaling);
                const int dw = v.width, dh = v.height;
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = width;
                info.bmiHeader.biHeight = -height;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(dc, COLORONCOLOR);
                StretchDIBits(dc, v.x, v.y, dw, dh, 0, 0, width, height,
                              state->player->rgb().data(), &info, DIB_RGB_COLORS, SRCCOPY);
            }
            BitBlt(paint.dc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
            ++state->result.paints;
            if (state->player)
                state->paintedFrame =
                    std::pair(state->player->segmentIndex(), state->player->frameIndex());
            if (state->player && state->player->waitsForClick()) {
                ++state->result.waitingPaints;
                if (!state->options.testWaitingCapture.empty()) {
                    ppm(state->options.testWaitingCapture, state->player->rgb(),
                        state->player->width(), state->player->height());
                    state->options.testWaitingCapture.clear();
                }
            }
            return 0;
        }
        }
    } catch (...) {
        state->failure = std::current_exception();
        state->running = false;
        return 0;
    }
    return state->previousProcedure ? CallWindowProcW(state->previousProcedure, window, msg, w, l)
                                    : DefWindowProcW(window, msg, w, l);
}
} // namespace
MovieWindowResult playMovieWindow(const std::filesystem::path &path, MovieWindowOptions options) {
    WindowState state;
    state.options = options;
    state.load(path);
    HWND window = options.owner;
    if (window) {
        if (!IsWindow(window) || GetPropW(window, movieStateProperty))
            throw std::runtime_error("invalid or busy game movie window");
        if (!SetPropW(window, movieStateProperty, &state))
            throw std::runtime_error("cannot attach movie to game window");
        SetLastError(0);
        auto previous =
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(procedure));
        if (!previous) {
            RemovePropW(window, movieStateProperty);
            throw std::runtime_error("cannot install game movie renderer");
        }
        state.previousProcedure = reinterpret_cast<WNDPROC>(previous);
        state.result.embedded = true;
    } else {
        WNDCLASSW wc{};
        wc.lpfnWndProc = procedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DustNativeMovie";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("cannot register movie window");
        window =
            CreateWindowExW(0, wc.lpszClassName, L"Dust — original movie (click or Space to skip)",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1040, 580,
                            options.owner, nullptr, wc.hInstance, &state);
        if (!window)
            throw std::runtime_error("cannot create movie window");
        ShowWindow(window, SW_SHOW);
    }
    auto releaseWindow = [&] {
        if (IsWindow(window)) {
            KillTimer(window, movieTimer);
            if (state.previousProcedure)
                SetWindowLongPtrW(window, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(state.previousProcedure));
            RemovePropW(window, movieStateProperty);
            if (options.owner)
                InvalidateRect(window, nullptr, FALSE);
            else
                DestroyWindow(window);
        }
    };
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);
    if (!SetTimer(window, movieTimer, 10, nullptr)) {
        releaseWindow();
        throw std::runtime_error("cannot create movie timer");
    }
    MSG msg{};
    bool quit = false;
    WPARAM exitCode = 0;
    while (state.running) {
        auto got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            quit = got == 0;
            exitCode = msg.wParam;
            state.stop(false, true);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    releaseWindow();
    state.result.observedPlayedSamples += state.voice.played() + state.bed.played();
    state.voice.stop();
    state.bed.stop();
    if (quit)
        PostQuitMessage(int(exitCode));
    if (state.failure)
        std::rethrow_exception(state.failure);
    return state.result;
}
} // namespace dust
