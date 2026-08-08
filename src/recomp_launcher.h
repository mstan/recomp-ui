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

/* Mod support is framework-available but developer opt-in. The integration
 * helper defines this from its default-OFF RECOMP_UI_ENABLE_MODS CMake option.
 * Keep a source-level default for hosts that compile the launcher sources
 * without recomp_ui.cmake. The public provider ABI remains visible either way
 * so enabling the option never changes C struct layouts. */
#ifndef RECOMP_UI_ENABLE_MODS
#define RECOMP_UI_ENABLE_MODS 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Storage ceiling, not a default: each SystemProfile supplies its console
// ceiling and each game supplies num_players (for example SMW Co-op remains
// 2). Consoles/games with fewer players never touch the upper slots. Hosts
// that predate the widening only ever wrote [0]/[1], and their memset(0)
// leaves new slots in the same "none" state they had implicitly before.
// Every consumer compiles this header from source (submodule pin), so the
// layout change is absorbed by the consumer's normal rebuild on a pin bump.
#define RECOMP_LAUNCHER_MAX_PLAYERS 8
/* Host may #ifdef this when reading player_gamepad_guid[] from settings. */
#define RECOMP_LAUNCHER_HAS_PLAYER_GAMEPAD_GUID 1

// N64 Transfer Pak slots — one per controller port.
#define RECOMP_LAUNCHER_MAX_TPAKS 4

/* Netplay lobby membership ceiling (party games up to 8). */
#define RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS 8

typedef struct RecompLauncherCSettings RecompLauncherCSettings;

typedef struct RecompLauncherCNetplayLobby {
    char lobby_id[40];
    char name[64];
    char game_name[64];
    char game_version[32];
    int  player_count;
    int  max_slots;
    int  has_password;
    /* Round-trip ms to the lobby host; -1 when unknown / timed out. */
    int  latency_ms;
} RecompLauncherCNetplayLobby;

typedef struct RecompLauncherCNetplayMember {
    int  slot;
    char display_name[64];
    int  ready;
    int  is_host;
    /* Round-trip ms from the local peer *to* this seat; -1 unknown / self. */
    int  latency_ms;
    /* 1 when this row is the local client's seat (never show self-RTT). */
    int  is_local;
} RecompLauncherCNetplayMember;

typedef struct RecompLauncherCNetplayLaunch {
    int      enabled;
    int      local_slot;
    /* Host PlayerInput index to sample for this peer. -1 = auto (prefer
     * dashboard P1 / NETPLAY card; else seat card; else sole assigned). */
    int      input_player;
    char     bind_hostport[64];
    char     peer_hostport[64];
    uint32_t session_id;
    int      input_delay;
    int      max_slots; /* lobby seat ceiling (2..RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS) */
    /* Seated players at launch — delay-sync slot_count. 0 = unknown / use max_slots. */
    int      player_count;
    /* Host match_caps: opt into lobby-server UDP input relay (0/1).
     * 3+ defaults to host-as-relay unless this is set. */
    int      force_input_relay;
    /* Host match_caps: ICE relay-only / Force TURN for UDP (0/1). */
    int      force_turn;
} RecompLauncherCNetplayLaunch;

typedef struct RecompLauncherCNetplayLocalAddress {
    /* Numeric address advertised to clients, currently normally IPv4. */
    char address[64];
    /* User-facing interface name, for example "Wi-Fi" or "Ethernet". */
    char label[64];
} RecompLauncherCNetplayLocalAddress;

