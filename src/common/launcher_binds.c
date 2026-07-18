// launcher_binds.c — real binding persistence (keybinds.ini + config.ini [KeyMap]).

#include "launcher_binds.h"
#include "launcher_sdlcompat.h"   // SDL header (2 or 3)
#include "keybinds.h"             // engine keyboard-binding store
#include "launcher_system.h"      // SystemProfile / ControllerSpec.button_count
#include "consoles/psx/psx_binds.h"   // PSX-native keybind bridge (psx_keybinds.c format)
#include "consoles/n64/n64_binds.h"   // N64-native input.cfg bridge (kb+pad tables)
#include "consoles/nes/nes_binds.h"   // NES-native keybind bridge (nesrecomp keybinds.c format)
#include "consoles/genesis/genesis_binds.h"   // Genesis-native bridge (settings.ini [input.pN])

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MSVC spells these differently than POSIX.
#ifdef _MSC_VER
  #define strtok_r     strtok_s
  #define strncasecmp  _strnicmp
  #define strdup       _strdup
#endif

const char* g_launcher_config_path = NULL;
const char* g_launcher_keybinds_path = NULL;

// Per-system rebind-spec index -> keybinds.c button index. keybinds order is
// a,b,x,y,l,r,start,select,up,down,left,right,l2,r2,l3,r3 (see keybinds.h).
//
// SNES rebind spec order (launcher_system.h kSnesPadButtons): Up,Down,Left,
// Right,A,B,X,Y,L,R,Start,Select. UNCHANGED from before per-system vocab —
// SNES bind persistence stays byte-identical (indices 0..11, same mapping).
//
// PSX no longer routes through this generic table/keybinds.c at all — see
// consoles/psx/psx_binds.c, which persists through psxrecomp's own
// psx_keybinds.c format instead so rebinds actually reach the game (routing
// PSX's 24 buttons through this 16-slot generic format silently discarded
// the 8 stick-direction binds and, worse, wrote a file the game's runtime
// can't parse at all). Every profile that still uses this table today
// borrows kSnesPadButtons (launcher_system.h stub macro), so it always
// matches LNG_SNES_PAD_BUTTON_COUNT.
static const int kKbIndexSnes[LNG_SNES_PAD_BUTTON_COUNT] = {
    /* UP    */ 8, /* DOWN  */ 9, /* LEFT  */ 10, /* RIGHT */ 11,
    /* A     */ 0, /* B     */ 1, /* X     */ 2,  /* Y     */ 3,
    /* L     */ 4, /* R     */ 5, /* START */ 6,  /* SELECT*/ 7,
};

// Resolve the keybinds-index table (and its length) for the model's ACTIVE
// SystemProfile. PSX is handled entirely by the native bridge below (see
// is_psx_profile()) before this is ever consulted; GBA persists through the
// generic store with its own 10-button vocabulary (kGbaKbIndex,
// consoles/gba/gba_profile.h); everything else is SNES-shaped (12 buttons).
static const int* active_kb_index(const LauncherModel* m, int* out_n) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    if (prof && prof->id && !strcmp(prof->id, "gba")) {
        *out_n = LNG_GBA_PAD_BUTTON_COUNT;
        return kGbaKbIndex;
    }
    *out_n = LNG_SNES_PAD_BUTTON_COUNT;
    return kKbIndexSnes;
}

// ---- PSX-native keybind bridge ---------------------------------------------
// Lives in consoles/psx/psx_binds.c (the console's own unit) — for a PSX
// SystemProfile, persistence routes through psxrecomp's native 24-scancode
// psx_keybinds.c format instead of the generic keybinds.c store, so rebinds
// actually reach the game. This file only decides WHICH store to use (by the
// active profile) and resolves the file path.
static int is_psx_profile(const LauncherModel* m) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    return prof && prof->id && !strcmp(prof->id, "psx");
}

// NES routes to its own native bridge too (consoles/nes/nes_binds.c): the
// nesrecomp runner's keybinds.ini format has 8 NES-named keys plus [zapper]
// and [gamepad1]/[gamepad2] sections the generic store's whole-file rewrite
// would destroy.
static int is_nes_profile(const LauncherModel* m) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    return prof && prof->id && !strcmp(prof->id, "nes");
}

