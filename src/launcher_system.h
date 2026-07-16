// launcher_system.h — SystemProfile: one row per console.
//
// Concrete C shapes for the "inheritance" half of the architecture (see
// docs/ARCHITECTURE.md): each module (Controller/Save/Video/Verify) has a
// universal BASE behavior implemented once in the panel `draw()` functions,
// specialized per system by a small data SPEC held here. A SystemProfile also
// carries the "composition" half: the three panel-id arrays that tell each
// view (dashboard/settings/controller) WHICH panels this console gets, in
// slot order. Adding a system = adding one row to this file.
//
// This header is pure data + lookup: no UI toolkit dependency, safe to
// include from both the C model and the C++ ImGui backend.
//
// ---- how a game's SystemProfile is chosen -----------------------------------
// The public C ABI (RecompLauncherCGameInfo, recomp_launcher.h) is NOT changed
// by this refactor — hosts keep calling launcher_profile_apply(name, gi) (or
// hand-building `gi`) exactly as before, and that keeps writing the same ABI
// capability flags it always has. launcher_system_infer() below derives the
// matching SystemProfile FROM those already-set ABI fields (primarily
// `platform`, with a capability-flag fallback for callers that build `gi` by
// hand without going through launcher_profile_apply). launcher_model_init()
// calls it once and caches the result on the model so panels never re-infer.

#ifndef LAUNCHER_NG_SYSTEM_H
#define LAUNCHER_NG_SYSTEM_H

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
    int  max_players;                              // 2 or 4
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

typedef struct SystemProfile {
    const char* id, *platform, *theme, *rom_noun;
    ControllerSpec controller;   SaveSpec save;   VideoSpec video;   VerifySpec verify;
    uint32_t hotkeys_mask;                         // universal hotkeys opted into
    const char* const* panels_dashboard;           // composition (panel ids, in slot order)
    const char* const* panels_settings;
    const char* const* panels_controller;
} SystemProfile;                                    // ONE ROW PER CONSOLE

// ---- shared button set (rebind page) -------------------------------------------
// Mirrors launcher_button_name()/kButtonNames in launcher_model.c (kept in sync
// by hand — small, stable table). The live carve still calls
// launcher_button_name() directly for the on-screen label (single source of
// truth for TEXT); this table exists so ControllerSpec is a complete,
// self-describing per-system unit as the architecture calls for.
static const ButtonDef kSnesPadButtons[] = {
    { "Up", 0 }, { "Down", 1 }, { "Left", 2 }, { "Right", 3 },
    { "A", 4 }, { "B", 5 }, { "X", 6 }, { "Y", 7 },
    { "L", 8 }, { "R", 9 }, { "Start", 10 }, { "Select", 11 },
};
#define LNG_SNES_PAD_BUTTON_COUNT ((int)(sizeof(kSnesPadButtons) / sizeof(kSnesPadButtons[0])))

// PSX DualShock/digital-pad base set (24) — real vocabulary for the PSX
// rebind page (was borrowing kSnesPadButtons; see kSystemProfilePsx below).
// `code` is just the button's own index (0..23): the rebind page and bind
// persistence (launcher_binds.c) both address buttons by this spec index,
// not by any engine-side enum. The first 16 are the physical DualShock
// inputs; the last 8 are the keyboard->analog-stick DIRECTION binds
// (psxrecomp's runtime/launcher/psx_keybinds.h PSX_KB_LS_UP..PSX_KB_RS_RIGHT)
// that drive the left/right analog stick from the keyboard in analog pad
// modes. Mirrors the game's full 24-button kButtons vocabulary so every bind
// psx_keybinds.c understands is reachable from this launcher's rebind page
// (see launcher_binds.c's PSX-native keybind bridge for the persistence side).
static const ButtonDef kPsxPadButtons[] = {
    { "Up", 0 }, { "Down", 1 }, { "Left", 2 }, { "Right", 3 },
    { "Triangle", 4 }, { "Circle", 5 }, { "Cross", 6 }, { "Square", 7 },
    { "L1", 8 }, { "L2", 9 }, { "R1", 10 }, { "R2", 11 },
    { "L3", 12 }, { "R3", 13 }, { "Start", 14 }, { "Select", 15 },
    { "L-Stick Up", 16 }, { "L-Stick Down", 17 }, { "L-Stick Left", 18 }, { "L-Stick Right", 19 },
    { "R-Stick Up", 20 }, { "R-Stick Down", 21 }, { "R-Stick Left", 22 }, { "R-Stick Right", 23 },
};
#define LNG_PSX_PAD_BUTTON_COUNT ((int)(sizeof(kPsxPadButtons) / sizeof(kPsxPadButtons[0])))

// ---- panel composition arrays (NULL-terminated) --------------------------------
static const char* const kPanelsDashboardCommon[] = { "game", "controller", NULL };
// PSX only: adds the standalone "save" panel (WIDE, memory-card block-grid
// UI) below the game/controller row. SNES keeps kPanelsDashboardCommon
// unchanged — its SRAM row stays folded into the GAME card exactly as today.
static const char* const kPanelsDashboardPsx[] = { "game", "controller", "save", NULL };