typedef struct RecompLauncherCNetplayCallbacks {
    void* ctx;
    /* Configuration and connection state are host-owned and may be persisted. */
    const char* (*default_url)(void* ctx);
    void (*set_lobby_url)(void* ctx, const char* url);
    int  (*connect)(void* ctx);
    int  (*connected)(void* ctx);
    void (*pump)(void* ctx);
    void (*set_player_name)(void* ctx, const char* name);
    const char* (*player_name)(void* ctx);
    /* list_* merges remote server lobbies with any same-machine LAN registry
     * row. Hosts advertise to exactly one channel (see create lan_only). */
    void (*request_list)(void* ctx);
    int  (*list_count)(void* ctx);
    int  (*list_get)(void* ctx, int index, RecompLauncherCNetplayLobby* out);
    /* Address discovery used by the Host Lobby modal. */
    int  (*local_ip)(void* ctx, char* out, size_t out_len);
    int  (*external_ip)(void* ctx, char* out, size_t out_len);
    /* Lobby operations return 0 when the request was accepted.
     * create: host_endpoint is in/out (capacity >= 64). recomp-ui applies the
     * universal UDP port policy before calling this — LAN keeps the exact port
     * (UI blocks create when busy); online may already have rewritten the port
     * to the first free value in preferred..preferred+31. Hosts should publish
     * the given endpoint as-is. Returns -4 only as a defensive fallback when
     * the host itself cannot use the port (UI surfaces the same messages).
     * lan_only != 0: publish only the local LAN registry (no lobby server).
     * lan_only == 0: publish only on the lobby server (no LAN registry). */
    /* max_slots: 2..RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS, clamped by the host
     * to the game's supported player count. */
    int  (*create)(void* ctx, const char* lobby_name, char* host_endpoint,
                   const char* password, const RecompLauncherCSettings* settings,
                   int lan_only, int max_slots);
    /* join: guest_bind is in/out (capacity >= 64). recomp-ui applies the
     * universal guest UDP bind policy before calling — prefer 7778, then
     * 7778+1 .. +31, written as "0.0.0.0:<port>". Hosts should advertise that
     * bind on the lobby join (never rewrite to :0). Engines may still normalize
     * NULL/empty/host:0 as a defensive fallback. */
    int  (*join)(void* ctx, const char* lobby_id, const char* password,
                 char* guest_bind);
    int  (*leave)(void* ctx);
    int  (*in_lobby)(void* ctx);
    int  (*is_host)(void* ctx);
    int  (*member_count)(void* ctx);
    int  (*member_get)(void* ctx, int index, RecompLauncherCNetplayMember* out);
    int  (*move_member)(void* ctx, int from_slot, int to_slot);
    int  (*local_ready)(void* ctx);
    int  (*all_ready)(void* ctx);
    int  (*set_ready)(void* ctx, int ready);
    int  (*request_start)(void* ctx, const RecompLauncherCSettings* settings);
    /* All peers launch only after the host's start request becomes pending. */
    int  (*launch_pending)(void* ctx);
    void (*clear_launch_pending)(void* ctx);
    int  (*fill_launch)(void* ctx, RecompLauncherCNetplayLaunch* out);
    /*
     * Optional multi-interface address discovery. Called with indices starting
     * at zero until it returns 0. The launcher clears out before each call;
     * address must be non-empty on success and label may be empty. Append-only
     * for compatibility with positional callback-table initializers; local_ip
     * remains the fallback.
     */
    int  (*local_address_get)(void* ctx, int index,
                              RecompLauncherCNetplayLocalAddress* out);
    /* Host-only: remove the player in `slot` (not the host). Optional. */
    int  (*kick_member)(void* ctx, int slot);
    /* Optional: latest lobby error code (need_players, missing_endpoints, …).
     * Cleared by the host after the UI reads it, or when a later op succeeds. */
    const char* (*last_error)(void* ctx);
    void (*clear_last_error)(void* ctx);
    /* Optional host waiting-room settings. input_delay is frames, clamped 2..20. */
    int  (*input_delay_get)(void* ctx);
    int  (*input_delay_set)(void* ctx, int delay_frames);
    /* Optional: host opt-in to server UDP input relay (0/1).
     * 3+ lobbies default to host-as-relay unless this is enabled. */
    int  (*force_input_relay_get)(void* ctx);
    int  (*force_input_relay_set)(void* ctx, int force);
    /* Optional: current room seat ceiling (2..RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS).
     * 0 when not in a lobby / unknown. Prefer this over game num_players. */
    int  (*lobby_max_slots)(void* ctx);
    /* Optional: host Force TURN / ICE relay-only (0/1). Server lobbies only;
     * published in match_caps.force_turn so every peer uses typ relay. */
    int  (*force_turn_get)(void* ctx);
    int  (*force_turn_set)(void* ctx, int force);
} RecompLauncherCNetplayCallbacks;

/* ---- schema-driven mods --------------------------------------------------
 * The host owns package parsing, persistence, dependency resolution, and
 * installation. recomp-ui only renders this stable query/mutation surface.
 * All strings are copied into fixed buffers so provider implementations may
 * rebuild their catalogs after any mutation without dangling UI pointers. */
#define RECOMP_LAUNCHER_MOD_ID_MAX 96
#define RECOMP_LAUNCHER_MOD_VALUE_MAX 128
#define RECOMP_LAUNCHER_MOD_AUTHOR_LINK_MAX 8

typedef struct RecompLauncherCModAuthorLink {
    char name[64];
    char url[256];
} RecompLauncherCModAuthorLink;

typedef enum RecompLauncherCModOptionType {
    RECOMP_MOD_OPTION_BOOLEAN = 0,
    RECOMP_MOD_OPTION_CHOICE = 1,
    RECOMP_MOD_OPTION_INTEGER = 2,
} RecompLauncherCModOptionType;

typedef struct RecompLauncherCModPackage {
    char id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char version[32];
    char name[128];
    char author[96];
    RecompLauncherCModAuthorLink author_links[RECOMP_LAUNCHER_MOD_AUTHOR_LINK_MAX];
    int  author_link_count;
    char description[512];
    char license[64];
    char source_name[128];
    char source_url[256];
    char status[256];
    int  enabled;
    int  option_count;
    int  removable;
    int  has_error;
} RecompLauncherCModPackage;

/* A package may contribute any number of independently configurable features.
 * Feature identity is the (package_id, id) pair; feature ids only need to be
 * unique within their owning package. */
typedef struct RecompLauncherCModFeature {
    char id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char package_id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char package_version[32];
    char package_name[128];
    char name[128];
    char author[96];
    RecompLauncherCModAuthorLink author_links[RECOMP_LAUNCHER_MOD_AUTHOR_LINK_MAX];
    int  author_link_count;
    char description[512];
    char source_name[128];
    char source_url[256];
    char group[96];
    char status[256];
    int  enabled;
    int  option_count;
    int  has_error;
    /* Feature exposes a live 3D camera. When enabled, the Controller page
     * conditionally presents camera bindings and the Mods detail links there.
     * Appended for ABI stability; zero keeps every existing feature unchanged. */
    int  camera_controls;
} RecompLauncherCModFeature;

typedef struct RecompLauncherCModOption {
    char id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char label[128];
    char description[512];
    char group[96];
    char value[RECOMP_LAUNCHER_MOD_VALUE_MAX];
    char default_value[RECOMP_LAUNCHER_MOD_VALUE_MAX];
    int  type;          /* RecompLauncherCModOptionType */
    int64_t min_value;
    int64_t max_value;
    int64_t step;
    int  choice_count;
    /* Non-zero when another option in the same feature currently overrides
     * this one (manifest key: disabled_by). The provider resolves it, so the
     * UI only has to grey the control out -- it never cross-references
     * options itself. Example: ticking "Instant" makes the speed box inert. */
    int  disabled;
} RecompLauncherCModOption;

