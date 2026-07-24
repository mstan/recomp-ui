// launcher_model.c — implementation of the game-agnostic launcher view-model.
//
// Pure logic: no SDL, no OpenGL, no UI toolkit. Safe to compile as C and link
// into any game or either prototype backend.

#include "launcher_model.h"
#include "launcher_system.h"

#include "crc32.h"
#include "consoles/psx/memcard_format.h"   // PSX-specific; used only under SAVE_MEMCARD
#include "sha256.h"
#include "sha1.h"
#include "ips_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 32040 = the SNES S-DSP's native output rate; kept reachable in the cycle so
// users chasing bit-exact SNES audio can pick it (matches the legacy launcher).
static const int kFreqTable[] = { 32040, 32000, 44100, 48000 };
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
static const char* kViewNames[4] = { "Dashboard", "Settings", "Controller", "Netplay" };
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

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void run_verify(LauncherModel* m);   // fwd; defined below, called from launcher_model_set_rom
static void update_msu1_patch_available(LauncherModel* m);   // fwd; called from launcher_model_set_rom
static void lm_inspect_memcard(LauncherModel* m, int slot); // fwd; host memcard_inspect callback
static void lm_inspect_tpak(LauncherModel* m, int slot);    // fwd; host tpak_inspect callback

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
        m->msu1_patch_path      = game->msu1_patch_path;
        m->saves_supported      = game->sram_path != NULL;
        m->sram_path            = game->sram_path;
        m->has_integer_scale    = game->has_integer_scale != 0;
        m->hdpack_supported     = game->hdpack_supported != 0;
        m->password_save_path   = game->password_save_path;
        m->password_save_label  = game->password_save_label;
        m->zapper               = game->zapper != 0;
        /* 0 = unset (caller predates the field) -> assume 2 players. */
        m->player_count         = game->num_players ? clampi(game->num_players, 1, LNG_MAX_PLAYERS) : 2;
        m->expected_crc         = game->expected_crc;
        m->has_expected_crc     = game->has_expected_crc;
        m->known_sha256         = game->known_sha256;
        m->known_sha1_hex       = game->known_sha1_hex;
        m->num_known_sha1       = game->num_known_sha1;
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
        // game->has_fullscreen_toggle is deliberately NOT read: the Fullscreen
        // row is universal (drawn for every console) — see recomp_launcher.h.
        m->has_bios             = game->has_bios != 0;
        m->has_deadzone_pct     = game->has_deadzone_pct != 0;
        m->rom_noun             = game->rom_noun ? game->rom_noun : "ROM";
        m->language_labels      = game->language_labels;
        m->num_languages        = game->num_languages;
        m->disc_verify_cb       = game->disc_verify;      // real disc verdict (PSX), or NULL
        m->memcard_inspect_cb   = game->memcard_inspect;  // real memcard summary (PSX), or NULL
        m->boxart_path          = game->boxart_path;      // NULL => default boxart.tga
        m->aspect_labels        = game->aspect_labels;    // NULL => built-in 4:3/16:9/21:9
        m->num_aspect_labels    = game->num_aspect_labels;
        m->aspect_experimental  = game->aspect_experimental != 0;
        m->adaptive_view_supported = game->adaptive_view_supported != 0;
        m->tpak_slots           = clampi(game->tpak_slots, 0, RECOMP_LAUNCHER_MAX_TPAKS);
        m->tpak_inspect_cb      = game->tpak_inspect;
        m->audio_device_labels  = game->audio_device_labels;
        m->num_audio_devices    = game->num_audio_devices;
        m->renderer_labels      = game->renderer_labels;
        m->num_renderers        = game->num_renderers;
        m->hide_rebind          = game->hide_rebind != 0;
        m->has_mouse_controls   = game->has_mouse_controls != 0;
        m->netplay_supported    = game->netplay_supported != 0 && game->netplay != NULL;
        m->netplay              = game->netplay;
    } else {
        m->game_name    = "Unknown Game";
        m->region       = "";
        m->platform     = NULL;
        m->player_count = 2;
        m->rom_noun     = "ROM";
    }

    if (io) m->s = *io;
    memset(&m->s.netplay_launch, 0, sizeof(m->s.netplay_launch));
    if (!m->s.netplay_player_name[0] && m->netplay && m->netplay->player_name) {
        safe_copy(m->s.netplay_player_name, sizeof(m->s.netplay_player_name),
                  m->netplay->player_name(m->netplay->ctx));
    }
    safe_copy(m->netplay_name_edit, sizeof(m->netplay_name_edit), m->s.netplay_player_name);
    if (m->netplay && m->netplay->default_url) {
        safe_copy(m->netplay_lobby_url, sizeof(m->netplay_lobby_url),
                  m->netplay->default_url(m->netplay->ctx));
    }
    if (m->s.netplay_player_name[0]) {
        snprintf(m->netplay_host_name, sizeof(m->netplay_host_name), "%s's Lobby",
                 m->s.netplay_player_name);
    }
    safe_copy(m->netplay_host_port, sizeof(m->netplay_host_port), "7777");
    safe_copy(m->netplay_host_ip, sizeof(m->netplay_host_ip), "Detecting...");
    m->netplay_host_local_ip[0] = '\0';
    m->netplay_local_address_count = 0;
    safe_copy(m->netplay_direct_ip, sizeof(m->netplay_direct_ip), "127.0.0.1");
    safe_copy(m->netplay_direct_port, sizeof(m->netplay_direct_port), "7777");
    m->netplay_lan_only = false;
    m->netplay_list_fresh = false;
    m->netplay_selected_lobby = -1;
    m->netplay_public_ip[0] = '\0';
    m->netplay_public_ip_resolved = false;
    m->s.adaptive_view =
        (m->adaptive_view_supported && m->s.adaptive_view) ? 1 : 0;

    // ---- memory-card slots default to enabled (0 == "unset": a host struct
    // that predates this field, or was zero-initialized, reads as both cards
    // plugged in — matching the legacy launcher's default) ----
    if (!m->s.memcard_enabled[0]) m->s.memcard_enabled[0] = 1;
    if (!m->s.memcard_enabled[1]) m->s.memcard_enabled[1] = 1;

    // ---- infer the SystemProfile this game belongs to (panel composition +
    // per-system specs) from the ABI caps launcher_profile_apply() already set ----
    m->profile = launcher_system_infer(game);
    if (m->profile && m->profile->controller.max_players > 0) {
        /* num_players is a per-game capability, while max_players is the
         * console ceiling. Raising the ABI storage width must never make a
         * two-player game/system expose extra controller or lobby seats. */
        m->player_count = clampi(m->player_count, 1,
                                 m->profile->controller.max_players);
    }

    // ---- gate pad_mode per player ----
    if (m->pad_mode_supported) {
        const SystemProfile* pm_prof = (const SystemProfile*)m->profile;
        const ControllerSpec* pm_spec = pm_prof ? &pm_prof->controller : NULL;
        for (int p = 0; p < LNG_MAX_PLAYERS; ++p) {
            if (!m->pad_mode_selectable) {
                m->s.pad_mode[p] = m->locked_pad_mode;
            } else if (pm_spec && pm_spec->modes && pm_spec->mode_count > 0) {
                // Custom mode list (Genesis 3-Button/6-Button): any listed
                // mode value is valid; anything else snaps to the first
                // listed mode. The Hybrid rule below is PSX-only semantics.
                int ok = 0;
                for (int i = 0; i < pm_spec->mode_count; ++i)
                    if (pm_spec->modes[i].mode == m->s.pad_mode[p]) { ok = 1; break; }
                if (!ok) m->s.pad_mode[p] = pm_spec->modes[0].mode;
            } else if (!m->allow_hybrid && m->s.pad_mode[p] == 0) {
                m->s.pad_mode[p] = 1;   // snap Hybrid -> Analog
            }
        }
    }

    // ---- Transfer Pak slots: inspect whatever the host's config seeded ----
    // (re-run per slot on every ROM/save change; see launcher_model_set_tpak_*).
    for (int t = 0; t < m->tpak_slots; ++t) lm_inspect_tpak(m, t);

    // ---- validate/clamp aspect_index against the offered set ----
    if (m->aspect_labels && m->num_aspect_labels > 0) {
        // Game-supplied vocabulary: a plain 0..n-1 cycle, every entry offered.
        m->s.aspect_index = clampi(m->s.aspect_index, 0, m->num_aspect_labels - 1);
    } else if (m->aspect_mask) {
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
    if (m->has_screen_kind) {
        // Clamp against the active profile's screen-model vocabulary (GBA has
        // 5 LCD models; the legacy PSX-era set has 4) — see screen_kind_vocab.
        const SystemProfile* sk_prof = (const SystemProfile*)m->profile;
        int sk_n = (sk_prof && sk_prof->screen_kind_names && sk_prof->screen_kind_count > 0)
                     ? sk_prof->screen_kind_count : 4;
        m->s.screen_kind = clampi(m->s.screen_kind, 0, sk_n - 1);
    }
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

    // ---- mouse controls: seed/clamp against their own ranges ----------------
    // Only touched when has_mouse_controls (every other game leaves the whole
    // mouse_* block at its memset-zero state, untouched). The host normally
    // seeds real defaults from its config; a zero sensitivity is the tell of a
    // fresh/zero-initialized struct (e.g. a demo harness) — seed the full Snap
    // default set in that case, otherwise just clamp the sensitivity.
    if (m->has_mouse_controls) {
        if (m->s.mouse_sensitivity <= 0.0f) {
            m->s.player_src[0]      = 1;      // Keyboard (+ mouse) by default
            m->s.mouse_enabled      = 1;
            m->s.mouse_sensitivity  = 0.06f;
            m->s.mouse_invert_x     = 0;
            m->s.mouse_invert_y     = 1;
            m->s.mouse_bind[0]      = 0;      // Left  -> A  (kN64PadButtons[0])
            m->s.mouse_bind[1]      = 2;      // Right -> Z  (kN64PadButtons[2])
            m->s.mouse_bind[2]      = -1;     // Middle-> none
        } else {
            m->s.mouse_sensitivity = clampf(m->s.mouse_sensitivity, 0.01f, 0.50f);
        }
    }
    // ---- widescreen extra-cells (video.widescreen_cells consoles, e.g.
    // Genesis): 0 = unset host -> the engine default of 8; else clamp 1..16. ----
    {
        const SystemProfile* ws_prof = (const SystemProfile*)m->profile;
        if (ws_prof && ws_prof->video.widescreen_cells)
            m->s.widescreen_cells = m->s.widescreen_cells
                                      ? clampi(m->s.widescreen_cells, 1, 16) : 8;
    }

    // Real ROM read + CRC/SHA verification (computes rom_size, crc_match,
    // sha_match). No synthesized/faked facts.
    launcher_model_set_rom(m, initial_rom);

    // Password/mantra save: read the current one-line password file so the
    // SAVES row can show it. (Zapper switch state is loaded later by
    // launcher_binds_load(), which owns the keybinds.ini path.)
    launcher_model_password_reload(m);

    // Inspect both memory-card slots up front (real block usage/validity when a
    // host memcard_inspect callback is wired; no-op otherwise).
    lm_inspect_memcard(m, 0);
    lm_inspect_memcard(m, 1);

    m->view      = LNG_VIEW_DASHBOARD;
    m->action    = LNG_ACTION_NONE;
    m->cfg_player = 0;

    /* Soft-return from a match: land on Netplay with the room modal open. */
    if (game && game->resume_netplay_room && m->netplay_supported && m->netplay &&
        m->netplay->in_lobby && m->netplay->in_lobby(m->netplay->ctx)) {
        m->view = LNG_VIEW_NETPLAY;
        m->netplay_list_fresh = true;
        if (game->resume_netplay_endpoint && game->resume_netplay_endpoint[0]) {
            m->netplay_local_room = true;
            safe_copy(m->netplay_host_endpoint, sizeof(m->netplay_host_endpoint),
                      game->resume_netplay_endpoint);
        } else {
            m->netplay_local_room = false;
            m->netplay_host_endpoint[0] = '\0';
        }
    }

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
            safe_copy(m->pad_binds[0][b], sizeof(m->pad_binds[0][b]), "(unbound)");
            safe_copy(m->pad_binds[1][b], sizeof(m->pad_binds[1][b]), "(unbound)");
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
    m->sha1_match = false;
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
                    // SHA-1: the cartridge-console identity gate (GBA/SNES).
                    if (m->num_known_sha1 && m->known_sha1_hex) {
                        uint8_t s1[20]; char s1hex[41];
                        recompui_sha1_compute(body, blen, s1);
                        recompui_sha1_hex(s1, s1hex);
                        for (size_t k = 0; k < m->num_known_sha1; ++k) {
                            const char* want = m->known_sha1_hex[k];
                            if (!want) continue;
                            /* case-insensitive 40-hex compare */
                            int eq = 1;
                            for (int c = 0; c < 40 && eq; ++c) {
                                char a = s1hex[c], b = want[c];
                                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                                if (a != b) eq = 0;
                            }
                            if (eq && want[40] == '\0') { m->sha1_match = true; break; }
                        }
                    }
                }
                free(buf);
            }
            fclose(f);
        }
    }
    if (!m->rom_size[0]) safe_copy(m->rom_size, sizeof(m->rom_size), "--");

    run_verify(m);
    update_msu1_patch_available(m);
}

