// launcher_binds.c — real binding persistence (keybinds.ini + config.ini [KeyMap]).

#include "launcher_binds.h"
#include "launcher_sdlcompat.h"   // SDL header (2 or 3)
#include "keybinds.h"             // engine keyboard-binding store
#include "launcher_system.h"      // SystemProfile / ControllerSpec.button_count

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
// the "PSX-native keybind bridge" block below, which persists through
// psxrecomp's own psx_keybinds.c format instead so rebinds actually reach
// the game (routing PSX's 24 buttons through this 16-slot generic format
// silently discarded the 8 stick-direction binds and, worse, wrote a file
// the game's runtime can't parse at all). Every profile that still uses this
// table today borrows kSnesPadButtons (launcher_system.h stub macro), so it
// always matches LNG_SNES_PAD_BUTTON_COUNT.
static const int kKbIndexSnes[LNG_SNES_PAD_BUTTON_COUNT] = {
    /* UP    */ 8, /* DOWN  */ 9, /* LEFT  */ 10, /* RIGHT */ 11,
    /* A     */ 0, /* B     */ 1, /* X     */ 2,  /* Y     */ 3,
    /* L     */ 4, /* R     */ 5, /* START */ 6,  /* SELECT*/ 7,
};

// Resolve the keybinds-index table (and its length) for the model's ACTIVE
// SystemProfile. PSX is handled entirely by the native bridge below (see
// is_psx_profile()) before this is ever consulted, so every caller here is
// SNES-shaped (12 buttons) today.
static const int* active_kb_index(const LauncherModel* m, int* out_n) {
    (void)m;
    *out_n = LNG_SNES_PAD_BUTTON_COUNT;
    return kKbIndexSnes;
}

// ---- PSX-native keybind bridge ---------------------------------------------
// psxrecomp's runtime (runtime/launcher/psx_keybinds.h + .c, upstream repo)
// owns its OWN keyboard-bind format: an INI with [player1]/[player2]
// sections and 24 named keys (up/down/left/right/cross/circle/square/
// triangle/l1/r1/l2/r2/l3/r3/start/select/ls_up/ls_down/ls_left/ls_right/
// rs_up/rs_down/rs_left/rs_right), storing SDL *scancode* names exactly like
// keybinds.c does. It is NOT the same format as keybinds.c's generic
// PlayerBinds (a/b/x/y/l/r/start/select/up/down/left/right/l2/r2/l3/r3) —
// before this bridge, the PSX rebind page persisted through that generic
// format anyway (see kKbIndexPsx, now removed), which wrote content the PSX
// runtime's INI parser can't make sense of. PSX rebinds therefore never
// reached the game. This block fixes that at the root: for a PSX
// SystemProfile, persistence routes through a native 24-scancode store,
// read/written in psx_keybinds.c's own key vocabulary, to the SAME default
// filename ("keybinds.ini") psx_keybinds_init() reads — so whichever process
// (launcher or game) runs first creates a file the other already understands.
//
// Rebind-spec order (launcher_system.h kPsxPadButtons, 24 entries) is a
// DIFFERENT physical ordering than psx_keybinds.c's kButtons — this table
// maps rebind-spec index -> ini key NAME (never by raw index).
static const char* kPsxKbKeyName[LNG_PSX_PAD_BUTTON_COUNT] = {
    "up", "down", "left", "right",
    "triangle", "circle", "cross", "square",
    "l1", "l2", "r1", "r2",
    "l3", "r3", "start", "select",
    "ls_up", "ls_down", "ls_left", "ls_right",
    "rs_up", "rs_down", "rs_left", "rs_right",
};

// Defaults mirror psxrecomp's PSXKB_DEFAULTS (psx_keybinds.c) exactly, just
// reordered into kPsxPadButtons's rebind-spec order, so a file either process
// creates first is byte-for-byte what the other would have generated.
static const SDL_Scancode kPsxDefaultsP1[LNG_PSX_PAD_BUTTON_COUNT] = {
    /* Up */ SDL_SCANCODE_UP, /* Down */ SDL_SCANCODE_DOWN,
    /* Left */ SDL_SCANCODE_LEFT, /* Right */ SDL_SCANCODE_RIGHT,
    /* Triangle */ SDL_SCANCODE_A, /* Circle */ SDL_SCANCODE_S,
    /* Cross */ SDL_SCANCODE_X, /* Square */ SDL_SCANCODE_Z,
    /* L1 */ SDL_SCANCODE_Q, /* L2 */ SDL_SCANCODE_E,
    /* R1 */ SDL_SCANCODE_W, /* R2 */ SDL_SCANCODE_R,
    /* L3 */ SDL_SCANCODE_T, /* R3 */ SDL_SCANCODE_Y,
    /* Start */ SDL_SCANCODE_RETURN, /* Select */ SDL_SCANCODE_RSHIFT,
    /* LS Up */ SDL_SCANCODE_UP, /* LS Down */ SDL_SCANCODE_DOWN,
    /* LS Left */ SDL_SCANCODE_LEFT, /* LS Right */ SDL_SCANCODE_RIGHT,
    /* RS Up */ SDL_SCANCODE_UNKNOWN, /* RS Down */ SDL_SCANCODE_UNKNOWN,
    /* RS Left */ SDL_SCANCODE_UNKNOWN, /* RS Right */ SDL_SCANCODE_UNKNOWN,
};
static const SDL_Scancode kPsxDefaultsP2[LNG_PSX_PAD_BUTTON_COUNT] = {
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
};

