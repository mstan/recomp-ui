# recomp-ui — console-agnostic Dear ImGui pre-boot launcher, shared across
# recomp ecosystems (SNES, NES, N64, PSX, ...).
#
# This top-level CMakeLists.txt builds ONLY a standalone self-test exe
# (recomp-ui-launcher, from src/proto_main.c) so the repo can verify itself
# in isolation. Real consumers do NOT build this project directly — they
# `include(recomp_ui.cmake)` from their own CMake project and call
# `recomp_target_launcher_ui(<their_target>)` (see recomp_ui.cmake and
# README.md).
#
# Build (mingw + ninja):
#   cmake -G Ninja -S . -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
#   cmake --build build

cmake_minimum_required(VERSION 3.24)
project(recomp_ui C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SNESRECOMP_SDL_BACKEND "SDL3" CACHE STRING
    "Standalone launcher SDL backend (SDL3 or SDL2)")
set_property(CACHE SNESRECOMP_SDL_BACKEND PROPERTY STRINGS SDL3 SDL2)
string(TOUPPER "${SNESRECOMP_SDL_BACKEND}" SNESRECOMP_SDL_BACKEND)
if(SNESRECOMP_SDL_BACKEND STREQUAL "SDL3")
    find_package(SDL3 CONFIG REQUIRED)
    set(RUI_SDL_TARGET SDL3::SDL3)
    # Any target that compiles a header off the launcher_platform.h chain needs
    # these, or that header falls through to <SDL.h> -- the SDL2 include layout --
    # and SDL_main's WinMain shim can rewrite standalone test entry points.
    # recomp_target_launcher_ui() sets the launcher up; standalone targets that
    # only link RUI_SDL_TARGET must be given the same contract explicitly.
    set(RUI_SDL_DEFS LNG_SDL3=1 SDL_MAIN_HANDLED=1)
    if(TARGET SDL3::SDL3-shared)
        set(RUI_SDL_RUNTIME_TARGET SDL3::SDL3-shared)
    endif()
elseif(SNESRECOMP_SDL_BACKEND STREQUAL "SDL2")
    find_package(SDL2 REQUIRED)
    set(RUI_SDL_TARGET SDL2::SDL2)
    get_target_property(RUI_SDL2_TARGET_TYPE SDL2::SDL2 TYPE)
    if(RUI_SDL2_TARGET_TYPE STREQUAL "SHARED_LIBRARY")
        set(RUI_SDL_RUNTIME_TARGET SDL2::SDL2)
    endif()
else()
    message(FATAL_ERROR "SNESRECOMP_SDL_BACKEND must be SDL3 or SDL2")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/recomp_gl.cmake)
if(WIN32)
    set(RUI_GL_LIBS opengl32)
else()
    recomp_resolve_gl(RUI_GL_TARGET)
    set(RUI_GL_LIBS ${RUI_GL_TARGET} ${CMAKE_DL_LIBS})
endif()

add_executable(recomp-ui-launcher src/proto_main.c)
target_link_libraries(recomp-ui-launcher PRIVATE ${RUI_SDL_TARGET} ${RUI_GL_LIBS})
if(WIN32 AND RUI_SDL_RUNTIME_TARGET)
    add_custom_command(TARGET recomp-ui-launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${RUI_SDL_RUNTIME_TARGET}>
            $<TARGET_FILE_DIR:recomp-ui-launcher>)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/recomp_ui.cmake)
recomp_target_launcher_ui(recomp-ui-launcher CONSOLE all)

