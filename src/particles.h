#ifndef PARTICLES_H
#define PARTICLES_H
#include "gl.h"
#include "mathx.h"

#define PARTICLE_COUNT 24000
#define PARTICLE_RADIUS 70.0f
#define KICK_COUNT 6000   /* short-lived grains and dust thrown up by the character */

/* Wind-blown sand. Struct-of-arrays, one fixed allocation, no per-particle heap use. */
typedef struct {
    float px[PARTICLE_COUNT], py[PARTICLE_COUNT], pz[PARTICLE_COUNT];
    float vx[PARTICLE_COUNT], vy[PARTICLE_COUNT], vz[PARTICLE_COUNT];
    float life[PARTICLE_COUNT], maxlife[PARTICLE_COUNT];
    float hgt[PARTICLE_COUNT];  /* height above ground, updated each step */
    float lift[PARTICLE_COUNT]; /* >0 for grains thrown off crests */
    /* kick-up pool (ring buffer): kind 0 = flying grain (streak), 1 = dust puff (soft sprite) */
    float kx[KICK_COUNT], ky[KICK_COUNT], kz[KICK_COUNT];
    float kvx[KICK_COUNT], kvy[KICK_COUNT], kvz[KICK_COUNT];
    float klife[KICK_COUNT], kmax[KICK_COUNT], ksize[KICK_COUNT];
    unsigned char kkind[KICK_COUNT];
    int khead;
    GLuint dvao, dvbo;
    int dust_count;
    int line_count;      /* streak segments drawn last frame (diagnostics) */
    int kick_grains;     /* live kick-up grains last frame */
    int skip_streaks;    /* debug: don't issue the streak draw */
    unsigned rng;
    GLuint vao, vbo;
} Particles;

void particles_init(Particles *p, V3 cam);
void particles_update(Particles *p, float dt, V3 cam, float time);
/* Uploads vertex data and draws; call with blending enabled and the particle program bound. */
void particles_draw(Particles *p, V3 cam, float time);

/* Emitters. (x,y,z) is the contact point, (dx,dz) the unit direction of travel. */
void particles_emit_step(Particles *p, float x, float y, float z, float dx, float dz, float speed);
void particles_emit_spray(Particles *p, float x, float y, float z, float dx, float dz, float speed);
/* Soft dust sprites (GL_POINTS); call after particles_draw with the dust program bound. */
void particles_draw_dust(const Particles *p);

#endif
