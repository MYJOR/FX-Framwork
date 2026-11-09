#ifndef CHARACTER_H
#define CHARACTER_H
#include "gl.h"
#include "mathx.h"

/* Skinned, animated character loaded from a glTF/GLB (Universal Animation Library).
 * Locomotion is a 1D blend over ground speed: idle, walk, jog, sprint. */

#define CH_MAX_JOINTS 80
#define CH_CLIPS 4
#define CHAR_HEIGHT_M 1.25f   /* stylised: a short body (was 1.75)... */
#define CHAR_HEAD_SCALE 0.8f  /* ...head smaller than the model's original proportions */
#define CHAR_LIMB_FAT 1.5f    /* arms and legs are this much thicker (across the bone, not along it) */
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

    char jname[CH_MAX_JOINTS][48];           /* joint names, used to retarget clips from other files */
    ChClip slide_start, slide_loop;          /* from UAL2 (optional) */
    float slide_w, slide_t;                  /* blend weight toward the slide pose, time in slide */
    int was_sliding;

    ChClip jump_start, jump_loop, jump_land; /* from UAL1 */
    int air, was_air;                        /* airborne flag from the controller */
    float air_w, air_t, land_t;

    /* spell casting: upper-body override from UAL1 Spell_Double_* */
    ChClip cast_enter, cast_loop;
    int casting, was_casting;
    float cast_w, cast_t;
    float cast_mask[CH_MAX_JOINTS];          /* 1 for spine_01 and everything above it */
    int hand_j[2], fore_j[2];                /* hand_l/r and lowerarm_l/r */
    V3 hand_pos[2];                          /* palm centre, model space (set each update) */

    /* foot IK */
    M4 ik_model;                             /* model matrix used for drawing (translation = feet reference point) */
    int ik_on;
    int ik_j[2][4];                          /* thigh, calf, foot, ball joint per leg (-1 if missing) */
    /* critically damped spring smoothing of the final pose */
    float sp_t[CH_MAX_JOINTS][3], sp_q[CH_MAX_JOINTS][4];   /* smoothed local pose */
    float sv_t[CH_MAX_JOINTS][3], sv_q[CH_MAX_JOINTS][4];   /* spring velocities */
    float sp_hl[CH_MAX_JOINTS];                             /* per-joint half-life, seconds */
    int sp_init, spring_on;
    int limb_j[8][2];                        /* limb joint, and the joint its bone points toward */
    float look_t[3];                         /* world point the head should look at */
    int look_on;
    int look_j[3];                           /* spine_03, neck_01, Head */
    float look_yaw, look_pitch;              /* smoothed head turn relative to the body */
    struct MotionDB *mdb;                    /* motion matching database, NULL = plain speed blend */
    float ik_tilt[2][3];                     /* smoothed foot tilt (mesh-space target up vector) */
    float ik_off;                            /* smoothed body height offset, metres */
    float ik_d[2];                           /* smoothed ground offset per foot, metres */
} Character;

/* Build the motion matching database from the root-motion libraries (UAL1_RM.glb / UAL2_RM.glb). */
int character_load_motion(Character *c, const char *ual1_rm, const char *ual2_rm);
/* Controller state for this frame (world space, m/s): actual velocity, desired velocity, capsule yaw. */
void character_set_motion(Character *c, float vx, float vz, float dvx, float dvz, float yaw);

/* 1 while a turn clip is playing; *yaw_rate is how fast it rotates the body (rad/s, capsule-yaw sign). */
int character_mm_turn(const Character *c, float *yaw_rate);
/* Head (and a bit of neck/spine) tracks a world-space point, e.g. where the camera is looking. */
void character_set_look(Character *c, float x, float y, float z, int on);
/* Name and time of the motion matching clip being played (NULL if motion matching is off). */
const char *character_mm_clip(const Character *c, float *t);
/* Whether the controller is airborne this frame (plays Jump_Start / Jump_Loop / Jump_Land). */
void character_set_air(Character *c, int air);

/* Hold to cast: arms raise (upper body only, legs keep moving). */
void character_set_cast(Character *c, int on);
/* Beam strength 0..1 (0 = not firing yet); fills the palm positions in model space. */
float character_cast_beam(const Character *c, V3 hands[2]);

/* Enable foot IK for this frame: model is the exact matrix the character is drawn with. */
void character_set_ik(Character *c, M4 model, int on);

/* Load the Slide_Start / Slide_Loop clips from another glTF with the same skeleton. */
int character_load_slide(Character *c, const char *glb_path);

/* Returns 1 on success. Needs a current GL context. */
int character_load(Character *c, const char *glb_path);
/* Advance the animation for the given ground speed (m/s) and rebuild the bone matrices. */
void character_update(Character *c, float dt, float speed, int sliding);
/* Uploads uBones and draws; the skinned program must already be bound. */
void character_draw(const Character *c, GLuint program);

#endif
