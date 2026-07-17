// consoles/psx/psx_binds.h — the PSX-native keybind persistence bridge.
//
// psxrecomp's runtime (runtime/launcher/psx_keybinds.h + .c, upstream repo)
// owns its OWN keyboard-bind format: an INI with [player1]/[player2]
// sections and 24 named keys (up/down/left/right/cross/circle/square/
// triangle/l1/r1/l2/r2/l3/r3/start/select/ls_up/ls_down/ls_left/ls_right/
// rs_up/rs_down/rs_left/rs_right), storing SDL *scancode* names exactly like
// keybinds.c does. It is NOT the same format as keybinds.c's generic
// PlayerBinds (a/b/x/y/l/r/start/select/up/down/left/right/l2/r2/l3/r3) —
// routing PSX's 24 buttons through that 16-slot generic format silently
// discarded the 8 stick-direction binds and wrote a file the PSX runtime's
// INI parser can't parse, so PSX rebinds never reached the game. This bridge
// fixes that at the root: for a PSX SystemProfile, launcher_binds.c routes
// persistence through this native 24-scancode store, read/written in
// psx_keybinds.c's own key vocabulary, to the SAME default filename
// ("keybinds.ini") psx_keybinds_init() reads — so whichever process
// (launcher or game) runs first creates a file the other already understands.
//
// Button indices are the kPsxPadButtons rebind-spec order (psx_profile.h),
// players are 0-based (0 = P1). The `path` every call takes is the resolved
// keybinds.ini path (launcher_binds.c owns path resolution).

#ifndef RUI_CONSOLE_PSX_BINDS_H
#define RUI_CONSOLE_PSX_BINDS_H

#ifdef __cplusplus
extern "C" {
#endif

// Load existing keybinds.ini if present (whichever process wrote it last —
// launcher or game, same format), else seed defaults and write it so the
// game's first run sees identical bindings to what the launcher displays.
// A file matching ZERO native-only keys is foreign-format (e.g. a stale
// generic keybinds.ini written pre-bridge) — defaults are re-seeded and the
// file rewritten cleanly rather than half-applying a foreign blend.
void rui_psx_binds_init(const char* path);

// Current binding (SDL_Scancode as int) for player (0..1), rebind-spec
// button b (0..LNG_PSX_PAD_BUTTON_COUNT-1). Auto-initializes from `path`
// on first use.
int rui_psx_binds_get(const char* path, int player, int b);

// Rebind + persist.
void rui_psx_binds_set(const char* path, int player, int b, int scancode);

// Reset one player to psxrecomp's PSXKB_DEFAULTS + persist.
void rui_psx_binds_reset(const char* path, int player);

#ifdef __cplusplus
}
#endif

#endif // RUI_CONSOLE_PSX_BINDS_H
