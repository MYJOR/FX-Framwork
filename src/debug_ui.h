#ifndef DEBUG_UI_H
#define DEBUG_UI_H
#include "platform.h"

/* Diagnostics + debug overlay, drawn with CUI (immediate-mode UI).
 *   F1 cycles: off -> HUD -> full panel.   F2 pauses the profiler capture.
 *   F3 wireframe.   F4 cycles the debug view.   F5 toggles vsync. */

typedef struct {
    int overlay;                 /* 0 off, 1 HUD, 2 panel */
    int tab;                     /* panel tab */
    int view_mode;               /* sand shader debug view, 0 = final */
    int wireframe;
    int draw_streaks, draw_dust, draw_player, draw_terrain;
    int vsync;
    int panel_hovered;           /* set by debug_ui_frame */
} DebugState;

/* What the world panel shows; the editable scene values are pointers. */
typedef struct {
    float pos[3];
    float speed, slope_deg, terrain_h, gust;
    int sliding, free_cam;
    float cam[3];
    float time;
    float *sun_elevation, *sun_azimuth, *fov; /* radians; edited in place */
    int trail_dirty_bytes;
    float anim_w[4];             /* idle, walk, jog, sprint blend weights */
} DebugWorld;

enum { DEBUG_VIEW_COUNT = 6 };

int debug_ui_init(DebugState *s);
void debug_ui_shutdown(void);
void debug_ui_event(void *user, const PlatEvent *e); /* register with platform_set_event_hook */
/* Builds and renders the overlay; restores the GL state the scene expects. */
void debug_ui_frame(DebugState *s, DebugWorld *w, int width, int height, float dt);
/* True while the pointer is over the panel (so the game should ignore it). */
int debug_ui_wants_mouse(const DebugState *s);

#endif