typedef struct RecompLauncherCModChoice {
    char value[RECOMP_LAUNCHER_MOD_VALUE_MAX];
    char label[128];
} RecompLauncherCModChoice;

typedef struct RecompLauncherCModVersion {
    char version[32];
    int selected;
    int removable;
} RecompLauncherCModVersion;

typedef enum RecompLauncherCModDiagnosticSeverity {
    RECOMP_MOD_DIAGNOSTIC_INFO = 0,
    RECOMP_MOD_DIAGNOSTIC_WARNING = 1,
    RECOMP_MOD_DIAGNOSTIC_ERROR = 2,
} RecompLauncherCModDiagnosticSeverity;

typedef struct RecompLauncherCModDiagnostic {
    int  severity; /* RecompLauncherCModDiagnosticSeverity */
    char resource[192];
    char message[512];
    char related_package_id[RECOMP_LAUNCHER_MOD_ID_MAX];
    char related_feature_id[RECOMP_LAUNCHER_MOD_ID_MAX];
} RecompLauncherCModDiagnostic;

typedef struct RecompLauncherCModProvider {
    void* ctx;
    int (*package_count)(void* ctx);
    int (*package_get)(void* ctx, int index, RecompLauncherCModPackage* out);
    int (*option_get)(void* ctx, const char* package_id, int index,
                      RecompLauncherCModOption* out);
    int (*choice_get)(void* ctx, const char* package_id, const char* option_id,
                      int index, RecompLauncherCModChoice* out);
    int (*version_count)(void* ctx, const char* package_id);
    int (*version_get)(void* ctx, const char* package_id, int index,
                       RecompLauncherCModVersion* out);
    /* Mutations return 1 on success and 0 on failure. */
    int (*install_archive)(void* ctx, const char* archive_path);
    int (*remove_package)(void* ctx, const char* package_id, const char* version);
    int (*set_enabled)(void* ctx, const char* package_id, int enabled);
    int (*select_version)(void* ctx, const char* package_id, const char* version);
    int (*set_option)(void* ctx, const char* package_id, const char* option_id,
                      const char* value);
    /* Resolve and persist the staged selection. Called before PLAY commits.
     * image_path is the ROM/disc currently selected in the launcher. */
    int (*commit)(void* ctx, const char* image_path);
    const char* (*last_error)(void* ctx);
    /* Feature-oriented surface. Appended for ABI stability. New providers
     * should implement this complete group; package callbacks above remain the
     * secondary install/version/removal surface. Feature identity is always
     * (package_id, feature_id). */
    int (*feature_count)(void* ctx);
    int (*feature_get)(void* ctx, int index, RecompLauncherCModFeature* out);
    int (*feature_option_get)(void* ctx, const char* package_id,
                              const char* feature_id, int index,
                              RecompLauncherCModOption* out);
    int (*feature_choice_get)(void* ctx, const char* package_id,
                              const char* feature_id, const char* option_id,
                              int index, RecompLauncherCModChoice* out);
    int (*feature_enable)(void* ctx, const char* package_id,
                          const char* feature_id, int enabled);
    int (*feature_set_option)(void* ctx, const char* package_id,
                              const char* feature_id, const char* option_id,
                              const char* value);
    int (*diagnostic_count)(void* ctx, const char* package_id,
                            const char* feature_id);
    int (*diagnostic_get)(void* ctx, const char* package_id,
                          const char* feature_id, int index,
                          RecompLauncherCModDiagnostic* out);
    /* Optional package vocabulary. NULL keeps the historical PSX defaults.
     * extension includes its leading dot, e.g. ".gbamod". */
    const char* archive_extension;
    const char* archive_description;
    /* Optional. Netplay lobbies (hosted / LAN / direct) call this instead of
     * commit() so matches stay vanilla. Must clear any in-session mod plan
     * without mutating the user's persisted offline selection. NULL means
     * "skip commit entirely" (no mods applied for that launch). Appended for
     * ABI stability — zero-init leaves it NULL. */
    int (*commit_netplay)(void* ctx, const char* image_path);
} RecompLauncherCModProvider;

// Plain-C mirror of the launcher's internal settings (bools as int).
struct RecompLauncherCSettings {
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
    int  player_src[RECOMP_LAUNCHER_MAX_PLAYERS];  // 0 none, 1 keyboard, 2 gamepad
    int  deadzone[RECOMP_LAUNCHER_MAX_PLAYERS];    // 0..100
    int  skip_launcher;     // bool: boot straight to the game next time
    int  msu1_enabled;      // bool
    char msu1_dir[512];
    int  pad_mode[RECOMP_LAUNCHER_MAX_PLAYERS];    // per player: 0=Hybrid, 1=Analog(DualShock), 2=D-Pad(digital)
    int  aspect_index;      // 0 = 4:3, 1 = 16:9, 2 = 21:9

    // ---- deeper PSX-style settings (capability-gated; see RecompLauncherCGameInfo
    // has_* flags below — consoles that don't set the flags leave these unused) ----
    int  window_width;        // px window width (height follows aspect)
    int  renderer;            // 0 = software, 1 = OpenGL
    int  supersampling;       // 1..4
    int  antialiasing;        // MSAA sample count: 0 = off, else 2/4/8 (x). (A
                              // legacy on/off host may still write 0/1.)
    int  texture_filter;      // 0 = nearest, 1 = bilinear
    int  screen_kind;         // 0 raw, 1 CRT, 2 composite, 3 trinitron
    int  frame_interp;        // bool
    int  frame_interp_fps;    // 0=display, else 90/120/144/165/240
    int  spu_hq;              // bool
    int  auto_skip_fmv;       // bool
    int  turbo_loads;         // bool
    int  language_index;      // selected index into GameInfo.languages
    char bios_path[512];      // BIOS file path (empty = default)