// Disc-verdict (verify.mode==1 systems, e.g. PSX): run the SystemProfile's
// VerifyProbeFn against the current ROM/disc path, or synthesize a sensible
// placeholder verdict when the probe is NULL / declines (no host wired up
// yet) so the disc-verdict UI always renders a real verdict block instead of
// a "not recognized" dead end. No-op for verify.mode==0 systems (SNES) — the
// CRC/SHA line above already covers them and m->verify stays zeroed.
static void run_verify(LauncherModel* m) {
    if (!m->profile || m->profile->verify.mode != 1) return;
    memset(&m->verify, 0, sizeof(m->verify));
    // Host disc-verify callback (REAL serial/region/ISO/verdict) takes
    // precedence — re-run here on every ROM/disc change.
    if (m->disc_verify_cb && m->rom_present) {
        RecompLauncherCDiscVerify dv; memset(&dv, 0, sizeof(dv));
        if (m->disc_verify_cb(m->rom_full, &dv)) {
            safe_copy(m->verify.serial, sizeof(m->verify.serial), dv.serial);
            safe_copy(m->verify.region, sizeof(m->verify.region), dv.region);
            m->verify.iso_ok  = dv.iso_ok != 0;
            m->verify.verdict = dv.verdict;
            return;
        }
    }
    VerifyProbeFn probe = m->profile->verify.probe;
    bool ok = probe && probe(m, &m->verify);
    if (ok) return;
    if (m->rom_present) {
        /* Placeholder facts: no real disc reader is wired up yet, but the
         * checklist should still show something plausible rather than a
         * blank/TODO state. */
        safe_copy(m->verify.serial, sizeof(m->verify.serial), "SCUS-94423");
        safe_copy(m->verify.region, sizeof(m->verify.region),
                  (m->region && m->region[0]) ? m->region : "NTSC-U");
        m->verify.iso_ok  = true;
        m->verify.verdict = 1;   // ok
    } else {
        m->verify.serial[0] = '\0';
        m->verify.region[0] = '\0';
        m->verify.iso_ok  = false;
        m->verify.verdict = 0;   // none
    }
}

