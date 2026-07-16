// recomp_launcher.h — C-callable entry point for the recomp-ui launcher.
//
// A host app's C main() can't speak the C++ Dear ImGui launcher internals
// directly, so this shim wraps it: it creates its own SDL/GL window, runs the
// launcher, maps a plain-C settings struct in/out, and tears the window down —
// leaving the host to just seed/read the struct and pick up the chosen ROM
// path.

#ifndef RECOMP_LAUNCHER_H
#define RECOMP_LAUNCHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Plain-C mirror of the launcher's internal settings (bools as int).
typedef struct RecompLauncherCSettings {
    int  output_method;     // 0 SDL, 1 SDL-software, 2 OpenGL
    int  window_scale;      // 1..N
    int  fullscreen;        // 0 off, 1 borderless, 2 exclusive
    int  ignore_aspect;     // bool
    int  linear_filter;     // bool
    int  widescreen;        // bool (EXPERIMENTAL, default 0)
    int  widescreen_hud;    // bool
    int  enable_audio;      // bool
    int  audio_freq;        // Hz
    int  volume;            // 0..100
    int  player_src[2];     // 0 none, 1 keyboard, 2 gamepad
    int  deadzone[2];       // 0..100
    int  skip_launcher;     // bool: boot straight to the game next time
    int  msu1_enabled;      // bool
    char msu1_dir[512];
    int  pad_mode[2];       // per player: 0=Hybrid, 1=Analog(DualShock), 2=D-Pad(digital)
    int  aspect_index;      // 0 = 4:3, 1 = 16:9, 2 = 21:9
} RecompLauncherCSettings;

typedef struct RecompLauncherCGameInfo {
    const char*    name;
    const char*    region;
    uint32_t       expected_crc;
    int            has_expected_crc;
    const uint8_t (*known_sha256)[32];
    size_t         num_known_sha256;
    int            widescreen_supported;   /* hide Widescreen settings when 0 */
    /* How many players the GAME supports (1 or 2). The launcher hides the
     * Player 2 row entirely when this is 1 — e.g. Mega Man X is 1-player, so a
     * P2 row is dead UI. 0 means "unset" and is treated as 2 for backward
     * compatibility with callers that predate this field. */
    int            num_players;
    int            msu1_supported;
    const char*    msu1_note;          /* shown under MSU-1 settings (which patch) */
    const char*    msu1_patch_path;
    const char*    sram_path;          /* "saves/<title>.srm" (exe-anchored) for SAVES panel */
    const char*    platform;           /* console subtitle under the title, e.g. "PLAYSTATION",
                                          "SUPER NINTENDO". NULL => no subtitle. */
    const char*    theme;              /* built-in theme name: "psx" for the PlayStation look,
                                          NULL/other => default CRT-console theme. */
    /* config.ini path the hotkey editor reads/writes ([KeyMap] section only,
     * surgical edits). NULL => "config.ini" in cwd (exe-anchored by main).
     * Games pass their --config override here so hotkey edits follow it. */
    const char*    config_path;

    // Controller pad-mode (PlayStation-style analog/digital emulation). Consoles
    // without pad modes (SNES) leave pad_mode_supported = 0 and the selector + the
    // analog/digital art are never shown (the generic pad.tga is used).
    int            pad_mode_supported;    // 0 = no pad-mode UI at all; 1 = show the selector + swapping art
    int            pad_mode_selectable;   // 0 = hide selector, force locked_pad_mode (game.lock_mode)
    int            allow_hybrid;          // 0 = hide the Hybrid option
    int            locked_pad_mode;       // forced mode when !pad_mode_selectable
    int            lock_device;           // 1 = hide the player controller cards entirely (fixed pad)
    // Aspect ratios offered. bit0 = 4:3 (implied/always), bit1 = 16:9, bit2 = 21:9.
    // 0 = fall back to the legacy widescreen_supported bool (SNES: 16:9 toggle).
    int            aspect_mask;
} RecompLauncherCGameInfo;

// Returns: 0 = LAUNCH (boot out_rom_path with the edited *io),
//          1 = QUIT (caller should exit),
//          2 = UNAVAILABLE (assets/GL failed — caller boots as if skipped).
int recomp_launcher_run_window(const char* window_title,
                             RecompLauncherCSettings* io,
                             const RecompLauncherCGameInfo* game,
                             const char* assets_dir,
                             const char* initial_rom,
                             char* out_rom_path, size_t out_rom_path_len);

#ifdef __cplusplus
}
#endif

#endif // RECOMP_LAUNCHER_H
