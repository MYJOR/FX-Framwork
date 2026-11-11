#ifndef HANDPRINTS_H
#define HANDPRINTS_H
#include "interior.h"

/* Handprints of something unseen, slapped down on the floor one after the other as it comes closer:
 * black tar, pressed in from the palm outwards with a splash, wet and glossy at first, then drying,
 * breaking up and gone.
 *
 * Each print is a quad lying just over the floor. Its shape, its thickness and its highlights are
 * worked out in shaders/handprint.frag. They are drawn by the interior renderer's overlay_draw hook,
 * blended over the lit room. */

#define HANDPRINT_MAX 96
#define HANDPRINT_LIFE 10.0f /* seconds from the slap until nothing is left */
#define HANDPRINT_HALF 0.20f /* metres: half the side of a print's quad; the hand itself is about 0.27 m long */

typedef struct {
    float x, z;      /* centre, on the floor */
    float yaw;       /* which way the fingers point; 0 = down -Z, like the player's yaw */
    float age;       /* seconds since it was put down; >= HANDPRINT_LIFE: free */
    float seed;      /* 0..1, varies the shape */
    int left;        /* a left hand: mirrored */
} Handprint;

typedef struct {
    Program prog;
    GLuint vao, vbo;
    Handprint p[HANDPRINT_MAX];
    int next;        /* slot the next print goes into: the oldest is overwritten */
    int live;        /* prints drawn last frame */
} Handprints;

/* Needs the shader directory and the interior prelude set (after interior_init). 0 on failure. */
int handprints_init(Handprints *h);
void handprints_reload_shaders(Handprints *h);
/* age: 0 for a print made now; more to put down one that has been there a while. */
void handprints_add(Handprints *h, float x, float z, float yaw, int left, float age);
void handprints_update(Handprints *h, float dt);
/* IntRenderer.overlay_draw, with the Handprints as user. */
void handprints_draw(void *user, const IntScene *s, const IntOverlayView *v);

#endif