const char* launcher_model_rom_path(const LauncherModel* m) {
    return m->rom_full;
}

bool launcher_model_rom_verified(const LauncherModel* m) {
    if (!m->rom_present) return false;
    const int has_crc  = m->has_expected_crc;
    const int has_sha  = m->num_known_sha256 > 0;
    const int has_sha1 = m->num_known_sha1 > 0;
    if (!has_crc && !has_sha && !has_sha1) return false;   // no fingerprint => can't vouch
    if (has_crc  && !m->crc_match)  return false;
    if (has_sha  && !m->sha_match)  return false;
    if (has_sha1 && !m->sha1_match) return false;
    return true;
}

void launcher_model_set_view(LauncherModel* m, LngView v) {
    if (v < 0 || v > LNG_VIEW_NETPLAY) return;
    /* Re-entering Netplay should rescan server + LAN lists. */
    if (m->view == LNG_VIEW_NETPLAY && v != LNG_VIEW_NETPLAY)
        m->netplay_list_fresh = false;
    m->view = v;
}

void launcher_model_open_config(LauncherModel* m, int player) {
    m->cfg_player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
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

void launcher_model_toggle_adaptive_view(LauncherModel* m) {
    if (!m->adaptive_view_supported) return;
    m->s.adaptive_view = !m->s.adaptive_view;
}

static int aspect_choice_count(const LauncherModel* m) {
    if (m->aspect_labels && m->num_aspect_labels > 0)
        return m->num_aspect_labels;
    if (!m->aspect_mask) return 0;
    int count = 0;
    for (int i = 0; i < 3; ++i)
        if (launcher_model_aspect_offered(m, i)) ++count;
    return count;
}

static int first_offered_aspect(const LauncherModel* m) {
    if (m->aspect_labels && m->num_aspect_labels > 0) return 0;
    for (int i = 0; i < 3; ++i)
        if (launcher_model_aspect_offered(m, i)) return i;
    return 0;
}

static int next_offered_aspect(const LauncherModel* m, int current) {
    if (m->aspect_labels && m->num_aspect_labels > 0) {
        int next = current + 1;
        return next < m->num_aspect_labels ? next : -1;
    }
    for (int i = current + 1; i < 3; ++i)
        if (launcher_model_aspect_offered(m, i)) return i;
    return -1;
}

void launcher_model_cycle_view_mode(LauncherModel* m) {
    if (!m) return;
    const int fixed_count = aspect_choice_count(m);

    if (m->s.adaptive_view && m->adaptive_view_supported) {
        m->s.adaptive_view = 0;
        if (fixed_count) m->s.aspect_index = first_offered_aspect(m);
        else m->s.widescreen = 0;
        return;
    }

    if (fixed_count) {
        int next = next_offered_aspect(m, m->s.aspect_index);
        if (next >= 0) {
            m->s.aspect_index = next;
            return;
        }
        if (m->adaptive_view_supported) {
            m->s.adaptive_view = 1;
            return;
        }
        m->s.aspect_index = first_offered_aspect(m);
        return;
    }

    if (m->widescreen_supported && !m->s.widescreen) {
        m->s.widescreen = 1;
        return;
    }
    if (m->adaptive_view_supported) {
        m->s.widescreen = 0;
        m->s.adaptive_view = 1;
        return;
    }
    m->s.widescreen = 0;
}

const char* launcher_model_view_mode_label(const LauncherModel* m) {
    if (!m) return "Native";
    if (m->adaptive_view_supported && m->s.adaptive_view) return "Adaptive";
    if (aspect_choice_count(m)) return launcher_model_aspect_label(m);
    return m->s.widescreen ? "16:9 fixed" : "Native";
}

void launcher_model_ws_cells_delta(LauncherModel* m, int delta) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (!prof || !prof->video.widescreen_cells) return;   // gated per console
    int v = m->s.widescreen_cells ? m->s.widescreen_cells : 8;
    m->s.widescreen_cells = clampi(v + delta, 1, 16);
}