static const char* keybinds_file_path(void) {
    return (g_launcher_keybinds_path && g_launcher_keybinds_path[0])
             ? g_launcher_keybinds_path : "keybinds.ini";
}

// ---- N64-native input.cfg bridge --------------------------------------------
// Lives in consoles/n64/n64_binds.c. The N64 runners persist bindings in
// their own input.cfg format — TWO device-type tables (keyboard, controller —
// shared by all pads, NOT per-port) with two alternate slots per input, where
// a controller bind can be a pad button, a signed pad axis, or a raw joystick
// field. This file only decides WHICH store to use and, for N64, WHICH device
// table a player edits: the one their current input source selects.
static int is_n64_profile(const LauncherModel* m) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    return prof && prof->id && !strcmp(prof->id, "n64");
}

static const char* n64_binds_file_path(void) {
    return (g_launcher_keybinds_path && g_launcher_keybinds_path[0])
             ? g_launcher_keybinds_path : "input.cfg";
}

// Device table (0 kb / 1 pad) player p's Configure page edits.
static int n64_device_for_player(const LauncherModel* m, int player /*0-based*/) {
    return m->s.player_src[player] == 2 ? 1 : 0;
}

// ---- Genesis-native bind bridge ----------------------------------------------
// Lives in consoles/genesis/genesis_binds.c — for a Genesis SystemProfile,
// persistence routes through segagenesisrecomp's own settings.ini
// [input.pN] key.<Name>/pad.<Name> format (runner/app_config.c) instead of
// the generic keybinds.c store, so rebinds actually reach the game's
// g_input_map. Note the DIFFERENT default filename: the Genesis engine's
// bind store is settings.ini, not keybinds.ini.
static int is_genesis_profile(const LauncherModel* m) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    return prof && prof->id && !strcmp(prof->id, "genesis");
}

static const char* genesis_binds_file_path(void) {
    return (g_launcher_keybinds_path && g_launcher_keybinds_path[0])
             ? g_launcher_keybinds_path : "settings.ini";
}

// The engine's config.ini [KeyMap] keys, in LngHotkey order.
static const char* kHotkeyKey[LNG_HK_COUNT] = {
    "Fullscreen", "Reset", "Pause", "PauseDimmed", "Turbo",
    "WindowBigger", "WindowSmaller", "VolumeUp", "VolumeDown",
    "DisplayPerf", "ToggleRenderer"
};
// Built-in defaults (shown when config.ini has no line; "" = unbound).
static const char* kHotkeyDef[LNG_HK_COUNT] = {
    "Alt+Return", "Ctrl+R", "Shift+P", "P", "Tab",
    "", "", "", "", "F", "R"
};

static void copy_str(char* d, size_t cap, const char* s) {
    if (!d || !cap) return;
    if (!s) { d[0] = 0; return; }
    size_t n = strlen(s); if (n >= cap) n = cap - 1;
    memcpy(d, s, n); d[n] = 0;
}

static const char* scancode_label(SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_UNKNOWN) return "(unbound)";
    const char* n = SDL_GetScancodeName(sc);
    return (n && n[0]) ? n : "(unbound)";
}

// Display label for a Genesis gamepad bind: SDL's own controller button/axis
// names ("dpup", "a", "leftshoulder"; axes get a direction suffix, "leftx+").
static void genesis_pad_label(int kind, int code, int axis_dir, char* out, size_t cap) {
    if (kind == RUI_GEN_BIND_BUTTON) {
        const char* n = SDL_GetGamepadStringForButton((LNG_GamepadButton)code);
        copy_str(out, cap, (n && n[0]) ? n : "(unbound)");
    } else if (kind == RUI_GEN_BIND_AXIS) {
        const char* n = SDL_GetGamepadStringForAxis(code);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%c", (n && n[0]) ? n : "axis",
                 axis_dir < 0 ? '-' : '+');
        copy_str(out, cap, buf);
    } else {
        copy_str(out, cap, "(unbound)");
    }
}

