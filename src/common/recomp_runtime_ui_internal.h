#ifndef RECOMP_RUNTIME_UI_INTERNAL_H
#define RECOMP_RUNTIME_UI_INTERNAL_H

#include "recomp_runtime_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

struct RecompRuntimeUi {
    RecompRuntimeUiConfig config;
    const char **sections;
    size_t section_count;
    size_t section_index;
    size_t row_index;
    int open;
    int in_section;
    unsigned status_frames;
    char status[48];
};

int recomp_runtime_ui_item_enabled(const RecompRuntimeUi *ui,
                                   const RecompRuntimeUiItem *item);
size_t recomp_runtime_ui_section_item_count(const RecompRuntimeUi *ui,
                                            size_t section);
const RecompRuntimeUiItem *recomp_runtime_ui_section_item(
    const RecompRuntimeUi *ui, size_t section, size_t row);
int recomp_runtime_ui_current_value(RecompRuntimeUi *ui,
                                    const RecompRuntimeUiItem *item,
                                    int *value);
void recomp_runtime_ui_adjust_current(RecompRuntimeUi *ui, int direction,
                                      int activate, int repeat);
void recomp_runtime_ui_enter_section(RecompRuntimeUi *ui, size_t section);
void recomp_runtime_ui_leave_section(RecompRuntimeUi *ui);

#ifdef __cplusplus
}
#endif

#endif
