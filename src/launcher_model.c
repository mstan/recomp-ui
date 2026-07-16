// launcher_model.c — implementation of the game-agnostic launcher view-model.
//
// Pure logic: no SDL, no OpenGL, no UI toolkit. Safe to compile as C and link
// into any game or either prototype backend.

#include "launcher_model.h"
#include "launcher_system.h"

#include "crc32.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int kFreqTable[] = { 32000, 44100, 48000 };
static const int kFreqCount   = (int)(sizeof(kFreqTable) / sizeof(kFreqTable[0]));

static const int kWindowWidths[]    = { 960, 1280, 1600, 1920 };
static const int kWindowWidthCount  = (int)(sizeof(kWindowWidths) / sizeof(kWindowWidths[0]));
static const int kInterpFpsTable[]  = { 0, 90, 120, 144, 165, 240 };
static const int kInterpFpsCount    = (int)(sizeof(kInterpFpsTable) / sizeof(kInterpFpsTable[0]));
static const char* kScreenKindNames[4] = { "Raw", "CRT", "Composite", "Trinitron" };

static const char* kButtonNames[LNG_BTN_COUNT] = {
    "Up", "Down", "Left", "Right", "A", "B", "X", "Y",
    "L", "R", "Start", "Select"
};
// Player 1 keyboard defaults (Player 2 defaults unbound, mirroring the RML note).
static const char* kP1Defaults[LNG_BTN_COUNT] = {
    "Up", "Down", "Left", "Right", "X", "Z", "S", "A", "D", "C", "Enter", "RShift"
};
// Display labels for the 11 engine hotkeys (order == LngHotkey == [KeyMap] keys).
static const char* kHotkeyNames[LNG_HK_COUNT] = {
    "Fullscreen", "Reset", "Pause", "Pause (dimmed)", "Fast-forward",
    "Window bigger", "Window smaller", "Volume up", "Volume down",
    "FPS readout", "Toggle renderer"
};
static const char* kViewNames[3] = { "Dashboard", "Settings", "Controller" };
static const char* kSrcNames[3]  = { "None", "Keyboard", "Gamepad" };

