# recomp-ui

A shared, **console-agnostic launcher and in-game settings UI** for static-
recompilation game ports. One Dear ImGui launcher core serves every recomp
ecosystem — SNES, PSX, N64, Genesis, NES, and beyond — while a small runtime
overlay API lets the same hosts expose live settings after boot.

It is the reusable extraction of the SNES-recomp "launcher_ng" launcher,
generalized behind a small C ABI. Consume it as a git submodule, hand it your
game's facts through a plain-C struct, and it renders the right UI for that
console.

Proven in production driving two very different consoles from the **same
binary core**:

- **Mega Man X** (Super Nintendo) — CRT theme, SNES pad, ROM verification.
- **Ape Escape** (PlayStation) — blue theme, DualShock + pad-modes, real disc
  verification, memory cards, deep PSX video settings.

---

## Architecture: composition + inheritance

recomp-ui is **one** launcher composed differently per console — never a
per-console fork, never a mega-`if`. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

- **Composition** decides *which* cards appear. A `SystemProfile` lists the
  panels each view (dashboard / settings / controller) gets. "PSX has memory
  cards, SNES has SRAM, this game has neither" = include/omit the panel.
- **Inheritance** decides *how* a card behaves. Each module — Controller, Save,
  Video, Verify, Hotkeys — has one base implementation specialized per console
  by a small data **spec**. "Every game has a controller; its art/buttons/count
  differ" = one base card, per-system data.

Adding a console is (ideally) **one `SystemProfile` row** in
[`src/launcher_system.h`](src/launcher_system.h). The console difference is pure
data passed through the C ABI — theme, platform label, box art, pad image,
which panels — so there is no `../snes` / `../psx` source fork.

The DRY heart is [`src/launcher_model.h`](src/launcher_model.h): all launcher
STATE + BEHAVIOR, free of any UI toolkit, built purely from the C ABI structs —
so behavior is identical across every game.

---

## Consuming it

### 1. Vendor the repo (git submodule)

```sh
git submodule add https://github.com/mstan/recomp-ui.git recomp-ui
```

### 2. Wire it into your CMake target

```cmake
include(${CMAKE_SOURCE_DIR}/recomp-ui/recomp_ui.cmake)
recomp_target_launcher_ui(my-game-runtime
    BOXART ${CMAKE_SOURCE_DIR}/art/boxart.tga   # optional: game box art
    PAD    ${CMAKE_SOURCE_DIR}/art/pad.tga       # optional: per-console controller image
    BRAND  ${CMAKE_SOURCE_DIR}/art/brand.tga)    # optional: per-console top-left mark
```

`recomp_ui.cmake` is fully self-contained: it bundles Dear ImGui, crc32/sha256/
keybinds, tinyfiledialogs, an IPS applier, and a PS1 memcard formatter, and
stages its fonts/images next to your exe. No network / FetchContent — builds
offline. It defines `RECOMP_LAUNCHER` on the target and needs SDL2 + OpenGL.

### 3. Call it from your `main()`

```c
#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#include "launcher_profile.h"

RecompLauncherCSettings io = { /* seed from your config */ };
RecompLauncherCGameInfo gi = {0};
launcher_profile_apply("snes", &gi);   // "psx", "n64", ... — one call sets the console identity
gi.name = "My Game";
gi.expected_crc = 0x1B4B2E9C; gi.has_expected_crc = 1;
/* ...per-game overrides... */

char out_rom[512];
int rc = recomp_launcher_run_window("My Game — Launcher", &io, &gi,
                                    ".", initial_rom, out_rom, sizeof(out_rom));
// rc: 0 = LAUNCH (boot out_rom with the edited io), 1 = QUIT, 2 = UNAVAILABLE
#endif
```

The whole contract is [`src/recomp_launcher.h`](src/recomp_launcher.h): a plain-C
settings struct in/out, a game-facts struct, and (optionally) **host callbacks**
for console-specific verification the launcher re-runs on change — e.g. PSX disc
identification (`disc_verify`) and memory-card inspection (`memcard_inspect`).

### In-game settings overlay

[`src/recomp_runtime_ui.h`](src/recomp_runtime_ui.h) is the separate runtime
contract. A game supplies sectioned item descriptors plus callbacks to read,
apply, persist, and enable its own settings. `recomp-ui` owns hierarchical
navigation, selection state, help/status text, and drawing.

The runtime model is renderer-independent. For a modern/GPU host, call
`recomp_runtime_ui_render_imgui()` inside the host's active Dear ImGui frame;
it uses the same responsive layout and console theme tokens as the launcher,
without exposing ImGui types through the C ABI. This is the preferred path for
RT64/N64 and other high-resolution renderers.

`recomp_runtime_ui_render_argb8888()` is a compatibility presentation for
games that expose a writable CPU framebuffer. It is intentionally compact and
works well for low-resolution framebuffer consoles, but it is not the visual
baseline for every platform. Both presentations drive the exact same menu
state, descriptors, callbacks, and navigation.

The host continues to own the game loop: while
`recomp_runtime_ui_is_open()` is true it should consume menu input before
emulated input and decide whether to pause simulation/audio. See
[`docs/RUNTIME_UI.md`](docs/RUNTIME_UI.md) for the integration boundary and
rollout matrix.

```c
static const RecompRuntimeUiItem rows[] = {
    { "fullscreen", "Display", "Fullscreen", "Choose the window mode.",
      RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, modes, 3, NULL },
    { "reset", "System", "Reset game", "Reset the emulated machine.",
      RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL },
};

RecompRuntimeUiConfig menu = {0};
menu.title = "My Game";
menu.subtitle = "SETTINGS";
menu.items = rows;
menu.item_count = sizeof(rows) / sizeof(rows[0]);
menu.callbacks = callbacks;
menu.theme = "n64"; /* same id used by launcher_profile_apply() */
RecompRuntimeUi *ui = recomp_runtime_ui_create(&menu);
```

