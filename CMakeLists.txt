cmake_minimum_required(VERSION 3.16)
project(desert_scene C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# CUI: immediate-mode UI for the diagnostics overlay. Core only: we bring our own window and GL context.
set(CUI_WITH_APP OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/CUI EXCLUDE_FROM_ALL)

add_executable(desert
  src/main.c
  src/platform_win32.c
  src/gl.c
  src/shader.c
  src/terrain.c
  src/particles.c
  src/player.c
  src/character.c
  src/trail.c
  src/png.c
  src/diag.c
  src/debug_ui.c)

target_include_directories(desert SYSTEM PRIVATE third_party/cgltf)
target_compile_options(desert PRIVATE -Wall -Wextra -O2)
target_link_libraries(desert PRIVATE cui::cui opengl32 gdi32 user32 psapi m)
target_link_options(desert PRIVATE -static)
