# Dust: A Tale of the Wired West — native reconstruction

**Requires the original Dust: A Tale of the Wired West game files to play.**
This repository and its downloadable builds contain only the reconstructed
engine. They do not include the original game files and cannot be played
without them. Use your own original installation, then select its folder
containing `DATA` and `MOVIES` in the launcher using **Browse**.

A Windows x64 C++20 reconstruction that reads the original game's data files.
The native launcher provides windowed and borderless display modes, 4K aspect
fit or integer scaling, audio preferences, captions and optional character
restoration.

This is a preview release. The campaign reaches its ending in scripted
verification, with additional native-window checks. Some optional story paths,
presentation details and hardware configurations still need verification.

## Build

Install Visual Studio 2022 or newer with **Desktop development with C++**, a
Windows SDK, and CMake 3.25 or newer. No downloaded libraries or game files are
needed to compile. From this folder:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The two application files are `build/Release/dust_launcher.exe` and
`build/Release/dust_game.exe`. Keep them together. The MSVC runtime is statically
linked by default; there are no separately supplied runtime DLLs.

To collect the applications and licence notices into a portable folder:

```powershell
cmake --install build --config Release --prefix portable
```

Only the launcher and game are built by default. Private regression suites,
developer probes, test assets and compiled binaries are excluded from this
source distribution. `BUILD_TESTING` and `DUST_BUILD_TOOLS` are development
options for the separate research workspace; leave both off here.

Each push also runs the Windows build workflow. Its Actions artifact contains
the portable applications, notices and matching source archive.

## Play

Use your own original Dust installation/data. Run `dust_launcher.exe`, select
the folder containing `DATA` and `MOVIES` using **Browse**, then press **Play**.
The engine does not include or download the original game, artwork or audio,
and does not run its original executables. Windows 10/11 x64 is the target;
the full range of hardware configurations has not been tested.

On a 4K display, **Fit** fills the height while preserving 4:3 proportions and
side borders. **Integer** uses a 5x image with top and bottom borders. Character
restoration is optional; temporal cleanup remains experimental.

Arrow keys move/turn. Click actors, objects and dialogue replies. `I` opens
inventory, `M` opens the map and Escape opens/closes menus. `F11` or `Alt+Enter`
toggles fullscreen. Original options allow movement-key rebinding. Save/Open
uses native `.dustsave` files; original game saves are not compatible.

Settings and logs are stored in `%LOCALAPPDATA%\DustRecomp`; the default save
folder is `%LOCALAPPDATA%\DustRecomp\saves`. Keep the game data intact and write
saves outside its installation folder.

## Licence and attribution

Engine source is GPL-3.0-only. See [LICENSE](LICENSE) and
[THIRD_PARTY.md](THIRD_PARTY.md) for the pinned dreamREfactory reference and
other provenance. The source licence does not cover the original game's data.
This project is a reconstruction and language port, not a claim of clean-room
independence. See [CHANGELOG.md](CHANGELOG.md) for changes.