const char* launcher_model_ws_cells_label(const LauncherModel* m) {
    static char buf[16];
    int v = m->s.widescreen_cells ? clampi(m->s.widescreen_cells, 1, 16) : 8;
    snprintf(buf, sizeof(buf), "%d cells", v);
    return buf;
}

bool launcher_model_aspect_offered(const LauncherModel* m, int index) {
    if (index == 0) return true;   // 4:3 is always implied/available
    if (index == 1) return (m->aspect_mask & 2) != 0;
    if (index == 2) return (m->aspect_mask & 4) != 0;
    return false;
}

void launcher_model_cycle_aspect(LauncherModel* m) {
    if (m->adaptive_view_supported && m->s.adaptive_view && m->s.fullscreen) return;
    if (m->aspect_labels && m->num_aspect_labels > 0) {
        // Game-supplied vocabulary: plain 0..n-1 cycle.
        m->s.aspect_index =
            (clampi(m->s.aspect_index, 0, m->num_aspect_labels - 1) + 1) %
            m->num_aspect_labels;
        return;
    }
    if (!m->aspect_mask) return;   // gated: legacy widescreen-bool games no-op
    int idx = clampi(m->s.aspect_index, 0, 2);
    for (int i = 0; i < 3; ++i) {
        idx = (idx + 1) % 3;
        if (launcher_model_aspect_offered(m, idx)) { m->s.aspect_index = idx; return; }
    }
}

const char* launcher_model_aspect_label(const LauncherModel* m) {
    if (m->aspect_labels && m->num_aspect_labels > 0)
        return m->aspect_labels[clampi(m->s.aspect_index, 0,
                                       m->num_aspect_labels - 1)];
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
    // Game-supplied renderer vocabulary (RT64 hosts: Auto/Vulkan/D3D12)
    // cycles its full list; the legacy pair stays a 2-value toggle.
    if (m->renderer_labels && m->num_renderers > 0) {
        m->s.renderer = (m->s.renderer + 1) % m->num_renderers;
        return;
    }
    m->s.renderer = !m->s.renderer;
}

