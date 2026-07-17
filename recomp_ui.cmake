# recomp_ui.cmake — reusable in-game Dear ImGui launcher integration.
#
# One call wires the whole GUI pre-boot launcher into any host target:
#
#     set(RECOMP_UI_ROOT <path-to-recomp-ui>)   # or add as a git submodule
#     include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
#     recomp_target_launcher_ui(<host_target> [BOXART <path-to-boxart.tga>]
#                                              [BOXART_NAME <dest-basename.tga>]
#                                              [PAD <pad.tga>] [BRAND <brand.tga>])
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
    # BOXART_NAME: destination basename for BOXART under assets/img/ (default
    # "boxart.tga"). Needed when several targets stage into ONE exe dir (Sonic
    # 3 & Knuckles builds three modes side by side) and each needs its own
    # box art file — pairs with GameInfo.boxart_path the runtime reads.
    # HOST_IMGUI: the host target already compiles Dear ImGui (imgui.cpp +
    # imgui_impl_sdl2/opengl3) — reuse that ONE copy instead of linking a
    # second, which would be a duplicate-symbol / ODR clash. IMGUI_DIR is the
    # host's ImGui source dir (must contain imgui.h + backends/imgui_impl_*.h)
    # that recomp-ui's own backend glue (launcher_imgui.cpp) compiles against.
    # Used by gb-recompiled, whose runtime already vendors + uses ImGui for its
    # in-game menu. Omit both to keep the default self-contained vendored ImGui.
    cmake_parse_arguments(RUI "" "BOXART;BOXART_NAME;PAD;BRAND;HOST_IMGUI;IMGUI_DIR" "" ${ARGN})

    set_target_properties(${TGT} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

    target_sources(${TGT} PRIVATE
        # console-agnostic launcher core (C) — src/common/
        ${RUI_SRC}/common/launcher_model.c
        ${RUI_SRC}/common/launcher_platform_sdl2.c
        ${RUI_SRC}/common/launcher_gl.c
        ${RUI_SRC}/common/launcher_input.c
        ${RUI_SRC}/common/launcher_files.c
        ${RUI_SRC}/common/launcher_debug.c
        ${RUI_SRC}/common/launcher_binds.c
        ${RUI_SRC}/common/launcher_ng_capi.c   # implements recomp_launcher_run_window()
        ${RUI_SRC}/third_party/tinyfiledialogs.c
        # console-specific helpers (src/consoles/<id>/) — always compiled, only
        # reached when the active SystemProfile opts into the capability
        ${RUI_SRC}/consoles/psx/memcard_format.c   # PS1 blank memory-card image writer
        ${RUI_SRC}/consoles/psx/psx_binds.c        # PSX-native keybind persistence bridge
        ${RUI_SRC}/consoles/genesis/genesis_binds.c # Genesis-native settings.ini key.*/pad.* bridge
        ${RUI_SRC}/consoles/gb/gb_binds.c          # Game Boy-native keybinds.ini [controls] bridge
        # bundled engine helpers (recomp-ui is self-contained; the host does
        # not need to already compile these)
        ${RUI_SRC}/common/crc32.c
        ${RUI_SRC}/common/sha256.c
        ${RUI_SRC}/common/sha1.c        # cartridge ROM identity (GBA/SNES gate on SHA-1)
        ${RUI_SRC}/common/keybinds.c
        ${RUI_SRC}/common/ips_patch.c   # MSU-1 IPS auto-patching (launcher_model.c)
        # recomp-ui's own Dear ImGui backend glue (the shipping UI). ALWAYS
        # compiled — it is recomp-ui code, not vendored ImGui.
        ${RUI_SRC}/common/backends/imgui/launcher_imgui.cpp
    )

    # Vendored Dear ImGui (C++) — compiled only when the host does NOT already
    # provide it. Under HOST_IMGUI the host's single copy is reused (below).
    if(NOT RUI_HOST_IMGUI)
        target_sources(${TGT} PRIVATE
            ${RUI_IMGUI}/imgui.cpp
            ${RUI_IMGUI}/imgui_draw.cpp
            ${RUI_IMGUI}/imgui_tables.cpp
            ${RUI_IMGUI}/imgui_widgets.cpp
            ${RUI_IMGUI}/backends/imgui_impl_sdl2.cpp
            ${RUI_IMGUI}/backends/imgui_impl_opengl3.cpp
        )
    endif()

    # ImGui include dir: the host's under HOST_IMGUI (so launcher_imgui.cpp
    # compiles against the SAME imgui.h the host's single copy was built from),
    # else recomp-ui's vendored tree.
    set(RUI_IMGUI_INC ${RUI_IMGUI})
    if(RUI_HOST_IMGUI)
        if(NOT RUI_IMGUI_DIR)
            message(FATAL_ERROR "recomp_target_launcher_ui(HOST_IMGUI ...) requires IMGUI_DIR <host imgui source dir>")
        endif()
        set(RUI_IMGUI_INC ${RUI_IMGUI_DIR})
    endif()

    target_include_directories(${TGT} PRIVATE
        ${RUI_SRC}                   # recomp_launcher.h / launcher_profile.h / launcher_system.h
                                     # + "third_party/..." + "consoles/<id>/..." includes
        ${RUI_SRC}/common            # launcher core headers (bare-name includes)
        ${RUI_IMGUI_INC}
        ${RUI_IMGUI_INC}/backends
    )

    target_compile_definitions(${TGT} PRIVATE
        RECOMP_LAUNCHER           # un-gate the GUI launcher block in the host's main()
        SDL_MAIN_HANDLED)         # our real main() is the entry point (no SDL_main redirect)

    # OpenGL: the ImGui GL3 backend + launcher_gl.c need the system GL library.
    # Link it here so a host gets it from this ONE call (self-contained) rather
    # than having to remember to link OpenGL itself — mirrors the standalone
    # CMakeLists.txt. SDL2 is still the host's to provide (its provenance varies:
    # vendored, find_package, etc.); GL is a uniform system lib, so it lives here.
    if(WIN32)
        target_link_libraries(${TGT} PRIVATE opengl32)
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(${TGT} PRIVATE OpenGL::GL ${CMAKE_DL_LIBS})
    endif()

    if(NOT MSVC)
        # the vendored ImGui + tinyfiledialogs compile clean; nothing extra needed.
    endif()

    # ---- stage runtime assets next to the exe -----------------------------------
    # Repo layout mirrors src/: assets/common/ (chrome shared by every
    # console) + assets/consoles/<id>/ (per-console art). The RUNTIME layout
    # next to the exe stays flat (assets/fonts + assets/img) — the launcher's
    # load paths are unchanged; only the repo organization is per-console.
    add_custom_command(TARGET ${TGT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${TGT}>/assets/fonts
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${TGT}>/assets/img
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${RUI_ASSETS}/common/fonts/LatoLatin-Regular.ttf
                ${RUI_ASSETS}/common/fonts/LatoLatin-Bold.ttf
                $<TARGET_FILE_DIR:${TGT}>/assets/fonts/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${RUI_ASSETS}/common/img/brand_mark.tga
                ${RUI_ASSETS}/common/img/verdict_ok.tga
                ${RUI_ASSETS}/common/img/verdict_warn.tga
                ${RUI_ASSETS}/common/img/verdict_bad.tga
                ${RUI_ASSETS}/common/img/verdict_none.tga
                ${RUI_ASSETS}/consoles/snes/img/pad.tga
                ${RUI_ASSETS}/consoles/psx/img/pad_analog.tga
                ${RUI_ASSETS}/consoles/psx/img/pad_digital.tga
                ${RUI_ASSETS}/consoles/psx/img/memcard.tga
                ${RUI_ASSETS}/consoles/gba/img/pad_gba.tga
                ${RUI_ASSETS}/consoles/genesis/img/pad_genesis.tga
                ${RUI_ASSETS}/consoles/genesis/img/brand_genesis.tga
                ${RUI_ASSETS}/consoles/genesis/img/boxart_sonic1.tga
                ${RUI_ASSETS}/consoles/gb/img/pad_gb.tga
                ${RUI_ASSETS}/consoles/gb/img/pad_gbc.tga
                ${RUI_ASSETS}/consoles/gb/img/brand_gb.tga
                ${RUI_ASSETS}/consoles/gb/img/brand_gbc.tga
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
        set(RUI_BOXART_DEST "boxart.tga")
        if(RUI_BOXART_NAME)
            set(RUI_BOXART_DEST "${RUI_BOXART_NAME}")
        endif()
        add_custom_command(TARGET ${TGT} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${RUI_BOXART} $<TARGET_FILE_DIR:${TGT}>/assets/img/${RUI_BOXART_DEST}
            VERBATIM)
    endif()
    # Per-console brand mark (top-left, next to the game title): overrides the
    # default brand_mark.tga (e.g. the PlayStation shapes for PSX). 32-bit TGA.
    if(RUI_BRAND AND EXISTS ${RUI_BRAND})
        add_custom_command(TARGET ${TGT} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${RUI_BRAND} $<TARGET_FILE_DIR:${TGT}>/assets/img/brand_mark.tga
            VERBATIM)
    endif()
endfunction()

# recomp_stage_launcher_assets(<exe_target> [BOXART <path>] [BOXART_NAME <name>])
#
# Staging-ONLY helper (no source compilation) for hosts that compile the
# recomp-ui launcher into a SHARED runtime library (e.g. gb-recompiled's gbrt)
# and therefore can't use recomp_target_launcher_ui() — its POST_BUILD asset
# copy has to attach to the final EXE target, not the static lib. Call this on
# the game exe from the generated project's CMake. Stages the shared chrome +
# the Game Boy family controller/logo art + optional per-game box art next to
# the exe (the flat assets/fonts + assets/img layout the launcher loads).
function(recomp_stage_launcher_assets TGT)
    cmake_parse_arguments(RSA "" "BOXART;BOXART_NAME" "" ${ARGN})
    add_custom_command(TARGET ${TGT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${TGT}>/assets/fonts
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${TGT}>/assets/img
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${RUI_ASSETS}/common/fonts/LatoLatin-Regular.ttf
                ${RUI_ASSETS}/common/fonts/LatoLatin-Bold.ttf
                $<TARGET_FILE_DIR:${TGT}>/assets/fonts/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${RUI_ASSETS}/common/img/brand_mark.tga
                ${RUI_ASSETS}/common/img/verdict_ok.tga
                ${RUI_ASSETS}/common/img/verdict_warn.tga
                ${RUI_ASSETS}/common/img/verdict_bad.tga
                ${RUI_ASSETS}/common/img/verdict_none.tga
                ${RUI_ASSETS}/consoles/gb/img/pad_gb.tga
                ${RUI_ASSETS}/consoles/gb/img/pad_gbc.tga
                ${RUI_ASSETS}/consoles/gb/img/brand_gb.tga
                ${RUI_ASSETS}/consoles/gb/img/brand_gbc.tga
                $<TARGET_FILE_DIR:${TGT}>/assets/img/
        VERBATIM)
    # Per-game box art (24/32-bit TGA). The seam points GameInfo.boxart_path at
    # "assets/img/boxart.tga" (or BOXART_NAME) next to the exe.
    if(RSA_BOXART AND EXISTS ${RSA_BOXART})
        set(RSA_DEST "boxart.tga")
        if(RSA_BOXART_NAME)
            set(RSA_DEST "${RSA_BOXART_NAME}")
        endif()
        add_custom_command(TARGET ${TGT} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${RSA_BOXART} $<TARGET_FILE_DIR:${TGT}>/assets/img/${RSA_DEST}
            VERBATIM)
    endif()
endfunction()
