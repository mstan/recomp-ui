// launcher_binds.h — real input-binding persistence for the launcher.
//
// Bridges the launcher's Controller view to the engine's persisted files:
//   * player buttons  -> keybinds.ini  (SDL *scancode* names), via keybinds.c
//     -- EXCEPT the PSX SystemProfile, which persists through psxrecomp's own
//        24-button psx_keybinds.c format instead (see the PSX-native bridge
//        below launcher_binds_load's declaration) so PSX rebinds actually
//        reach the game. SNES (and every stub profile) is unaffected.
//   * system hotkeys   -> config.ini [KeyMap] (SDL *keycode* names), surgical edit
//
// This is the module that makes remaps actually STICK. Kept separate from the
// pure view-model so the model stays SDL/engine-free; the backend calls these
// on capture, and the capi/proto driver calls launcher_binds_load() at startup.

#ifndef LAUNCHER_NG_BINDS_H
#define LAUNCHER_NG_BINDS_H

#include "launcher_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// Load current bindings from disk into the model for DISPLAY. Initializes
// keybinds.ini (generating defaults if absent) and reads config.ini [KeyMap].
// config_path may be NULL (=> "config.ini" in the exe dir). keybinds_path may
// be NULL (=> "keybinds.ini" in the exe dir); for a PSX SystemProfile this is
// the psx_keybinds.c-format file, for every other profile it is this
// launcher's own generic keybinds.c-format file (see recomp_launcher.h
// RecompLauncherCGameInfo.keybinds_path).
void launcher_binds_load(LauncherModel* m, const char* config_path, const char* keybinds_path);

// A player button was rebound to `scancode` (an SDL_Scancode). `b` is a
// generic index into the active profile's ControllerSpec.buttons[]
// (0..button_count-1), matching LauncherModel.capture_btn. Persist to
// keybinds.ini and refresh the model's display string.
void launcher_binds_set_button(LauncherModel* m, int player, int b, int scancode);

// N64-native store only: bind an arbitrary FIELD (type/id per
// consoles/n64/n64_binds.h — key, pad button, signed pad axis, raw joystick
// button/axis) into alternate slot 0/1 of the device table the player's
// current input source selects. No-op for every other profile.
void launcher_binds_set_field(LauncherModel* m, int player, int b, int slot,
                              int type, int id);

// Re-read every player's bind display strings from the active store. Cheap;
// the backend calls it when a Configure page's input source changes (the N64
// store is per-device-TYPE, so the shown table follows the source).
void launcher_binds_refresh(LauncherModel* m);

// Whether a capture for `player` (1-based) should listen for GAMEPAD events
// (pad buttons / axis throws / raw joystick fields) instead of the keyboard —
// true only for the N64 store when that player's source is a gamepad.
int launcher_binds_wants_pad_capture(const LauncherModel* m, int player);

// Reset one player's keyboard bindings to defaults and persist.
// (N64: resets the whole device TABLE the player's source selects.)
void launcher_binds_reset_player(LauncherModel* m, int player);

// A system hotkey was rebound. `keycode` is an SDL_Keycode, `kmod` the SDL
// modifier mask; pass keycode==0 to UNBIND. Persists to config.ini [KeyMap]
// and refreshes the model's display string.
void launcher_binds_set_hotkey(LauncherModel* m, LngHotkey h, int keycode, int kmod);

// The config.ini path hotkeys are written to (NULL => default). Set once at load.
extern const char* g_launcher_config_path;
// The keybinds file path player buttons are written to (NULL => default).
// Set once at load; see launcher_binds_load(). Format is chosen from the
// active profile, not from this path.
extern const char* g_launcher_keybinds_path;

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_BINDS_H