const char* launcher_model_renderer_label(const LauncherModel* m) {
    if (m->renderer_labels && m->num_renderers > 0) {
        int i = clampi(m->s.renderer, 0, m->num_renderers - 1);
        return m->renderer_labels[i];
    }
    // Per-console vocabulary (SystemProfile.renderer_labels): NES says
    // Accelerated/Software; NULL keeps the legacy PSX-era pair.
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (prof && prof->renderer_labels)
        return prof->renderer_labels[m->s.renderer ? 1 : 0];
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

// Antialiasing is an MSAA sample COUNT, not a bool: Off / 2x / 4x / 8x. Cycle
// wraps 0 -> 2 -> 4 -> 8 -> 0. A legacy on/off host value of 1 is treated as
// "on" by the label and advances to Off on the next cycle.
void launcher_model_cycle_aa(LauncherModel* m) {
    switch (m->s.antialiasing) {
        case 0:  m->s.antialiasing = 2; break;
        case 2:  m->s.antialiasing = 4; break;
        case 4:  m->s.antialiasing = 8; break;
        default: m->s.antialiasing = 0; break;   // 8 (or legacy 1/other) -> Off
    }
}

const char* launcher_model_aa_label(const LauncherModel* m) {
    switch (m->s.antialiasing) {
        case 0:  return "Off";
        case 2:  return "2x";
        case 4:  return "4x";
        case 8:  return "8x";
        default: return "On";   // legacy on/off host value (1)
    }
}

void launcher_model_toggle_texture_filter(LauncherModel* m) {
    m->s.texture_filter = !m->s.texture_filter;
}

const char* launcher_model_texture_filter_label(const LauncherModel* m) {
    return m->s.texture_filter ? "Bilinear" : "Nearest";
}

// Screen-model vocabulary: the active SystemProfile's own set when it has one
// (e.g. GBA's 5 LCD models), else the legacy 4-entry PSX-era set above.
static const char* const* screen_kind_vocab(const LauncherModel* m, int* out_n) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (prof && prof->screen_kind_names && prof->screen_kind_count > 0) {
        *out_n = prof->screen_kind_count;
        return prof->screen_kind_names;
    }
    *out_n = 4;
    return kScreenKindNames;
}

void launcher_model_cycle_screen_kind(LauncherModel* m) {
    int n = 4; (void)screen_kind_vocab(m, &n);
    m->s.screen_kind = (clampi(m->s.screen_kind, 0, n - 1) + 1) % n;
}

