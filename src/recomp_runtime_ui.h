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

typedef enum RecompRuntimeUiViewMode {
    RECOMP_RUNTIME_UI_VIEW_NATIVE = 0,
    RECOMP_RUNTIME_UI_VIEW_FIXED_16_9 = 1,
    RECOMP_RUNTIME_UI_VIEW_ADAPTIVE = 2,
} RecompRuntimeUiViewMode;

enum {
    RECOMP_RUNTIME_UI_VIEW_MODE_NATIVE = 1u << RECOMP_RUNTIME_UI_VIEW_NATIVE,
    RECOMP_RUNTIME_UI_VIEW_MODE_FIXED_16_9 = 1u << RECOMP_RUNTIME_UI_VIEW_FIXED_16_9,
    RECOMP_RUNTIME_UI_VIEW_MODE_ADAPTIVE = 1u << RECOMP_RUNTIME_UI_VIEW_ADAPTIVE,
};

/* Stable semantic keys used by the standard settings builder and host hooks. */
#define RECOMP_RUNTIME_UI_KEY_FULLSCREEN       "display.fullscreen"
#define RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE     "display.window_scale"
#define RECOMP_RUNTIME_UI_KEY_VIEW_MODE        "display.view_mode"
#define RECOMP_RUNTIME_UI_KEY_WIDESCREEN_HUD   "display.widescreen_hud"
#define RECOMP_RUNTIME_UI_KEY_INTEGER_SCALE    "graphics.integer_scale"
#define RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER    "graphics.linear_filter"
#define RECOMP_RUNTIME_UI_KEY_TEXTURE_FILTER   "graphics.texture_filter"
#define RECOMP_RUNTIME_UI_KEY_RESOLUTION_SCALE "graphics.resolution_scale"
#define RECOMP_RUNTIME_UI_KEY_COLOR_CORRECTION "graphics.color_correction"
#define RECOMP_RUNTIME_UI_KEY_LCD_GHOSTING     "graphics.lcd_ghosting"
#define RECOMP_RUNTIME_UI_KEY_AUDIO            "audio.enabled"
#define RECOMP_RUNTIME_UI_KEY_VOLUME           "audio.volume"
#define RECOMP_RUNTIME_UI_KEY_RESUME            "system.resume"
#define RECOMP_RUNTIME_UI_KEY_SAVE_STATE        "system.save_state"
#define RECOMP_RUNTIME_UI_KEY_LOAD_STATE        "system.load_state"
#define RECOMP_RUNTIME_UI_KEY_RESET             "system.reset"

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
    /* Optional stable values for sparse choices. NULL means minimum + index. */
    const int *choice_values;
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

typedef uint64_t RecompRuntimeUiStandardFeatures;
enum {
    RECOMP_RUNTIME_UI_STANDARD_FULLSCREEN       = UINT64_C(1) << 0,
    RECOMP_RUNTIME_UI_STANDARD_WINDOW_SCALE     = UINT64_C(1) << 1,
    RECOMP_RUNTIME_UI_STANDARD_VIEW_MODE        = UINT64_C(1) << 2,
    RECOMP_RUNTIME_UI_STANDARD_WIDESCREEN_HUD   = UINT64_C(1) << 3,
    RECOMP_RUNTIME_UI_STANDARD_INTEGER_SCALE    = UINT64_C(1) << 4,
    RECOMP_RUNTIME_UI_STANDARD_LINEAR_FILTER    = UINT64_C(1) << 5,
    RECOMP_RUNTIME_UI_STANDARD_TEXTURE_FILTER   = UINT64_C(1) << 6,
    RECOMP_RUNTIME_UI_STANDARD_RESOLUTION_SCALE = UINT64_C(1) << 7,
    RECOMP_RUNTIME_UI_STANDARD_COLOR_CORRECTION = UINT64_C(1) << 8,
    RECOMP_RUNTIME_UI_STANDARD_LCD_GHOSTING     = UINT64_C(1) << 9,
    RECOMP_RUNTIME_UI_STANDARD_AUDIO            = UINT64_C(1) << 10,
    RECOMP_RUNTIME_UI_STANDARD_VOLUME           = UINT64_C(1) << 11,
    RECOMP_RUNTIME_UI_STANDARD_RESUME            = UINT64_C(1) << 12,
    RECOMP_RUNTIME_UI_STANDARD_SAVE_STATE        = UINT64_C(1) << 13,
    RECOMP_RUNTIME_UI_STANDARD_LOAD_STATE        = UINT64_C(1) << 14,
    RECOMP_RUNTIME_UI_STANDARD_RESET             = UINT64_C(1) << 15,
};

typedef struct RecompRuntimeUiStandardConfig {
    RecompRuntimeUiConfig menu;
    RecompRuntimeUiStandardFeatures features;
    unsigned view_modes;
    const char *native_view_label;   /* default: Native */
    const char *fixed_view_label;    /* default: 16:9 fixed */
    const char *adaptive_view_label; /* default: Adaptive */
    int window_scale_max;            /* default: 8 */
    int resolution_scale_max;        /* default: 8 */
    const RecompRuntimeUiItem *extra_items;
    size_t extra_item_count;
} RecompRuntimeUiStandardConfig;

/*
 * Creates a host-owned in-game menu. The descriptor and all strings referenced
 * by it must outlive the returned object. The menu does not own or advance the
 * game: the host decides whether an open menu pauses simulation.
 */
RecompRuntimeUi *recomp_runtime_ui_create(const RecompRuntimeUiConfig *config);
/* Builds the common cross-ecosystem settings surface, then appends extras. */
RecompRuntimeUi *recomp_runtime_ui_create_standard(
    const RecompRuntimeUiStandardConfig *config);
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
