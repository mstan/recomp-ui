#ifdef NDEBUG
#undef NDEBUG
#endif
#include "recomp_launcher.h"
#include "launcher_profile.h"
#include "launcher_system.h"
#include <assert.h>
#include <string.h>
int main(void) {
    RecompLauncherCGameInfo game = {0};
    assert(launcher_profile_apply("virtualboy", &game));
    assert(game.settings_bindings == 1 && game.num_players == 1);
    assert(strcmp(game.platform, "VIRTUAL BOY") == 0);
    const SystemProfile* profile = &kSystemProfile_vb;
    assert(profile->controller.button_count == 14);
    assert(profile->controller.max_players == 1);
    assert(profile->save.kind == SAVE_NONE);
    assert(strcmp(profile->controller.buttons[0].label, "L-pad Up") == 0);
    assert(strcmp(profile->controller.buttons[4].label, "R-pad Up") == 0);
    for (int i = 0; i < 14; ++i) assert(profile->controller.buttons[i].code == i);
    assert(strcmp(profile->rom_filter.patterns[0], "*.vb") == 0);
    return 0;
}