static const char* const kPanelsSettingsSnes[]  = { "video", "audio", "hotkeys", NULL };
static const char* const kPanelsSettingsPsx[]   = { "video", "audio", "system", "hotkeys", NULL };
static const char* const kPanelsSettingsStub[]  = { "video", "audio", "hotkeys", NULL };

static const char* const kPanelsControllerCommon[] = { "controller_config", NULL };

// ---- SNES row -------------------------------------------------------------------
static const SystemProfile kSystemProfileSnes = {
    /* id       */ "snes",
    /* platform */ "SUPER NINTENDO",
    /* theme    */ NULL,        // default CRT-console theme
    /* rom_noun */ "ROM",
    /* controller */ {
        kSnesPadButtons, LNG_SNES_PAD_BUTTON_COUNT,
        "pad.tga", NULL, NULL,
        /* max_players */ 2, /* has_pad_mode */ 0,
    },
    /* save */    { SAVE_SRAM, 1, NULL },
    /* video */   {
        /*window_scale*/1, /*fullscreen*/0, /*linear_filter*/1, /*widescreen*/1,
        /*renderer*/0, /*supersampling*/0, /*screen_kind*/0, /*frame_interp*/0, /*aspect*/0,
        /*texture_filter*/0, /*antialiasing*/0, /*spu_hq*/0, /*skip_fmv*/0, /*turbo_loads*/0,
        /*bios*/0, /*deadzone*/0,
    },
    /* verify */  { 0, NULL },
    /* hotkeys_mask */ LNG_HOTKEYS_ALL,
    /* panels_dashboard  */ kPanelsDashboardCommon,
    /* panels_settings   */ kPanelsSettingsSnes,
    /* panels_controller */ kPanelsControllerCommon,
};

// ---- PSX row --------------------------------------------------------------------
static const SystemProfile kSystemProfilePsx = {
    /* id       */ "psx",
    /* platform */ "PLAYSTATION",
    /* theme    */ "psx",
    /* rom_noun */ "Disc",
    /* controller */ {
        kPsxPadButtons, LNG_PSX_PAD_BUTTON_COUNT,     // real PSX pad vocabulary (Triangle/Circle/Cross/Square/L1-2/R1-2/L3/R3)
        "pad.tga", "pad_analog.tga", "pad_digital.tga",
        /* max_players */ 2, /* has_pad_mode */ 1,
    },
    // MEMCARD is PSX's real target shape (2 slots): the standalone "save" panel
    // (see kPanelsDashboardPsx above) renders a dual-slot picker + 15-block
    // usage grid per slot (panel_save_draw in launcher_imgui.cpp). `probe` stays
    // NULL until a host wires up real per-block card scanning; the panel still
    // renders a full, real placeholder grid in the meantime (never a TODO).
    /* save */    { SAVE_MEMCARD, 2, NULL },
    /* video */   {
        /*window_scale*/0, /*fullscreen*/1, /*linear_filter*/0, /*widescreen*/0,
        /*renderer*/1, /*supersampling*/1, /*screen_kind*/1, /*frame_interp*/1, /*aspect*/1,
        /*texture_filter*/1, /*antialiasing*/1, /*spu_hq*/1, /*skip_fmv*/1, /*turbo_loads*/1,
        /*bios*/1, /*deadzone*/1,
    },
    /* verify */  { 1, NULL },    // disc-verdict mode; probe==NULL -> synthesized verdict (see launcher_model.c)
    // PSX's real hotkey catalog: the everyday transport controls only. Omits
    // LNG_HK_PAUSE_DIMMED (an SNES-engine-only attract-loop affordance) and
    // the window-resize pair (SNES-only integer-scale window; PSX sizes via
    // window_scale + fullscreen instead) — see requirement in the hotkey
    // catalog task. Everything else in the universal LngHotkey catalog applies.
    /* hotkeys_mask */ (uint32_t)((1u << LNG_HK_FULLSCREEN)     |
                                   (1u << LNG_HK_RESET)          |
                                   (1u << LNG_HK_PAUSE)          |
                                   (1u << LNG_HK_TURBO)          |
                                   (1u << LNG_HK_VOLUME_UP)      |
                                   (1u << LNG_HK_VOLUME_DOWN)    |
                                   (1u << LNG_HK_DISPLAY_PERF)   |
                                   (1u << LNG_HK_TOGGLE_RENDERER)),
    /* panels_dashboard  */ kPanelsDashboardPsx,
    /* panels_settings   */ kPanelsSettingsPsx,
    /* panels_controller */ kPanelsControllerCommon,
};

