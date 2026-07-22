#ifndef RECOMP_RUNTIME_UI_H
#define RECOMP_RUNTIME_UI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompRuntimeUi RecompRuntimeUi;

typedef enum RecompRuntimeUiItemType {
    RECOMP_RUNTIME_UI_BOOL,
    RECOMP_RUNTIME_UI_INT,
    RECOMP_RUNTIME_UI_CHOICE,
    RECOMP_RUNTIME_UI_ACTION,
} RecompRuntimeUiItemType;

typedef enum RecompRuntimeUiInput {
    RECOMP_RUNTIME_UI_INPUT_TOGGLE,
    RECOMP_RUNTIME_UI_INPUT_BACK,
    RECOMP_RUNTIME_UI_INPUT_UP,
    RECOMP_RUNTIME_UI_INPUT_DOWN,
    RECOMP_RUNTIME_UI_INPUT_LEFT,
    RECOMP_RUNTIME_UI_INPUT_RIGHT,
    RECOMP_RUNTIME_UI_INPUT_ACCEPT,
} RecompRuntimeUiInput;

typedef struct RecompRuntimeUiItem {
    const char *key;
    const char *section;
    const char *label;
    const char *description;
    RecompRuntimeUiItemType type;
    int minimum;
    int maximum;
    int step;
    const char *const *choices;
    size_t choice_count;
} RecompRuntimeUiItem;

typedef struct RecompRuntimeUiCallbacks {
    void *context;
    int (*get_value)(void *context, const RecompRuntimeUiItem *item,
                     int *value_out);
    int (*set_value)(void *context, const RecompRuntimeUiItem *item,
                     int value);
    int (*run_action)(void *context, const RecompRuntimeUiItem *item);
    int (*is_enabled)(void *context, const RecompRuntimeUiItem *item);
    void (*save)(void *context);
    void (*visibility_changed)(void *context, int open);
} RecompRuntimeUiCallbacks;

typedef struct RecompRuntimeUiConfig {
    const char *title;
    const char *subtitle;
    const RecompRuntimeUiItem *items;
    size_t item_count;
    RecompRuntimeUiCallbacks callbacks;

    /*
     * Uses the same built-in theme vocabulary as the launcher ("snes",
     * "n64", "gba", "genesis", ...). NULL selects the neutral default.
     * Presentation backends consume this value; game logic never needs to.
     */
    const char *theme;

    /* Optional platform/input vocabulary for the footer. */
    const char *accept_label;
    const char *back_label;
} RecompRuntimeUiConfig;

/*
 * Creates a host-owned in-game menu. The descriptor and all strings referenced
 * by it must outlive the returned object. The menu does not own or advance the
 * game: the host decides whether an open menu pauses simulation.
 */
RecompRuntimeUi *recomp_runtime_ui_create(const RecompRuntimeUiConfig *config);
void recomp_runtime_ui_destroy(RecompRuntimeUi *ui);

int recomp_runtime_ui_is_open(const RecompRuntimeUi *ui);
void recomp_runtime_ui_open(RecompRuntimeUi *ui);
void recomp_runtime_ui_close(RecompRuntimeUi *ui);

/* Returns non-zero when the input belongs to the menu. */
int recomp_runtime_ui_handle_input(RecompRuntimeUi *ui,
                                   RecompRuntimeUiInput input,
                                   int pressed, int repeat);

/*
 * Composites the menu over a little-endian SDL_PIXELFORMAT_ARGB8888/BGRA frame.
 * The frame remains owned by the host and may be presented by SDL, OpenGL, or
 * another backend after this call. No renderer/context dependency is required.
 */
void recomp_runtime_ui_render_argb8888(RecompRuntimeUi *ui, void *pixels,
                                       int width, int height, int pitch_bytes);

/*
 * Draws the menu into the caller's active Dear ImGui frame. This is the
 * preferred presentation for GPU-backed and high-resolution hosts (including
 * RT64). It does not create an ImGui context, begin/end a frame, or submit draw
 * data; the host keeps ownership of those renderer-specific operations.
 *
 * The symbol is available when recomp-ui's ImGui backend is compiled. Keeping
 * ImGui types out of this C ABI lets C games and hosts with a different ImGui
 * version consume the same runtime model.
 */
void recomp_runtime_ui_render_imgui(RecompRuntimeUi *ui);

#ifdef __cplusplus
}
#endif

#endif
