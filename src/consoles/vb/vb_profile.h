#ifndef RUI_CONSOLE_VB_PROFILE_H
#define RUI_CONSOLE_VB_PROFILE_H
#include "launcher_system_types.h"

/* Indices are host binding slots, not hardware pad-register bit numbers. */
static const ButtonDef kVbPadButtons[] = {
    { "L-pad Up", 0 }, { "L-pad Down", 1 },
    { "L-pad Left", 2 }, { "L-pad Right", 3 },
    { "R-pad Up", 4 }, { "R-pad Down", 5 },
    { "R-pad Left", 6 }, { "R-pad Right", 7 },
    { "A", 8 }, { "B", 9 }, { "L", 10 }, { "R", 11 },
    { "Start", 12 }, { "Select", 13 },
};
#define LNG_VB_PAD_BUTTON_COUNT 14
static const char* const kVbRomPatterns[] = { "*.vb", "*.vboy", "*.bin" };
static const char* const kPanelsSettingsVb[] = { "video", "audio", NULL };
static const SystemProfile kSystemProfile_vb = {
    "vb", "VIRTUAL BOY", "vb", "ROM",
    { kVbPadButtons, LNG_VB_PAD_BUTTON_COUNT, NULL, NULL, NULL, 1, 0, 1, NULL, 0, 1 },
    { SAVE_NONE, 0, NULL },
    { 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, NULL }, 0,
    kPanelsDashboardCommon, kPanelsSettingsVb, kPanelsControllerCommon,
    NULL, 0, { kVbRomPatterns, 3, "Virtual Boy ROM (.vb .vboy .bin)" },
    NULL, 1, "", NULL,
};
static inline int launcher_console_is_vb(const char* name) {
    return lps_streq_ci(name, "vb") || lps_streq_ci(name, "virtualboy");
}
static inline void launcher_profile_apply_vb(RecompLauncherCGameInfo* gi) {
    gi->theme = "vb";
    gi->platform = "VIRTUAL BOY";
    gi->rom_noun = "ROM";
    gi->num_players = 1;
    gi->settings_bindings = 1;
}
#endif