static SDL_Scancode s_psx_binds[2][LNG_PSX_PAD_BUTTON_COUNT];
static int s_psx_binds_init = 0;

static int is_psx_profile(const LauncherModel* m) {
    const SystemProfile* prof = m ? (const SystemProfile*)m->profile : NULL;
    return prof && prof->id && !strcmp(prof->id, "psx");
}

static const char* psx_keybinds_file_path(void) {
    return (g_launcher_keybinds_path && g_launcher_keybinds_path[0])
             ? g_launcher_keybinds_path : "keybinds.ini";
}

// Same scancode<->name normalization psx_keybinds.c/keybinds.c use (SDL name
// first, then a handful of common aliases) so files round-trip identically
// whichever side writes them.
static SDL_Scancode psx_kb_name_to_scancode(const char* name) {
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN) return sc;
    char buf[32]; size_t i = 0;
    for (; name[i] && i < sizeof(buf) - 1; i++) buf[i] = (char)tolower((unsigned char)name[i]);
    buf[i] = '\0';
    if (!strcmp(buf, "enter") || !strcmp(buf, "return")) return SDL_SCANCODE_RETURN;
    if (!strcmp(buf, "tab"))     return SDL_SCANCODE_TAB;
    if (!strcmp(buf, "space"))   return SDL_SCANCODE_SPACE;
    if (!strcmp(buf, "lshift"))  return SDL_SCANCODE_LSHIFT;
    if (!strcmp(buf, "rshift"))  return SDL_SCANCODE_RSHIFT;
    if (!strcmp(buf, "lctrl"))   return SDL_SCANCODE_LCTRL;
    if (!strcmp(buf, "rctrl"))   return SDL_SCANCODE_RCTRL;
    if (!strcmp(buf, "lalt"))    return SDL_SCANCODE_LALT;
    if (!strcmp(buf, "ralt"))    return SDL_SCANCODE_RALT;
    if (!strcmp(buf, "backslash")) return SDL_SCANCODE_BACKSLASH;
    if (!strcmp(buf, "escape") || !strcmp(buf, "esc")) return SDL_SCANCODE_ESCAPE;
    if (!strcmp(buf, "backspace")) return SDL_SCANCODE_BACKSPACE;
    if (!strcmp(buf, "none") || !buf[0]) return SDL_SCANCODE_UNKNOWN;
    return SDL_SCANCODE_UNKNOWN;
}
static const char* psx_kb_scancode_to_name(SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_UNKNOWN) return "None";
    const char* n = SDL_GetScancodeName(sc);
    return (n && n[0]) ? n : "None";
}

static void psx_kb_write_ini(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "# PSXRecomp Keyboard Keybinds (keyboard -> DualShock).\n"
        "# Written by recomp-ui's launcher (psx_keybinds.c-compatible format);\n"
        "# also read/regenerated by the game's own runtime. Edit values and\n"
        "# restart, or rebind live in the launcher's Controls page.\n"
        "# Use SDL key names, or \"None\" to leave an input unbound.\n\n");
    for (int p = 0; p < 2; ++p) {
        fprintf(f, "[player%d]\n", p + 1);
        for (int b = 0; b < LNG_PSX_PAD_BUTTON_COUNT; ++b)
            fprintf(f, "%-9s = %s\n", kPsxKbKeyName[b], psx_kb_scancode_to_name(s_psx_binds[p][b]));
        fprintf(f, "\n");
    }
    fclose(f);
}

// Key indices present ONLY in psx_keybinds.c's vocabulary (never in
// keybinds.c's generic PlayerBinds format): triangle/circle/cross/square,
// l1/r1 (generic only has l/r), and all 8 ls_*/rs_* stick binds. up/down/
// left/right/start/select/l2/r2/l3/r3 (the other 10 keys) exist in BOTH
// formats by coincidence of naming, so matching one of those alone does NOT
// prove the file is psx_keybinds.c-shaped — see psx_kb_load_ini()'s use of
// this, which is what stops a stale pre-bridge generic-format keybinds.ini
// from being silently half-applied as if it were native.
static int psx_kb_key_is_native_only(int b) {
    switch (b) {
        case 4: case 5: case 6: case 7:        // triangle/circle/cross/square
        case 8: case 10:                        // l1/r1
        case 16: case 17: case 18: case 19:     // ls_up/down/left/right
        case 20: case 21: case 22: case 23:     // rs_up/down/left/right
            return 1;
        default:
            return 0;
    }
}