static void reload_player_display(LauncherModel* m, int player) {
    if (is_psx_profile(m)) {
        if (player > 2) return;   // PSX store is 2-player
        for (int b = 0; b < LNG_PSX_PAD_BUTTON_COUNT; ++b)
            copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                     scancode_label((SDL_Scancode)rui_psx_binds_get(
                         keybinds_file_path(), player - 1, b)));
        return;
    }
    if (is_nes_profile(m)) {
        for (int b = 0; b < LNG_NES_PAD_BUTTON_COUNT; ++b)
            copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                     scancode_label((SDL_Scancode)rui_nes_binds_get(
                         keybinds_file_path(), player - 1, b)));
        return;
    }
    if (is_genesis_profile(m)) {
        const char* path = genesis_binds_file_path();
        for (int b = 0; b < LNG_GENESIS_PAD_BUTTON_COUNT; ++b) {
            copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                     scancode_label((SDL_Scancode)rui_genesis_binds_get_key(path, player - 1, b)));
            int kind = 0, code = 0, dir = 0;
            rui_genesis_binds_get_pad(path, player - 1, b, &kind, &code, &dir);
            genesis_pad_label(kind, code, dir,
                              m->pad_binds[player - 1][b], sizeof(m->pad_binds[player - 1][b]));
        }
        return;
    }
    if (is_n64_profile(m)) {
        // Per-device-TYPE tables: every player assigned the same device kind
        // shows (and edits) the same table — exactly the SS Anne contract.
        const int dev = n64_device_for_player(m, player - 1);
        for (int b = 0; b < LNG_N64_PAD_BUTTON_COUNT; ++b) {
            int type = 0, id = -1;
            rui_n64_binds_get(n64_binds_file_path(), dev, b, 0, &type, &id);
            rui_n64_binds_label(type, id, m->binds[player - 1][b],
                                sizeof(m->binds[player - 1][b]));
            rui_n64_binds_get(n64_binds_file_path(), dev, b, 1, &type, &id);
            rui_n64_binds_label(type, id, m->binds_alt[player - 1][b],
                                sizeof(m->binds_alt[player - 1][b]));
        }
        return;
    }
    if (player > 2) return;   // generic keybinds.c store is 2-player
    int n = 0;
    const int* kb_index = active_kb_index(m, &n);
    for (int b = 0; b < n; ++b) {
        SDL_Scancode sc = recompui_keybinds_get_button(player, kb_index[b]);
        copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]), scancode_label(sc));
    }
}

// ---- config.ini [KeyMap] surgical read/write (ported from the RmlUi launcher) --

static int ieq(const char* a, size_t alen, const char* b) {
    size_t bl = strlen(b);
    if (alen != bl) return 0;
    for (size_t i = 0; i < alen; ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static char* read_whole(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1);
    if (buf) { *out_len = (long)fread(buf, 1, (size_t)n, f); buf[*out_len] = 0; }
    fclose(f);
    return buf;
}

static const char* config_path(void) {
    return (g_launcher_config_path && g_launcher_config_path[0])
             ? g_launcher_config_path : "config.ini";
}

// Fill m->hotkeys[] from config.ini [KeyMap] (or defaults where absent).
static void reload_hotkey_display(LauncherModel* m) {
    for (int h = 0; h < LNG_HK_COUNT; ++h)
        copy_str(m->hotkeys[h], sizeof(m->hotkeys[h]), kHotkeyDef[h]);

    long len = 0; char* text = read_whole(config_path(), &len);
    if (!text) return;

    int in_keymap = 0;
    char* save = NULL;
    for (char* line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        size_t l = strlen(p);
        while (l && (p[l-1] == '\r' || p[l-1] == ' ' || p[l-1] == '\t')) p[--l] = 0;
        if (!*p || *p == '#') continue;
        if (*p == '[') {
            char* close = strchr(p, ']');
            size_t sl = close ? (size_t)(close - p - 1) : strlen(p + 1);
            in_keymap = ieq(p + 1, sl, "KeyMap");
            continue;
        }
        if (!in_keymap) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        char* ke = eq; while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) --ke;
        size_t klen = (size_t)(ke - p);
        char* v = eq + 1; while (*v == ' ' || *v == '\t') ++v;
        char* hash = strchr(v, '#'); if (hash) *hash = 0;
        size_t vl = strlen(v); while (vl && (v[vl-1] == ' ' || v[vl-1] == '\t')) v[--vl] = 0;
        for (int h = 0; h < LNG_HK_COUNT; ++h)
            if (ieq(p, klen, kHotkeyKey[h])) { copy_str(m->hotkeys[h], sizeof(m->hotkeys[h]), v); break; }
    }
    free(text);
}

