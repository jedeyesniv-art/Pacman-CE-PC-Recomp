# Pac-Man Championship Edition: native PC port

A native Windows build of **Pac-Man Championship Edition** (Xbox 360 Live Arcade, 2007), made by statically
recompiling the game's PowerPC code to x86-64 with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
(built on Xenia's kernel, GPU and audio emulation). It is a hobby project, not affiliated with or endorsed by
Bandai Namco or Microsoft.

**No game files are included.** You need your own copy of the game, dumped from your own Xbox 360.

## Features

- **Random mazes**: a bonus mode for Championship Mode. Press R3 on the title screen; the logo turns red and
  each game plays the mazes in a new random order (high scores aren't recorded for those games).
- Runs light: about 7% CPU in gameplay on a desktop PC; 60 fps on a Steam Deck under Proton. Significant
  amounts of work spent tuning performance for Steam Deck, expect some minor dips but very playable.
- Renders at your screen's resolution (3x on 4K) with the picture never stretched; Vulkan or Direct3D 12.
- Plays from the game's package file as dumped from the console (STFS), or from unpacked files.
- Xbox controller and keyboard; fullscreen toggle (F11 / Alt+Enter).
- The playfield moved up slightly, so the bottom of the maze's frame is no longer cut off (optional).
- Your saves are backed up automatically; achievements are recorded locally (F7 or the Achievements menu).
- One build for Windows 10/11 and the Steam Deck.

## Playing

Download the release zip, unpack it anywhere, and put your copy of the game in its `game\` folder: either the
package file from your console (a file with a long hexadecimal name) or the unpacked files (`default.xex`,
`media\`, `data\`). Run `pacman.exe`. The included `README.txt` has the controls and options.

## Building from source

Requirements: Windows, Visual Studio 2026 with the C++ workload and its Clang/LLVM tools, CMake, and your own
copy of the game's `default.xex` (the recompiler reads the game code from it).

1. Build the ReXGlue SDK at the pinned commit with this project's patches and the Vulkan backend:
   see [patches/rexglue-sdk/README.md](patches/rexglue-sdk/README.md).
2. Put your game files in `private\` (git-ignored): at least `default.xex`, plus `media\` and `data\` to run
   from the build folder. If you only have the package file, extract `default.xex` from it with an STFS tool.
3. In a Visual Studio x64 developer prompt (`vcvars64.bat`) with Visual Studio's LLVM `bin` folder on `PATH`,
   from `rexglue\`:

   ```
   cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH=<SDK install dir>
   cmake --build --preset win-amd64-release
   ```

   The recompiler runs as part of the build and generates the game code into `rexglue\generated\default\`
   (git-ignored: it is derived from the game).
4. `rexglue\out\build\win-amd64-release\pacman.exe` runs with the game files in `private\`.
   `Tools\package_portable.ps1` builds the portable folder in `dist\` (`-Release` also writes the release zips).

## Repository layout

- `rexglue\`: the project: recompiler configuration (`pacman_config.toml`, `extra_functions.toml`) and the host
  code in `src\` (startup and defaults, the hooks that fix busy-waiting and frame pacing, random mazes, ...).
- `patches\rexglue-sdk\`: the SDK patches this build needs.
- `CLAUDE.md`: detailed technical notes: what was found in the game's code and why each change was made.
- `config\`, `host\`, root `CMakeLists.txt`: an earlier attempt with XenonRecomp, kept for reference.

## Licence

This project's own code is released under [MIT No Attribution](LICENSE). The release binaries include
third-party software under its own licences; see `licenses\THIRD-PARTY-NOTICES.txt` in the release (from
`rexglue\licenses\`). The source code of the LGPL parts (FFmpeg, libmspack) at the exact revisions used is
attached to each release.

Pac-Man Championship Edition is © Bandai Namco Entertainment Inc.