include(CTest)
if(BUILD_TESTING)
    add_executable(recomp-ui-ips-patch-test
        tests/ips_patch_test.c
        src/common/ips_patch.c
        src/common/crc32.c)
    target_include_directories(recomp-ui-ips-patch-test PRIVATE src/common)
    add_test(NAME recomp-ui-ips-patch COMMAND $<TARGET_FILE:recomp-ui-ips-patch-test>)

    add_executable(recomp-ui-rom-patch-tool
        tools/rom_patch_tool.c
        src/common/ips_patch.c
        src/common/crc32.c
        src/common/sha1.c)
    target_include_directories(recomp-ui-rom-patch-tool PRIVATE src/common)

    add_test(
        NAME recomp-ui-asset-manifests
        COMMAND ${CMAKE_COMMAND}
            "-DRECOMP_UI_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/asset_manifest_test.cmake)

    add_executable(recomp-ui-psx-asset-stage-probe
        tests/asset_stage_probe.c)
    set_target_properties(
        recomp-ui-psx-asset-stage-probe
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/asset-stage-psx")
    add_custom_command(TARGET recomp-ui-psx-asset-stage-probe PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:recomp-ui-psx-asset-stage-probe>/assets/img"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RUI_ASSETS}/consoles/n64/img/cart_red.tga"
                "$<TARGET_FILE_DIR:recomp-ui-psx-asset-stage-probe>/assets/img/"
        VERBATIM)
    _recomp_ui_stage_assets(recomp-ui-psx-asset-stage-probe psx)
    add_test(
        NAME recomp-ui-psx-asset-staging
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_DIR=$<TARGET_FILE_DIR:recomp-ui-psx-asset-stage-probe>/assets"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/asset_staging_test.cmake)

    # Compile the exact SDL_Renderer2 adapter through its public integration
    # helper. Calling the helper twice also guards its no-duplicate contract.
    find_package(SDL2 QUIET)
    if(TARGET SDL2::SDL2)
        add_library(recomp-runtime-ui-sdlrenderer2-check OBJECT)
        recomp_target_runtime_ui_sdlrenderer2(
            recomp-runtime-ui-sdlrenderer2-check)
        recomp_target_runtime_ui_sdlrenderer2(
            recomp-runtime-ui-sdlrenderer2-check)
        target_link_libraries(recomp-runtime-ui-sdlrenderer2-check PRIVATE
            SDL2::SDL2)
    endif()

    add_executable(recomp-runtime-ui-test
        tests/runtime_ui_test.c
        src/common/recomp_runtime_ui.c
        src/common/recomp_runtime_settings.c)
    target_include_directories(recomp-runtime-ui-test PRIVATE src)
    add_test(NAME recomp-runtime-ui COMMAND $<TARGET_FILE:recomp-runtime-ui-test>)

    add_executable(recomp-ui-psx-binds-test
        tests/psx_binds_test.c
        src/consoles/psx/psx_binds.c)
    target_include_directories(recomp-ui-psx-binds-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-psx-binds-test PRIVATE ${RUI_SDL_TARGET})
    target_compile_definitions(recomp-ui-psx-binds-test PRIVATE ${RUI_SDL_DEFS})
    add_test(
        NAME recomp-ui-psx-binds
        COMMAND $<TARGET_FILE:recomp-ui-psx-binds-test>
                "${CMAKE_CURRENT_BINARY_DIR}/psx-binds-test.ini")

    # Multi-disc roster: sibling autofill + per-slot binding. Includes
    # launcher_model.c (not links it) to reach the static path rewriter, so the
    # model's own deps are compiled in alongside.
    add_executable(recomp-ui-launcher-discs-test
        tests/launcher_discs_test.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-discs-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-launcher-discs-test PRIVATE ${RUI_SDL_TARGET})
    add_test(
        NAME recomp-ui-launcher-discs
        COMMAND $<TARGET_FILE:recomp-ui-launcher-discs-test>
                "${CMAKE_CURRENT_BINARY_DIR}")

    add_executable(recomp-ui-psx-profile-test
        tests/psx_profile_test.c)
    target_include_directories(recomp-ui-psx-profile-test PRIVATE
        src src/common src/consoles/psx)
    add_test(
        NAME recomp-ui-psx-profile
        COMMAND $<TARGET_FILE:recomp-ui-psx-profile-test>)

    add_executable(recomp-runtime-ui-imgui-test
        tests/runtime_ui_imgui_test.cpp
        src/common/recomp_runtime_ui.c
        src/common/recomp_runtime_settings.c
        src/common/backends/imgui/runtime_ui_imgui.cpp
        src/third_party/imgui/imgui.cpp
        src/third_party/imgui/imgui_draw.cpp
        src/third_party/imgui/imgui_tables.cpp
        src/third_party/imgui/imgui_widgets.cpp)
    target_include_directories(recomp-runtime-ui-imgui-test PRIVATE
        src src/common src/third_party/imgui)
    add_test(NAME recomp-runtime-ui-imgui COMMAND $<TARGET_FILE:recomp-runtime-ui-imgui-test>)
endif()