static void safe_copy(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void launcher_model_init(LauncherModel* m,
                         const RecompLauncherCSettings* io,
                         const RecompLauncherCGameInfo* game,
                         const char* initial_rom) {
    memset(m, 0, sizeof(*m));

    if (game) {
        m->game_name            = game->name ? game->name : "Unknown Game";
        m->region               = game->region ? game->region : "";
        m->platform             = game->platform;   // NULL => no subtitle
        m->widescreen_supported = game->widescreen_supported != 0;
        m->msu1_supported       = game->msu1_supported != 0;
        m->msu1_note            = game->msu1_note;
        m->saves_supported      = game->sram_path != NULL;
        m->sram_path            = game->sram_path;
        /* 0 = unset (caller predates the field) -> assume 2 players. */
        m->player_count         = game->num_players ? clampi(game->num_players, 1, 2) : 2;
        m->expected_crc         = game->expected_crc;
        m->has_expected_crc     = game->has_expected_crc;
        m->known_sha256         = game->known_sha256;
        m->num_known_sha256     = game->num_known_sha256;

        m->pad_mode_supported   = game->pad_mode_supported != 0;
        m->pad_mode_selectable  = game->pad_mode_selectable != 0;
        m->allow_hybrid         = game->allow_hybrid != 0;
        m->locked_pad_mode      = clampi(game->locked_pad_mode, 0, 2);
        m->lock_device          = game->lock_device != 0;
        m->aspect_mask          = game->aspect_mask;

        m->has_window_size      = game->has_window_size != 0;
        m->has_renderer         = game->has_renderer != 0;
        m->has_supersampling    = game->has_supersampling != 0;
        m->has_antialiasing     = game->has_antialiasing != 0;
        m->has_texture_filter   = game->has_texture_filter != 0;
        m->has_screen_kind      = game->has_screen_kind != 0;
        m->has_frame_interp     = game->has_frame_interp != 0;
        m->has_spu_hq           = game->has_spu_hq != 0;
        m->has_skip_fmv         = game->has_skip_fmv != 0;
        m->has_turbo_loads      = game->has_turbo_loads != 0;
        m->has_fullscreen_toggle = game->has_fullscreen_toggle != 0;
        m->has_bios             = game->has_bios != 0;
        m->has_deadzone_pct     = game->has_deadzone_pct != 0;
        m->rom_noun             = game->rom_noun ? game->rom_noun : "ROM";
        m->language_labels      = game->language_labels;
        m->num_languages        = game->num_languages;
    } else {
        m->game_name    = "Unknown Game";
        m->region       = "";
        m->platform     = NULL;
        m->player_count = 2;
        m->rom_noun     = "ROM";
    }

    if (io) m->s = *io;

    // ---- infer the SystemProfile this game belongs to (panel composition +
    // per-system specs) from the ABI caps launcher_profile_apply() already set ----
    m->profile = launcher_system_infer(game);

    // ---- gate pad_mode per player ----
    if (m->pad_mode_supported) {
        for (int p = 0; p < 2; ++p) {
            if (!m->pad_mode_selectable) {
                m->s.pad_mode[p] = m->locked_pad_mode;
            } else if (!m->allow_hybrid && m->s.pad_mode[p] == 0) {
                m->s.pad_mode[p] = 1;   // snap Hybrid -> Analog
            }
        }
    }

    // ---- validate/clamp aspect_index against the offered set ----
    if (m->aspect_mask) {
        int idx = clampi(m->s.aspect_index, 0, 2);
        // walk down from the requested index to the nearest offered aspect;
        // 4:3 (bit0, index 0) is always offered so this always terminates.
        while (idx > 0 && !(m->aspect_mask & (1 << idx))) --idx;
        m->s.aspect_index = idx;
    }

    // ---- clamp/seed the deeper PSX-style settings against their own ranges ----
    if (m->has_window_size) {
        int ok = 0;
        for (int i = 0; i < kWindowWidthCount; ++i)
            if (kWindowWidths[i] == m->s.window_width) { ok = 1; break; }
        if (!ok) m->s.window_width = kWindowWidths[0];
    }
    if (m->has_supersampling) m->s.supersampling = clampi(m->s.supersampling ? m->s.supersampling : 1, 1, 4);
    if (m->has_screen_kind)   m->s.screen_kind   = clampi(m->s.screen_kind, 0, 3);
    if (m->has_texture_filter) m->s.texture_filter = m->s.texture_filter ? 1 : 0;
    if (m->has_renderer)      m->s.renderer      = m->s.renderer ? 1 : 0;
    if (m->has_frame_interp) {
        int ok = 0;
        for (int i = 0; i < kInterpFpsCount; ++i)
            if (kInterpFpsTable[i] == m->s.frame_interp_fps) { ok = 1; break; }
        if (!ok) m->s.frame_interp_fps = 0;
    }
    if (m->num_languages > 0)
        m->s.language_index = clampi(m->s.language_index, 0, m->num_languages - 1);
    if (m->has_deadzone_pct) {
        m->s.deadzone[0] = clampi((m->s.deadzone[0] / 5) * 5, 0, 50);
        m->s.deadzone[1] = m->s.deadzone[0];
    }

    // Real ROM read + CRC/SHA verification (computes rom_size, crc_match,
    // sha_match). No synthesized/faked facts.
    launcher_model_set_rom(m, initial_rom);

    m->view      = LNG_VIEW_DASHBOARD;
    m->action    = LNG_ACTION_NONE;
    m->cfg_player = 0;

    // Placeholder display until launcher_binds_load() fills real values from
    // keybinds.ini / config.ini [KeyMap]. Walk the ACTIVE profile's button
    // count (SNES 12, PSX 16, ...) so every rebind slot the page will render
    // gets a placeholder — kP1Defaults only names the SNES-shaped first 12.
    {
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        int bc = prof ? prof->controller.button_count : LNG_BTN_COUNT;
        if (bc > LNG_MAX_BUTTONS) bc = LNG_MAX_BUTTONS;
        for (int b = 0; b < bc; ++b) {
            safe_copy(m->binds[0][b], sizeof(m->binds[0][b]),
                      b < LNG_BTN_COUNT ? kP1Defaults[b] : "(unbound)");
            safe_copy(m->binds[1][b], sizeof(m->binds[1][b]), "(unbound)");
        }
    }
    for (int h = 0; h < LNG_HK_COUNT; ++h)
        m->hotkeys[h][0] = '\0';
}

void launcher_model_commit(const LauncherModel* m, RecompLauncherCSettings* io) {
    if (io) *io = m->s;
}

void launcher_model_set_rom(LauncherModel* m, const char* path) {
    m->rom_present = path && path[0] != '\0';
    safe_copy(m->rom_full, sizeof(m->rom_full), m->rom_present ? path : "");

    // Display just the basename (handles both / and \ separators).
    const char* base = m->rom_full;
    for (const char* q = m->rom_full; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;
    safe_copy(m->rom_file, sizeof(m->rom_file), m->rom_present ? base : "(none)");

    /* Read the ROM once: real size, and real CRC32 + SHA-256 over the cartridge
     * body (SMC copier header stripped) compared against the expected
     * fingerprint. No faking — a wrong/corrupt ROM fails verification. */
    m->rom_size[0] = '\0';
    m->crc_match = false;
    m->sha_match = false;
    if (m->rom_present) {
        FILE* f = fopen(m->rom_full, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0) {
                snprintf(m->rom_size, sizeof(m->rom_size), "%.2f MB (%ld Mbit)",
                         (double)n / (1024.0 * 1024.0), (long)((n * 8) / (1024 * 1024)));
                uint8_t* buf = (uint8_t*)malloc((size_t)n);
                if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                    /* SMC copier header is present when (size % 1024 == 512). */
                    size_t hdr  = ((size_t)n % 1024 == 512) ? 512 : 0;
                    const uint8_t* body = buf + hdr;
                    size_t blen = (size_t)n - hdr;
                    uint32_t crc = recompui_crc32_compute(body, blen);
                    uint8_t  sha[32];
                    recompui_sha256_compute(body, blen, sha);
                    m->crc_match = m->has_expected_crc && crc == m->expected_crc;
                    for (size_t k = 0; k < m->num_known_sha256; ++k)
                        if (memcmp(sha, m->known_sha256[k], 32) == 0) { m->sha_match = true; break; }
                }
                free(buf);
            }
            fclose(f);
        }
    }
    if (!m->rom_size[0]) safe_copy(m->rom_size, sizeof(m->rom_size), "--");
}

