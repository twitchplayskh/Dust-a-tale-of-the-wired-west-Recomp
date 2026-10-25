// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <windows.h>
namespace dust {
struct MovieWindowOptions {
    // Use this existing game window as the playback surface. Null creates a
    // standalone window for the movie laboratory only.
    HWND owner = nullptr;
    bool followChains = true;
    // Deterministic native integration test: stop after this many frame entries.
    // Zero plays the complete authored path. Regular gameplay leaves this zero.
    size_t testFrameLimit = 0;
    std::filesystem::path testWaitingCapture;
    bool integerScaling = false;
    uint8_t masterGain = 255;
    bool testLiveHotspots = false;
    bool testPreferExitHotspot = false;
    int testCallIndex = -1;
};
struct MovieWindowResult {
    uint32_t actionFrames = 0;
    bool embedded = false;
    bool completed = false, skipped = false, cancelled = false;
    size_t movies = 0, frames = 0, paints = 0, voiceStarts = 0, bedStarts = 0;
    size_t hotspotClicks = 0, waitingPaints = 0;
    uint64_t submittedPcmSamples = 0, observedPlayedSamples = 0;
};
// Modal playback on the calling UI thread, with a normal Windows message pump.
// Escape/Space skip. A click follows authored hotspots when present, otherwise
// skips; closing the window cancels. Device/codec
// failures throw after both audio channels and the temporary renderer have been
// released. The supplied game window and its original procedure are preserved.
MovieWindowResult playMovieWindow(const std::filesystem::path &, MovieWindowOptions = {});
} // namespace dust
