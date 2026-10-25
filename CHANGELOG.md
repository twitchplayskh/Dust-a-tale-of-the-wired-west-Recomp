# Changes

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