// Does `line` (leading ws / optional '#') assign `key`? Mirrors config.c.
static int line_is_key(const char* line, const char* key) {
    const char* i = line;
    while (*i == ' ' || *i == '\t') ++i;
    if (*i == '#') { ++i; while (*i == ' ' || *i == '\t') ++i; }
    size_t kl = strlen(key);
    if (strncasecmp(i, key, kl) != 0) return 0;
    i += kl;
    while (*i == ' ' || *i == '\t') ++i;
    return *i == '=';
}

// Surgically set "Key = value" inside [section] of `path`, preserving every
// other line (comments, blank lines, and unrelated sections — e.g. the NES
// keybinds.ini's [zapper]/[gamepad1]/[gamepad2] sections survive untouched).
// Creates the file and/or section when absent. Exported (launcher_binds.h)
// for console units whose native bind files carry sections the launcher
// doesn't own (consoles/nes/nes_binds.c).
void launcher_ini_kv_write(const char* path, const char* section,
                           const char* key, const char* value) {
    long len = 0; char* text = read_whole(path, &len);
    /* split into a growable line array, PRESERVING blank lines (strtok would
     * collapse them, losing the user's config formatting). */
    int cap = 64, n = 0;
    char** lines = (char**)malloc(sizeof(char*) * cap);
    if (text) {
        char* start = text;
        for (long i = 0; i <= len; ++i) {
            if (i == len || text[i] == '\n') {
                char* end = text + i;
                if (end > start && end[-1] == '\r') end[-1] = 0;
                else if (i < len) text[i] = 0;
                else text[i] = 0;
                if (i == len && start == text + len) break;  // no trailing empty
                if (n == cap) { cap *= 2; lines = (char**)realloc(lines, sizeof(char*) * cap); }
                lines[n++] = strdup(start);
                start = text + i + 1;
            }
        }
    }
    char assign[128];
    snprintf(assign, sizeof(assign), "%s = %s", key, value ? value : "");

    /* locate [section] body [start,end) */
    int ks = -1, ke = -1;
    for (int i = 0; i < n; ++i) {
        const char* p = lines[i]; while (*p == ' ' || *p == '\t') ++p;
        if (*p != '[') continue;
        const char* close = strchr(p, ']');
        size_t sl = close ? (size_t)(close - p - 1) : strlen(p + 1);
        if (ks >= 0) { ke = i; break; }
        if (ieq(p + 1, sl, section)) ks = i + 1;
    }
    if (ks >= 0 && ke < 0) ke = n;

    if (ks < 0) {
        char header[96];
        snprintf(header, sizeof(header), "[%s]", section);
        if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        if (n && lines[n-1][0]) lines[n++] = strdup("");
        if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        lines[n++] = strdup(header);
        if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        lines[n++] = strdup(assign);
    } else {
        int hit = -1;
        for (int i = ks; i < ke; ++i) if (line_is_key(lines[i], key)) { hit = i; break; }
        if (hit >= 0) { free(lines[hit]); lines[hit] = strdup(assign); }
        else {
            int at = ke;
            while (at > ks) { const char* p = lines[at-1]; while (*p==' '||*p=='\t') ++p; if (*p) break; --at; }
            if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
            for (int i = n; i > at; --i) lines[i] = lines[i-1];
            lines[at] = strdup(assign); ++n;
        }
    }

    FILE* f = fopen(path, "wb");
    if (f) { for (int i = 0; i < n; ++i) { fputs(lines[i], f); fputc('\n', f); } fclose(f); }
    for (int i = 0; i < n; ++i) free(lines[i]);
    free(lines); free(text);
}

