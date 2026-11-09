cmake_minimum_required(VERSION 3.16)
project(desert_scene C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(desert
  src/main.c
  src/platform_win32.c
  src/gl.c
  src/shader.c
  src/terrain.c
  src/particles.c
  src/player.c
  src/trail.c
  src/png.c)

target_compile_options(desert PRIVATE -Wall -Wextra -O2)
target_link_libraries(desert PRIVATE opengl32 gdi32 user32 m)
target_link_options(desert PRIVATE -static)
