#ifndef TERRAIN_H
#define TERRAIN_H
#include "gl.h"
#include "mathx.h"

typedef struct {
    GLuint vao, vbo, ibo;
    GLsizei index_count;
} Terrain;

/* Procedural dune field: asymmetric wind-built dunes (gentle windward side,
   steep slip face, sharp crest) over rolling base terrain. */
float terrain_height(float x, float z);
V3 terrain_normal(float x, float z);

/* Builds a (cells+1)^2 grid of vertices spanning [-half, half] on x and z. */
void terrain_build(Terrain *t, int cells, float half);
void terrain_draw(const Terrain *t);

#endif
