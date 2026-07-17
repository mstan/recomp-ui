// gb_binds.c — Game Boy-native keybind persistence bridge (see gb_binds.h).
//
// Reads/writes gb-recompiled's keybinds.ini [controls] format (SDL scancode
// names) so a launcher rebind reaches the game and both sides round-trip
// byte-identically. Modeled on the PSX bridge (consoles/psx/psx_binds.c): the
// bridge OWNS the file and rewrites it wholesale in the runtime's exact format.

#include "gb_binds.h"
#include "gb_profile.h"           // LNG_GB_PAD_BUTTON_COUNT (rebind-spec order)
#include "launcher_sdlcompat.h"   // SDL header (2 or 3)

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// The file stores 9 inputs, in gb-recompiled keybinds.c's [controls] order.
// The first 8 are the rebindable pad buttons; `turbo` (index 8) is a fixed
// speed toggle preserved verbatim across rewrites, not a rebind-page row.
#define GB_FILE_KEY_COUNT 9
enum { GBF_A, GBF_B, GBF_SELECT, GBF_START, GBF_UP, GBF_DOWN, GBF_LEFT, GBF_RIGHT, GBF_TURBO };

static const char* kGbFileKeys[GB_FILE_KEY_COUNT] = {
    "a", "b", "select", "start", "up", "down", "left", "right", "turbo"
};

// gb-recompiled keybinds.c s_binds defaults (exact), in file order.
static const SDL_Scancode kGbDefaults[GB_FILE_KEY_COUNT] = {
    /* a      */ SDL_SCANCODE_Z,      /* b     */ SDL_SCANCODE_X,
    /* select */ SDL_SCANCODE_RSHIFT, /* start */ SDL_SCANCODE_RETURN,
    /* up     */ SDL_SCANCODE_UP,     /* down  */ SDL_SCANCODE_DOWN,
    /* left   */ SDL_SCANCODE_LEFT,   /* right */ SDL_SCANCODE_RIGHT,
    /* turbo  */ SDL_SCANCODE_TAB,
};

// Rebind-spec index (kGbPadButtons: Up/Down/Left/Right/A/B/Start/Select) ->
// file index. Addressed by NAME/mapping, never by raw coincidence.
static const int kGbSpecToFile[LNG_GB_PAD_BUTTON_COUNT] = {
    /* Up */ GBF_UP, /* Down */ GBF_DOWN, /* Left */ GBF_LEFT, /* Right */ GBF_RIGHT,
    /* A */ GBF_A,   /* B */ GBF_B,       /* Start */ GBF_START, /* Select */ GBF_SELECT,
};

static SDL_Scancode s_gb_binds[GB_FILE_KEY_COUNT];
static int s_gb_binds_init = 0;

// Same scancode<->name normalization gb-recompiled keybinds.c uses (SDL name
// first, then the same handful of aliases) so files round-trip identically.
static SDL_Scancode gb_name_to_scancode(const char* name) {
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN) return sc;
    char buf[32]; size_t i = 0;
    for (; name[i] && i < sizeof(buf) - 1; i++) buf[i] = (char)tolower((unsigned char)name[i]);
    buf[i] = '\0';
    if (!strcmp(buf, "enter") || !strcmp(buf, "return")) return SDL_SCANCODE_RETURN;
    if (!strcmp(buf, "tab"))       return SDL_SCANCODE_TAB;
    if (!strcmp(buf, "space"))     return SDL_SCANCODE_SPACE;
    if (!strcmp(buf, "lshift"))    return SDL_SCANCODE_LSHIFT;
    if (!strcmp(buf, "rshift"))    return SDL_SCANCODE_RSHIFT;
    if (!strcmp(buf, "backslash")) return SDL_SCANCODE_BACKSLASH;
    if (!strcmp(buf, "escape") || !strcmp(buf, "esc")) return SDL_SCANCODE_ESCAPE;
    if (!strcmp(buf, "backspace")) return SDL_SCANCODE_BACKSPACE;
    return SDL_SCANCODE_UNKNOWN;
}
static const char* gb_scancode_to_name(SDL_Scancode sc) {
    const char* n = SDL_GetScancodeName(sc);
    return (n && n[0]) ? n : "Space";
}

static void gb_write_ini(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "# Game Boy Keybinds\n"
        "# Edit key names to customize. Uses SDL key names.\n"
        "# Common: Z, X, Tab, Return, Up, Down, Left, Right\n"
        "# A-Z letters, Space, Left Shift, Right Shift, Backspace\n\n"
        "[controls]\n");
    for (int i = 0; i < GB_FILE_KEY_COUNT; ++i)
        fprintf(f, "%s = %s\n", kGbFileKeys[i], gb_scancode_to_name(s_gb_binds[i]));
    fclose(f);
}

static void gb_load_ini(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    int in_controls = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' || isspace((unsigned char)line[n-1]))) line[--n] = '\0';
        char* s = line;
        while (*s && isspace((unsigned char)*s)) ++s;
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            if (end) *end = '\0';
            in_controls = !strcmp(s + 1, "controls");
            continue;
        }
        if (!in_controls) continue;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = s; char* val = eq + 1;
        size_t kl = strlen(key); while (kl > 0 && isspace((unsigned char)key[kl-1])) key[--kl] = '\0';
        while (*val && isspace((unsigned char)*val)) ++val;
        size_t vl = strlen(val); while (vl > 0 && isspace((unsigned char)val[vl-1])) val[--vl] = '\0';
        for (char* c = key; *c; c++) *c = (char)tolower((unsigned char)*c);
        for (int i = 0; i < GB_FILE_KEY_COUNT; ++i) {
            if (!strcmp(key, kGbFileKeys[i])) { s_gb_binds[i] = gb_name_to_scancode(val); break; }
        }
    }
    fclose(f);
}

void rui_gb_binds_init(const char* path) {
    memcpy(s_gb_binds, kGbDefaults, sizeof(kGbDefaults));
    FILE* test = path ? fopen(path, "r") : NULL;
    if (test) { fclose(test); gb_load_ini(path); }
    else if (path)      { gb_write_ini(path); }
    s_gb_binds_init = 1;
}

int rui_gb_binds_get(const char* path, int b) {
    if (!s_gb_binds_init) rui_gb_binds_init(path);
    if (b < 0 || b >= LNG_GB_PAD_BUTTON_COUNT) return SDL_SCANCODE_UNKNOWN;
    return (int)s_gb_binds[kGbSpecToFile[b]];
}

void rui_gb_binds_set(const char* path, int b, int scancode) {
    if (b < 0 || b >= LNG_GB_PAD_BUTTON_COUNT) return;
    if (!s_gb_binds_init) rui_gb_binds_init(path);
    s_gb_binds[kGbSpecToFile[b]] = (SDL_Scancode)scancode;
    gb_write_ini(path);
}

void rui_gb_binds_reset(const char* path) {
    if (!s_gb_binds_init) rui_gb_binds_init(path);
    memcpy(s_gb_binds, kGbDefaults, sizeof(kGbDefaults));
    gb_write_ini(path);
}
