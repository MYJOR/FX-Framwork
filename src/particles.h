#ifndef PARTICLES_H
#define PARTICLES_H
#include "gl.h"
#include "mathx.h"

#define PARTICLE_COUNT 24000
#define PARTICLE_RADIUS 70.0f

/* Wind-blown sand. Struct-of-arrays, one fixed allocation, no per-particle heap use. */
typedef struct {
    float px[PARTICLE_COUNT], py[PARTICLE_COUNT], pz[PARTICLE_COUNT];
    float vx[PARTICLE_COUNT], vy[PARTICLE_COUNT], vz[PARTICLE_COUNT];
    float life[PARTICLE_COUNT], maxlife[PARTICLE_COUNT];
    float hgt[PARTICLE_COUNT];  /* height above ground, updated each step */
    float lift[PARTICLE_COUNT]; /* >0 for grains thrown off crests */
    unsigned rng;
    GLuint vao, vbo;
} Particles;

void particles_init(Particles *p, V3 cam);
void particles_update(Particles *p, float dt, V3 cam, float time);
/* Uploads vertex data and draws; call with blending enabled and the particle program bound. */
void particles_draw(Particles *p, V3 cam, float time);

#endif
