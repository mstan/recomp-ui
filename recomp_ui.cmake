# recomp_ui.cmake — reusable in-game Dear ImGui launcher integration.
#
# One call wires the whole GUI pre-boot launcher into any host target:
#
#     set(RECOMP_UI_ROOT <path-to-recomp-ui>)   # or add as a git submodule
#     include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
#     recomp_target_launcher_ui(<host_target> [BOXART <path-to-boxart.tga>])
#
# This is the console-agnostic extraction of the SNES-recomp "launcher_ng"
# Dear ImGui launcher: same UI, same behavior, generic C ABI
# (recomp_launcher_run_window(), declared in src/recomp_launcher.h). Any
# recomp ecosystem (SNES, NES, N64, PSX, ...) can consume this repo as a git
# submodule and get byte-identical launcher behavior with zero UI code of
# its own.
#
# Unlike the in-tree snesrecomp launcher_ng, this module BUNDLES crc32,
# sha256, and keybinds — recomp-ui is fully self-contained and does not
# assume the host already compiles those helpers.
#
# It uses the VENDORED ImGui at src/third_party/imgui — no network /
# FetchContent, so every consumer builds offline and reproducibly.
#
# The host's own main() should call recomp_launcher_run_window() from
# recomp_launcher.h, typically behind a compile-time gate on RECOMP_LAUNCHER
# (defined PRIVATE on the target by this module, so the host can also
# #ifdef RECOMP_LAUNCHER around its call site to match).

# Root of this recomp-ui checkout. A consumer can override RECOMP_UI_ROOT
# before including this file (e.g. when vendoring recomp-ui somewhere
# non-standard); otherwise it defaults to this file's own directory, which is
# correct for the normal "add as a git submodule, include() it" usage.
set(RECOMP_UI_ROOT "${CMAKE_CURRENT_LIST_DIR}" CACHE PATH
    "Root directory of the recomp-ui launcher repo")

set(RUI_SRC    ${RECOMP_UI_ROOT}/src)
set(RUI_IMGUI  ${RUI_SRC}/third_party/imgui)
set(RUI_ASSETS ${RECOMP_UI_ROOT}/assets)

# The ImGui backend is C++; the host project() is often C-only. enable_language
# must run at directory scope (not inside the function, which executes during
# generation), so it lives here — safe/idempotent if CXX is already enabled.
enable_language(CXX)

function(recomp_target_launcher_ui TGT)
    cmake_parse_arguments(RUI "" "BOXART;PAD" "" ${ARGN})

    set_target_properties(${TGT} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

    target_sources(${TGT} PRIVATE
        # console-agnostic launcher core (C)
        ${RUI_SRC}/launcher_model.c
        ${RUI_SRC}/launcher_platform_sdl2.c
        ${RUI_SRC}/launcher_gl.c
        ${RUI_SRC}/launcher_input.c
        ${RUI_SRC}/launcher_files.c
        ${RUI_SRC}/launcher_debug.c
        ${RUI_SRC}/launcher_binds.c
        ${RUI_SRC}/launcher_ng_capi.c          # implements recomp_launcher_run_window()
        ${RUI_SRC}/third_party/tinyfiledialogs.c
        # bundled engine helpers (recomp-ui is self-contained; the host does
        # not need to already compile these)
        ${RUI_SRC}/crc32.c
        ${RUI_SRC}/sha256.c
        ${RUI_SRC}/keybinds.c
        # Dear ImGui backend (the shipping UI) + vendored ImGui (C++)
        ${RUI_SRC}/backends/imgui/launcher_imgui.cpp
        ${RUI_IMGUI}/imgui.cpp
        ${RUI_IMGUI}/imgui_draw.cpp
        ${RUI_IMGUI}/imgui_tables.cpp
        ${RUI_IMGUI}/imgui_widgets.cpp
        ${RUI_IMGUI}/backends/imgui_impl_sdl2.cpp
        ${RUI_IMGUI}/backends/imgui_impl_opengl3.cpp
    )

    target_include_directories(${TGT} PRIVATE
        ${RUI_SRC}                   # recomp_launcher.h + launcher_ng headers + relative third_party/stb_*.h
        ${RUI_IMGUI}
        ${RUI_IMGUI}/backends
    )

    target_compile_definitions(${TGT} PRIVATE
        RECOMP_LAUNCHER           # un-gate the GUI launcher block in the host's main()
        SDL_MAIN_HANDLED)         # our real main() is the entry point (no SDL_main redirect)

    if(NOT MSVC)
        # the vendored ImGui + tinyfiledialogs compile clean; nothing extra needed.
    endif()

    # ---- stage runtime assets next to the exe -----------------------------------
    add_custom_command(TARGET ${TGT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${TGT}>/assets/fonts
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${TGT}>/assets/img
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${RUI_ASSETS}/fonts/LatoLatin-Regular.ttf
                ${RUI_ASSETS}/fonts/LatoLatin-Bold.ttf
                $<TARGET_FILE_DIR:${TGT}>/assets/fonts/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${RUI_ASSETS}/img/brand_mark.tga ${RUI_ASSETS}/img/pad.tga
                $<TARGET_FILE_DIR:${TGT}>/assets/img/
        VERBATIM)
    # Per-console controller image: overrides the default pad.tga (e.g. a
    # PlayStation DualShock for PSX). 24-bit TGA, top-left pixel = colorkey.
    if(RUI_PAD AND EXISTS ${RUI_PAD})
        add_custom_command(TARGET ${TGT} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${RUI_PAD} $<TARGET_FILE_DIR:${TGT}>/assets/img/pad.tga
            VERBATIM)
    endif()
    if(RUI_BOXART AND EXISTS ${RUI_BOXART})
        add_custom_command(TARGET ${TGT} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${RUI_BOXART} $<TARGET_FILE_DIR:${TGT}>/assets/img/boxart.tga
            VERBATIM)
    endif()
endfunction()
