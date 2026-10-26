# Changes

## 2026-10-26 — update

Document verified temple puzzle exits, item retention, failed-answer retries and partial flute/tumbler save restoration; 17 earned model cases and six native input checks pass. Source-only update, no release artifacts.

## 2026-10-26 — update

Fix the mine skeleton snapping back after map wraps: actor position setters now cancel pending movement, matching the original setters. Verified 60 alternate campaign routes, native mask drag and duel/finale, 18 Debug tests and a clean source build. No release artifacts.

## 2026-10-26 — update

Verify Mayor/Sonoma progression with ten Debug routes and six native cases; verify Doc advice, cancellation and equipment clock gating with fourteen model and five native cases. Extend private native assertions to actor visibility. Clean Debug source build passes; no release artifacts.

## 2026-10-26 — update

Add read-only authored camera previews without changing game state. Verify Sophie ring recovery through 41 earned checkpoints and five native payment menus, plus nested dialogue cancellation and timeout. Debug tests and clean public-source build pass; no release artifacts.

## 2026-10-25 — update

Verify Ruby gun theft and its 28-checkpoint alternate ring chain; cover all Side pie cancellations and native same-conversation retry. Reset the private native picker between inventory openings. Debug only; no release artifacts.

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
