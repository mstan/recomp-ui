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

find_package(SDL2 REQUIRED)

if(WIN32)
    set(RUI_GL_LIBS opengl32)
else()
    find_package(OpenGL REQUIRED)
    set(RUI_GL_LIBS OpenGL::GL ${CMAKE_DL_LIBS})
endif()

add_executable(recomp-ui-launcher src/proto_main.c)
target_link_libraries(recomp-ui-launcher PRIVATE SDL2::SDL2 ${RUI_GL_LIBS})

include(${CMAKE_CURRENT_SOURCE_DIR}/recomp_ui.cmake)
recomp_target_launcher_ui(recomp-ui-launcher)

include(CTest)
if(BUILD_TESTING)
    # Compile the exact SDL_Renderer2 adapter through its public integration
    # helper. Calling the helper twice also guards its no-duplicate contract.
    add_library(recomp-runtime-ui-sdlrenderer2-check OBJECT)
    recomp_target_runtime_ui_sdlrenderer2(
        recomp-runtime-ui-sdlrenderer2-check)
    recomp_target_runtime_ui_sdlrenderer2(
        recomp-runtime-ui-sdlrenderer2-check)
    target_link_libraries(recomp-runtime-ui-sdlrenderer2-check PRIVATE
        SDL2::SDL2)

    add_executable(recomp-runtime-ui-test
        tests/runtime_ui_test.c
        src/common/recomp_runtime_ui.c
        src/common/recomp_runtime_settings.c)
    target_include_directories(recomp-runtime-ui-test PRIVATE src)
    add_test(NAME recomp-runtime-ui COMMAND $<TARGET_FILE:recomp-runtime-ui-test>)

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
