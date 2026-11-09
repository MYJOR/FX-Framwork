#ifndef SHADER_H
#define SHADER_H
#include "gl.h"
#include <time.h>

/* A program built from shaders/common.glsl + a vertex and a fragment file.
   shader_reload_if_changed() recompiles on file change and keeps the old
   program if the new source fails to compile. */
typedef struct {
    GLuint id;
    char vs_path[260], fs_path[260];
    time_t stamp;
} Program;

int shader_program_load(Program *p, const char *vs_file, const char *fs_file);
void shader_reload_if_changed(Program *p);
void shader_set_dir(const char *dir);

#endif