// Original config.ini [KeyMap] entry point, now a thin wrapper.
static void keymap_write(const char* key, const char* value) {
    launcher_ini_kv_write(config_path(), "KeyMap", key, value);
}

// Format SDL keycode + mods the way config.c's ParseKeyArray reads back.
static void format_hotkey(int keycode, int kmod, char* out, size_t cap) {
    out[0] = 0;
    if (keycode == 0) return;              // unbound
    char buf[96]; buf[0] = 0;
    if (kmod & SDL_KMOD_CTRL)  strncat(buf, "Ctrl+",  sizeof(buf)-strlen(buf)-1);
    if (kmod & SDL_KMOD_ALT)   strncat(buf, "Alt+",   sizeof(buf)-strlen(buf)-1);
    if (kmod & SDL_KMOD_SHIFT) strncat(buf, "Shift+", sizeof(buf)-strlen(buf)-1);
    const char* kn = SDL_GetKeyName((SDL_Keycode)keycode);
    if (!kn || !kn[0]) return;
    strncat(buf, kn, sizeof(buf)-strlen(buf)-1);
    copy_str(out, cap, buf);
}

// ---- public API -------------------------------------------------------------

void launcher_binds_load(LauncherModel* m, const char* config_path_in, const char* keybinds_path_in) {
    g_launcher_config_path = config_path_in;
    g_launcher_keybinds_path = keybinds_path_in;
    if (is_psx_profile(m)) {
        rui_psx_binds_init(keybinds_file_path());   // load/generate psx_keybinds.c-format keybinds.ini
    } else if (is_n64_profile(m)) {
        rui_n64_binds_init(n64_binds_file_path());      // load input.cfg (defaults if absent; never seeds the file)
    } else if (is_nes_profile(m)) {
        rui_nes_binds_init(keybinds_file_path());   // load/generate nesrecomp-format keybinds.ini
        // Zapper switches live in the same file ([zapper]); surface them on
        // the model for the controller page's Zapper block.
        int zm = 1, zc = 1;
        rui_nes_zapper_get(keybinds_file_path(), &zm, &zc);
        m->zapper_mouse     = zm != 0;
        m->zapper_crosshair = zc != 0;
    } else if (is_genesis_profile(m)) {
        rui_genesis_binds_init(genesis_binds_file_path());   // overlay settings.ini [input.pN] onto engine defaults
    } else {
        recompui_keybinds_init(NULL);              // load/generate keybinds.ini (exe-anchored)
    }
    launcher_binds_refresh(m);
    reload_hotkey_display(m);
}

void launcher_binds_refresh(LauncherModel* m) {
    for (int p = 1; p <= LNG_MAX_PLAYERS; ++p)
        reload_player_display(m, p);
}

int launcher_binds_wants_pad_capture(const LauncherModel* m, int player) {
    if (player < 1 || player > LNG_MAX_PLAYERS) return 0;
    return is_n64_profile(m) && n64_device_for_player(m, player - 1) == 1;
}

// Persist the Zapper switches to keybinds.ini [zapper] (surgical: the rest of
// the file — player binds, gamepad sections, comments — is preserved).
// Called by launcher_model_toggle_zapper_* on every flip.
void launcher_binds_set_zapper(int mouse_enabled, int crosshair) {
    rui_nes_zapper_set(keybinds_file_path(), mouse_enabled, crosshair);
}

