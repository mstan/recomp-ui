// launcher_system_types.h — the SystemProfile TYPE and its module specs.
//
// Concrete C shapes for the "inheritance" half of the architecture (see
// docs/ARCHITECTURE.md): each module (Controller/Save/Video/Verify) has a
// universal BASE behavior implemented once in the panel `draw()` functions,
// specialized per system by a small data SPEC held in a SystemProfile row.
// A SystemProfile also carries the "composition" half: the three panel-id
// arrays that tell each view (dashboard/settings/controller) WHICH panels
// this console gets, in slot order.
//
// This header is the CONSOLE-AGNOSTIC part only: the types, the shared panel
// composition arrays, and the string matcher. The per-console ROWS live in
// src/consoles/<id>/<id>_profile.h (one unit per console: pad vocabulary +
// profile row + ABI capability defaults); src/launcher_system.h aggregates
// them and provides the by-id / inference lookups. Adding a system = adding
// one directory under src/consoles/ and registering it in the aggregator.
//
// Pure data + lookup: no UI toolkit dependency, safe to include from both
// the C model and the C++ ImGui backend.

#ifndef LAUNCHER_NG_SYSTEM_TYPES_H
#define LAUNCHER_NG_SYSTEM_TYPES_H

#include "recomp_launcher.h"
#include "launcher_model.h"   // LauncherModel (SaveProbeFn/VerifyProbeFn take a pointer to it)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Controller module -------------------------------------------------------
typedef struct { const char* label; int code; } ButtonDef;

typedef struct {
    const ButtonDef* buttons; int button_count;   // per-system base set (rebind page)
    const char* image;                             // base pad art (fallback / no pad-mode)
    const char* image_analog, *image_digital;       // optional mode-swap pair (PSX-style)
    int  max_players;                              // 1, 2 or 4
    int  has_pad_mode;                              // analog/digital selector offered
} ControllerSpec;

// ---- Save module --------------------------------------------------------------
typedef enum { SAVE_NONE = 0, SAVE_SRAM, SAVE_MEMCARD } SaveKind;
typedef bool (*SaveProbeFn)(const LauncherModel* m, int slot);   // unimplemented hook (NULL today)
typedef struct { SaveKind kind; int slots; SaveProbeFn probe; } SaveSpec;

// ---- Video / Display module ----------------------------------------------------
// Documents each system's DEFAULT capability template. The model's own
// has_*/aspect_mask/widescreen_supported fields (sourced straight from the C
// ABI GameInfo, unchanged by this refactor) remain the single source of truth
// panels actually gate on — this spec is the authored "shape" for the system,
// kept here so a system row is legible as ONE unit.
typedef struct {
    int window_scale, fullscreen;                  // base (every system)
    int linear_filter, widescreen;                  // SNES-ish legacy surface
    int renderer, supersampling, screen_kind, frame_interp, aspect, texture_filter,
        antialiasing, spu_hq, skip_fmv, turbo_loads, bios, deadzone; // PSX-ish deep surface
} VideoSpec;

// ---- Verify module --------------------------------------------------------------
// Host-provided disc-verdict probe (mode==1 systems, e.g. PSX): given the
// model (rom_full/rom_present etc.), fill `out` with the real serial/region/
// ISO-header/verdict facts and return true. Return false (or pass probe ==
// NULL, the default today) to have launcher_model_set_rom() synthesize a
// placeholder verdict from available facts instead, so the disc-verdict UI
// always has something real to render even before a host wires this up.
typedef bool (*VerifyProbeFn)(const LauncherModel* m, VerifyResult* out);
typedef struct { int mode; /* 0 rom-hash, 1 disc-verdict */ VerifyProbeFn probe; } VerifySpec;

// ---- Hotkeys module: a bitmask over LngHotkey (launcher_model.h) --------------
// LNG_HK_COUNT is 11 today; ALL bits set = every catalog hotkey opted in.
// SNES uses this (full legacy catalog, byte-identical to the original single
// hardcoded panel); PSX opts into a tailored subset instead — see its row.
#define LNG_HOTKEYS_ALL 0x7FFu

// ---- ROM file-picker filter --------------------------------------------------
// The native "Change ROM" dialog's extension filter, per console — so a GBA
// game offers *.gba, a PSX game *.cue/*.bin, never a hardcoded SNES set.
// patterns is a tinyfiledialogs glob list ("*.gba"); desc is the filter label.
typedef struct {
    const char* const* patterns; int pattern_count;
    const char* desc;                              // e.g. "Game Boy Advance ROM (.gba)"
} RomFilterSpec;

typedef struct SystemProfile {
    const char* id, *platform, *theme, *rom_noun;
    ControllerSpec controller;   SaveSpec save;   VideoSpec video;   VerifySpec verify;
    uint32_t hotkeys_mask;                         // universal hotkeys opted into
    const char* const* panels_dashboard;           // composition (panel ids, in slot order)
    const char* const* panels_settings;
    const char* const* panels_controller;
    // Screen-model vocabulary for the "Screen model" cycle (video.screen_kind
    // capability). NULL/0 => the legacy 4-entry PSX-era set (Raw/CRT/
    // Composite/Trinitron, launcher_model.c) — existing rows are positional
    // initializers, so trailing zero-init keeps them on the legacy set.
    const char* const* screen_kind_names;
    int                screen_kind_count;
    // "Change ROM" native-dialog filter (per console). All-zero => the
    // built-in SNES default (back-compat); every built-out console sets it.
    RomFilterSpec rom_filter;
} SystemProfile;                                    // ONE ROW PER CONSOLE

// ---- shared panel composition arrays (NULL-terminated) --------------------------
// Per-console arrays that DIFFER from these live in that console's profile
// header (e.g. kPanelsDashboardPsx adds the "save" memcard panel).
static const char* const kPanelsDashboardCommon[] = { "game", "controller", NULL };
static const char* const kPanelsSettingsStub[]  = { "video", "audio", "hotkeys", NULL };
static const char* const kPanelsControllerCommon[] = { "controller_config", NULL };

// Case-insensitive exact-string match, shared by every console's name-alias
// matcher and the aggregator lookups (launcher_system.h).
static inline int lps_streq_ci(const char* a, const char* b) {
    if (!a || !b) return 0;
    for (; *a && *b; ++a, ++b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_SYSTEM_TYPES_H