    // ---- PSX-style memory-card save slots (SAVE_MEMCARD; see launcher_system.h
    // SaveSpec) — appended at the end to keep this struct additive/ABI-stable.
    // Per-slot card-image file path (empty = none picked yet), editable via the
    // Save panel's Browse/New controls; mirrors bios_path's pattern exactly.
    char memcard_path[2][512];
    // Per-slot enable/disable (mirrors the legacy PSX launcher's per-card
    // "Enabled" switch / SIO-port concept: a disabled slot reports no card
    // present). 0 = unset (host predates this field) -> the model defaults it
    // to enabled at init. Appended additively; see launcher_model_toggle_memcard().
    int  memcard_enabled[2];

    // ---- audio output device (GameInfo.audio_device_labels consoles) --------
    // The chosen device's display name as enumerated by the HOST (SDL device
    // names are stable across runs on the same machine, not across machines —
    // exactly the contract the N64 SS Anne launcher's launcher.cfg used).
    // "" = system default. Appended additively.
    char audio_device[128];

    // ---- N64 Transfer Pak slots (GameInfo.tpak_slots consoles) --------------
    // Per controller port: the GB cartridge ROM inserted into that port's
    // Transfer Pak, its battery-save file, and whether the pak responds at
    // all. Mirrors the SS Anne launcher's per-player card set (launcher.cfg
    // pN_rom/pN_save/pN_enabled). Empty rom path = no cartridge inserted.
    // tpak_enabled: 0 = unset (host predates the field / fresh config) -> the
    // model defaults a slot WITH a rom to enabled; use -1 for explicit off.
    char tpak_rom_path[RECOMP_LAUNCHER_MAX_TPAKS][512];
    char tpak_save_path[RECOMP_LAUNCHER_MAX_TPAKS][512];
    int  tpak_enabled[RECOMP_LAUNCHER_MAX_TPAKS];

    // ---- mouse controls (GameInfo.has_mouse_controls games; Snap) -----------
    // Opt-in mouse-aim for a keyboard-family source. Only meaningful for
    // player 0 and only when the game sets has_mouse_controls; every other
    // consumer leaves these zero (memset default) and is byte-for-byte
    // unaffected. Appended additively at the end to keep the struct ABI-stable.
    int   mouse_enabled;      // 1 = "Keyboard + Mouse" source (mouse-aim on),
                              // 0 = plain "Keyboard" (mouse off). 0 = also the
                              // unset default; the host seeds the real default.
    float mouse_sensitivity;  // aim rate per mouse-pixel; default 0.06 (host
                              // seeds it). Model clamps to [0.01, 0.50].
                              // 0 = unset -> the model seeds 0.06.
    int   mouse_invert_x;     // bool: invert horizontal mouse aim
    int   mouse_invert_y;     // bool: invert vertical mouse aim (Snap default 1)
    // Left/Right/Middle mouse button -> index into the active profile's
    // ControllerSpec.buttons[] (0..button_count-1), or -1 = none/unbound.
    // NOTE: 0 is a VALID index (the n64 profile's "A"), so 0 is NOT "unset"
    // here — the host seeds real defaults ({A, Z, none} = {0, 2, -1}).
    int   mouse_bind[3];
    // ---- NES-style settings (capability-gated; see has_integer_scale /
    // hdpack_supported below) — appended additively, same ABI convention. ----
    int  integer_scale;       // bool: snap the game image to integer multiples
    int  hdpack_enabled;      // bool: load a Mesen-format HD texture pack
    char hdpack_dir[512];     // folder containing the pack's hires.txt
    // ---- Genesis-style widescreen width (SystemProfile.video.widescreen_cells
    // consoles only) — how many extra 8-px background cells EACH SIDE renders
    // while `widescreen` is on. 0 = unset (host predates this field) -> the
    // model defaults it to 8, the Genesis engine default. Appended additively.
    int  widescreen_cells;    // 1..16

    // ---- live aspect-driven extended view ---------------------------------
    // In a window, the fixed aspect selects the initial size before live
    // resizing takes over. Adaptive + fullscreen ignores the fixed aspect.
    int  adaptive_view;       // bool: logical width follows host drawable aspect

    // ---- netplay launch result (capability-gated by GameInfo.netplay_supported)
    // player_name is persistent host-owned identity; netplay_launch is a
    // transient output and is cleared by the launcher when it initializes.
    char netplay_player_name[64];
    RecompLauncherCNetplayLaunch netplay_launch;

    // ---- per-player SDL gamepad GUID (player_src==2). Empty when none/keyboard.
    // Appended additively; see RECOMP_LAUNCHER_HAS_PLAYER_GAMEPAD_GUID. Hosts
    // persist these as pN_device in settings.toml so multi-pad assignments
    // survive relaunches (instance IDs do not).
    char player_gamepad_guid[RECOMP_LAUNCHER_MAX_PLAYERS][40];

    // ---- controller motion (GameInfo.has_gyro_controls games) ------------
    // Multiplier applied by the host to a controller's angular-rate sensor.
    // 0 means unset and is seeded to 1.0 by the launcher model.
    float gyro_sensitivity;    // 0.25..4.00, 1.00 = game default

    // ---- optional presentation enhancements (capability-gated) -----------
    // Appended additively so zero-initialized existing consumers keep their
    // current settings surface and behavior.
    int sharp_filter;           // integer prescale + fractional linear finish
    int affine_filter;          // selective game-authorized affine smoothing

