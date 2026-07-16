// launcher_profile.h — per-system "variant profiles".
//
// A variant profile bundles a whole console's launcher identity as ONE unit so
// theme, controller art, platform label, ROM noun, and capability set can never
// drift apart. A host selects a profile by name (its system), then overrides the
// per-GAME specifics (which aspects THIS title offers, whether pad-mode is
// locked, the box art, ROM hashes, etc.).
//
//   RecompLauncherCGameInfo gi = {0};
//   launcher_profile_apply("psx", &gi);   // system defaults (theme+pad+caps)
//   gi.name = "Ape Escape"; gi.aspect_mask = 0x7; gi.pad_mode_selectable = 0; ...
//
// The controller IMAGE is staged per variant by the build (recomp_ui.cmake PAD
// override, or the shipped pad_analog/pad_digital.tga when pad_mode_supported).
// This header only sets the DATA (theme name, platform, rom_noun, capability
// flags); it never hardcodes a look into the core.
//
// Adding a new system = add one row here. Nothing else in the core changes.

#ifndef LAUNCHER_PROFILE_H
#define LAUNCHER_PROFILE_H

#include "recomp_launcher.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Case-insensitive first-two-chars match helper (avoids <ctype.h> in a header).
static inline int lpr_is(const char* a, const char* b) {
    if (!a || !b) return 0;
    for (; *a && *b; ++a, ++b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

// Apply a system's default identity + capabilities onto `gi`. Unknown names get
// the neutral default (CRT theme, "ROM" noun, no extra capabilities). Returns 1
// if the name matched a known profile, 0 otherwise. The host should call this
// FIRST, then set per-game fields (name, box art, hashes, and any per-title
// capability overrides such as which aspects the game actually offers).
static inline int launcher_profile_apply(const char* name, RecompLauncherCGameInfo* gi) {
    if (!gi) return 0;

    // --- PlayStation ---------------------------------------------------------
    if (lpr_is(name, "psx") || lpr_is(name, "ps1") || lpr_is(name, "playstation")) {
        gi->theme    = "psx";              // PlayStation blue, no CRT scanlines
        gi->platform = "PLAYSTATION";
        gi->rom_noun = "Disc";
        // Controller: PSX has analog/digital pad modes + swapping DualShock art.
        gi->pad_mode_supported  = 1;
        gi->pad_mode_selectable = 1;       // per-game lock_mode may set this to 0
        gi->allow_hybrid        = 1;
        gi->aspect_mask         = 0x1;     // 4:3 always; game adds 16:9 (0x2) / 21:9 (0x4)
        // Full PS1 settings surface.
        gi->has_window_size = 1; gi->has_renderer = 1; gi->has_supersampling = 1;
        gi->has_antialiasing = 1; gi->has_texture_filter = 1; gi->has_screen_kind = 1;
        gi->has_frame_interp = 1; gi->has_spu_hq = 1; gi->has_skip_fmv = 1;
        gi->has_turbo_loads = 1; gi->has_fullscreen_toggle = 1; gi->has_bios = 1;
        gi->has_deadzone_pct = 1;
        return 1;
    }

    // --- Super Nintendo ------------------------------------------------------
    if (lpr_is(name, "snes") || lpr_is(name, "sfc") || lpr_is(name, "supernintendo")) {
        gi->theme    = NULL;               // default CRT-console theme (violet + scanlines)
        gi->platform = "SUPER NINTENDO";
        gi->rom_noun = "ROM";
        gi->widescreen_supported = 1;      // 16:9 toggle (legacy aspect path)
        // (MSU-1 / SRAM are per-game; window scale + linear filter are the defaults.)
        return 1;
    }

    // --- other systems: identity now, capabilities refined as each is built --
    if (lpr_is(name, "n64") || lpr_is(name, "nintendo64")) {
        gi->theme = NULL; gi->platform = "NINTENDO 64"; gi->rom_noun = "ROM"; return 1;
    }
    if (lpr_is(name, "genesis") || lpr_is(name, "megadrive")) {
        gi->theme = NULL; gi->platform = "GENESIS"; gi->rom_noun = "ROM"; return 1;
    }
    if (lpr_is(name, "gba")) {
        gi->theme = NULL; gi->platform = "GAME BOY ADVANCE"; gi->rom_noun = "ROM"; return 1;
    }
    if (lpr_is(name, "nes")) {
        gi->theme = NULL; gi->platform = "NINTENDO"; gi->rom_noun = "ROM"; return 1;
    }
    if (lpr_is(name, "gbc")) {
        gi->theme = NULL; gi->platform = "GAME BOY COLOR"; gi->rom_noun = "ROM"; return 1;
    }
    if (lpr_is(name, "smsgg") || lpr_is(name, "sms") || lpr_is(name, "gg")) {
        gi->theme = NULL; gi->platform = "MASTER SYSTEM"; gi->rom_noun = "ROM"; return 1;
    }
    if (lpr_is(name, "vb") || lpr_is(name, "virtualboy")) {
        gi->theme = NULL; gi->platform = "VIRTUAL BOY"; gi->rom_noun = "ROM"; return 1;
    }

    // Neutral fallback.
    gi->theme = NULL; gi->rom_noun = "ROM";
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_PROFILE_H