const char* launcher_model_rom_path(const LauncherModel* m) {
    return m->rom_full;
}

bool launcher_model_rom_verified(const LauncherModel* m) {
    if (!m->rom_present) return false;
    const int has_crc = m->has_expected_crc;
    const int has_sha = m->num_known_sha256 > 0;
    if (!has_crc && !has_sha) return false;   // no fingerprint => can't vouch
    if (has_crc && !m->crc_match) return false;
    if (has_sha && !m->sha_match) return false;
    return true;
}

void launcher_model_set_view(LauncherModel* m, LngView v) {
    m->view = v;
}

void launcher_model_open_config(LauncherModel* m, int player) {
    m->cfg_player = clampi(player, 0, 1);
    m->view = LNG_VIEW_CONTROLLER;
}

void launcher_model_cycle_scale(LauncherModel* m) {
    m->s.window_scale = (m->s.window_scale >= 6) ? 1 : m->s.window_scale + 1;
    if (m->s.window_scale < 1) m->s.window_scale = 1;
}

void launcher_model_toggle_filter(LauncherModel* m) {
    m->s.linear_filter = !m->s.linear_filter;
}

void launcher_model_toggle_widescreen(LauncherModel* m) {
    if (!m->widescreen_supported) return;   // gated: no-op when unsupported
    m->s.widescreen = !m->s.widescreen;
}

bool launcher_model_aspect_offered(const LauncherModel* m, int index) {
    if (index == 0) return true;   // 4:3 is always implied/available
    if (index == 1) return (m->aspect_mask & 2) != 0;
    if (index == 2) return (m->aspect_mask & 4) != 0;
    return false;
}

void launcher_model_cycle_aspect(LauncherModel* m) {
    if (!m->aspect_mask) return;   // gated: legacy widescreen-bool games no-op
    int idx = clampi(m->s.aspect_index, 0, 2);
    for (int i = 0; i < 3; ++i) {
        idx = (idx + 1) % 3;
        if (launcher_model_aspect_offered(m, idx)) { m->s.aspect_index = idx; return; }
    }
}

const char* launcher_model_aspect_label(const LauncherModel* m) {
    static const char* kLabels[3] = {
        "4:3 (Native)", "16:9 (Widescreen)", "21:9 (Ultrawide)"
    };
    int idx = clampi(m->s.aspect_index, 0, 2);
    return kLabels[idx];
}

void launcher_model_cycle_freq(LauncherModel* m) {
    int idx = 0;
    for (int i = 0; i < kFreqCount; ++i)
        if (kFreqTable[i] == m->s.audio_freq) { idx = i; break; }
    m->s.audio_freq = kFreqTable[(idx + 1) % kFreqCount];
}

void launcher_model_volume_delta(LauncherModel* m, int delta) {
    m->s.volume = clampi(m->s.volume + delta, 0, 100);
}

// ---- deeper PSX-style settings ---------------------------------------------

void launcher_model_cycle_window_size(LauncherModel* m) {
    int idx = 0;
    for (int i = 0; i < kWindowWidthCount; ++i)
        if (kWindowWidths[i] == m->s.window_width) { idx = i; break; }
    m->s.window_width = kWindowWidths[(idx + 1) % kWindowWidthCount];
}