    // ---- cartridge light sensor (GameInfo.has_solar_sensor games) ---------
    // Appended additively. A few GBA cartridges carry a photodiode the game
    // reads as gameplay input -- Boktai's Gun del Sol charges from real
    // sunlight -- so "how bright is it where the player is" is a launch
    // setting, not an emulator preference.
    //
    // solar_zip is a POSTAL CODE, deliberately text: many are not numeric
    // ("SW1A", "K1A"). Empty means the host must not consult any network.
    char solar_zip[16];
    char solar_country[8];      // Zippopotam-style code: us, ca, gb, de, ...
    int  solar_source;          // 0 = live local weather, 1 = fixed level
    int  solar_manual_step;     // 0..8, used when solar_source == 1
    int  solar_full_sun;        // W/m^2 that reads as full sun; 0 = host default

    // ---- multi-display layout --------------------------------------------
    // Index into GameInfo.display_layout_labels. This is intentionally
    // independent of aspect/widescreen: it describes physical host windows,
    // not how a game's camera is rendered inside one of them.
    int display_layout;
    // ---- PSX geometry-precision settings (capability-gated by
    // GameInfo.has_geometry_precision) — appended additively, same ABI
    // convention as every block above. ----
    // The PS1's GTE projects in 16.16 and then discards the fraction when it
    // saturates screen coordinates to whole pixels, so a moving mesh shimmers;
    // and the GPU interpolates UVs affinely, so large floor/wall textures swim.
    // These are the two opt-in corrections. Both are visual only — the
    // guest-visible GTE screen coordinates stay integer and fully faithful.
    // 0 = off (the faithful default on a fresh config).
    int  geometry_correction;    // bool: sub-pixel vertex precision
    int  perspective_texturing;  // bool: perspective-correct UVs
};

// ---- host verification/inspection results (filled by the callbacks below) ----
// Plain-C structs so a host can implement the callbacks with zero launcher
// internal types. Mirror what the legacy launcher computed inline.
typedef struct RecompLauncherCDiscVerify {
    char serial[16];   // e.g. "SCUS-94423"; "" = unknown/unread
    char region[8];    // e.g. "NTSC-U"; "" = unknown
    int  iso_ok;       // ISO9660 / system header present
    int  verdict;      // 0 none, 1 ok, 2 warn, 3 bad
} RecompLauncherCDiscVerify;

/* Host BIOS check for the first-run setup wizard (has_bios games). */
typedef struct RecompLauncherCBiosVerify {
    int  ok;           // 1 = usable BIOS present
    int  warn;         // 1 = size/CRC soft mismatch (still ok to boot)
    char detail[160];  // short status for the UI
} RecompLauncherCBiosVerify;

/* Optional progress callback for prepare_with_progress (worker thread).
 * pct is 0..1 when known; negative means indeterminate. message may be NULL. */
typedef void (*RecompLauncherCPrepareProgressFn)(void* ctx, float pct,
                                                 const char* message);

typedef struct RecompLauncherCMemcard {
    int           valid;          // 128 KB + "MC" magic present
    int           used_blocks;    // 0..15
    unsigned char block_used[15]; // per-block: 1 = occupied
} RecompLauncherCMemcard;

// One Transfer Pak slot's inspection result (filled by the tpak_inspect
// callback below). The HOST owns all cartridge knowledge — header sniffing,
// which Gen-1 charmap decodes the trainer name (ASCII for Stadium US, kana
// for Pocket Monsters Stadium J), what the cart is called on screen — so the
// launcher stays console-generic and just renders these facts.
typedef struct RecompLauncherCTpak {
    int  valid;             // recognized GB cartridge
    char cart_label[96];    // display name, UTF-8 (kana ok; "" => show the file name)
    char trainer_name[32];  // decoded save-file trainer name ("" = no/unreadable save)
    char trainer_id[16];    // decoded trainer ID, ready to display ("" = none)
    // Cartridge art tint drawn by the launcher's native cart glyph:
    // 0 unknown/other (gray), 1 red, 2 blue, 3 yellow, 4 green.
    int  cart_kind;
} RecompLauncherCTpak;