// Returns the count of NATIVE-ONLY keys matched (see above) — the caller
// uses this to detect a foreign-format file instead of silently blending
// its overlapping-name values into the defaults.
static int psx_kb_load_ini(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int player = -1;   // -1 none, 0 p1, 1 p2
    int native_hits = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' || isspace((unsigned char)line[n-1]))) line[--n] = '\0';
        char* s = line;
        while (*s && isspace((unsigned char)*s)) ++s;
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            if (end) *end = '\0';
            const char* section = s + 1;
            player = -1;
            if (!strcmp(section, "player1")) player = 0;
            else if (!strcmp(section, "player2")) player = 1;
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq || player < 0) continue;
        *eq = '\0';
        char* key = s; char* val = eq + 1;
        // trim key/val
        size_t kl = strlen(key); while (kl > 0 && isspace((unsigned char)key[kl-1])) key[--kl] = '\0';
        while (*val && isspace((unsigned char)*val)) ++val;
        size_t vl = strlen(val); while (vl > 0 && isspace((unsigned char)val[vl-1])) val[--vl] = '\0';
        for (char* c = key; *c; c++) *c = (char)tolower((unsigned char)*c);
        for (int b = 0; b < LNG_PSX_PAD_BUTTON_COUNT; ++b) {
            if (!strcmp(key, kPsxKbKeyName[b])) {
                s_psx_binds[player][b] = psx_kb_name_to_scancode(val);
                if (psx_kb_key_is_native_only(b)) ++native_hits;
                break;
            }
        }
    }
    fclose(f);
    return native_hits;
}

// Load existing keybinds.ini if present (whichever process wrote it last —
// launcher or game, same format), else seed defaults and write it so the
// game's first run sees identical bindings to what the launcher displays.
// If a file exists but matches ZERO native-only keys, it is a foreign-format
// file (e.g. a stale keybinds.ini this launcher wrote pre-bridge, in the
// generic SNES-shaped format, before a PSX game ever ran) — defaults are
// re-seeded and the file is rewritten cleanly rather than left holding a
// half-applied blend of foreign values.
static void psx_kb_init(void) {
    memcpy(s_psx_binds[0], kPsxDefaultsP1, sizeof(kPsxDefaultsP1));
    memcpy(s_psx_binds[1], kPsxDefaultsP2, sizeof(kPsxDefaultsP2));
    const char* path = psx_keybinds_file_path();
    FILE* test = fopen(path, "r");
    if (test) {
        fclose(test);
        int native_hits = psx_kb_load_ini(path);
        if (native_hits == 0) {
            memcpy(s_psx_binds[0], kPsxDefaultsP1, sizeof(kPsxDefaultsP1));
            memcpy(s_psx_binds[1], kPsxDefaultsP2, sizeof(kPsxDefaultsP2));
            psx_kb_write_ini(path);
        }
    } else {
        psx_kb_write_ini(path);
    }
    s_psx_binds_init = 1;
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

static void reload_player_display(LauncherModel* m, int player) {
    if (is_psx_profile(m)) {
        for (int b = 0; b < LNG_PSX_PAD_BUTTON_COUNT; ++b)
            copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
                     scancode_label(s_psx_binds[player - 1][b]));
        return;
    }
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

// Surgically set "Key = value" inside [KeyMap], preserving all other lines.
static void keymap_write(const char* key, const char* value) {
    const char* path = config_path();
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

    /* locate [KeyMap] body [start,end) */
    int ks = -1, ke = -1;
    for (int i = 0; i < n; ++i) {
        const char* p = lines[i]; while (*p == ' ' || *p == '\t') ++p;
        if (*p != '[') continue;
        const char* close = strchr(p, ']');
        size_t sl = close ? (size_t)(close - p - 1) : strlen(p + 1);
        if (ks >= 0) { ke = i; break; }
        if (ieq(p + 1, sl, "KeyMap")) ks = i + 1;
    }
    if (ks >= 0 && ke < 0) ke = n;

    if (ks < 0) {
        if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        if (n && lines[n-1][0]) lines[n++] = strdup("");
        if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        lines[n++] = strdup("[KeyMap]");
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
        psx_kb_init();                            // load/generate psx_keybinds.c-format keybinds.ini
    } else {
        recompui_keybinds_init(NULL);              // load/generate keybinds.ini (exe-anchored)
    }
    reload_player_display(m, 1);
    reload_player_display(m, 2);
    reload_hotkey_display(m);
}

void launcher_binds_set_button(LauncherModel* m, int player, int b, int scancode) {
    if (player < 1 || player > 2) return;
    if (is_psx_profile(m)) {
        if (b < 0 || b >= LNG_PSX_PAD_BUTTON_COUNT) return;
        if (!s_psx_binds_init) psx_kb_init();
        s_psx_binds[player - 1][b] = (SDL_Scancode)scancode;
        psx_kb_write_ini(psx_keybinds_file_path());
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

void launcher_binds_reset_player(LauncherModel* m, int player) {
    if (player < 1 || player > 2) return;
    if (is_psx_profile(m)) {
        if (!s_psx_binds_init) psx_kb_init();
        memcpy(s_psx_binds[player - 1], player == 2 ? kPsxDefaultsP2 : kPsxDefaultsP1,
               sizeof(s_psx_binds[player - 1]));
        psx_kb_write_ini(psx_keybinds_file_path());
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
