// launcher_model.h — game-agnostic view-model for the next-gen launcher.
//
// This is the DRY heart of the new launcher: it owns all launcher STATE and
// BEHAVIOR (which panels exist, what a control does, how a rebind is captured)
// and is completely free of any UI toolkit, SDL, or OpenGL. Both prototype
// render backends (Dear ImGui and Clay) draw this same model and call the same
// mutators, so behavior is identical across backends and — because it is built
// purely from the existing C ABI structs (RecompLauncherCSettings /
// RecompLauncherCGameInfo) — identical across every game in the ecosystem.
//
// The surface mirrors the shipping RmlUi MMX launcher (launcher.rml) so the
// prototype is a faithful parity check of what we offer the end user:
//   Dashboard  : game/ROM info + CRC/SHA badges + Change ROM + controllers
//   Settings   : window scale, linear filter, sample rate, volume, hotkeys
//   Controller : input source, deadzone, keyboard rebinds
//   Footer     : Skip-on-Boot (+confirm modal), Settings/Back, PLAY
// Per-game gating (widescreen/MSU-1/saves) hides panels exactly as today.

#ifndef LAUNCHER_NG_MODEL_H
#define LAUNCHER_NG_MODEL_H

#include "recomp_launcher.h"   // RecompLauncherCSettings, RecompLauncherCGameInfo

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque forward decl: the model carries a pointer to its inferred
// SystemProfile (launcher_system.h) so panels can read per-system specs
// (pad art, save kind, hotkeys mask, panel composition) without every TU that
// touches LauncherModel needing the full SystemProfile definition.
struct SystemProfile;

typedef enum {
    LNG_VIEW_DASHBOARD = 0,
    LNG_VIEW_SETTINGS,
    LNG_VIEW_CONTROLLER,
} LngView;

typedef enum {
    LNG_ACTION_NONE = 0,   // still running
    LNG_ACTION_LAUNCH,     // boot the game with committed settings
    LNG_ACTION_QUIT        // user quit
} LngAction;

// Representative subset of the SNES pad for the rebind UI.
typedef enum {
    LNG_BTN_UP = 0, LNG_BTN_DOWN, LNG_BTN_LEFT, LNG_BTN_RIGHT,
    LNG_BTN_A, LNG_BTN_B, LNG_BTN_X, LNG_BTN_Y,
    LNG_BTN_L, LNG_BTN_R, LNG_BTN_START, LNG_BTN_SELECT,
    LNG_BTN_COUNT
} LngButton;

// System hotkeys — mirrors the engine's config.ini [KeyMap] keys exactly, so
// editing them here surgically rewrites the same lines config.c parses.
typedef enum {
    LNG_HK_FULLSCREEN = 0, LNG_HK_RESET, LNG_HK_PAUSE, LNG_HK_PAUSE_DIMMED,
    LNG_HK_TURBO, LNG_HK_WINDOW_BIGGER, LNG_HK_WINDOW_SMALLER,
    LNG_HK_VOLUME_UP, LNG_HK_VOLUME_DOWN, LNG_HK_DISPLAY_PERF, LNG_HK_TOGGLE_RENDERER,
    LNG_HK_COUNT
} LngHotkey;

