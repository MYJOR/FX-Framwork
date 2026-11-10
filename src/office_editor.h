#ifndef OFFICE_EDITOR_H
#define OFFICE_EDITOR_H
#include "platform.h"
#include "interior.h"
#include "office_scene.h"

/* Scene editor for the office, drawn with CUI over the 3D view.
 *
 *   viewport   click an object to select it, drag to slide it over the floor (furniture) or the
 *              ceiling (lights, vents), mouse wheel to rotate it. The selected object carries a
 *              gizmo. Move tool: drag its X or Z arrow to move along that axis only. Rotate
 *              tool: drag its ring to turn the object about the vertical axis
 *   panel      object list, exact placement, light intensity and warmth, the look, rebake controls
 *
 * The editor only changes the Office and the IntLook and reports what changed; the caller applies
 * it (office_update) and drives the rebake. */

typedef struct {
    int active;       /* editor open: cursor free, panel visible */
    int selected;     /* object index, or -1 */
    int tool;         /* gizmo on the selected object: EDIT_TOOL_MOVE or EDIT_TOOL_ROTATE */
    int live;         /* rebake lighting while editing */
    int snap;         /* snap movement to the object's grid and rotation to 15 degrees */
    int view;         /* INT_VIEW_* */
    int ao, reflections, vsync;
    float ao_strength; /* remembered while AO is switched off */
    int stale;        /* lighting is out of date (live rebake off): set and cleared by the caller */
    float fps;
} EditorState;

enum { EDIT_TOOL_MOVE, EDIT_TOOL_ROTATE };

enum {
    EDIT_SCENE = 1,     /* objects or lights changed: office_update, then rebake shadows and lighting */
    EDIT_BAKE_LOOK = 2, /* a look setting that is baked in changed */
    EDIT_FULL_BAKE = 4, /* "Full rebake" pressed */
    EDIT_VSYNC = 8
};

int editor_init(EditorState *st, const IntLook *look);
void editor_shutdown(void);
/* Platform event hook. Events are ignored while the editor is closed. */
void editor_event(void *user, const PlatEvent *e);
/* The left mouse button went down since the last editor_frame. */
int editor_clicked(void);
/* Width of the 3D view: the window, minus the panel while the editor is open. Render the scene this
   wide, at the left edge of the window. */
int editor_view_width(const EditorState *st, int window_width);
/* Where the selected object's gizmo arrow is on screen (axis 0 = X, 1 = Z): shaft start x, y and
   tip x, y in window pixels. Returns 0 if there is no such arrow. For scripted tests. */
int editor_gizmo_handle(int axis, float out[4]);
/* The point of the rotate ring at a given angle (radians, same sense as OfficeObject.yaw), in
   window pixels. Returns 0 if the ring is not shown. For scripted tests. */
int editor_gizmo_ring_point(float angle, float out[2]);
/* A text field has the keyboard: typing must not move the camera. */
int editor_wants_keyboard(const EditorState *st);
/* Builds, handles and draws the editor for this frame, on top of the rendered scene. Returns EDIT_* flags. */
int editor_frame(EditorState *st, Office *o, IntLook *look, const IntRenderer *r, V3 cam, V3 fwd, int width, int height, float dt);

#endif
