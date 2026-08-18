// launcher_platform_sdl3.c — SDL3 implementation of the shared platform layer.

#include "launcher_platform.h"
#include "launcher_boot_timing.h"
#include "launcher_gl.h"

#include <stdio.h>
#include <stdlib.h>

static bool s_quit_sdl = true;

void launcher_platform_set_quit_sdl(bool quit_sdl) {
    s_quit_sdl = quit_sdl;
}

static bool forced_usable_bounds(SDL_Rect* out) {
    const char* value = getenv("LNG_FORCE_USABLE_BOUNDS");
    if (!value || !value[0] || !out) return false;
    int w = 0, h = 0;
    if (sscanf(value, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)
        return false;
    out->x = 0;
    out->y = 0;
    out->w = w;
    out->h = h;
    return true;
}

static void fit_initial_window_to_display(int* width, int* height) {
    if (!width || !height || *width <= 0 || *height <= 0) return;

    SDL_Rect usable = {0};
    bool have_bounds = forced_usable_bounds(&usable);
    if (!have_bounds) {
        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        have_bounds = display != 0 && SDL_GetDisplayUsableBounds(display, &usable);
    }
    if (!have_bounds || usable.w <= 0 || usable.h <= 0) return;

    // Leave room for desktop panels/title bars. Normal desktop sizes keep the
    // default 1100x880 window; only cramped displays get a smaller launch size.
    const int margin = 64;
    int max_w = usable.w - margin;
    int max_h = usable.h - margin;
    if (max_w <= 0 || max_h <= 0) return;
    if (*width <= max_w && *height <= max_h) return;

    float scale_w = (float)max_w / (float)*width;
    float scale_h = (float)max_h / (float)*height;
    float scale = scale_w < scale_h ? scale_w : scale_h;
    if (scale <= 0.0f || scale >= 1.0f) return;

    const int min_w = 820;
    const int min_h = 600;
    int fitted_w = (int)((float)*width * scale);
    int fitted_h = (int)((float)*height * scale);
    if (fitted_w < min_w && max_w >= min_w) fitted_w = min_w;
    if (fitted_h < min_h && max_h >= min_h) fitted_h = min_h;
    if (fitted_w > max_w) fitted_w = max_w;
    if (fitted_h > max_h) fitted_h = max_h;
    if (fitted_w > 0) *width = fitted_w;
    if (fitted_h > 0) *height = fitted_h;
}

bool launcher_platform_open(LauncherPlatform* p, const char* title,
                            int logical_w, int logical_h) {
    if (!p) return false;
    SDL_zerop(p);
    launcher_boot_timing_mark("rui:platform_open:begin");

    SDL_SetMainReady();   // we built with SDL_MAIN_HANDLED (real main() is entry)
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_SENSOR)) {
        fprintf(stderr, "[launcher] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    // HIGH_PIXEL_DENSITY is the linchpin: it asks SDL for a native-resolution
    // backbuffer on fractional-scale displays (esp. Wayland) instead of a
    // logical-size buffer the compositor blurs up. RESIZABLE lets us exercise
    // the live-resize requirement.
    const SDL_WindowFlags flags =
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    int win_w = logical_w;
    int win_h = logical_h;
    fit_initial_window_to_display(&win_w, &win_h);

    p->window = SDL_CreateWindow(title ? title : "Launcher",
                                 win_w, win_h, flags);
    if (!p->window) {
        fprintf(stderr, "[launcher] SDL_CreateWindow failed: %s\n", SDL_GetError());
        if (s_quit_sdl) SDL_Quit();
        return false;
    }

    p->gl = SDL_GL_CreateContext(p->window);
    if (!p->gl) {
        fprintf(stderr, "[launcher] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(p->window);
        p->window = NULL;
        if (s_quit_sdl) SDL_Quit();
        return false;
    }

    SDL_GL_MakeCurrent(p->window, p->gl);
    SDL_GL_SetSwapInterval(1);   // vsync — a launcher has no reason to spin

    launcher_platform_refresh_metrics(p);
    launcher_boot_timing_mark("rui:platform_open:window+gl_ready");
    return true;
}

void launcher_platform_set_icon(LauncherPlatform* p, const char* image_path) {
    if (!p || !p->window || !image_path || !image_path[0]) return;
    int w = 0, h = 0;
    unsigned char* pixels = launcher_image_load_rgba(image_path, &w, &h);
    if (!pixels) return;
    /* SDL copies the pixels into its own icon storage, so the decoded buffer
     * is ours to free as soon as SetWindowIcon returns. */
    SDL_Surface* surf =
        SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (surf) {
        SDL_SetWindowIcon(p->window, surf);
        SDL_DestroySurface(surf);
    }
    launcher_image_free(pixels);
}

void launcher_platform_refresh_metrics(LauncherPlatform* p) {
    if (!p || !p->window) return;

    SDL_GetWindowSize(p->window, &p->logical_w, &p->logical_h);
    SDL_GetWindowSizeInPixels(p->window, &p->pixel_w, &p->pixel_h);

    float s = SDL_GetWindowDisplayScale(p->window);
    if (s <= 0.0f) s = 1.0f;
    p->display_scale = s;
}

void launcher_platform_present(LauncherPlatform* p) {
    if (p && p->window) SDL_GL_SwapWindow(p->window);
}

void launcher_platform_close(LauncherPlatform* p) {
    if (!p) return;
    if (p->gl)     { SDL_GL_DestroyContext(p->gl); p->gl = NULL; }
    if (p->window) { SDL_DestroyWindow(p->window); p->window = NULL; }
    /* Soft-return rematch hosts that skip SDL_Quit (s_quit_sdl=false) keep the
     * subsystem alive across launcher↔game transitions. When we do quit, hosts
     * MUST re-SDL_Init (video+audio+gamecontroller) before recreating the game
     * window — see docs/HOST_NETPLAY.md. */
    if (s_quit_sdl)
        SDL_Quit();
}