const char* launcher_model_window_size_label(const LauncherModel* m) {
    static char buf[32];
    int w = m->s.window_width > 0 ? m->s.window_width : kWindowWidths[0];
    int aspect = clampi(m->s.aspect_index, 0, 2);
    int h = (aspect == 1) ? (w * 9 / 16) : (aspect == 2) ? (w * 9 / 21) : (w * 3 / 4);
    snprintf(buf, sizeof(buf), "%d \xC3\x97 %d", w, h);   // "×" (U+00D7)
    return buf;
}

void launcher_model_toggle_renderer(LauncherModel* m) {
    m->s.renderer = !m->s.renderer;
}

const char* launcher_model_renderer_label(const LauncherModel* m) {
    return m->s.renderer ? "OpenGL" : "Software";
}

void launcher_model_cycle_supersampling(LauncherModel* m) {
    int v = clampi(m->s.supersampling ? m->s.supersampling : 1, 1, 4);
    m->s.supersampling = (v % 4) + 1;
}

const char* launcher_model_supersampling_label(const LauncherModel* m) {
    static char buf[8];
    int v = clampi(m->s.supersampling ? m->s.supersampling : 1, 1, 4);
    snprintf(buf, sizeof(buf), "%dx", v);
    return buf;
}

void launcher_model_toggle_aa(LauncherModel* m) {
    m->s.antialiasing = !m->s.antialiasing;
}

void launcher_model_toggle_texture_filter(LauncherModel* m) {
    m->s.texture_filter = !m->s.texture_filter;
}

const char* launcher_model_texture_filter_label(const LauncherModel* m) {
    return m->s.texture_filter ? "Bilinear" : "Nearest";
}

void launcher_model_cycle_screen_kind(LauncherModel* m) {
    m->s.screen_kind = (clampi(m->s.screen_kind, 0, 3) + 1) % 4;
}

const char* launcher_model_screen_kind_label(const LauncherModel* m) {
    return kScreenKindNames[clampi(m->s.screen_kind, 0, 3)];
}

void launcher_model_toggle_frame_interp(LauncherModel* m) {
    m->s.frame_interp = !m->s.frame_interp;
}

void launcher_model_cycle_interp_fps(LauncherModel* m) {
    int idx = 0;
    for (int i = 0; i < kInterpFpsCount; ++i)
        if (kInterpFpsTable[i] == m->s.frame_interp_fps) { idx = i; break; }
    m->s.frame_interp_fps = kInterpFpsTable[(idx + 1) % kInterpFpsCount];
}

const char* launcher_model_interp_fps_label(const LauncherModel* m) {
    static char buf[24];
    if (m->s.frame_interp_fps == 0) return "Display refresh";
    snprintf(buf, sizeof(buf), "%d fps", m->s.frame_interp_fps);
    return buf;
}

void launcher_model_toggle_spu_hq(LauncherModel* m) {
    m->s.spu_hq = !m->s.spu_hq;
}

void launcher_model_toggle_skip_fmv(LauncherModel* m) {
    m->s.auto_skip_fmv = !m->s.auto_skip_fmv;
}

void launcher_model_toggle_turbo_loads(LauncherModel* m) {
    m->s.turbo_loads = !m->s.turbo_loads;
}

void launcher_model_toggle_fullscreen(LauncherModel* m) {
    // Simple on/off PSX row: reuses the existing tri-state `fullscreen` field
    // (0 off / 1 borderless / 2 exclusive) but only ever toggles between 0/1 —
    // SNES's own fullscreen path (which offers exclusive mode) is untouched.
    m->s.fullscreen = m->s.fullscreen ? 0 : 1;
}

void launcher_model_cycle_language(LauncherModel* m) {
    if (m->num_languages <= 0) return;
    m->s.language_index = (m->s.language_index + 1) % m->num_languages;
}

const char* launcher_model_language_label(const LauncherModel* m) {
    if (m->num_languages <= 0 || !m->language_labels) return "";
    int idx = clampi(m->s.language_index, 0, m->num_languages - 1);
    return m->language_labels[idx] ? m->language_labels[idx] : "";
}

void launcher_model_cycle_deadzone_pct(LauncherModel* m) {
    int v = clampi(m->s.deadzone[0], 0, 50);
    v = ((v / 5) + 1) * 5;
    if (v > 50) v = 0;
    m->s.deadzone[0] = v;
    m->s.deadzone[1] = v;
}

const char* launcher_model_deadzone_pct_label(const LauncherModel* m) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", clampi(m->s.deadzone[0], 0, 50));
    return buf;
}

