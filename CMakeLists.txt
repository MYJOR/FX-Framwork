cmake_minimum_required(VERSION 3.16)
project(desert_scene C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# CUI: immediate-mode UI for the desert's diagnostics overlay and the office's scene editor.
# Core only: we bring our own window and GL context.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CUI/CMakeLists.txt")
  message(FATAL_ERROR "third_party/CUI is empty. Clone https://github.com/yappologistic/CUI into it "
                      "(the commit this repository pins is shown by: git ls-tree HEAD third_party/CUI).")
endif()
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
  src/inflate.c
  src/diag.c
  src/debug_ui.c
  src/gamepad.c
  src/coins.c
  src/beam.c)

target_include_directories(desert SYSTEM PRIVATE third_party/cgltf)
target_compile_options(desert PRIVATE -Wall -Wextra -O2)
target_link_libraries(desert PRIVATE cui::cui opengl32 gdi32 user32 psapi m)
target_link_options(desert PRIVATE -static)

# Office: first-person interior scene. Same platform / GL / shader layer as the desert, its own
# lighting pipeline (src/interior.c), and a scene editor drawn with CUI (src/office_editor.c).
add_executable(office
  src/office_main.c
  src/office_scene.c
  src/office_editor.c
  src/interior.c
  src/tracker.c
  src/handprints.c
  src/knock.c
  src/fbx.c
  src/inflate.c
  src/platform_win32.c
  src/gl.c
  src/shader.c
  src/png.c
  src/gamepad.c)

target_compile_options(office PRIVATE -Wall -Wextra -O2)
target_link_libraries(office PRIVATE cui::cui opengl32 gdi32 user32 winmm m)
target_link_options(office PRIVATE -static)