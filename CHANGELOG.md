# Changes

## 2026-10-25 — update

Verify all ten native history chapter links, their page images and returns to contents; preserve original masked tab input. Debug checks only.

## 2026-10-25 — update

Verify all 71 history and Yunni book pages through native input, reverse pixel checks, endpoint limits and bookmark jump/fold/unfold; retain torn-page checks. Debug verification only, with no release package.

## 2026-10-25 — update

Fix movie wipes revealing black instead of the current room; preserve fade levels and chained-film backdrops. Verified Debug transitions, native movie navigation, Help and piano paths.

## 2026-10-25 — update

Verify alternate ring recovery and use development-only source checks

- Verify earned Oona-to-Isao ring ownership, piano cancellation/retry/solution and return to Jones.
- Add a private native piano regression target; test files remain outside the public checkout.
- Change per-push Windows CI to Debug verification without release artifacts.
- Reserve release builds and packages for substantial milestones; retain the original-game-files requirement.

## 2026-10-25 — update

State original game files requirement prominently at the top of the README

## 2026-10-25 — update

Prepare source-only Windows preview and portable build

- Publish production C++ source and CMake instructions; exclude tests, game assets and binaries.
- Fix clipped launcher scaling and 4K text by increasing the preview area.
- Clear the developer-specific default game path and keep the game console hidden when launched.
- Link the MSVC runtime statically and add portable install and Windows CI builds.
- Include GPL licensing, provenance and preview limitations.

## 0.1.0-preview — 2026-10-25

- Prepare a source-only CMake build of the launcher and game, with a statically
  linked MSVC runtime and portable install target.
- Increase the launcher's scaling-description area so the 4K line is visible.
- Start with an empty game-folder preference on a fresh installation.
- Preserve independent music and NPC voice levels through software mixing.
- Remove stale scene callbacks when leaving a location.
- Include the campaign, dialogue, inventory, combat and minigame reconstruction
  developed so far. Further optional branches and hardware testing remain.

Future completed fixes are recorded in Git commit messages and dated entries
here when the production source is synchronized from the development workspace.
