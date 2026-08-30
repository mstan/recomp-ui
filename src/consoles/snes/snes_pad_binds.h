// consoles/snes/snes_pad_binds.h — SNES gamepad bind persistence bridge.
//
// Mirrors consoles/psx/psx_pad_binds.h deliberately: same file shape, same
// call surface, same semantics. A player who configures a pad on a PSX title
// and then a SNES one should meet the identical model, and a maintainer
// fixing one bridge should find the other in the same shape.
//
// snesrecomp's runtime historically read ONE flat gamepad map from
// config.ini ([GamepadMap] Controls / ControlsP2) — no device identity, so
// two different pads shared one mapping and swapping controllers silently
// inherited the other's binds. This bridge moves SNES onto per-GUID
// input.ini, matching PSX:
//
//   [gamepads]              guid = display name
//   [mapping.<guid>]        button sources + deadzone (percent) + name_custom
//   [mapping]               global fallback (also seeded from a legacy
//                           config.ini [GamepadMap] during migration)
//
// Button sources use SDL controller names ("dpup", "a", "leftshoulder",
// "lefttrigger", axis forms "leftx-"/"leftx+"), the same vocabulary PSX
// uses — NOT the legacy config.ini token spelling ("DpadUp", "Lb").
// snes_pad_binds_legacy_token() converts the old spelling on migration.
//
// Default display name is the SDL driver name; Rename Gamepad sets a custom
// name that wins over the driver name until cleared.

#ifndef RUI_CONSOLE_SNES_PAD_BINDS_H
#define RUI_CONSOLE_SNES_PAD_BINDS_H

#ifdef __cplusplus
extern "C" {
#endif

#define RUI_SNES_PAD_MAX_KNOWN 16
#define RUI_SNES_PAD_DEFAULT_DEADZONE_PCT 10

// Load input.ini (or seed defaults). `path` is the resolved input.ini path.
void rui_snes_pad_binds_init(const char* path);

// Fill `out` (cap bytes) with the display label for `guid`'s button `b`
// (0..LNG_SNES_PAD_BUTTON_COUNT-1, UI order). Falls back to global [mapping]
// then built-in defaults when a GUID section is absent. Does NOT create a
// GUID section (Save Profile / remember / Play do that).
void rui_snes_pad_binds_label(const char* path, const char* guid, int b,
                              char* out, int cap);

// Rebind button `b` for `guid`. kind: 0 none, 1 button, 2 axis.
void rui_snes_pad_binds_set(const char* path, const char* guid, int b,
                            int kind, int code, int axis_dir);

// Reset one GUID's mapping sources to built-in defaults (keeps name/deadzone).
void rui_snes_pad_binds_reset(const char* path, const char* guid);

// Ensure a GUID profile exists. `name` is the driver (or current) display name
// when not custom; does not clear an existing custom name. `deadzone_pct` < 0
// leaves the stored deadzone unchanged (default 10 on first create).
void rui_snes_pad_binds_remember(const char* path, const char* guid,
                                 const char* name, int deadzone_pct);

// Full profile save: name, custom flag, deadzone, and current mappings on disk.
void rui_snes_pad_binds_save_profile(const char* path, const char* guid,
                                     const char* name, int name_custom,
                                     int deadzone_pct);

// Set a user-chosen display name (marks name_custom).
void rui_snes_pad_binds_rename(const char* path, const char* guid,
                               const char* name);

// Remove a GUID's mapping + registry entry from input.ini.
void rui_snes_pad_binds_delete(const char* path, const char* guid);

int  rui_snes_pad_binds_known_count(const char* path);
int  rui_snes_pad_binds_known_at(const char* path, int index,
                                 char* guid, int guid_cap,
                                 char* name, int name_cap);

void rui_snes_pad_binds_name(const char* path, const char* guid,
                             char* out, int cap);
int  rui_snes_pad_binds_name_is_custom(const char* path, const char* guid);

// Deadzone percent (0..100) for GUID; default RUI_SNES_PAD_DEFAULT_DEADZONE_PCT.
int  rui_snes_pad_binds_deadzone(const char* path, const char* guid);

// Legacy config.ini [GamepadMap] token ("DpadUp", "Lb", "Back") -> the SDL
// source name this bridge stores ("dpup", "leftshoulder", "back"). Returns
// NULL for an unrecognised token. Used by the one-time migration so a port's
// existing mapping becomes the global [mapping] fallback rather than being
// silently replaced by defaults.
const char* rui_snes_pad_binds_legacy_token(const char* legacy);

#ifdef __cplusplus
}
#endif

#endif // RUI_CONSOLE_SNES_PAD_BINDS_H
