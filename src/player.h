#ifndef PLAYER_H
#define PLAYER_H
#include "gl.h"
#include "mathx.h"

#define PLAYER_RADIUS 0.32f
#define PLAYER_HEIGHT 1.65f

typedef struct {
    V3 pos;        /* feet position, always on the terrain */
    float vx, vz;  /* horizontal velocity (m/s) */
    float yaw;     /* facing; 0 looks down -Z */
    float lean;    /* pitch in radians: + tilts forward, - tilts backward (spring driven) */
    float lean_vel, fwd_prev, accel_f; /* spring velocity, last forward speed, smoothed forward accel */
    float speed;
    float des_vx, des_vz; /* velocity the input is asking for (m/s), used by motion matching */
    float vy;      /* vertical velocity while airborne */
    int air;       /* 1 while off the ground */
    int yaw_drive;     /* 1: a turn animation owns the heading this frame */
    float yaw_rate;    /* rad/s applied while yaw_drive is set */
    int jump_req;  /* set by the caller for one frame to request a jump */
    int sliding;   /* true while the slope is steep enough to carry the player */
    /* capsule mesh */
    GLuint vao, vbo, ibo;
    GLsizei index_count;
} Player;

void player_init(Player *p, float x, float z);
/* move: desired direction in the world XZ plane, length 0..1. run: gait, 0 jog, 1 sprint, 2 walk. */
void player_update(Player *p, float dt, float move_x, float move_z, int run);
M4 player_model(const Player *p);
void player_draw(const Player *p);

#endif
