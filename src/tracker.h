#ifndef TRACKER_H
#define TRACKER_H
#include "interior.h"

/* Motion tracker: a hand-held scanner the player raises in front of the camera.
 *
 * It sends out a ping, and whatever is moving within range shows up on its screen as a blip that
 * lights when the ping passes over it and fades until the next one: ahead is up, the player is the
 * mark near the bottom. Things that hold still do not show.
 *
 * The device is a view model. It is drawn by the interior renderer's held_draw hook into the scene's
 * HDR target, lit by the room it is in, and its screen is emissive: it blooms, it adapts with the eye
 * and it spills onto its own bezel. While it is up the lens focuses on it and the room behind goes
 * soft; tracker_update's focus_room pulls the focus back to the room, and the tracker goes soft
 * instead (IntLook.dof_hand). */

#define TRACKER_MAX_CONTACTS 8
#define TRACKER_RANGE 12.0f /* metres: the outermost ring */

typedef struct {
    float x, z;    /* where it is, world space */
    float motion;  /* 0 = still (not shown) .. 1 = moving */
} TrackerContact;

typedef struct {
    Program prog;
    GLuint vao, vbo;
    int vertex_count;

    float raise, raise_vel;   /* 0 = lowered out of sight, 1 = held up; eased with a spring */
    float room_focus;         /* 0 = the lens is on the tracker, 1 = on the room */
    float sway_x, sway_y;     /* the hand trails the view when it turns, metres */
    float bob_x, bob_y;       /* and swings with each step */
    float ping;               /* the ping's phase: 0..1 is the ring crossing the screen, then a short rest */
    float time;
    V3 origin; float yaw;     /* where the player stands and faces */
    TrackerContact contacts[TRACKER_MAX_CONTACTS];
    float level[TRACKER_MAX_CONTACTS]; /* each contact's motion, smoothed: a blip does not snap on and off */
    int contact_count;
    float nearest;            /* metres to the nearest moving contact, < 0: none */
} Tracker;

/* Needs the shader directory and the interior prelude set (after interior_init). 0 on failure. */
int tracker_init(Tracker *t);
void tracker_reload_shaders(Tracker *t);

typedef struct {
    int raised;                   /* the player holds it up */
    int focus_room;               /* ... but looks past it at the room */
    float yaw_rate, pitch_rate;   /* how fast the view turns, radians per second */
    float bob_phase, bob_amount;  /* the walk cycle (FpsPlayer) */
    V3 origin; float yaw;         /* the player */
    const TrackerContact *contacts; int contact_count;
} TrackerInput;
/* dt = 0 settles everything at once (screenshots). */
void tracker_update(Tracker *t, const TrackerInput *in, float dt);

/* Is any of it in view? */
int tracker_visible(const Tracker *t);
/* Points the lens: sets look->dof_hand from how far the tracker is up and where the player looks. */
void tracker_apply_look(const Tracker *t, IntLook *look);
/* IntRenderer.held_draw, with the Tracker as user. */
void tracker_draw(void *user, const IntScene *s, const IntHeldView *v);

#endif
