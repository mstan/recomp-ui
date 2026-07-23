// launcher_ng_capi.c — the C ABI the game calls, backed by the launcher_ng UI.
//
// Implements recomp_launcher_run_window() (declared in recomp_launcher.h),
// the generic C ABI a host app calls to run the pre-boot launcher UI. Hosts
// seed the C structs, call the function, and read back the chosen ROM +
// settings.
//
// Returns: 0 = LAUNCH, 1 = QUIT, 2 = UNAVAILABLE (assets/GL failed → boot as if
// skipped), matching recomp_launcher.h.

#include "recomp_launcher.h"

#include "launcher_backend.h"
#include "launcher_binds.h"
#include "launcher_model.h"
#include "launcher_platform.h"
#include "launcher_theme.h"

#include <stdio.h>
#include <string.h>

/* Some WMs/title bars mishandle UTF-8 punctuation and show mojibake (e.g. "â"
 * for an em dash). Fold common dashes to ASCII before SDL_CreateWindow. */
static void title_ascii_dashes(char* dst, size_t dst_cap, const char* src) {
    if (!src) src = "Launcher";
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dst_cap; ) {
        const unsigned char a = (unsigned char)src[i];
        const unsigned char b = (unsigned char)src[i + 1];
        const unsigned char c = (unsigned char)src[i + 2];
        /* U+2013 en dash / U+2014 em dash */
        if (a == 0xE2u && b == 0x80u && (c == 0x93u || c == 0x94u)) {
            dst[o++] = '-';
            i += 3;
            continue;
        }
        dst[o++] = src[i++];
    }
    dst[o] = '\0';
}

int recomp_launcher_run_window(const char* window_title,
                             RecompLauncherCSettings* io,
                             const RecompLauncherCGameInfo* game,
                             const char* assets_dir,
                             const char* initial_rom,
                             char* out_rom_path, size_t out_rom_path_len) {
    (void)assets_dir;   // launcher_ng resolves assets next to the exe (SDL base path)

    char title[256];
    title_ascii_dashes(title, sizeof(title), window_title);

    LauncherPlatform plat;
    if (!launcher_platform_open(&plat, title, 1100, 840)) {
        // Window/GL init failed — tell the caller to boot as if the launcher was
        // skipped, exactly like the old launcher's UNAVAILABLE path.
        return 2;
    }

    LauncherModel model;
    launcher_model_init(&model, io, game, initial_rom);
    launcher_binds_load(&model, game ? game->config_path : NULL,
                                game ? game->keybinds_path : NULL);

    LauncherTheme theme = launcher_theme_by_name(game ? game->theme : NULL);

    LngAction act = launcher_backend_run(&plat, &model, &theme);

    launcher_platform_close(&plat);

    if (act == LNG_ACTION_LAUNCH) {
        launcher_model_commit(&model, io);   // edited settings back to the caller
        const char* rom = launcher_model_rom_path(&model);
        if (out_rom_path && out_rom_path_len) {
            if (rom && rom[0])
                snprintf(out_rom_path, out_rom_path_len, "%s", rom);
            else if (initial_rom)
                snprintf(out_rom_path, out_rom_path_len, "%s", initial_rom);
            else
                out_rom_path[0] = '\0';
        }
        return 0;   // LAUNCH
    }

    return 1;       // QUIT
}
