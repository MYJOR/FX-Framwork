#ifndef BEAM_H
#define BEAM_H
#include "gl.h"
#include "mathx.h"

typedef struct { GLuint vao, vbo; } Beams;

void beams_init(Beams *b);
/* Additive glowing beam from a to b. Call with the beam program bound, uViewProj / uTime set, blend = ONE,ONE. */
void beam_draw(const Beams *b, GLuint prog, V3 a, V3 b_end, V3 cam, float width, float strength);
/* Round camera-facing glow of the given radius. */
void beam_flare(const Beams *b, GLuint prog, V3 centre, V3 cam, V3 cam_right, float radius, float strength);

#endif