void launcher_binds_set_button(LauncherModel* m, int player, int b, int scancode) {
    if (is_n64_profile(m)) {
        // Keyboard capture into the N64 store routes through the field API
        // (slot 0) so there is exactly one write path for that store.
        launcher_binds_set_field(m, player, b, 0, RUI_N64_FIELD_KEY, scancode);
        return;
    }
    if (player < 1 || player > 2) return;
    if (is_psx_profile(m)) {
        if (b < 0 || b >= LNG_PSX_PAD_BUTTON_COUNT) return;
        rui_psx_binds_set(keybinds_file_path(), player - 1, b, scancode);
        copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                 scancode_label((SDL_Scancode)scancode));
        return;
    }
    if (is_nes_profile(m)) {
        if (b < 0 || b >= LNG_NES_PAD_BUTTON_COUNT) return;
        rui_nes_binds_set(keybinds_file_path(), player - 1, b, scancode);
        copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                 scancode_label((SDL_Scancode)scancode));
        return;
    }
    if (is_genesis_profile(m)) {
        if (b < 0 || b >= LNG_GENESIS_PAD_BUTTON_COUNT) return;
        rui_genesis_binds_set_key(genesis_binds_file_path(), player - 1, b, scancode);
        copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                 scancode_label((SDL_Scancode)scancode));
        return;
    }
    int n = 0;
    const int* kb_index = active_kb_index(m, &n);
    if (b < 0 || b >= n) return;
    recompui_keybinds_set_button(player, kb_index[b], (SDL_Scancode)scancode);
    recompui_keybinds_save();
    copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
             scancode_label((SDL_Scancode)scancode));
}

void launcher_binds_set_field(LauncherModel* m, int player, int b, int slot,
                              int type, int id) {
    if (!is_n64_profile(m)) return;   // field binds exist only in the N64 store
    if (player < 1 || player > LNG_MAX_PLAYERS) return;
    if (b < 0 || b >= LNG_N64_PAD_BUTTON_COUNT) return;
    const int dev = n64_device_for_player(m, player - 1);
    rui_n64_binds_set(n64_binds_file_path(), dev, b, slot, type, id);
    // The table is shared by every player on the same device kind — refresh
    // ALL players' display strings, not just the one that captured.
    launcher_binds_refresh(m);
}

void launcher_binds_set_pad_button(LauncherModel* m, int player, int b,
                                   int kind, int code, int axis_dir) {
    if (player < 1 || player > 2) return;
    // Gamepad binds only exist on the Genesis bridge today (has_pad_binds
    // consoles). Every other profile's store is scancode-only — no-op there
    // (the UI never offers the control outside has_pad_binds).
    if (!is_genesis_profile(m)) return;
    if (b < 0 || b >= LNG_GENESIS_PAD_BUTTON_COUNT) return;
    rui_genesis_binds_set_pad(genesis_binds_file_path(), player - 1, b, kind, code, axis_dir);
    genesis_pad_label(kind, code, axis_dir,
                      m->pad_binds[player - 1][b], sizeof(m->pad_binds[player - 1][b]));
}

void launcher_binds_reset_player(LauncherModel* m, int player) {
    if (is_n64_profile(m)) {
        if (player < 1 || player > LNG_MAX_PLAYERS) return;
        rui_n64_binds_reset_device(n64_binds_file_path(),
                                   n64_device_for_player(m, player - 1));
        launcher_binds_refresh(m);
        return;
    }
    if (player < 1 || player > 2) return;
    if (is_psx_profile(m)) {
        rui_psx_binds_reset(keybinds_file_path(), player - 1);
        reload_player_display(m, player);
        return;
    }
    if (is_nes_profile(m)) {
        rui_nes_binds_reset(keybinds_file_path(), player - 1);
        reload_player_display(m, player);
        return;
    }
    if (is_genesis_profile(m)) {
        rui_genesis_binds_reset(genesis_binds_file_path(), player - 1);
        reload_player_display(m, player);
        return;
    }
    recompui_keybinds_reset_player(player);
    recompui_keybinds_save();
    reload_player_display(m, player);
}

void launcher_binds_set_hotkey(LauncherModel* m, LngHotkey h, int keycode, int kmod) {
    if (h < 0 || h >= LNG_HK_COUNT) return;
    char val[64];
    format_hotkey(keycode, kmod, val, sizeof(val));
    keymap_write(kHotkeyKey[h], val);
    copy_str(m->hotkeys[h], sizeof(m->hotkeys[h]), val[0] ? val : "(unbound)");
}