typedef struct RecompLauncherCGameInfo {
    const char*    name;
    const char*    region;
    uint32_t       expected_crc;
    int            has_expected_crc;
    const uint8_t (*known_sha256)[32];
    size_t         num_known_sha256;
    /* Accepted SHA-1 fingerprints as 40-char lowercase hex strings — the
     * identity cartridge consoles (GBA, SNES) actually gate on. The launcher
     * computes SHA-1 over the picked ROM and matches any entry, so its
     * "verified" check agrees with the game runtime's real gate. NULL/0 =>
     * no SHA-1 check. Preferred over expected_crc for those consoles (a
     * CRC32 is dump-specific; SHA-1 is the canonical ROM identity). */
    const char* const* known_sha1_hex;
    size_t         num_known_sha1;
    int            widescreen_supported;   /* hide Widescreen settings when 0 */
    /* How many players the GAME supports (1..RECOMP_LAUNCHER_MAX_PLAYERS),
     * additionally capped by the active console profile. The launcher hides
     * Player N+ rows when this is N — e.g. SMW Co-op is 2-player even though
     * the shared ABI can store 5. 0 means "unset" and is treated as 2 for
     * backward compatibility with callers that predate this field. */
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
    /* Keyboard-bind file path the Controller rebind page persists to. NULL
     * => "keybinds.ini" in cwd (exe-anchored), matching each runtime's own
     * default (recompui_keybinds_init(NULL) / psx_keybinds_init(NULL)) so the
     * launcher and the game agree on one file without a host having to set
     * this. The ON-DISK FORMAT is chosen automatically from the active
     * SystemProfile (launcher_system.h) — not from a separate flag here:
     * PSX games get psxrecomp's own psx_keybinds.c format (24 keys, section
     * [player1]/[player2], names up/down/.../rs_right) so rebinds actually
     * reach the game; every other console keeps this launcher's generic
     * keybinds.c format exactly as before. Pass a host-specific path only
     * when the game's cwd won't match the launcher's (e.g. a differently
     * anchored --keybinds override). */
    const char*    keybinds_path;

    // Controller pad-mode (PlayStation-style analog/digital emulation). Consoles
    // without pad modes (SNES) leave pad_mode_supported = 0 and the selector + the
    // analog/digital art are never shown (the generic pad.tga is used).
    int            pad_mode_supported;    // 0 = no pad-mode UI at all; 1 = show the selector + swapping art
    int            pad_mode_selectable;   // 0 = hide selector, force locked_pad_mode (game.lock_mode)
    int            locked_pad_mode;       // forced mode when !pad_mode_selectable
    int            lock_device;           // 1 = hide the player controller cards entirely (fixed pad)
    // Aspect ratios offered. bit0 = 4:3 (implied/always), bit1 = 16:9, bit2 = 21:9.
    // 0 = fall back to the legacy widescreen_supported bool (SNES: 16:9 toggle).
    int            aspect_mask;

    // ---- deeper PSX-style settings capability flags ----
    // 0 => that control is hidden entirely; SNES/other consoles that leave all
    // of these 0 keep exactly today's minimal settings surface.
    int  has_window_size;       // px window-size control (else the legacy window_scale cycle stays)
    int  has_renderer;          // Software/OpenGL toggle
    int  has_supersampling;
    int  has_antialiasing;
    int  has_texture_filter;    // Nearest/Bilinear (else the legacy Linear filtering checkbox stays)
    int  has_screen_kind;       // CRT/screen-model filter
    int  has_frame_interp;
    int  has_spu_hq;
    int  has_skip_fmv;          // Skip FMVs
    int  has_turbo_loads;
    int  has_fullscreen_toggle; // DEPRECATED, ignored: the Fullscreen row is universal
                                // (every console, tri-state 0 off/1 borderless/2 exclusive).
                                // Kept only for ABI layout compatibility.
    int  has_bios;              // BIOS path picker
    int  has_deadzone_pct;      // single analog-deadzone % control
    const char* rom_noun;       // "ROM" (default/NULL) | "Disc" | "Cartridge" — the Change-<noun>
                                 // button label + File row
    // Languages (Localization menu shown only when num_languages > 0).
    const char* const* language_labels;  // e.g. {"English","Japanese"}
    int  num_languages;

    // ---- host verification/inspection callbacks (optional; PSX uses them) ----
    // When set, the launcher shows REAL disc/memcard facts and RE-runs the
    // callback whenever the user changes the disc / a memory card (matching the
    // legacy launcher). NULL => the launcher falls back to a placeholder verdict
    // / empty card summary. `disc_verify` gets the current disc path; return 1
    // if `out` was filled. `memcard_inspect` gets one slot's card path; return
    // 1 if `out` was filled.
    int (*disc_verify)(const char* disc_path, RecompLauncherCDiscVerify* out);
    int (*memcard_inspect)(const char* card_path, RecompLauncherCMemcard* out);

    /* Box-art image path relative to the assets dir. NULL/"" => the default
     * "assets/img/boxart.tga". Multi-variant repos whose variants share one
     * build dir stage one file per variant (e.g. "assets/img/boxart_firered
     * .tga") and point each exe's GameInfo here. */
    const char* boxart_path;

    /* Game-supplied aspect vocabulary: overrides the built-in PSX-style
     * 4:3/16:9/21:9 set. Settings.aspect_index cycles 0..num_aspect_labels-1;
     * index 0 should be the native aspect. The host maps the committed index
     * onto its own render parameter (e.g. gbarecomp --view-width).
     * aspect_experimental=1 draws the amber EXPERIMENTAL tag next to the
     * cycle (the snesrecomp/psxrecomp widescreen convention for per-game
     * enhancement surfaces that are still maturing). */
    const char* const* aspect_labels;
    int  num_aspect_labels;
    int  aspect_experimental;

    /* ---- audio output device picker (N64/RT64 hosts) --------------------
     * When num_audio_devices > 0, Settings->Audio grows an "Output device"
     * dropdown over these HOST-enumerated display names (the host queries
     * SDL_GetAudioDeviceName itself, pre-launcher, exactly as the SS Anne
     * launcher did). The pick round-trips through Settings.audio_device by
     * NAME; a "(system default)" row is always offered first and commits "".
     * NULL/0 => no device row (every existing console unchanged). */
    const char* const* audio_device_labels;
    int  num_audio_devices;

    /* ---- renderer vocabulary override ------------------------------------
     * When set, the has_renderer cycle walks these 0..num_renderers-1 labels
     * instead of the built-in Software/OpenGL pair — e.g. the RT64 hosts'
     * {"Auto","Vulkan","D3D12"} graphics-API pick. Settings.renderer holds
     * the committed index. NULL/0 => the legacy 2-value toggle. */
    const char* const* renderer_labels;
    int  num_renderers;

    /* ---- N64 Transfer Pak (dashboard "tpak" panel) ------------------------
     * tpak_slots (0..4): how many controller ports offer a Transfer Pak GB
     * cartridge card. 0 => the panel never composes (Snap, Pikachu). Stadium
     * passes 4. The launcher edits Settings.tpak_* (ROM/save paths + enabled)
     * and calls tpak_inspect — the HOST's cartridge brain — on every change
     * to refresh the card's label/trainer/tint facts. tpak_inspect may be
     * NULL: cards then show file names with the neutral tint. */
    int  tpak_slots;
    int (*tpak_inspect)(const char* rom_path, const char* save_path,
                        RecompLauncherCTpak* out);

    /* ---- rebind-page opt-out ---------------------------------------------
     * 1 = hide the keyboard/controller bindings grid on the Configure page
     * (input source + deadzone remain). For games whose runtime consumes no
     * bind file at all (PMS-J today) an editor that writes a file nothing
     * reads would be a lying UI. 0 (default/memset) keeps the grid. */
    int  hide_rebind;

    /* ---- mouse controls (opt-in; Pokemon Snap) ---------------------------
     * 1 = this game supports mouse-aim: the input-source dropdown grows a
     * "Keyboard + Mouse" entry (the keyboard source with mouse-aim on) beside
     * the plain "Keyboard" one, and a "MOUSE" card (sensitivity / invert /
     * three rebindable mouse buttons) appears on the Controller page whenever
     * a keyboard-family source is selected. Drives Settings.mouse_* above.
     * 0 (default/memset) => none of that surface exists and every non-mouse
     * consumer (SNES/PSX/GBA/PSR/PMS-J) is byte-for-byte unchanged. */
    int  has_mouse_controls;
    // ---- NES-style capability flags (appended additively) ----
    int  has_integer_scale;   // Integer-scale checkbox in Display settings
    // HD texture packs (Mesen hires.txt format): 1 shows the HD-pack toggle +
    // folder picker in Display settings (NES defaults this ON per game; a
    // stock build that must not load packs passes 0 — e.g. unpatched Zelda).
    int  hdpack_supported;
    // Password/mantra save (e.g. Faxanadu): when password_save_path is
    // non-NULL the SAVES row shows the password text (read-only, editable
    // behind an Edit + confirm step) instead of the binary SRAM file UI.
    // The file is a single line of text. Independent of sram_path.
    const char* password_save_path;   // abs path to the 1-line password file
    const char* password_save_label;  // row label, e.g. "Password" / "Mantra"
    // Light-gun (NES Zapper) game: the controller config page shows a Zapper
    // block (mouse-as-gun + crosshair toggles, persisted to the engine's
    // keybinds.ini [zapper] section) alongside the pad UI.
    int  zapper;

    // Cartridge light sensor: 1 adds a Solar sensor panel to Settings, where
    // the player sets the location its brightness is read from. Games without
    // the hardware pass 0 and the panel never composes, so every existing
    // consumer is byte-for-byte unchanged.
    int  has_solar_sensor;

    // Live aspect-driven view capability. When present, Display settings show
    // an Adaptive view toggle. Adaptive + fullscreen leaves the fixed aspect
    // control visible but disabled because the display chooses the live width.
    int  adaptive_view_supported;

    // Netplay is a title/developer capability, not a user setting. When set,
    // the dashboard exposes lobby host/join controls through host-owned
    // callbacks.
    int netplay_supported;
    const RecompLauncherCNetplayCallbacks* netplay;
    /* Soft-return from a netplay match: open Netplay + LOBBY room if still
     * seated (WS or LAN). Optional resume_netplay_endpoint is "ip:port" for
     * LAN room header (NULL/empty => online Lobby Server URL). */
    int resume_netplay_room;
    const char* resume_netplay_endpoint;

    /* ---- first-run setup wizard -------------------------------------------
     * When needs_setup is 1, OR the launcher detects a missing ROM/disc (and
     * missing BIOS when has_bios), a blocking setup modal opens before the
     * dashboard. Cart-only games (has_bios=0) only prompt for a ROM.
     *
     * bios_verify (optional): host checks BIOS size/CRC. Return 1 and fill
     * `out` (ok/warn/detail). Called with an empty path when the player has
     * not chosen a dump — host should accept that when a bundled BIOS
     * (e.g. OpenBIOS) is available, or set ok=0 when a retail dump is
     * required. NULL => empty path = bundled OK; non-empty path must exist.
     *
     * prepare_disc (optional): convert a raw dump into a playable image.
     * Blocking host callback; the UI shows a busy state while it runs.
     * Return 1 and write the playable .cue/.bin/.img/.iso/.car path into out_disc_path.
     * prepare_disc_label / prepare_disc_note are button + help text (NULL =>
     * "Convert raw dump…" / default note).
     *
     * Path persistence (Continue to launcher / Change ROM / BIOS browse):
     * The launcher writes `rom_cache_path` (NULL => "rom.cfg") immediately so
     * quitting without PLAY still remembers the ROM. Optional persist_setup
     * lets the host also flush BIOS / config.ini (return 0 on success). */
    int needs_setup;
    int (*bios_verify)(const char* bios_path, RecompLauncherCBiosVerify* out);
    int (*prepare_disc)(const char* source_path, char* out_disc_path, size_t out_cap,
                        char* err_msg, size_t err_cap);
    const char* prepare_disc_label;
    const char* prepare_disc_note;
    const char* rom_cache_path; /* NULL => "rom.cfg" next to cwd/exe */
    int (*persist_setup)(void* ctx, const char* rom_path, const char* bios_path);
    void* persist_setup_ctx;

    /* Optional schema-driven mod provider. Appended for ABI stability. The
     * Mods view requires both RECOMP_UI_ENABLE_MODS=1 and a non-NULL provider;
     * default builds therefore remain inert even if a caller populates this
     * field. Features are the primary user-facing surface; packages are the
     * secondary installation/maintenance surface. */
    const RecompLauncherCModProvider* mods;

    /* ---- controller motion ----------------------------------------------
     * 1 adds a MOTION card to the Controller configuration page with a gyro
     * sensitivity slider. The launcher only edits Settings.gyro_sensitivity;
     * discovery, sensor selection, and axis mapping remain host-owned. */
    int has_gyro_controls;

    /* Add independent checkboxes to Display settings. The host maps their
     * committed Settings values onto renderer configuration. */
    int has_sharp_filter;
    int has_affine_filter;

    /* ---- multi-display layout -------------------------------------------
     * Optional host-defined Display row. Settings.display_layout cycles over
     * these labels. Nintendo DS uses {"Stacked window","Separate windows"}.
     * NULL/0 keeps every existing single-display launcher unchanged. */
    const char* const* display_layout_labels;
    int num_display_layouts;

    /* ---- prepare job UX (appended; disc convert / local codegen) ---------
     * prepare_use_selected_rom: 1 = the prepare button uses the already-
     * picked ROM/disc (no second file picker). Cart codegen hosts use this.
     * prepare_section_title / prepare_busy_status / prepare_success_status
     * override the default "Convert raw dump…" copy when non-NULL.
     *
     * prepare_with_progress: when non-NULL, preferred over prepare_disc.
     * Same success contract (return 1 + out_path); may invoke on_progress
     * from the worker thread. Zero-init leaves legacy prepare_disc behavior. */
    int prepare_use_selected_rom;
    const char* prepare_section_title;
    const char* prepare_busy_status;
    const char* prepare_success_status;
    int (*prepare_with_progress)(const char* source_path,
                                 char* out_path, size_t out_cap,
                                 char* err_msg, size_t err_cap,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx);

    /* ---- rebuild + relaunch after prepare (local codegen hosts) ----------
     * rebuild_with_progress: compile the project after sources are generated.
     * Return 1 and write the new/updated executable path into out_exe_path.
     * rebuild_after_prepare: when 1 and rebuild_with_progress is set, the
     * setup wizard auto-starts rebuild after a successful prepare.
     * relaunch_after_rebuild: when 1, a successful rebuild makes
     * recomp_launcher_run_window return RECOMP_LAUNCHER_RESULT_RELAUNCH (3);
     * call recomp_launcher_relaunch_exe() for the path to exec. */
    int (*rebuild_with_progress)(const char* rom_path,
                                 char* out_exe_path, size_t out_cap,
                                 char* err_msg, size_t err_cap,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx);
    int rebuild_after_prepare;
    int relaunch_after_rebuild;
    const char* rebuild_busy_status;     /* NULL => "Building game…" */
    const char* rebuild_success_status;  /* NULL => "Build complete." */

    /* When 1, the setup modal hides "Continue to launcher" and requires
     * prepare (and rebuild when rebuild_after_prepare is set). Local codegen
     * first-run: Generate & rebuild, then relaunch — Quit is the only other exit. */
    int prepare_required_before_continue;

    /* ---- portable toolchain step (appended; local codegen hosts) ----------
     * When setup_needs_toolchain is 1, the first-run wizard shows a page to
     * download cmake-clang-v1 or pick an offline zip before BIOS/ROM/generate.
     * toolchain_is_ready: optional quick check (non-NULL + returns 1 => skip
     * page 0). ensure_toolchain_with_progress: download (download!=0) or install
     * from zip_path (non-empty); download==0 and empty zip = resolve cache only. */
    int setup_needs_toolchain;
    int (*toolchain_is_ready)(void);
    int (*ensure_toolchain_with_progress)(
        int download, const char* zip_path, char* err_msg, size_t err_cap,
        RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx);
    /* PSX geometry-precision controls (Settings.geometry_correction /
     * perspective_texturing). 0 => no row drawn, so every console that leaves
     * this unset keeps exactly today's settings surface. One flag still gates
     * both settings because they are two halves of the same enhancement, but
     * only perspective_texturing currently draws a control: geometry_correction
     * is known to crack meshes at the coverage the runtime can achieve and is
     * withdrawn from the UI while staying readable from game.toml/settings.toml
     * (psxrecomp ENHANCEMENTS.md G1.8/G1.9). Appended for ABI stability. */
    int  has_geometry_precision;
} RecompLauncherCGameInfo;

