#ifndef CHARACTER_H
#define CHARACTER_H
#include "gl.h"
#include "mathx.h"

/* Skinned, animated character loaded from a glTF/GLB (Universal Animation Library).
 * Locomotion is a 1D blend over ground speed: idle, walk, jog, sprint. */

#define CH_MAX_JOINTS 80
#define CH_CLIPS 4
enum { CH_IDLE, CH_WALK, CH_JOG, CH_SPRINT };

typedef struct { int n; float *t; float *v; } ChTrack;           /* keyframes of one channel */
typedef struct { ChTrack tr[3]; } ChJointAnim;                    /* 0 translation, 1 rotation, 2 scale */
typedef struct { float dur; ChJointAnim *ja; int loaded; } ChClip;

typedef struct {
    int njoints;
    int parent[CH_MAX_JOINTS];               /* joint index of parent, -1 if none */
    float base[CH_MAX_JOINTS][16];           /* world matrix of a non-joint parent (roots only) */
    float rest_t[CH_MAX_JOINTS][3], rest_q[CH_MAX_JOINTS][4], rest_s[CH_MAX_JOINTS][3];
    float inv_bind[CH_MAX_JOINTS][16];
    float inv_mesh[16];
    M4 bones[CH_MAX_JOINTS];                 /* skinning matrices, uploaded as uBones[] */

    GLuint vao, vbo, ibo;
    GLsizei index_count;
    float height;                            /* model height in file units */

    ChClip clip[CH_CLIPS];
    float t_idle, phase, spd;                /* idle time, shared locomotion phase 0..1, smoothed speed */
    float w[CH_CLIPS];                       /* current blend weights (diagnostics) */
} Character;

/* Returns 1 on success. Needs a current GL context. */
int character_load(Character *c, const char *glb_path);
/* Advance the animation for the given ground speed (m/s) and rebuild the bone matrices. */
void character_update(Character *c, float dt, float speed);
/* Uploads uBones and draws; the skinned program must already be bound. */
void character_draw(const Character *c, GLuint program);

#endif