typedef struct {
    // ---- static game facts (borrowed from RecompLauncherCGameInfo) ----
    const char* game_name;          // e.g. "Mega Man X"
    const char* region;             // e.g. "USA"
    const char* platform;           // console subtitle, e.g. "PLAYSTATION" (NULL => none)
    bool        widescreen_supported;
    bool        msu1_supported;      // sram-like: show the MSU-1 module when true
    const char* msu1_note;           // borrowed; which patch, shown in the card
    bool        saves_supported;     // sram_path != NULL -> show the SAVES panel
    const char* sram_path;           // borrowed; NULL when the game has no SRAM
    // Number of players the GAME actually supports. Mega Man X is 1-player, so
    // the launcher must not show a dead Player 2 row. Games that support 2
    // report 2 and the second row appears. Driven by data, never hardcoded.
    int         player_count;

    // ---- ROM verification ----
    // Expected fingerprint, borrowed from the game's C-ABI struct.
    uint32_t        expected_crc;
    int             has_expected_crc;
    const uint8_t (*known_sha256)[32];
    size_t          num_known_sha256;

    // ---- controller pad-mode caps (PlayStation-style analog/digital) ----
    bool     pad_mode_supported;    // false => no selector/art swap; generic pad.tga
    bool     pad_mode_selectable;   // false => selector hidden, mode forced to locked_pad_mode
    bool     allow_hybrid;          // false => Hybrid option hidden
    int      locked_pad_mode;       // forced mode when !pad_mode_selectable
    bool     lock_device;           // true => hide the player controller cards entirely

    // ---- aspect ratio caps ----
    // bit0 = 4:3 (implied/always), bit1 = 16:9, bit2 = 21:9. 0 => legacy
    // widescreen_supported bool drives display settings instead.
    int      aspect_mask;

    // ---- deeper PSX-style settings capability flags (0 => control hidden) ----
    bool     has_window_size;
    bool     has_renderer;
    bool     has_supersampling;
    bool     has_antialiasing;
    bool     has_texture_filter;
    bool     has_screen_kind;
    bool     has_frame_interp;
    bool     has_spu_hq;
    bool     has_skip_fmv;
    bool     has_turbo_loads;
    bool     has_fullscreen_toggle;
    bool     has_bios;
    bool     has_deadzone_pct;
    const char* rom_noun;             // "ROM" default; e.g. "Disc" for PSX
    const char* const* language_labels;  // borrowed; NULL/num_languages==0 => no Localization menu
    int      num_languages;

    // ---- inferred SystemProfile (launcher_system.h) ----
    // Derived once in launcher_model_init() from the ABI GameInfo's `platform`
    // field (falling back to a capability-flag heuristic) — see
    // launcher_system_infer(). Drives which panels compose into each view and
    // supplies per-system specs (pad art, save kind, hotkeys mask). Never
    // NULL after init.
    const struct SystemProfile* profile;

    bool     rom_present;
    char     rom_full[512];          // absolute path (what we hand to the game)
    char     rom_file[128];          // basename for display, e.g. "mmx.sfc"
    char     rom_size[48];           // "1.50 MB"
    char     rom_header[24];         // "LoROM"
    char     rom_crc_str[16];        // "1B4B2E9C"
    char     rom_sha_str[24];        // "9c2e…d41f"
    bool     crc_match;
    bool     sha_match;

    // ---- editable settings (working copy of the C ABI struct) ----
    RecompLauncherCSettings s;

    // ---- transient UI state ----
    LngView   view;
    LngAction action;
    int       cfg_player;            // 0/1 — which player the Controller view edits
    bool      skip_modal_open;       // "Skip the launcher on boot?" confirm

    // Selected gamepad per player (when player_src == 2). pad_id is the live
    // SDL_JoystickID; name is cached for display if the device disconnects.
    uint32_t  player_pad_id[2];
    char      player_pad_name[2][64];

    // rebind capture state machine
    bool      capturing;         // capturing a player button
    LngButton capture_btn;
    bool      hk_capturing;      // capturing a system hotkey
    LngHotkey capture_hk;
    char      binds[2][LNG_BTN_COUNT][32];  // per-player keyboard binding labels
    char      hotkeys[LNG_HK_COUNT][32];    // [KeyMap] value strings, e.g. "Ctrl+R"
} LauncherModel;

// Build the model from the inbound C ABI structs. `initial_rom` may be NULL.
void launcher_model_init(LauncherModel* m,
                         const RecompLauncherCSettings* io,
                         const RecompLauncherCGameInfo* game,
                         const char* initial_rom);

// Copy the working settings back into the caller's struct (on LAUNCH).
void launcher_model_commit(const LauncherModel* m, RecompLauncherCSettings* io);

// Adopt a newly-picked ROM path (from the native file dialog): updates the
// displayed file name / verification state.
void launcher_model_set_rom(LauncherModel* m, const char* path);

// Full path of the currently selected ROM ("" when none).
const char* launcher_model_rom_path(const LauncherModel* m);

// True iff a ROM is loaded and every fingerprint the game provides (CRC and/or
// SHA-256) matches. If the game provides no fingerprint at all, returns false
// (we can't vouch for an unknown ROM).
bool launcher_model_rom_verified(const LauncherModel* m);

// ---- navigation ----
void launcher_model_set_view(LauncherModel* m, LngView v);
void launcher_model_open_config(LauncherModel* m, int player);  // -> Controller view

// ---- display settings ----
void launcher_model_cycle_scale(LauncherModel* m);   // 1..6 wrap
void launcher_model_toggle_filter(LauncherModel* m);
void launcher_model_toggle_widescreen(LauncherModel* m);  // gated

// ---- aspect ratio (PSX-style; only meaningful when aspect_mask != 0) ----
// Cycle through the OFFERED aspects only (4:3 always offered; 16:9/21:9 per
// aspect_mask). No-op when aspect_mask == 0 (legacy widescreen bool games).
void launcher_model_cycle_aspect(LauncherModel* m);
const char* launcher_model_aspect_label(const LauncherModel* m);  // "4:3 (Native)" etc.
bool launcher_model_aspect_offered(const LauncherModel* m, int index);  // 0=4:3,1=16:9,2=21:9