/* recomp_launcher_run_window return codes */
#define RECOMP_LAUNCHER_RESULT_LAUNCH       0
#define RECOMP_LAUNCHER_RESULT_QUIT         1
#define RECOMP_LAUNCHER_RESULT_UNAVAILABLE  2
#define RECOMP_LAUNCHER_RESULT_RELAUNCH     3

// Returns: 0 = LAUNCH (boot out_rom_path with the edited *io),
//          1 = QUIT (caller should exit),
//          2 = UNAVAILABLE (assets/GL failed — caller boots as if skipped),
//          3 = RELAUNCH (host should exec recomp_launcher_relaunch_exe()).
int recomp_launcher_run_window(const char* window_title,
                             RecompLauncherCSettings* io,
                             const RecompLauncherCGameInfo* game,
                             const char* assets_dir,
                             const char* initial_rom,
                             char* out_rom_path, size_t out_rom_path_len);

/* Valid after run_window returns RECOMP_LAUNCHER_RESULT_RELAUNCH. Copies the
 * executable path produced by rebuild_with_progress. Returns 1 on success. */
int recomp_launcher_relaunch_exe(char* out, size_t out_cap);

/* When preserve != 0, the launcher tears down its window/GL context but does
 * NOT call SDL_Quit(), so an in-process host can keep SDL subsystems across
 * launcher → game (and rematch soft-return). Default is 0 (full Quit). */
void recomp_launcher_set_preserve_sdl(int preserve);

#ifdef __cplusplus
}
#endif

#endif // RECOMP_LAUNCHER_H