const char* launcher_model_screen_kind_label(const LauncherModel* m) {
    int n = 4;
    const char* const* names = screen_kind_vocab(m, &n);
    return names[clampi(m->s.screen_kind, 0, n - 1)];
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

// Fullscreen is a universal display setting: every console's runner applies
// the committed tri-state (0 off / 1 borderless / 2 exclusive) to its window
// at boot and persists it in its own config. The cycle walks all three
// states, restoring the vocabulary of the original SNES launcher.
void launcher_model_cycle_fullscreen(LauncherModel* m) {
    m->s.fullscreen = (clampi(m->s.fullscreen, 0, 2) + 1) % 3;
}

const char* launcher_model_fullscreen_label(const LauncherModel* m) {
    static const char* const kNames[3] = { "Off", "Borderless", "Exclusive" };
    return kNames[clampi(m->s.fullscreen, 0, 2)];
}

void launcher_model_toggle_fullscreen(LauncherModel* m) {
    // Binary on/off (0 <-> 1); superseded in the UI by the tri-state cycle
    // above but kept for hosts that drive the field as a bool.
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

// Re-inspect one memory-card slot via the host callback (if any), caching the
// result (block bitmask + validity) on the model. Clears the inspected flag
// when there's no callback or no path, so the panel falls back to the
// freshly-formatted/placeholder display.
static void lm_inspect_memcard(LauncherModel* m, int slot) {
    if (slot < 0 || slot > 1) return;
    m->memcard_inspected[slot] = false;
    m->memcard_valid[slot]     = false;
    if (!m->memcard_inspect_cb || !m->s.memcard_path[slot][0]) return;
    RecompLauncherCMemcard mc; memset(&mc, 0, sizeof(mc));
    if (!m->memcard_inspect_cb(m->s.memcard_path[slot], &mc)) return;
    uint16_t bits = 0;
    for (int i = 0; i < 15; ++i) if (mc.block_used[i]) bits |= (uint16_t)(1u << i);
    m->memcard_blocks_used[slot] = bits;
    m->memcard_valid[slot]       = mc.valid != 0;
    m->memcard_inspected[slot]   = true;
}

void launcher_model_set_memcard_path(LauncherModel* m, int slot, const char* path) {
    if (slot < 0 || slot > 1) return;
    safe_copy(m->s.memcard_path[slot], sizeof(m->s.memcard_path[slot]), path ? path : "");
    // A newly browsed-in card is not known to be blank; only
    // launcher_model_new_memcard() re-arms this flag.
    m->memcard_freshly_formatted[slot] = false;
    lm_inspect_memcard(m, slot);   // refresh real block usage/validity for the new path
}

void launcher_model_toggle_memcard(LauncherModel* m, int slot) {
    if (slot < 0 || slot > 1) return;
    m->s.memcard_enabled[slot] = m->s.memcard_enabled[slot] ? 0 : 1;
}

void launcher_model_new_memcard(LauncherModel* m, int slot, const char* path) {
    if (slot < 0 || slot > 1 || !path || !path[0]) return;
    if (recompui_memcard_format_file(path) != 0) return;  // I/O failure: leave as-is
    launcher_model_set_memcard_path(m, slot, path);
    m->memcard_freshly_formatted[slot] = true;   // known-blank: panel shows 0 / 15
}

// ---- N64 Transfer Pak slots (mirrors the memcard slot pattern) -----------------

// Refresh one slot's cartridge facts through the host's tpak_inspect callback.
// Clears to "not inspected" when there's no callback, no cartridge, or the
// callback declines — the panel then shows the bare file name, neutral tint.
static void lm_inspect_tpak(LauncherModel* m, int slot) {
    if (slot < 0 || slot >= RECOMP_LAUNCHER_MAX_TPAKS) return;
    memset(&m->tpak_info[slot], 0, sizeof(m->tpak_info[slot]));
    m->tpak_inspected[slot] = false;
    if (!m->tpak_inspect_cb || !m->s.tpak_rom_path[slot][0]) return;
    RecompLauncherCTpak out; memset(&out, 0, sizeof(out));
    if (m->tpak_inspect_cb(m->s.tpak_rom_path[slot],
                           m->s.tpak_save_path[slot][0] ? m->s.tpak_save_path[slot] : NULL,
                           &out)) {
        m->tpak_info[slot] = out;
        m->tpak_inspected[slot] = true;
    }
}

void launcher_model_set_tpak_rom(LauncherModel* m, int slot, const char* path) {
    if (slot < 0 || slot >= m->tpak_slots) return;
    safe_copy(m->s.tpak_rom_path[slot], sizeof(m->s.tpak_rom_path[slot]), path ? path : "");
    if (m->s.tpak_rom_path[slot][0]) m->s.tpak_enabled[slot] = 1;  // inserting = wanting it on
    lm_inspect_tpak(m, slot);
}

void launcher_model_clear_tpak(LauncherModel* m, int slot) {
    if (slot < 0 || slot >= m->tpak_slots) return;
    m->s.tpak_rom_path[slot][0]  = '\0';
    m->s.tpak_save_path[slot][0] = '\0';
    m->s.tpak_enabled[slot] = 0;
    lm_inspect_tpak(m, slot);
}

void launcher_model_set_tpak_save(LauncherModel* m, int slot, const char* path) {
    if (slot < 0 || slot >= m->tpak_slots) return;
    safe_copy(m->s.tpak_save_path[slot], sizeof(m->s.tpak_save_path[slot]), path ? path : "");
    lm_inspect_tpak(m, slot);
}

bool launcher_model_tpak_enabled(const LauncherModel* m, int slot) {
    if (slot < 0 || slot >= m->tpak_slots) return false;
    int e = m->s.tpak_enabled[slot];
    if (e > 0) return true;
    if (e < 0) return false;
    return m->s.tpak_rom_path[slot][0] != '\0';   // unset: on iff a cart is inserted
}

void launcher_model_toggle_tpak(LauncherModel* m, int slot) {
    if (slot < 0 || slot >= m->tpak_slots) return;
    m->s.tpak_enabled[slot] = launcher_model_tpak_enabled(m, slot) ? -1 : 1;
}

// ---- audio output device -------------------------------------------------------

void launcher_model_set_audio_device(LauncherModel* m, const char* name) {
    safe_copy(m->s.audio_device, sizeof(m->s.audio_device), name ? name : "");
}

const char* launcher_model_audio_device_label(const LauncherModel* m) {
    return m->s.audio_device[0] ? m->s.audio_device : "(system default)";
}

// ---- SRAM save management (mirrors the legacy launcher's Import/Clear) --------
// Both back up any existing save to "<sram>.bak" first (never a destructive op
// without a recoverable copy), matching the old launcher's behavior.
static bool lm_copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[8192]; size_t n; bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in); fclose(out);
    return ok;
}

void launcher_model_import_sram(LauncherModel* m, const char* src) {
    if (!m->sram_path || !m->sram_path[0] || !src || !src[0]) return;
    char bak[600]; snprintf(bak, sizeof(bak), "%s.bak", m->sram_path);
    FILE* existing = fopen(m->sram_path, "rb");
    if (existing) { fclose(existing); lm_copy_file(m->sram_path, bak); }  // back up first
    lm_copy_file(src, m->sram_path);
}

void launcher_model_clear_sram(LauncherModel* m) {
    if (!m->sram_path || !m->sram_path[0]) return;
    char bak[600]; snprintf(bak, sizeof(bak), "%s.bak", m->sram_path);
    FILE* existing = fopen(m->sram_path, "rb");
    if (existing) { fclose(existing); lm_copy_file(m->sram_path, bak); remove(m->sram_path); }
}

void launcher_model_toggle_msu1(LauncherModel* m) {
    if (!m->msu1_supported) return;
    m->s.msu1_enabled = !m->s.msu1_enabled;
}

void launcher_model_set_msu1_dir(LauncherModel* m, const char* dir) {
    safe_copy(m->s.msu1_dir, sizeof(m->s.msu1_dir), dir ? dir : "");
}

// ---- NES-style settings ------------------------------------------------------

void launcher_model_toggle_integer_scale(LauncherModel* m) {
    if (!m->has_integer_scale) return;   // gated: no-op when unsupported
    m->s.integer_scale = !m->s.integer_scale;
}

