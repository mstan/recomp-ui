# recomp-ui

A self-contained, console-agnostic Dear ImGui pre-boot launcher, extracted
from the SNES-recomp "launcher_ng" launcher. Any recomp ecosystem (SNES, NES,
N64, PSX, ...) can consume this repo as a git submodule to get the exact same
launcher UI/behavior with zero UI code of its own.

## What it is

- A view-model (`src/launcher_model.*`) that owns all launcher state and
  behavior (panels, controls, rebind capture), free of any UI toolkit.
- A Dear ImGui render backend (`src/backends/imgui/launcher_imgui.cpp`) that
  draws that model: Dashboard (ROM info + CRC/SHA badges + Change ROM),
  Settings (window scale, filter, sample rate, volume, hotkeys), Controller
  (input source, deadzone, keyboard rebinds), and Footer (Skip-on-Boot, PLAY).
- A plain C ABI (`src/recomp_launcher.h`) so a host's C `main()` can call into
  the C++ launcher without speaking C++:

  ```c
  int recomp_launcher_run_window(const char* window_title,
                                  RecompLauncherCSettings* io,
                                  const RecompLauncherCGameInfo* game,
                                  const char* assets_dir,
                                  const char* initial_rom,
                                  char* out_rom_path, size_t out_rom_path_len);
  ```

- Bundled, self-contained helpers: `crc32.c/.h`, `sha256.c/.h` (ROM
  verification) and `keybinds.c/.h` (SDL-scancode keyboard-binding store,
  `keybinds.ini`, backing the rebind UI). The host does not need to already
  provide these.
- Vendored Dear ImGui (MIT) + `tinyfiledialogs` under `src/third_party/` —
  no network fetch, fully offline builds.

## Consuming it

Add this repo as a git submodule, then from the host's CMake project:

```cmake
set(RECOMP_UI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/recomp-ui)
include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
recomp_target_launcher_ui(<your_target> [BOXART <path/to/boxart.tga>])
```

That call:
- adds all launcher sources (core + Dear ImGui backend + vendored ImGui +
  tinyfiledialogs + bundled crc32/sha256/keybinds) to `<your_target>`,
- sets the include directories,
- defines `RECOMP_LAUNCHER` (un-gate your own launcher call site with
  `#ifdef RECOMP_LAUNCHER`) and `SDL_MAIN_HANDLED`,
- stages `assets/fonts` + `assets/img` (and an optional per-game
  `assets/img/boxart.tga`) next to the built exe post-build, matching the
  runtime's `SDL_GetBasePath()` asset lookup.

Your host's C `main()` seeds `RecompLauncherCSettings` /
`RecompLauncherCGameInfo`, calls `recomp_launcher_run_window(...)`, and boots
whichever ROM path it returns. See `src/proto_main.c` for a full worked
example.

Requires: SDL2 (found via `find_package(SDL2)`), OpenGL, a C++17 compiler.

## Building the standalone self-test

This repo's own `CMakeLists.txt` builds `recomp-ui-launcher`, a standalone
exe from `src/proto_main.c` that seeds a neutral placeholder game and runs
the launcher UI, so the repo can verify itself without any host game:

```sh
cmake -G Ninja -S . -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build
```

It also understands `LNG_SCRIPT` (a `;`-separated scripted-input / screenshot
harness — see `src/launcher_debug.h`) for headless CI verification, e.g.:

```sh
LNG_SCRIPT="wait:40;shot:out.png;quit" ./build/recomp-ui-launcher.exe
```

## Layout

```
src/            launcher core (.c/.h), Dear ImGui backend, third_party/, C ABI header
assets/         fonts/ + img/ shipped with the launcher
recomp_ui.cmake reusable recomp_target_launcher_ui() integration helper
CMakeLists.txt  standalone self-test build (recomp-ui-launcher)
```