// ---- audio settings ----
void launcher_model_cycle_freq(LauncherModel* m);    // 32000/44100/48000
void launcher_model_volume_delta(LauncherModel* m, int delta);  // clamp 0..100

// ---- deeper PSX-style settings (capability-gated; no-op / harmless when the
// corresponding has_* flag is false — callers should still gate the UI on the
// flag so the control isn't shown at all, per the RmlUi PSX launcher parity). ----
void launcher_model_cycle_window_size(LauncherModel* m);       // {960,1280,1600,1920} wrap
const char* launcher_model_window_size_label(const LauncherModel* m);  // "1280 x 960" (H follows aspect)
void launcher_model_toggle_renderer(LauncherModel* m);         // Software/OpenGL
const char* launcher_model_renderer_label(const LauncherModel* m);
void launcher_model_cycle_supersampling(LauncherModel* m);     // 1x..4x wrap
const char* launcher_model_supersampling_label(const LauncherModel* m);
void launcher_model_toggle_aa(LauncherModel* m);
void launcher_model_toggle_texture_filter(LauncherModel* m);   // Nearest/Bilinear
const char* launcher_model_texture_filter_label(const LauncherModel* m);
void launcher_model_cycle_screen_kind(LauncherModel* m);       // Raw/CRT/Composite/Trinitron
const char* launcher_model_screen_kind_label(const LauncherModel* m);
void launcher_model_toggle_frame_interp(LauncherModel* m);
void launcher_model_cycle_interp_fps(LauncherModel* m);        // {0,90,120,144,165,240} wrap
const char* launcher_model_interp_fps_label(const LauncherModel* m);  // "Display refresh"/"90 fps"
void launcher_model_toggle_spu_hq(LauncherModel* m);
void launcher_model_toggle_skip_fmv(LauncherModel* m);
void launcher_model_toggle_turbo_loads(LauncherModel* m);
void launcher_model_toggle_fullscreen(LauncherModel* m);       // simple on/off (PSX row)
void launcher_model_cycle_language(LauncherModel* m);          // wraps over num_languages
const char* launcher_model_language_label(const LauncherModel* m);
void launcher_model_cycle_deadzone_pct(LauncherModel* m);      // 0..50 step 5, wraps; mirrors both players
const char* launcher_model_deadzone_pct_label(const LauncherModel* m);  // "37%"
void launcher_model_set_bios_path(LauncherModel* m, const char* path);

// ---- MSU-1 (only when msu1_supported) ----
void launcher_model_toggle_msu1(LauncherModel* m);
void launcher_model_set_msu1_dir(LauncherModel* m, const char* dir);

// ---- controllers ----
// PSX-style pad mode: 0=Hybrid, 1=Analog, 2=D-Pad. Gated: no-op when
// !pad_mode_selectable (mode is locked); snaps away from Hybrid when
// !allow_hybrid.
void launcher_model_set_pad_mode(LauncherModel* m, int player, int mode);
void launcher_model_cycle_player_src(LauncherModel* m, int player); // None/Kbd/Pad
void launcher_model_deadzone_delta(LauncherModel* m, int player, int delta);
// Set the input source explicitly (used by the device dropdown). kind: 0 None,
// 1 Keyboard, 2 Gamepad. For gamepad, pass the SDL id + display name.
void launcher_model_set_source(LauncherModel* m, int player, int kind,
                               uint32_t pad_id, const char* pad_name);

// ---- skip-on-boot (footer switch + confirm modal) ----
void launcher_model_request_skip_toggle(LauncherModel* m); // opens modal when enabling
void launcher_model_skip_confirm(LauncherModel* m);
void launcher_model_skip_cancel(LauncherModel* m);

// ---- rebind capture (player buttons) ----
void launcher_model_begin_capture(LauncherModel* m, LngButton b);
void launcher_model_cancel_capture(LauncherModel* m);
// ---- hotkey capture ----
void launcher_model_begin_hk_capture(LauncherModel* m, LngHotkey h);
void launcher_model_cancel_hk_capture(LauncherModel* m);

// ---- display-string helpers (single source of truth across backends) ----
const char* launcher_model_scale_label(const LauncherModel* m);        // "3x"
const char* launcher_model_freq_label(const LauncherModel* m);         // "44100 Hz"
const char* launcher_model_player_src_label(const LauncherModel* m, int player);
const char* launcher_button_name(LngButton b);
const char* launcher_hotkey_name(LngHotkey h);
const char* launcher_view_name(LngView v);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_MODEL_H
