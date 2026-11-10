#ifndef OFFICE_EDITOR_H
#define OFFICE_EDITOR_H
#include "platform.h"
#include "interior.h"
#include "office_scene.h"

/* Scene editor for the office, drawn with CUI around the 3D view.
 *
 *   menu bar   File (new, save, revert), Edit (undo, redo, duplicate, delete...), Add, View, Build
 *   toolbar    save, undo / redo, the Select / Move / Rotate tools, snapping, object actions,
 *              build lighting, play
 *   viewport   click an object to select it. Move tool: drag the object over the floor or ceiling,
 *              or pull its X or Z arrow. Rotate tool: turn its ring. Mouse wheel rotates
 *   outliner   every object in folders by kind, with search, show / hide and a right-click menu
 *   details    name, transform, light settings, visibility and lock of the selected object; the
 *              "World settings" tab holds the lighting build and the image settings
 *   status bar lighting build state, selection, unsaved changes, frame rate
 *
 * The editor only changes the Office and the IntLook and reports what changed; the caller applies
 * it (office_update) and drives the rebake. */

typedef struct {
    int active;       /* editor open: cursor free, panels visible */
    int selected;     /* object index, or -1 */
    int tool;         /* EDIT_TOOL_* */
    int live;         /* rebake lighting while editing */
    int snap;         /* snap movement to the object's grid and rotation to 15 degrees */
    int view;         /* INT_VIEW_* */
    int ao, reflections, vsync, dof;
    float ao_strength; /* remembered while AO is switched off */
    int stale;        /* lighting is out of date (live rebake off): set and cleared by the caller */
    float fps;
    /* requests to the caller, cleared by the caller once served */
    int focus;        /* turn the camera to this object, or -1 */
    int play;         /* leave the editor */
    char scene_path[320]; /* where Save writes and Revert reads */
} EditorState;

enum { EDIT_TOOL_MOVE, EDIT_TOOL_ROTATE, EDIT_TOOL_SELECT };

enum {
    EDIT_SCENE = 1,     /* objects or lights changed: office_update, then rebake shadows and lighting */
    EDIT_BAKE_LOOK = 2, /* a look setting that is baked in changed */
    EDIT_FULL_BAKE = 4, /* "Build lighting" pressed */
    EDIT_VSYNC = 8
};

int editor_init(EditorState *st, const IntLook *look);
void editor_shutdown(void);
/* Platform event hook. Events are ignored while the editor is closed. */
void editor_event(void *user, const PlatEvent *e);
/* The left mouse button went down since the last editor_frame. */
int editor_clicked(void);
/* Where the 3D view is in the window: x, y (from the top), width, height in pixels. The whole
   window while playing; the space between the editor's panels while editing. */
void editor_view_rect(const EditorState *st, int window_width, int window_height, int out[4]);
/* Where the selected object's gizmo arrow is on screen (axis 0 = X, 1 = Z): shaft start x, y and
   tip x, y in window pixels. Returns 0 if there is no such arrow. For scripted tests. */
int editor_gizmo_handle(int axis, float out[4]);
/* The point of the rotate ring at a given angle (radians, same sense as OfficeObject.yaw), in
   window pixels. Returns 0 if the ring is not shown. For scripted tests. */
int editor_gizmo_ring_point(float angle, float out[2]);
/* A text field has the keyboard: typing must not move the camera. */
int editor_wants_keyboard(const EditorState *st);
/* Runs a command by name, as if chosen from the menus, on the next editor_frame. For scripted
   tests: "add-chair", "add-desk", "add-light", "add-vent", "duplicate", "delete", "hide", "lock",
   "undo", "redo", "save", "revert", "new". Returns 0 for an unknown name. */
int editor_command(const char *name);
/* Builds, handles and draws the editor for this frame, around the rendered scene. Returns EDIT_* flags. */
int editor_frame(EditorState *st, Office *o, IntLook *look, const IntRenderer *r, V3 cam, V3 fwd, int width, int height, float dt);

#endif