// ---- stub rows: identity only, minimal composition, NOT verified today --------
// These never get selected by launcher_system_infer() unless a host sets
// `platform` to exactly match (launcher_profile_apply already does, per name)
// — no game ships on these yet, so they render with the same minimal
// (legacy/SNES-shaped) settings surface as a safe default.
#define LNG_STUB_PROFILE(ID, PLATFORM, ROM_NOUN)                                   \
    static const SystemProfile kSystemProfile_##ID = {                            \
        #ID, PLATFORM, NULL, ROM_NOUN,                                             \
        { kSnesPadButtons, LNG_SNES_PAD_BUTTON_COUNT, "pad.tga", NULL, NULL, 2, 0 },\
        { SAVE_NONE, 0, NULL },                                                    \
        { 1, 0, 1, 0, 0,0,0,0,0, 0,0,0,0,0, 0,0 },                                 \
        { 0, NULL },                                                               \
        LNG_HOTKEYS_ALL,                                                           \
        kPanelsDashboardCommon, kPanelsSettingsStub, kPanelsControllerCommon,      \
    }

LNG_STUB_PROFILE(n64,    "NINTENDO 64",        "ROM");
LNG_STUB_PROFILE(genesis,"GENESIS",            "ROM");
LNG_STUB_PROFILE(gba,    "GAME BOY ADVANCE",   "ROM");
LNG_STUB_PROFILE(nes,    "NINTENDO",           "ROM");
LNG_STUB_PROFILE(gbc,    "GAME BOY COLOR",     "ROM");
LNG_STUB_PROFILE(smsgg,  "MASTER SYSTEM",      "ROM");
LNG_STUB_PROFILE(vb,     "VIRTUAL BOY",        "ROM");

#undef LNG_STUB_PROFILE

// Case-insensitive exact-string match (local copy of launcher_profile.h's
// lpr_is — kept file-local to avoid a header coupling in either direction).
static inline int lps_streq_ci(const char* a, const char* b) {
    if (!a || !b) return 0;
    for (; *a && *b; ++a, ++b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

// Resolve a SystemProfile by its short id ("psx", "snes", ...) — same names
// launcher_profile_apply() accepts. Returns NULL if unknown.
static inline const SystemProfile* launcher_system_by_id(const char* name) {
    if (!name || !name[0]) return NULL;
    if (lps_streq_ci(name, "psx") || lps_streq_ci(name, "ps1") || lps_streq_ci(name, "playstation"))
        return &kSystemProfilePsx;
    if (lps_streq_ci(name, "snes") || lps_streq_ci(name, "sfc") || lps_streq_ci(name, "supernintendo"))
        return &kSystemProfileSnes;
    if (lps_streq_ci(name, "n64") || lps_streq_ci(name, "nintendo64")) return &kSystemProfile_n64;
    if (lps_streq_ci(name, "genesis") || lps_streq_ci(name, "megadrive")) return &kSystemProfile_genesis;
    if (lps_streq_ci(name, "gba")) return &kSystemProfile_gba;
    if (lps_streq_ci(name, "nes")) return &kSystemProfile_nes;
    if (lps_streq_ci(name, "gbc")) return &kSystemProfile_gbc;
    if (lps_streq_ci(name, "smsgg") || lps_streq_ci(name, "sms") || lps_streq_ci(name, "gg"))
        return &kSystemProfile_smsgg;
    if (lps_streq_ci(name, "vb") || lps_streq_ci(name, "virtualboy")) return &kSystemProfile_vb;
    return NULL;
}

// Infer the SystemProfile for a game built through the C ABI. Primary signal
// is `platform` (the same console-subtitle string launcher_profile_apply()
// already sets per system — a de facto system id already present in the ABI).
// Falls back to a capability-flag heuristic for hosts that hand-build
// RecompLauncherCGameInfo without a recognized platform string (matches
// today's ONLY two real capability shapes: the PSX "deep settings" surface,
// or the legacy/SNES-shaped minimal surface).
static inline const SystemProfile* launcher_system_infer(const RecompLauncherCGameInfo* gi) {
    if (gi && gi->platform && gi->platform[0]) {
        if (lps_streq_ci(gi->platform, "PLAYSTATION"))       return &kSystemProfilePsx;
        if (lps_streq_ci(gi->platform, "SUPER NINTENDO"))    return &kSystemProfileSnes;
        if (lps_streq_ci(gi->platform, "NINTENDO 64"))       return &kSystemProfile_n64;
        if (lps_streq_ci(gi->platform, "GENESIS"))           return &kSystemProfile_genesis;
        if (lps_streq_ci(gi->platform, "GAME BOY ADVANCE"))  return &kSystemProfile_gba;
        if (lps_streq_ci(gi->platform, "NINTENDO"))          return &kSystemProfile_nes;
        if (lps_streq_ci(gi->platform, "GAME BOY COLOR"))    return &kSystemProfile_gbc;
        if (lps_streq_ci(gi->platform, "MASTER SYSTEM"))     return &kSystemProfile_smsgg;
        if (lps_streq_ci(gi->platform, "VIRTUAL BOY"))       return &kSystemProfile_vb;
    }
    if (gi && (gi->pad_mode_supported || gi->has_window_size || gi->has_renderer ||
               gi->has_bios || gi->has_supersampling))
        return &kSystemProfilePsx;
    return &kSystemProfileSnes;   // default: today's minimal/legacy surface
}

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_SYSTEM_H
