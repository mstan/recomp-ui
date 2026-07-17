// consoles/gb/gb_binds.h — the Game Boy-native bind persistence bridge.
//
// gb-recompiled's runtime owns its OWN bind format: keybinds.ini with a single
// [controls] section (runtime/src/keybinds.c), one line per input using SDL
// scancode NAMES:
//
//     [controls]
//     a = Z
//     b = X
//     select = Right Shift
//     start = Return
//     up = Up
//     down = Down
//     left = Left
//     right = Right
//     turbo = Tab
//
// Unlike the generic PlayerBinds store (keybinds.c) the GBA console persists
// through, gb-recompiled's `[controls]` naming (a/b/select/start/up/down/left/
// right/turbo) is its own — so, like PSX and Genesis, the Game Boy family gets
// a native bridge that reads/writes this exact file, so a rebind in the
// launcher reaches the game and both sides round-trip byte-identically.
//
// Single player only (the Game Boy is a one-player handheld). Button indices
// are the kGbPadButtons rebind-spec order (gb_profile.h: Up/Down/Left/Right/A/
// B/Start/Select); the bridge maps each to its `[controls]` key name. The
// `turbo` line is NOT a rebindable pad button here — it is preserved verbatim
// across rewrites (seeded to its default / whatever the file held). The `path`
// every call takes is the resolved keybinds.ini path (launcher_binds.c owns
// path resolution; the Game Boy default is "keybinds.ini").

#ifndef RUI_CONSOLE_GB_BINDS_H
#define RUI_CONSOLE_GB_BINDS_H

#ifdef __cplusplus
extern "C" {
#endif

// Seed the in-memory store with gb-recompiled's keybinds.c defaults, then
// overlay whatever [controls] lines exist in `path`. Writes the file in
// gb-recompiled's exact format if it is absent (so both sides agree on first
// run whichever process starts first).
void rui_gb_binds_init(const char* path);

// Current keyboard binding (SDL_Scancode as int; 0 = unbound) for rebind-spec
// button b (0..7). Auto-initializes from `path` on first use.
int  rui_gb_binds_get(const char* path, int b);

// Rebind + persist (rewrites keybinds.ini in gb-recompiled's format,
// preserving the turbo line).
void rui_gb_binds_set(const char* path, int b, int scancode);

// Reset all buttons to gb-recompiled's defaults + persist.
void rui_gb_binds_reset(const char* path);

#ifdef __cplusplus
}
#endif

#endif // RUI_CONSOLE_GB_BINDS_H
