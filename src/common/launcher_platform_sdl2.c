// launcher_platform_sdl2.c — SDL2 implementation of the shared platform layer.
//
// Compatibility implementation for SDL2 hosts. It implements the same
// launcher_platform.h contract as launcher_platform_sdl3.c.
//
// DPI on SDL2: SDL_WINDOW_ALLOW_HIGHDPI makes the window size logical (points)
// while SDL_GL_GetDrawableSize reports physical pixels; their ratio is the
// content scale. That covers Windows per-monitor, macOS Retina and X11.
// It does NOT cover Wayland fractional scaling — SDL2 only supports integer
// buffer scale, so at 125%/150% the compositor downscales and text softens.
// SDL3 is the preferred path for fractional Wayland scaling.

#include <stdlib.h>
#include "launcher_platform.h"
#include "launcher_boot_timing.h"
#include "launcher_gl.h"

#include <stdio.h>

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
    if (!have_bounds)
        have_bounds = SDL_GetDisplayUsableBounds(0, &usable) == 0;
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
#if defined(_WIN32)
    // Match the Windows compatibility override users discovered manually:
    // let the launcher own DPI scaling instead of being bitmap-scaled by the OS.
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    SDL_SetHint("SDL_WINDOWS_DPI_SCALING", "0");
#endif
#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    SDL_SetHintWithPriority("SDL_JOYSTICK_HIDAPI_PS5", "1",
                            SDL_HINT_DEFAULT);
    // Bluetooth DualSense controllers start in basic-report mode. Requesting
    // enhanced reports exposes their motion sensors through SDL's standard
    // game-controller sensor API; USB controllers already use this mode.
    SDL_SetHintWithPriority("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1",
                            SDL_HINT_DEFAULT);
#ifdef LNG_GLES2
    // The host links ANGLE's libGLESv2/libEGL; SDL must create the context
    // through that same ES library (via EGL), or the directly-linked ANGLE
    // entry points run with no current context and crash on the first GL call.
    // Must be set BEFORE SDL_Init. Mirrors gb-recompiled's own platform init.
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER |
                 SDL_INIT_SENSOR) != 0) {   // SDL2: 0 == success
        fprintf(stderr, "[launcher] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    // GL dialect. Default: desktop GL 3.3 core (matches the vendored ImGui
    // opengl3 backend + "#version 330"). A host that reuses its OWN ImGui copy
    // compiled for GLES 2 (e.g. gb-recompiled, which renders through ANGLE)
    // defines LNG_GLES2 so the launcher's context MATCHES that backend — a
    // core-profile context with a GLES2 backend has no VAO and crashes.
#ifdef LNG_GLES2
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);   // ES 2.0, matching the host backend
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    // Test hook: LNG_FORCE_SCALE=<1..4> simulates a HiDPI display on ANY platform
    // (incl. Windows, which has no native point/pixel split) — the window is
    // created that many times larger in pixels while the UI keeps laying out in
    // the original logical size, so the DPI-independent layout + framebuffer
    // scaling can be validated end to end. See launcher_platform_refresh_metrics.
    int win_w = logical_w, win_h = logical_h;
    fit_initial_window_to_display(&win_w, &win_h);
    {
        const char* fs = getenv("LNG_FORCE_SCALE");
        if (fs && fs[0]) {
            float v = (float)atof(fs);
            if (v > 1.0f && v <= 4.0f) { win_w = (int)(logical_w * v); win_h = (int)(logical_h * v); }
        }
    }
    Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                          SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__ANDROID__)
    window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS;
#endif
    p->window = SDL_CreateWindow(title ? title : "Launcher",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 win_w, win_h,
                                 window_flags);
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
    SDL_GL_SetSwapInterval(1);

    SDL_RaiseWindow(p->window);   // foreground + keyboard focus (gamepad/kbd nav)

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
     * is ours to free as soon as SetWindowIcon returns. The channel masks are
     * byte-order dependent: stb hands back R,G,B,A in memory order. */
    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(
        pixels, w, h, 32, w * 4,
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff
#else
        0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000
#endif
    );
    if (surf) {
        SDL_SetWindowIcon(p->window, surf);
        SDL_FreeSurface(surf);
    }
    launcher_image_free(pixels);
}

void launcher_platform_refresh_metrics(LauncherPlatform* p) {
    if (!p || !p->window) return;

    SDL_GetWindowSize(p->window, &p->logical_w, &p->logical_h);
    SDL_GL_GetDrawableSize(p->window, &p->pixel_w, &p->pixel_h);

    // SDL2 has no SDL_GetWindowDisplayScale. Derive the content scale from the
    // drawable/window ratio, which is what ALLOW_HIGHDPI exposes.
    float s = 1.0f;
    if (p->logical_w > 0 && p->pixel_w > 0)
        s = (float)p->pixel_w / (float)p->logical_w;

    // On Windows the drawable and window sizes are both in pixels (no
    // point/pixel split), so the ratio is always 1.0 and we must ask the OS for
    // the real DPI instead. 96 dpi == 100% scaling.
    if (s <= 1.001f) {
        int disp = SDL_GetWindowDisplayIndex(p->window);
        float ddpi = 0.0f, hdpi = 0.0f, vdpi = 0.0f;
        if (disp >= 0 && SDL_GetDisplayDPI(disp, &ddpi, &hdpi, &vdpi) == 0 && hdpi > 0.0f) {
            float dpi_scale = hdpi / 96.0f;
            if (dpi_scale > s) s = dpi_scale;
        }
    }
    if (s <= 0.0f) s = 1.0f;
    // Test hook (see launcher_platform_open): when LNG_FORCE_SCALE enlarged the
    // window, treat it as a HiDPI display — the LOGICAL size is pixel/scale, and
    // that scale drives DisplayFramebufferScale so ImGui renders at pixel density
    // over a logical-sized layout (the Retina/Deck model, on any OS).
    {
        const char* fs = getenv("LNG_FORCE_SCALE");
        if (fs && fs[0]) {
            float v = (float)atof(fs);
            if (v > 1.0f && v <= 4.0f) {
                s = v;
                p->logical_w = (int)(p->pixel_w / v);
                p->logical_h = (int)(p->pixel_h / v);
            }
        }
    }
    p->display_scale = s;
}

void launcher_platform_present(LauncherPlatform* p) {
    if (p && p->window) SDL_GL_SwapWindow(p->window);
}

void launcher_platform_close(LauncherPlatform* p) {
    if (!p) return;
    if (p->gl)     { SDL_GL_DeleteContext(p->gl); p->gl = NULL; }
    if (p->window) { SDL_DestroyWindow(p->window); p->window = NULL; }
    SDL_GL_ResetAttributes();   // leave a clean slate for the game's SDL usage
    /* Soft-return rematch hosts that skip SDL_Quit (s_quit_sdl=false) keep the
     * subsystem alive across launcher↔game transitions. When we do quit, hosts
     * MUST re-SDL_Init (video+audio+gamecontroller) before recreating the game
     * window — see docs/HOST_NETPLAY.md. */
    if (s_quit_sdl)
        SDL_Quit();
}