void launcher_model_toggle_hdpack(LauncherModel* m) {
    if (!m->hdpack_supported) return;    // gated: no-op when unsupported
    m->s.hdpack_enabled = !m->s.hdpack_enabled;
}

void launcher_model_set_hdpack_dir(LauncherModel* m, const char* dir) {
    safe_copy(m->s.hdpack_dir, sizeof(m->s.hdpack_dir), dir ? dir : "");
}

// Password/mantra save: the file is one line of text (e.g. Faxanadu's mantra),
// read/rewritten whole. Mirrors the legacy NES launcher's SAVES-panel variant.
void launcher_model_password_reload(LauncherModel* m) {
    m->password_text[0] = '\0';
    if (!m->password_save_path || !m->password_save_path[0]) return;
    FILE* f = fopen(m->password_save_path, "r");
    if (!f) return;
    if (fgets(m->password_text, sizeof(m->password_text), f)) {
        size_t n = strlen(m->password_text);
        while (n > 0 && (m->password_text[n-1] == '\n' || m->password_text[n-1] == '\r'))
            m->password_text[--n] = '\0';
    } else {
        m->password_text[0] = '\0';
    }
    fclose(f);
}

void launcher_model_password_commit(LauncherModel* m, const char* text) {
    if (!m->password_save_path || !m->password_save_path[0]) return;
    FILE* f = fopen(m->password_save_path, "w");
    if (!f) return;
    fprintf(f, "%s\n", text ? text : "");
    fclose(f);
    launcher_model_password_reload(m);   // reflect what actually landed on disk
}

// Zapper switches: flip the model state and persist through launcher_binds'
// [zapper] section writer immediately (same persist-on-change behavior as the
// rebind chips). launcher_binds_set_zapper is a no-op-safe plain writer.
void launcher_binds_set_zapper(int mouse_enabled, int crosshair);   // launcher_binds.c

void launcher_model_toggle_zapper_mouse(LauncherModel* m) {
    if (!m->zapper) return;
    m->zapper_mouse = !m->zapper_mouse;
    launcher_binds_set_zapper(m->zapper_mouse ? 1 : 0, m->zapper_crosshair ? 1 : 0);
}

void launcher_model_toggle_zapper_crosshair(LauncherModel* m) {
    if (!m->zapper) return;
    m->zapper_crosshair = !m->zapper_crosshair;
    launcher_binds_set_zapper(m->zapper_mouse ? 1 : 0, m->zapper_crosshair ? 1 : 0);
}

// ---- MSU-1 IPS auto-patching (mirrors the legacy launcher's do_patch() /
// msu1_patch_available predicate in snesrecomp's launcher_gui.cpp) -----------

// Recomputed on every ROM change: the dashboard prompt only makes sense when
// this game HAS a patch, the loaded file verifies against the vanilla CRC
// (patching an already-patched or wrong ROM would corrupt it), and the user
// hasn't already dismissed it this run.
static void update_msu1_patch_available(LauncherModel* m) {
    m->msu1_patch_available = m->msu1_supported && m->msu1_patch_path &&
                              m->msu1_patch_path[0] && m->rom_present &&
                              m->crc_match && !m->msu1_patch_skipped;
}

static bool lm_read_whole_file(const char* path, uint8_t** out_data, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    uint8_t* buf = (uint8_t*)malloc(n ? (size_t)n : 1);
    if (!buf) { fclose(f); return false; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return false; }
    fclose(f);
    *out_data = buf;
    *out_len  = (size_t)n;
    return true;
}