New integrations should zero-initialize `RecompRuntimeUiConfig`, then assign
its fields. Set `theme` to the same system id passed to
`launcher_profile_apply()` (`"snes"`, `"n64"`, `"gba"`, `"genesis"`, ...),
so pre-boot and in-game UI cannot drift visually.

The API deliberately does not prescribe settings. Backend swaps that require a
restart can be omitted or exposed as actions; safe live values can be applied
immediately in `set_value`. This keeps game-specific state and policy in the
game while making the menu implementation reusable.

### Optional netplay surface

Netplay is enabled by the game developer, not by an end-user setting. Set
`RecompLauncherCGameInfo.netplay_supported` and provide a
`RecompLauncherCNetplayCallbacks` table to expose the Netplay button. The host
owns player-name and lobby-server persistence, LAN/remote lobby discovery,
password checks, member ordering, and synchronized launch signaling. The UI
only presents that state and returns a `RecompLauncherCNetplayLaunch` result
after the host starts the lobby.

Hosts with more than one local network interface may implement the optional
`local_address_get` callback to enumerate labeled address choices. The launcher
prefills the first address, preserves the user's selection across refreshes,
and falls back to the legacy single-address `local_ip` callback when the new
callback is absent or returns no choices.

The initial waiting-room UI supports two players. Hosts always bind UDP on
`0.0.0.0`. **LAN/Direct IP** (unchecked by default) enables the interface
dropdown and port so hosts can advertise a chosen LAN address to joiners; when
unchecked those controls stay visible but disabled (selection preserved) —
create ignores the greyed port field, prefers UDP `7777`, and auto-picks a
nearby free port (`7777`..`7808`) for lobby-server rewrite / ICE. Before
`join()`, the UI fills `guest_bind` the same way (prefer `7778`..`7809`) so
online guests advertise a real UDP port — hosts must pass that buffer through
to the lobby client rather than rewriting to `:0`. Direct / LAN join still
receives a prepared bind; games may ignore it for file-registry LAN rooms.

---

## Feature surface (per console, capability-gated)

- **Game / verification** — box art, ROM CRC/SHA verification, or PSX disc
  verdict (serial / region / ISO header) via the host `disc_verify` callback.
- **Controllers** — none/keyboard/gamepad per player, per-console pad art +
  button vocabulary, PSX analog/digital/hybrid pad modes or a custom mode list
  (Genesis 3-Button / 6-Button, which also sets the visible rebind-row count),
  deadzone, keyboard rebinding that writes each runtime's native keybind format,
  and an optional per-row **GAMEPAD** bind column (Genesis) alongside the key.
- **Video** — window scale/size, renderer, supersampling, aspect (4:3/16:9/21:9),
  widescreen 16:9 with an optional "extra cells / side" stepper (Genesis),
  texture filtering, antialiasing (Off/2×/4×/8×), screen model, frame
  interpolation, and more — each shown only when the console exposes it.
- **Save** — SRAM Import/Clear (with `.bak` backup) or PS1 memory cards (per-slot
  enable, Browse / New-formats-a-blank-card, real block-usage grid via the host
  `memcard_inspect` callback).
- **MSU-1** — dashboard IPS auto-patching for SNES MSU-1 titles.
- **Hotkeys** — per-console subset of the emulator hotkey catalog, editable.
- **Footer** — PLAY, skip-launcher-on-boot (+ confirm modal), gamepad navigation.

---

## Build & self-test

recomp-ui builds a standalone harness that fabricates the same C ABI a real host
passes, so you can iterate on the UI without a game:

```sh
cmake -G Ninja -S . -B build
cmake --build build -j
# Preview a console: LNG_VARIANT = psx | snes | gba | genesis | ...
LNG_VARIANT=genesis ./build/recomp-ui-launcher
```

`LNG_SCRIPT` drives it headless for screenshot regression, e.g.
`LNG_SCRIPT="wait:40;view:settings;shot:out.png;quit"` (see
[`src/launcher_debug.h`](src/launcher_debug.h)).

Requires SDL2 (`find_package(SDL2)`), OpenGL, a C++17 compiler.

---

## Layout

```
src/common/             launcher core (.c/.h), Dear ImGui backend, C ABI header
src/common/launcher_model.*   UI-toolkit-free view-model (state + behavior)
src/consoles/<id>/      per-console unit: <id>_profile.h (SystemProfile row) +
                        any native bridge (e.g. genesis_binds.c, psx_binds.c)
src/launcher_system.h   aggregates the console units + by-id / infer lookups
src/third_party/        vendored Dear ImGui, stb, tinyfiledialogs
assets/common/          fonts + chrome art shared by every console
assets/consoles/<id>/   per-console art (pad_genesis.tga, pad_gba.tga, ...)
recomp_ui.cmake         reusable recomp_target_launcher_ui() integration helper
docs/ARCHITECTURE.md    composition + inheritance design
CMakeLists.txt          standalone self-test build (recomp-ui-launcher)
```

Adding a console = one directory under `src/consoles/<id>/` (profile row +
optional native bind bridge) registered in `src/launcher_system.h`, a theme in
`src/common/launcher_theme.h`, per-console art under `assets/consoles/<id>/`,
and its `.c` files + assets wired into `recomp_ui.cmake`.

## License

Bundled third-party code (Dear ImGui, stb, tinyfiledialogs) retains its own
licenses under `src/third_party/`.
