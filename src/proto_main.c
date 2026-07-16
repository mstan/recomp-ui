// proto_main.c — standalone self-test driver for recomp-ui.
#include <stdlib.h>
//
// Console-agnostic by construction: it fabricates the SAME C ABI structs any
// consuming host passes (here seeded with a neutral placeholder game), then
// runs the compiled-in Dear ImGui backend. This is the harness the recomp-ui
// repo itself builds to self-verify; a real host implements its own main()
// that calls recomp_launcher_run_window() (see recomp_launcher.h) with its
// own game facts.

#include "launcher_backend.h"
#include "launcher_binds.h"
#include "launcher_model.h"
#include "launcher_platform.h"
#include "launcher_profile.h"
#include "launcher_theme.h"

#include <SDL.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // ---- seed the C ABI structs the same way a real host would ----
    RecompLauncherCSettings s;
    memset(&s, 0, sizeof(s));
    s.output_method  = 2;      // OpenGL
    s.window_scale   = 3;
    s.fullscreen     = 0;
    s.linear_filter  = 0;
    s.widescreen     = 0;
    s.enable_audio   = 1;
    s.audio_freq     = 32000;
    s.volume         = 100;
    s.player_src[0]  = 1;      // keyboard
    s.player_src[1]  = 0;      // none
    s.skip_launcher  = 0;
    s.pad_mode[0]    = 0;      // Hybrid
    s.pad_mode[1]    = 0;
    s.aspect_index   = 1;      // 16:9

    // Neutral placeholder game: no CRC/SHA pinning, no MSU-1, no SRAM. Just
    // enough to exercise every panel that doesn't require real game facts.
    RecompLauncherCGameInfo gi;
    memset(&gi, 0, sizeof(gi));
    gi.name                 = "Recomp UI Test";
    gi.region               = "";
    gi.has_expected_crc     = 0;
    gi.widescreen_supported = 0;
    gi.msu1_supported       = 0;
    gi.sram_path            = NULL;
    gi.num_players          = 1;
    // One coherent VARIANT PROFILE picks the whole launcher identity so nothing
    // drifts (theme + controller + platform + rom_noun + capability set):
    //   LNG_VARIANT=psx  -> PlayStation (blue theme, DualShock, Disc, full PS settings)
    //   LNG_VARIANT=snes -> Super Nintendo (CRT theme, SNES pad, widescreen)
    // Unset = neutral default. See launcher_profile.h — one row per system.
    static const char* kPreviewLanguages[2] = { "English", "Japanese" };
    const char* variant = getenv("LNG_VARIANT");
    if (variant && variant[0]) {
        launcher_profile_apply(variant, &gi);
        if (lpr_is(variant, "psx") || lpr_is(variant, "ps1") || lpr_is(variant, "playstation")) {
            // Preview a PS1 title that offers the full surface (both wide aspects,
            // a language menu, a "configured" settings state).
            gi.aspect_mask     = 0x7;   // 4:3 + 16:9 + 21:9
            gi.language_labels = kPreviewLanguages;
            gi.num_languages   = 2;
            gi.num_players     = 1;
            s.window_width = 1280; s.renderer = 1; s.supersampling = 1;
            s.screen_kind = 1; s.frame_interp = 0; s.spu_hq = 1; s.aspect_index = 1;
        }
    }

    // Flip these to preview the 2-player + SRAM module set (what a save-game
    // capable, 2-player host contributes), e.g. LNG_DEMO_FULL=1.
    const char* demo = SDL_getenv("LNG_DEMO_FULL");
    if (demo && demo[0] == '1') {
        gi.num_players = 1;
        gi.sram_path   = "saves/save.srm";
        gi.widescreen_supported = 1;
        gi.msu1_supported = 1;
        gi.msu1_note = "Place the MSU-1 pack (.pcm + .msu) in this folder.";
    }
    if (demo && demo[0] == '2') {   // 2-player variant for layout testing
        gi.num_players = 2;
        gi.sram_path   = "saves/save.srm";
    }

    // Harness-only: exercises the MSU-1 dashboard "Patch ROM"/"Skip" flow
    // (launcher_model_apply_msu1_patch / launcher_model_skip_msu1_patch, drawn
    // in draw_game_panel — launcher_imgui.cpp). Points at a tiny fixture ROM
    // + IPS patch (test_data/) whose expected_crc matches the vanilla fixture
    // byte-for-byte, so msu1_patch_available comes up true without a real
    // game. See test_data/README-ish note in the fixture generator this was
    // produced from (ips_apply unit test) for the exact byte layout.
    const char* demo_msu_rom = NULL;
    const char* demo_msu = SDL_getenv("LNG_DEMO_MSU");
    if (demo_msu && demo_msu[0] == '1') {
        gi.msu1_supported   = 1;
        gi.msu1_note        = "This demo ROM has a tiny synthetic MSU-1 IPS patch "
                              "(test_data/msu1_demo.ips) for harness verification.";
        gi.msu1_patch_path  = "test_data/msu1_demo.ips";
        gi.has_expected_crc = 1;
        gi.expected_crc     = 0xA2B10169u;   // CRC32 of test_data/msu1_demo_vanilla.rom
        demo_msu_rom = "test_data/msu1_demo_vanilla.rom";
    }

    LauncherModel model;
    const char* rom = SDL_getenv("LNG_ROM");
    if (!rom || !rom[0]) rom = demo_msu_rom ? demo_msu_rom : "test.rom";
    launcher_model_init(&model, &s, &gi, rom);
    launcher_binds_load(&model, NULL, NULL);   // keybinds.ini + config.ini [KeyMap]
    fprintf(stderr, "[proto] rom=%s present=%d crc_match=%d sha_match=%d verified=%d size=%s\n",
            rom, model.rom_present, model.crc_match, model.sha_match,
            launcher_model_rom_verified(&model), model.rom_size);

    LauncherTheme theme = launcher_theme_by_name(gi.theme);

    char title[128];
    snprintf(title, sizeof(title), "Recomp UI — Launcher [%s]", launcher_backend_name());

    LauncherPlatform plat;
    if (!launcher_platform_open(&plat, title, 1100, 840)) {
        fprintf(stderr, "[proto] platform init failed; a real host would boot as if skipped.\n");
        return 2;
    }

    LngAction act = launcher_backend_run(&plat, &model, &theme);
    launcher_platform_close(&plat);

    // In production this is the value recomp_launcher_run_window() returns to
    // the host, which then boots the game IN-PROCESS with the committed
    // settings (0=LAUNCH, 1=QUIT). The standalone harness just reports it.
    if (act == LNG_ACTION_LAUNCH) {
        launcher_model_commit(&model, &s);
        printf("[proto] LAUNCH  scale=%s filter=%d freq=%d skip=%d rom=%s\n",
               launcher_model_scale_label(&model), s.linear_filter,
               s.audio_freq, s.skip_launcher, launcher_model_rom_path(&model));
    } else {
        printf("[proto] QUIT\n");
    }
    return 0;
}