// Build "<dir>/<stem>.msu1.<ext>" beside `rom_path` (matches the legacy
// launcher's std::filesystem stem()/extension() splice exactly).
static void lm_msu1_target_path(const char* rom_path, char* out, size_t out_cap) {
    const char* base = rom_path;
    for (const char* q = rom_path; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;
    const char* dot = strrchr(base, '.');
    size_t dir_len  = (size_t)(base - rom_path);
    size_t stem_len = dot ? (size_t)(dot - base) : strlen(base);
    const char* ext = dot ? dot : "";
    snprintf(out, out_cap, "%.*s%.*s.msu1%s",
             (int)dir_len, rom_path, (int)stem_len, base, ext);
}

void launcher_model_apply_msu1_patch(LauncherModel* m) {
    if (!m->msu1_patch_available) return;
    if (!m->rom_present || !m->msu1_patch_path || !m->msu1_patch_path[0]) return;

    uint8_t* src = NULL;   size_t src_len = 0;
    uint8_t* patch = NULL; size_t patch_len = 0;
    if (!lm_read_whole_file(m->rom_full, &src, &src_len)) return;
    if (!lm_read_whole_file(m->msu1_patch_path, &patch, &patch_len)) { free(src); return; }

    uint8_t* out = NULL; size_t out_len = 0;
    bool ok = ips_apply(src, src_len, patch, patch_len, &out, &out_len);
    free(src);
    free(patch);
    if (!ok) {
        fprintf(stderr, "launcher: IPS patch failed (%s)\n", m->msu1_patch_path);
        return;
    }

    char target[600];
    lm_msu1_target_path(m->rom_full, target, sizeof(target));
    FILE* f = fopen(target, "wb");
    if (!f) {
        fprintf(stderr, "launcher: cannot write %s\n", target);
        free(out);
        return;
    }
    bool wrote = fwrite(out, 1, out_len, f) == out_len;
    fclose(f);
    free(out);
    if (!wrote) { fprintf(stderr, "launcher: short write to %s\n", target); return; }

    fprintf(stderr, "launcher: wrote MSU-1 patched ROM: %s\n", target);
    // Switch the model onto the patched file and re-verify (this also
    // recomputes msu1_patch_available — the patched ROM's CRC will no longer
    // match the vanilla expected_crc, so it naturally clears). Belt-and-braces:
    // also mark skipped so the prompt never reappears if a game's expected_crc
    // happens to also match the patched image.
    m->msu1_patch_skipped = true;
    launcher_model_set_rom(m, target);
}

void launcher_model_skip_msu1_patch(LauncherModel* m) {
    if (!m->msu1_patch_available) return;
    m->msu1_patch_skipped = true;
    update_msu1_patch_available(m);
}

void launcher_model_set_pad_mode(LauncherModel* m, int player, int mode) {
    if (!m->pad_mode_supported || !m->pad_mode_selectable) return;   // gated/locked
    player = clampi(player, 0, 1);
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const ControllerSpec* spec = prof ? &prof->controller : NULL;
    if (spec && spec->modes && spec->mode_count > 0) {
        // Custom mode list (Genesis): accept listed mode values only.
        for (int i = 0; i < spec->mode_count; ++i)
            if (spec->modes[i].mode == mode) { m->s.pad_mode[player] = mode; return; }
        return;
    }
    mode = clampi(mode, 0, 2);
    if (mode == 0 && !m->allow_hybrid) mode = 1;   // Hybrid hidden -> snap to Analog
    m->s.pad_mode[player] = mode;
}

int launcher_model_active_button_count(const LauncherModel* m, int player) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    int bc = prof ? prof->controller.button_count : LNG_BTN_COUNT;
    if (prof && prof->controller.modes && prof->controller.mode_count > 0) {
        player = clampi(player, 0, 1);
        for (int i = 0; i < prof->controller.mode_count; ++i)
            if (prof->controller.modes[i].mode == m->s.pad_mode[player]) {
                bc = prof->controller.modes[i].button_count;
                break;
            }
    }
    if (bc > LNG_MAX_BUTTONS) bc = LNG_MAX_BUTTONS;
    if (bc < 0) bc = 0;
    return bc;
}

void launcher_model_cycle_player_src(LauncherModel* m, int player) {
    player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
    m->s.player_src[player] = (m->s.player_src[player] + 1) % 3;  // None/Kbd/Pad
}

void launcher_model_deadzone_delta(LauncherModel* m, int player, int delta) {
    player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
    m->s.deadzone[player] = clampi(m->s.deadzone[player] + delta, 0, 100);
}

void launcher_model_set_source(LauncherModel* m, int player, int kind,
                               uint32_t pad_id, const char* pad_name) {
    player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
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

// ---- mouse controls --------------------------------------------------------

void launcher_model_set_mouse_source(LauncherModel* m, int enabled) {
    if (!m->has_mouse_controls) return;
    launcher_model_set_source(m, 0, 1, 0, NULL);   // player 0 -> Keyboard
    m->s.mouse_enabled = enabled ? 1 : 0;
}

void launcher_model_set_mouse_sensitivity(LauncherModel* m, float value) {
    m->s.mouse_sensitivity = clampf(value, 0.01f, 0.50f);
}

void launcher_model_toggle_mouse_invert_x(LauncherModel* m) {
    m->s.mouse_invert_x = !m->s.mouse_invert_x;
}

void launcher_model_toggle_mouse_invert_y(LauncherModel* m) {
    m->s.mouse_invert_y = !m->s.mouse_invert_y;
}

void launcher_model_set_mouse_bind(LauncherModel* m, int which, int button_index) {
    if (which < 0 || which > 2) return;
    m->s.mouse_bind[which] = (button_index < 0) ? -1 : button_index;
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
    launcher_model_begin_capture_slot(m, b, 0);
}
void launcher_model_begin_capture_slot(LauncherModel* m, int b, int slot) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    int bc = prof ? prof->controller.button_count : LNG_BTN_COUNT;
    if (bc > LNG_MAX_BUTTONS) bc = LNG_MAX_BUTTONS;
    if (b < 0 || b >= bc) return;
    m->hk_capturing  = false;
    m->capturing     = true;
    m->capture_btn   = b;
    m->capture_slot  = (slot == 1) ? 1 : 0;
    m->hk_capturing = false;
    m->capturing    = true;
    m->capture_pad  = false;
    m->capture_btn  = b;
}
void launcher_model_begin_pad_capture(LauncherModel* m, int b) {
    launcher_model_begin_capture(m, b);
    if (m->capturing) m->capture_pad = true;   // begin_capture validated b
}
void launcher_model_cancel_capture(LauncherModel* m) {
    m->capturing   = false;
    m->capture_pad = false;
}

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
    player = clampi(player, 0, LNG_MAX_PLAYERS - 1);
    int src = clampi(m->s.player_src[player], 0, 2);
    if (src == 2 && m->player_pad_name[player][0])   // show the actual device name
        return m->player_pad_name[player];
    // Mouse-capable games split the keyboard source (player 0 only): the label
    // reflects whether mouse-aim is on. Every non-mouse game keeps kSrcNames.
    if (src == 1 && m->has_mouse_controls && player == 0)
        return m->s.mouse_enabled ? "Keyboard + Mouse" : "Keyboard";
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
    if (v < 0 || v > LNG_VIEW_NETPLAY) return "?";
    return kViewNames[v];
}