void launcher_model_set_bios_path(LauncherModel* m, const char* path) {
    safe_copy(m->s.bios_path, sizeof(m->s.bios_path), path ? path : "");
}

void launcher_model_toggle_msu1(LauncherModel* m) {
    if (!m->msu1_supported) return;
    m->s.msu1_enabled = !m->s.msu1_enabled;
}

void launcher_model_set_msu1_dir(LauncherModel* m, const char* dir) {
    safe_copy(m->s.msu1_dir, sizeof(m->s.msu1_dir), dir ? dir : "");
}

void launcher_model_set_pad_mode(LauncherModel* m, int player, int mode) {
    if (!m->pad_mode_supported || !m->pad_mode_selectable) return;   // gated/locked
    player = clampi(player, 0, 1);
    mode = clampi(mode, 0, 2);
    if (mode == 0 && !m->allow_hybrid) mode = 1;   // Hybrid hidden -> snap to Analog
    m->s.pad_mode[player] = mode;
}

void launcher_model_cycle_player_src(LauncherModel* m, int player) {
    player = clampi(player, 0, 1);
    m->s.player_src[player] = (m->s.player_src[player] + 1) % 3;  // None/Kbd/Pad
}

void launcher_model_deadzone_delta(LauncherModel* m, int player, int delta) {
    player = clampi(player, 0, 1);
    m->s.deadzone[player] = clampi(m->s.deadzone[player] + delta, 0, 100);
}

void launcher_model_set_source(LauncherModel* m, int player, int kind,
                               uint32_t pad_id, const char* pad_name) {
    player = clampi(player, 0, 1);
    m->s.player_src[player] = clampi(kind, 0, 2);
    if (kind == 2) {
        m->player_pad_id[player] = pad_id;
        safe_copy(m->player_pad_name[player], sizeof(m->player_pad_name[player]),
                  pad_name ? pad_name : "Gamepad");
    } else {
        m->player_pad_id[player] = 0;
        m->player_pad_name[player][0] = '\0';
    }
}

void launcher_model_request_skip_toggle(LauncherModel* m) {
    if (!m->s.skip_launcher) {
        m->skip_modal_open = true;    // enabling: confirm first
    } else {
        m->s.skip_launcher = 0;       // disabling: immediate
    }
}

void launcher_model_skip_confirm(LauncherModel* m) {
    m->s.skip_launcher = 1;
    m->skip_modal_open = false;
}

void launcher_model_skip_cancel(LauncherModel* m) {
    m->skip_modal_open = false;
}

void launcher_model_begin_capture(LauncherModel* m, int b) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    int bc = prof ? prof->controller.button_count : LNG_BTN_COUNT;
    if (bc > LNG_MAX_BUTTONS) bc = LNG_MAX_BUTTONS;
    if (b < 0 || b >= bc) return;
    m->hk_capturing = false;
    m->capturing    = true;
    m->capture_btn  = b;
}
void launcher_model_cancel_capture(LauncherModel* m) { m->capturing = false; }

void launcher_model_begin_hk_capture(LauncherModel* m, LngHotkey h) {
    if (h < 0 || h >= LNG_HK_COUNT) return;
    m->capturing    = false;
    m->hk_capturing = true;
    m->capture_hk   = h;
}
void launcher_model_cancel_hk_capture(LauncherModel* m) { m->hk_capturing = false; }

const char* launcher_model_scale_label(const LauncherModel* m) {
    static char buf[8];
    int s = m->s.window_scale < 1 ? 1 : m->s.window_scale;
    snprintf(buf, sizeof(buf), "%dx", s);
    return buf;
}

const char* launcher_model_freq_label(const LauncherModel* m) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d Hz", m->s.audio_freq);
    return buf;
}

const char* launcher_model_player_src_label(const LauncherModel* m, int player) {
    player = clampi(player, 0, 1);
    int src = clampi(m->s.player_src[player], 0, 2);
    if (src == 2 && m->player_pad_name[player][0])   // show the actual device name
        return m->player_pad_name[player];
    return kSrcNames[src];
}

const char* launcher_button_name(LngButton b) {
    if (b < 0 || b >= LNG_BTN_COUNT) return "?";
    return kButtonNames[b];
}

const char* launcher_hotkey_name(LngHotkey h) {
    if (h < 0 || h >= LNG_HK_COUNT) return "?";
    return kHotkeyNames[h];
}

const char* launcher_view_name(LngView v) {
    if (v < 0 || v > LNG_VIEW_CONTROLLER) return "?";
    return kViewNames[v];
}
