#include "office_editor.h"
#include "gl.h"
#include <cui/cui.h>
#include <cui/cui_icons.h>
#include <cui/cui_render.h>
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define PI_F 3.14159265f
#define PANEL_W 344        /* outliner + details column */
#define TOP_GUESS 78       /* menu bar + toolbar, until the first layout says how tall they are */
#define BOTTOM_GUESS 28    /* status bar */
#define PICK_PAD 0.04f     /* ceiling fixtures are flat: give them some thickness to click on */
#define CAM_NEAR 0.05f
#define MAX_SEGMENTS 64
#define UNDO_MAX 48

#define COL_BAR CUI_RGB(0x15171C)
#define COL_PANEL CUI_RGB(0x0E1014)

static cui_ctx *g_ui;
static cui_gl_renderer *g_gl;
static EditorState *g_state;

/* raw input since the last frame */
static float g_mouse_x = -1, g_mouse_y = -1, g_wheel;
static int g_lmb_down, g_lmb_pressed;

/* where the 3D view was laid out, window pixels */
static cui_rect g_vp;
static int g_vp_valid;

/* drag in progress */
static int g_dragging;
static float g_grab_y;          /* height of the plane the object slides on */
static float g_grab_dx, g_grab_dz; /* object position minus the grabbed point */

/* Move gizmo on the selected object: one arrow per axis it can travel along (world X and Z; height
   is fixed by what the object is). Dragging an arrow moves the object along that axis only. */
enum { AXIS_X, AXIS_Z, AXIS_COUNT };
typedef struct { cui_vec2 a, b; int visible; } GizmoHandle; /* shaft start and arrow tip, window pixels */
static GizmoHandle g_gizmo[AXIS_COUNT];
static int g_gizmo_shown;       /* the selected object carries a gizmo this frame */
static int g_gizmo_hot = -1;    /* axis under the pointer */
static int g_gizmo_drag = -1;   /* axis being dragged */
static V3 g_gizmo_origin;       /* gizmo origin when the drag began */
static float g_gizmo_s0;        /* where on the axis line it was grabbed */
static V3 g_gizmo_start;        /* the object's position when the drag began */

/* Rotate gizmo: a ring around the vertical axis through the object. Dragging it turns the object by
   the angle the pointer sweeps around the centre. */
#define RING_POINTS 64
static cui_vec2 g_ring[RING_POINTS];
static int g_ring_ok[RING_POINTS];
static int g_ring_visible, g_ring_hot, g_ring_drag;
static V3 g_ring_centre;        /* in the world, at the gizmo's height */
static float g_ring_radius;
static float g_ring_a0;         /* pointer angle when the drag began */
static float g_ring_yaw0;       /* the object's yaw when the drag began */
static float g_ring_pointer;    /* pointer angle now, for the spoke that is drawn while dragging */

/* outlines for this frame, in window pixels */
typedef struct { cui_vec2 a, b; int strong; } Segment;
static Segment g_segments[MAX_SEGMENTS];
static int g_segment_count;

/* Undo: whole-scene snapshots. The objects are a few kilobytes, so a snapshot per edit is cheap and
   every kind of edit (move, add, delete, rename, hide...) is undone the same way. */
typedef struct { OfficeObject objects[OFFICE_MAX_OBJECTS]; int count, selected; } Snapshot;
static Snapshot g_undo[UNDO_MAX], g_redo[UNDO_MAX];
static int g_undo_n, g_redo_n;
static Snapshot g_prev;         /* the scene as it was when the last frame ended */
static int g_prev_valid;
static int g_gesture;           /* a drag or a run of slider changes is one undo step */
static int g_unsaved;

/* commands: chosen from a menu, the toolbar, a shortcut or a context menu; run once the UI is built */
enum {
    CMD_NONE, CMD_NEW, CMD_SAVE, CMD_REVERT, CMD_UNDO, CMD_REDO, CMD_ADD, CMD_DUPLICATE, CMD_DELETE,
    CMD_HIDE, CMD_LOCK, CMD_FOCUS, CMD_RESET, CMD_DESELECT, CMD_PLAY, CMD_BUILD
};
static int g_cmd, g_cmd_arg;         /* arg: object index (-1 = the selection) or the type to add */
static int g_ext_cmd, g_ext_arg;     /* from editor_command */
static void request(int cmd, int arg) { g_cmd = cmd; g_cmd_arg = arg; }

/* outliner and details state */
static char g_filter[48];
static bool g_folder_open[OBJ_KIND_COUNT] = {true, true, true};
static int g_tab;
static float g_side_split = 0.5f;
static V3 g_cam, g_fwd;

static const char *k_views[INT_VIEW_COUNT] = {"Final", "Albedo", "Normals", "Lighting", "Direct light", "Indirect light",
                                              "Ambient obscurance", "Shadows", "Roughness / metallic", "Specular"};
static const char *k_folders[OBJ_KIND_COUNT] = {"Furniture", "Lights", "Ceiling"};
static const char *k_type_icons[OT_COUNT] = {CUI_ICON_BOX, CUI_ICON_PACKAGE, CUI_ICON_SUN, CUI_ICON_GRID_3X3};

static void *loader(const char *name) { return platform_gl_proc(name); }

int editor_init(EditorState *st, const IntLook *look) {
    memset(st, 0, sizeof *st);
    st->selected = -1;
    st->focus = -1;
    st->live = 1;
    st->snap = 0;
    st->view = INT_VIEW_FINAL;
    st->ao = look->ao_strength > 0.0f;
    st->ao_strength = st->ao ? look->ao_strength : interior_default_look().ao_strength;
    st->reflections = 1;
    st->dof = look->dof_strength > 0.0f;
    st->vsync = 1;
    g_state = st;
    g_gl = cui_gl_create(loader);
    if (!g_gl) { fprintf(stderr, "CUI: OpenGL renderer unavailable\n"); return 0; }
    g_ui = cui_create(&(cui_ctx_desc){.renderer = cui_gl_iface(g_gl), .theme = CUI_THEME_DARK});
    return g_ui != NULL;
}

void editor_shutdown(void) {
    if (g_ui) cui_destroy(g_ui);
    if (g_gl) cui_gl_destroy(g_gl);
    g_ui = NULL; g_gl = NULL;
}

static cui_key map_key(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (cui_key)(CUI_KEY_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return (cui_key)(CUI_KEY_0 + (vk - '0'));
    switch (vk) {
    case VK_RETURN: return CUI_KEY_ENTER;  case VK_BACK: return CUI_KEY_BACKSPACE;
    case VK_DELETE: return CUI_KEY_DELETE; case VK_SPACE: return CUI_KEY_SPACE;
    case VK_LEFT: return CUI_KEY_LEFT;     case VK_RIGHT: return CUI_KEY_RIGHT;
    case VK_UP: return CUI_KEY_UP;         case VK_DOWN: return CUI_KEY_DOWN;
    case VK_HOME: return CUI_KEY_HOME;     case VK_END: return CUI_KEY_END;
    case VK_OEM_MINUS: return CUI_KEY_MINUS; case VK_OEM_PERIOD: return CUI_KEY_PERIOD;
    }
    return CUI_KEY_NONE; /* Tab, Esc and the F keys belong to the application */
}

void editor_event(void *user, const PlatEvent *e) {
    (void)user;
    if (e->type == PE_MOUSE_BUTTON && e->a == 0) {
        if (e->down && !g_lmb_down) g_lmb_pressed = 1;
        g_lmb_down = e->down != 0;
    }
    if (!g_ui || !g_state || !g_state->active) return;
    switch (e->type) {
    case PE_MOUSE_MOVE: g_mouse_x = e->x; g_mouse_y = e->y; cui_input_mouse_move(g_ui, e->x, e->y); break;
    case PE_MOUSE_BUTTON: cui_input_mouse_button(g_ui, (cui_mouse_button)e->a, e->down != 0); break;
    case PE_WHEEL: g_wheel += e->y; cui_input_scroll(g_ui, e->x, e->y); break;
    case PE_MOUSE_LEAVE: g_mouse_x = g_mouse_y = -1; cui_input_mouse_leave(g_ui); break;
    case PE_FOCUS: cui_input_focus(g_ui, e->a != 0); break;
    case PE_CHAR: cui_input_text(g_ui, e->utf8); break;
    case PE_KEY: {
        cui_key k = map_key(e->a);
        if (k != CUI_KEY_NONE) cui_input_key(g_ui, k, e->down != 0, e->mods);
        break;
    }
    }
}

int editor_clicked(void) { return g_lmb_pressed; }

void editor_view_rect(const EditorState *st, int window_width, int window_height, int out[4]) {
    if (!st->active) { out[0] = out[1] = 0; out[2] = window_width; out[3] = window_height; return; }
    if (g_vp_valid) {
        out[0] = (int)(g_vp.x + 0.5f); out[1] = (int)(g_vp.y + 0.5f);
        out[2] = (int)(g_vp.w + 0.5f); out[3] = (int)(g_vp.h + 0.5f);
    } else {
        out[0] = 0; out[1] = TOP_GUESS;
        out[2] = window_width - PANEL_W; out[3] = window_height - TOP_GUESS - BOTTOM_GUESS;
    }
    if (out[0] < 0) out[0] = 0;
    if (out[1] < 0) out[1] = 0;
    if (out[2] > window_width - out[0]) out[2] = window_width - out[0];
    if (out[3] > window_height - out[1]) out[3] = window_height - out[1];
    if (out[2] < 64) out[2] = 64;
    if (out[3] < 64) out[3] = 64;
}

int editor_wants_keyboard(const EditorState *st) { return st->active && g_ui && cui_wants_keyboard(g_ui); }

int editor_command(const char *name) {
    static const struct { const char *name; int cmd, arg; } table[] = {
        {"add-chair", CMD_ADD, OT_CHAIR}, {"add-desk", CMD_ADD, OT_DESK}, {"add-light", CMD_ADD, OT_LIGHT_PANEL},
        {"add-vent", CMD_ADD, OT_VENT},   {"duplicate", CMD_DUPLICATE, -1}, {"delete", CMD_DELETE, -1},
        {"hide", CMD_HIDE, -1},           {"lock", CMD_LOCK, -1},           {"undo", CMD_UNDO, -1},
        {"redo", CMD_REDO, -1},           {"save", CMD_SAVE, -1},           {"revert", CMD_REVERT, -1},
        {"new", CMD_NEW, -1},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(name, table[i].name)) { g_ext_cmd = table[i].cmd; g_ext_arg = table[i].arg; return 1; }
    return 0;
}

/* ---- camera maths: the same projection interior_render uses -------------------------------------- */

typedef struct { V3 pos, fwd, right, up; float tan_y, tan_x; int w, h; float ox, oy; } View; /* ox, oy: the view's top left corner in the window */

static View make_view(V3 cam, V3 fwd, float fov, int x, int y, int w, int h) {
    View v;
    v.pos = cam; v.fwd = v3_norm(fwd);
    v.right = v3_norm(v3_cross(v.fwd, v3(0, 1, 0)));
    v.up = v3_cross(v.right, v.fwd);
    v.tan_y = tanf(fov * 0.5f);
    v.tan_x = v.tan_y * (float)w / (float)h;
    v.w = w; v.h = h;
    v.ox = (float)x; v.oy = (float)y;
    return v;
}

static V3 view_ray(const View *v, float mx, float my) {
    float x = (2.0f * (mx - v->ox) / (float)v->w - 1.0f) * v->tan_x, y = (1.0f - 2.0f * (my - v->oy) / (float)v->h) * v->tan_y;
    return v3_norm(v3_add(v->fwd, v3_add(v3_scale(v->right, x), v3_scale(v->up, y))));
}

/* Adds the visible part of a world-space segment to the outline list. */
static void add_segment(const View *v, V3 a, V3 b, int strong) {
    if (g_segment_count >= MAX_SEGMENTS) return;
    V3 ra = v3_add(a, v3_scale(v->pos, -1.0f)), rb = v3_add(b, v3_scale(v->pos, -1.0f));
    float za = v3_dot(ra, v->fwd), zb = v3_dot(rb, v->fwd);
    if (za < CAM_NEAR && zb < CAM_NEAR) return;
    if (za < CAM_NEAR) { float t = (CAM_NEAR - za) / (zb - za); ra = v3_add(ra, v3_scale(v3_add(rb, v3_scale(ra, -1.0f)), t)); za = CAM_NEAR; }
    if (zb < CAM_NEAR) { float t = (CAM_NEAR - zb) / (za - zb); rb = v3_add(rb, v3_scale(v3_add(ra, v3_scale(rb, -1.0f)), t)); zb = CAM_NEAR; }
    Segment *s = &g_segments[g_segment_count++];
    s->a.x = (v3_dot(ra, v->right) / (za * v->tan_x) + 1.0f) * 0.5f * (float)v->w + v->ox;
    s->a.y = (1.0f - v3_dot(ra, v->up) / (za * v->tan_y)) * 0.5f * (float)v->h + v->oy;
    s->b.x = (v3_dot(rb, v->right) / (zb * v->tan_x) + 1.0f) * 0.5f * (float)v->w + v->ox;
    s->b.y = (1.0f - v3_dot(rb, v->up) / (zb * v->tan_y)) * 0.5f * (float)v->h + v->oy;
    s->strong = strong;
}

/* The object's bounding box as twelve edges. */
static void add_outline(const View *v, const OfficeObject *obj, int strong) {
    V3 c[8];
    for (int i = 0; i < 8; i++)
        c[i] = office_object_to_world(obj, v3(i & 1 ? obj->bmax.x : obj->bmin.x, i & 2 ? obj->bmax.y : obj->bmin.y,
                                              i & 4 ? obj->bmax.z : obj->bmin.z));
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    int flat = obj->bmax.y - obj->bmin.y < 0.01f; /* a ceiling fixture: one rectangle is enough */
    for (int e = 0; e < 12; e++) {
        if (flat && (e == 1 || e == 3 || (e >= 4 && e <= 7) || e >= 10)) continue; /* keep the four edges at the lower height */
        add_segment(v, c[edges[e][0]], c[edges[e][1]], strong);
    }
}

/* Distance along the ray to the object's bounding box, or -1. */
static float ray_object(const OfficeObject *obj, V3 origin, V3 dir) {
    float cs = cosf(obj->yaw), sn = sinf(obj->yaw);
    V3 d = v3_add(origin, v3_scale(obj->pos, -1.0f));
    float o[3] = {d.x * cs - d.z * sn, d.y, d.x * sn + d.z * cs};
    float r[3] = {dir.x * cs - dir.z * sn, dir.y, dir.x * sn + dir.z * cs};
    float mn[3] = {obj->bmin.x, obj->bmin.y - PICK_PAD, obj->bmin.z}, mx[3] = {obj->bmax.x, obj->bmax.y + PICK_PAD, obj->bmax.z};
    float t0 = 0.0f, t1 = 1e9f;
    for (int a = 0; a < 3; a++) {
        if (fabsf(r[a]) < 1e-7f) {
            if (o[a] < mn[a] || o[a] > mx[a]) return -1.0f;
            continue;
        }
        float ta = (mn[a] - o[a]) / r[a], tb = (mx[a] - o[a]) / r[a];
        if (ta > tb) { float t = ta; ta = tb; tb = t; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return -1.0f;
    }
    return t0;
}

static int pick(const Office *o, V3 origin, V3 dir, float *t_out) {
    int best = -1;
    float best_t = 1e9f;
    for (int i = 0; i < o->object_count; i++) {
        if (!o->objects[i].visible || o->objects[i].locked) continue; /* hidden and locked objects are out of reach */
        float t = ray_object(&o->objects[i], origin, dir);
        if (t >= 0.0f && t < best_t) { best_t = t; best = i; }
    }
    if (t_out) *t_out = best_t;
    return best;
}

static float snap_to(float v, float origin, float grid) { return origin + floorf((v - origin) / grid + 0.5f) * grid; }

static float wrap_angle(float a) {
    while (a > PI_F) a -= 2.0f * PI_F;
    while (a < -PI_F) a += 2.0f * PI_F;
    return a;
}

/* ---- gizmos ------------------------------------------------------------------------------------- */

static View g_view;      /* this frame's view, for painting and the test hooks */
static int g_view_valid;
static float g_yaw_now;  /* the selected object's yaw, for the readout while rotating */

#define GIZMO_SIZE 0.20f   /* arrow length as a fraction of the view height */
#define GIZMO_GRAB 10.0f   /* pixels around an arrow that count as on it */

static V3 axis_dir(int axis) { return axis == AXIS_X ? v3(1, 0, 0) : v3(0, 0, 1); }

static int project(const View *v, V3 p, cui_vec2 *out) {
    V3 r = v3_add(p, v3_scale(v->pos, -1.0f));
    float z = v3_dot(r, v->fwd);
    if (z < CAM_NEAR) return 0;
    out->x = (v3_dot(r, v->right) / (z * v->tan_x) + 1.0f) * 0.5f * (float)v->w + v->ox;
    out->y = (1.0f - v3_dot(r, v->up) / (z * v->tan_y)) * 0.5f * (float)v->h + v->oy;
    return 1;
}

/* The gizmo sits at the middle of the object: furniture at half its height, fixtures on the ceiling. */
static V3 gizmo_origin(const OfficeObject *obj) {
    return office_object_to_world(obj, v3((obj->bmin.x + obj->bmax.x) * 0.5f, (obj->bmin.y + obj->bmax.y) * 0.5f,
                                          (obj->bmin.z + obj->bmax.z) * 0.5f));
}

/* Lays the arrows out on screen. They keep the same size whatever the distance. */
static void gizmo_layout(const View *v, const OfficeObject *obj) {
    V3 origin = gizmo_origin(obj);
    float dist = v3_dot(v3_add(origin, v3_scale(v->pos, -1.0f)), v->fwd);
    float len = dist * v->tan_y * 2.0f * GIZMO_SIZE;
    for (int a = 0; a < AXIS_COUNT; a++) {
        V3 d = axis_dir(a);
        g_gizmo[a].visible = dist > CAM_NEAR && project(v, v3_add(origin, v3_scale(d, len * 0.12f)), &g_gizmo[a].a) &&
                             project(v, v3_add(origin, v3_scale(d, len)), &g_gizmo[a].b);
        if (g_gizmo[a].visible) { /* an axis seen end-on has no length to grab */
            float dx = g_gizmo[a].b.x - g_gizmo[a].a.x, dy = g_gizmo[a].b.y - g_gizmo[a].a.y;
            if (dx * dx + dy * dy < 12.0f * 12.0f) g_gizmo[a].visible = 0;
        }
    }
}

static V3 ring_point(float angle) {
    return v3(g_ring_centre.x + cosf(angle) * g_ring_radius, g_ring_centre.y, g_ring_centre.z - sinf(angle) * g_ring_radius);
}

static void ring_layout(const View *v, const OfficeObject *obj) {
    V3 origin = gizmo_origin(obj);
    float dist = v3_dot(v3_add(origin, v3_scale(v->pos, -1.0f)), v->fwd);
    g_ring_centre = v3(obj->pos.x, origin.y, obj->pos.z); /* the axis the object actually turns about */
    g_ring_radius = dist * v->tan_y * 2.0f * GIZMO_SIZE * 0.8f;
    g_ring_visible = 0;
    for (int i = 0; i < RING_POINTS; i++) {
        g_ring_ok[i] = dist > CAM_NEAR && project(v, ring_point((float)i * 2.0f * PI_F / RING_POINTS), &g_ring[i]);
        g_ring_visible |= g_ring_ok[i];
    }
}

/* Angle of the pointer around the ring's centre, measured on the ring's plane. */
static int ring_angle(const View *v, V3 ray, float *angle) {
    if (fabsf(ray.y) < 1e-3f) return 0;
    float t = (g_ring_centre.y - v->pos.y) / ray.y;
    if (t <= 0.0f || t > 60.0f) return 0;
    float x = v->pos.x + ray.x * t - g_ring_centre.x, z = v->pos.z + ray.z * t - g_ring_centre.z;
    if (x * x + z * z < 1e-6f) return 0;
    *angle = atan2f(-z, x);
    return 1;
}

int editor_gizmo_ring_point(float angle, float out[2]) {
    if (!g_state || g_state->tool != EDIT_TOOL_ROTATE || !g_ring_visible || !g_view_valid) return 0;
    cui_vec2 p;
    if (!project(&g_view, ring_point(angle), &p)) return 0;
    out[0] = p.x; out[1] = p.y;
    return 1;
}

static float point_segment_distance(float px, float py, cui_vec2 a, cui_vec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((px - a.x) * dx + (py - a.y) * dy) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float qx = a.x + dx * t - px, qy = a.y + dy * t - py;
    return sqrtf(qx * qx + qy * qy);
}

static int gizmo_hit(float mx, float my) {
    int best = -1;
    float best_d = GIZMO_GRAB;
    for (int a = 0; a < AXIS_COUNT; a++) {
        if (!g_gizmo[a].visible) continue;
        float d = point_segment_distance(mx, my, g_gizmo[a].a, g_gizmo[a].b);
        if (d < best_d) { best_d = d; best = a; }
    }
    return best;
}

/* Where along the line origin + s * dir the pointer's ray passes closest. 0 if they are parallel. */
static int axis_param(V3 origin, V3 dir, V3 ray_pos, V3 ray, float *s) {
    V3 w = v3_add(origin, v3_scale(ray_pos, -1.0f));
    float b = v3_dot(dir, ray), d = v3_dot(dir, w), e = v3_dot(ray, w);
    float denom = 1.0f - b * b;
    if (denom < 1e-3f) return 0;
    *s = (b * e - d) / denom;
    return 1;
}

int editor_gizmo_handle(int axis, float out[4]) {
    if (axis < 0 || axis >= AXIS_COUNT || !g_gizmo[axis].visible || (g_state && g_state->tool != EDIT_TOOL_MOVE)) return 0;
    out[0] = g_gizmo[axis].a.x; out[1] = g_gizmo[axis].a.y; out[2] = g_gizmo[axis].b.x; out[3] = g_gizmo[axis].b.y;
    return 1;
}

static int ring_hit(float mx, float my) {
    for (int i = 0; i < RING_POINTS; i++) {
        int j = (i + 1) % RING_POINTS;
        if (g_ring_ok[i] && g_ring_ok[j] && point_segment_distance(mx, my, g_ring[i], g_ring[j]) < GIZMO_GRAB) return 1;
    }
    return 0;
}

static void paint_ring(cui_ctx *ui) {
    int lit = g_ring_drag || g_ring_hot;
    cui_color col = lit ? CUI_RGB(0xFFE066) : CUI_RGB(0x5BD68A); /* Y green */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < RING_POINTS; i++) {
            int j = (i + 1) % RING_POINTS;
            if (!g_ring_ok[i] || !g_ring_ok[j]) continue;
            if (pass == 0) cui_draw_line(ui, g_ring[i], g_ring[j], lit ? 6.0f : 5.0f, CUI_RGBA(0x00000066));
            else cui_draw_line(ui, g_ring[i], g_ring[j], lit ? 3.5f : 2.5f, col);
        }
    if (g_ring_drag && g_view_valid && g_state->selected >= 0) { /* spokes: where it started, where it is; and the angle */
        cui_vec2 c, a, b;
        if (project(&g_view, g_ring_centre, &c) && project(&g_view, ring_point(g_ring_a0), &a) &&
            project(&g_view, ring_point(g_ring_pointer), &b)) {
            cui_draw_line(ui, c, a, 1.5f, CUI_RGBA(0xFFFFFF80));
            cui_draw_line(ui, c, b, 2.5f, col);
            cui_draw_circle(ui, b, 4.5f, col);
            char text[24];
            int n = snprintf(text, sizeof text, "%+.0f deg", (g_yaw_now - g_ring_yaw0) * 180.0f / PI_F);
            cui_vec2 tp = {b.x + 10.0f, b.y - 20.0f};
            cui_draw_text(ui, 0, 13.0f, tp, CUI_RGB(0xFFFFFF), text, n);
        }
    }
}

static void paint_gizmo(cui_ctx *ui) {
    if (g_state->tool == EDIT_TOOL_ROTATE) { paint_ring(ui); return; }
    static const unsigned colours[AXIS_COUNT] = {0xF0524F, 0x4C8DF6}; /* X red, Z blue */
    static const char *names[AXIS_COUNT] = {"X", "Z"};
    for (int a = 0; a < AXIS_COUNT; a++) {
        const GizmoHandle *g = &g_gizmo[a];
        if (!g->visible) continue;
        int lit = a == g_gizmo_drag || (g_gizmo_drag < 0 && a == g_gizmo_hot);
        if (g_gizmo_drag >= 0 && a != g_gizmo_drag) continue; /* only the axis in use while dragging */
        cui_color col = lit ? CUI_RGB(0xFFE066) : CUI_RGB(colours[a]);
        float dx = g->b.x - g->a.x, dy = g->b.y - g->a.y, len = sqrtf(dx * dx + dy * dy);
        dx /= len; dy /= len;
        const float head = 13.0f, half = 5.5f;
        cui_vec2 neck = {g->b.x - dx * head, g->b.y - dy * head};
        cui_vec2 tri[3] = {g->b, {neck.x - dy * half, neck.y + dx * half}, {neck.x + dy * half, neck.y - dx * half}};
        cui_draw_line(ui, g->a, neck, lit ? 6.0f : 5.0f, CUI_RGBA(0x00000066));
        cui_draw_line(ui, g->a, neck, lit ? 3.5f : 2.5f, col);
        cui_draw_polygon(ui, tri, 3, col);
        cui_vec2 label = {g->b.x + dx * 9.0f - 4.0f, g->b.y + dy * 9.0f - 8.0f};
        cui_draw_text(ui, 0, 13.0f, label, col, names[a], 1);
    }
}

/* ---- viewport ----------------------------------------------------------------------------------- */

static void paint_viewport(cui_ctx *ui, cui_rect rect, void *user) {
    (void)user;
    for (int i = 0; i < g_segment_count; i++) {
        const Segment *s = &g_segments[i];
        if (s->strong) cui_draw_line(ui, s->a, s->b, 3.5f, CUI_RGBA(0x00000055));
        cui_draw_line(ui, s->a, s->b, s->strong ? 1.75f : 1.0f, s->strong ? CUI_RGB(0xFFB23E) : CUI_RGBA(0xFFFFFF90));
    }
    if (g_state && g_gizmo_shown) paint_gizmo(ui);
    if (g_state) { /* what this view shows, like the label in the corner of any editor viewport */
        static const char *tools[] = {"Move", "Rotate", "Select"};
        char text[96];
        int n = snprintf(text, sizeof text, "Perspective  |  %s  |  %s%s", k_views[g_state->view], tools[g_state->tool],
                         g_state->snap ? "  |  Snap" : "");
        cui_vec2 p = {rect.x + 12.0f, rect.y + 10.0f}, shadow = {p.x + 1.0f, p.y + 1.0f};
        cui_draw_text(ui, 0, 12.0f, shadow, CUI_RGBA(0x000000B0), text, n);
        cui_draw_text(ui, 0, 12.0f, p, CUI_RGBA(0xFFFFFFD0), text, n);
    }
}

static int movable(const Office *o, int index) {
    return index >= 0 && index < o->object_count && o->objects[index].visible && !o->objects[index].locked;
}

/* Selecting, dragging and rotating objects with the mouse. Returns 1 if an object moved. */
static int viewport_interact(EditorState *st, Office *o, const View *v, int hovered) {
    int changed = 0;
    int have_mouse = g_mouse_x >= 0.0f && g_mouse_y >= 0.0f;
    V3 ray = view_ray(v, g_mouse_x, g_mouse_y);
    float hit_t = 0.0f;
    int over = hovered && have_mouse ? pick(o, v->pos, ray, &hit_t) : -1;
    if (st->selected >= o->object_count) st->selected = -1;

    /* the gizmo comes first: it lies over the object and whatever is behind it. Only an object that
       may be moved carries one, and the Select tool shows none. */
    int sel = movable(o, st->selected) ? st->selected : -1;
    int tool = sel >= 0 ? st->tool : EDIT_TOOL_SELECT;
    g_gizmo_hot = -1;
    g_ring_hot = 0;
    g_view = *v; g_view_valid = 1;
    if (tool == EDIT_TOOL_ROTATE) {
        OfficeObject *obj = &o->objects[sel];
        g_gizmo_drag = -1;
        ring_layout(v, obj);
        if (hovered && have_mouse && !g_dragging && !g_ring_drag) g_ring_hot = ring_hit(g_mouse_x, g_mouse_y);
        if (g_lmb_pressed && g_ring_hot) {
            if (ring_angle(v, ray, &g_ring_a0)) { g_ring_drag = 1; g_ring_yaw0 = obj->yaw; g_ring_pointer = g_ring_a0; }
            g_lmb_pressed = 0; /* used up: must not reselect what is under the ring */
        }
        float a;
        if (g_ring_drag && have_mouse && ring_angle(v, ray, &a)) {
            float before = obj->yaw, step = 15.0f * PI_F / 180.0f;
            float yaw = g_ring_yaw0 + wrap_angle(a - g_ring_a0);
            if (st->snap) yaw = floorf(yaw / step + 0.5f) * step;
            obj->yaw = wrap_angle(yaw);
            g_ring_pointer = a;
            office_clamp_object(o, obj);
            if (fabsf(wrap_angle(obj->yaw - before)) > 1e-5f) changed = 1;
            else obj->yaw = before;
        }
        if (!g_lmb_down) g_ring_drag = 0;
        g_yaw_now = obj->yaw;
    } else if (tool == EDIT_TOOL_MOVE) {
        g_ring_drag = 0;
        gizmo_layout(v, &o->objects[sel]);
        if (hovered && have_mouse && !g_dragging) g_gizmo_hot = gizmo_hit(g_mouse_x, g_mouse_y);
    } else {
        g_gizmo_drag = -1; g_ring_drag = 0;
    }
    if (g_lmb_pressed && hovered && have_mouse && g_gizmo_hot >= 0) {
        const OfficeObject *obj = &o->objects[sel];
        g_gizmo_origin = gizmo_origin(obj);
        if (axis_param(g_gizmo_origin, axis_dir(g_gizmo_hot), v->pos, ray, &g_gizmo_s0)) {
            g_gizmo_drag = g_gizmo_hot;
            g_gizmo_start = obj->pos;
        }
        g_lmb_pressed = 0; /* the press is used up: it must not reselect what is under the arrow */
    }
    if (sel < 0) g_gizmo_drag = -1;
    if (g_gizmo_drag >= 0 && have_mouse) {
        OfficeObject *obj = &o->objects[sel];
        float s;
        if (axis_param(g_gizmo_origin, axis_dir(g_gizmo_drag), v->pos, ray, &s) && fabsf(s - g_gizmo_s0) < 40.0f) {
            V3 before = obj->pos;
            float *coord = g_gizmo_drag == AXIS_X ? &obj->pos.x : &obj->pos.z;
            float start = g_gizmo_drag == AXIS_X ? g_gizmo_start.x : g_gizmo_start.z;
            float home = g_gizmo_drag == AXIS_X ? obj->home.x : obj->home.z;
            *coord = start + (s - g_gizmo_s0);
            if (st->snap) *coord = snap_to(*coord, home, obj->snap);
            office_clamp_object(o, obj);
            if (fabsf(obj->pos.x - before.x) > 1e-4f || fabsf(obj->pos.z - before.z) > 1e-4f) changed = 1;
            else obj->pos = before;
        }
    }
    if (!g_lmb_down) g_gizmo_drag = -1;
    if (g_gizmo_drag >= 0 || g_gizmo_hot >= 0 || g_ring_drag || g_ring_hot) over = -1; /* no hover outline through the gizmo */

    if (g_lmb_pressed && hovered && have_mouse) {
        st->selected = over; /* hidden and locked objects cannot be picked, so this one can be dragged */
        if (over >= 0 && st->tool != EDIT_TOOL_SELECT) {
            const OfficeObject *obj = &o->objects[over];
            V3 p = v3_add(v->pos, v3_scale(ray, hit_t));
            g_dragging = 1;
            g_grab_y = p.y;
            g_grab_dx = obj->pos.x - p.x;
            g_grab_dz = obj->pos.z - p.z;
        }
    }
    if (!movable(o, st->selected)) g_dragging = 0;

    /* (a quick drag can deliver its last movement together with the release: still apply it) */
    if (g_dragging && have_mouse && fabsf(ray.y) > 1e-4f) {
        OfficeObject *obj = &o->objects[st->selected];
        float t = (g_grab_y - v->pos.y) / ray.y;
        if (t > 0.0f && t < 40.0f) { /* the pointer is on the plane the object slides on */
            V3 before = obj->pos;
            obj->pos.x = v->pos.x + ray.x * t + g_grab_dx;
            obj->pos.z = v->pos.z + ray.z * t + g_grab_dz;
            if (st->snap) {
                obj->pos.x = snap_to(obj->pos.x, obj->home.x, obj->snap);
                obj->pos.z = snap_to(obj->pos.z, obj->home.z, obj->snap);
            }
            office_clamp_object(o, obj);
            if (fabsf(obj->pos.x - before.x) > 1e-4f || fabsf(obj->pos.z - before.z) > 1e-4f) changed = 1;
            else obj->pos = before;
        }
    }
    if (!g_lmb_down) g_dragging = 0;

    if (g_wheel != 0.0f && hovered && movable(o, st->selected)) {
        OfficeObject *obj = &o->objects[st->selected];
        float step = (st->snap ? 15.0f : 5.0f) * PI_F / 180.0f;
        obj->yaw = wrap_angle(obj->yaw + g_wheel * step);
        if (st->snap) obj->yaw = floorf(obj->yaw / step + 0.5f) * step;
        office_clamp_object(o, obj);
        changed = 1;
    }

    g_segment_count = 0;
    g_gizmo_shown = 0;
    if (over >= 0 && over != st->selected && !g_dragging) add_outline(v, &o->objects[over], 0);
    if (st->selected >= 0 && o->objects[st->selected].visible) {
        add_outline(v, &o->objects[st->selected], 1);
        if (movable(o, st->selected) && st->tool != EDIT_TOOL_SELECT) {
            /* lay the gizmo out again: the object may have moved or just been selected */
            g_gizmo_shown = 1;
            if (st->tool == EDIT_TOOL_ROTATE) ring_layout(v, &o->objects[st->selected]);
            else gizmo_layout(v, &o->objects[st->selected]);
        }
    }
    return changed;
}

/* ---- undo --------------------------------------------------------------------------------------- */

static void take_snapshot(Snapshot *s, const Office *o, const EditorState *st) {
    memcpy(s->objects, o->objects, sizeof s->objects);
    s->count = o->object_count;
    s->selected = st->selected;
}

static void apply_snapshot(const Snapshot *s, Office *o, EditorState *st) {
    memcpy(o->objects, s->objects, sizeof s->objects);
    o->object_count = s->count;
    st->selected = s->selected < s->count ? s->selected : -1;
}

static void push_snapshot(Snapshot *stack, int *n, const Snapshot *s) {
    if (*n == UNDO_MAX) { memmove(stack, stack + 1, (UNDO_MAX - 1) * sizeof(Snapshot)); (*n)--; }
    stack[(*n)++] = *s;
}

/* ---- commands ----------------------------------------------------------------------------------- */

enum { RES_SCENE = 1, RES_META = 2, RES_HISTORY = 4, RES_BUILD = 8, RES_CLEAN = 16 };

static const char *file_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}

static int run_command(EditorState *st, Office *o, int cmd, int arg) {
    int target = arg >= 0 ? arg : st->selected;
    OfficeObject *obj = (cmd != CMD_ADD && target >= 0 && target < o->object_count) ? &o->objects[target] : NULL;
    char text[128];
    switch (cmd) {
    case CMD_NEW:
        office_reset(o);
        st->selected = -1;
        cui_toast(g_ui, "New scene", .description = "Back to the default layout. Undo brings the old one back.");
        return RES_SCENE;
    case CMD_SAVE:
        if (office_save(o, st->scene_path)) {
            snprintf(text, sizeof text, "%d objects written to %s", o->object_count, file_name(st->scene_path));
            cui_toast(g_ui, "Scene saved", .description = text, .tone = CUI_TONE_SUCCESS);
            return RES_CLEAN;
        }
        cui_toast(g_ui, "Could not save the scene", .description = st->scene_path, .tone = CUI_TONE_DANGER);
        return 0;
    case CMD_REVERT:
        if (office_load(o, st->scene_path)) {
            st->selected = -1;
            cui_toast(g_ui, "Reverted to the saved scene", .tone = CUI_TONE_INFO);
            return RES_SCENE | RES_CLEAN;
        }
        cui_toast(g_ui, "No saved scene to revert to", .description = "Save the scene first (Ctrl S).", .tone = CUI_TONE_WARNING);
        return 0;
    case CMD_UNDO:
    case CMD_REDO: {
        Snapshot *from = cmd == CMD_UNDO ? g_undo : g_redo, *to = cmd == CMD_UNDO ? g_redo : g_undo;
        int *from_n = cmd == CMD_UNDO ? &g_undo_n : &g_redo_n, *to_n = cmd == CMD_UNDO ? &g_redo_n : &g_undo_n;
        if (!*from_n) return 0;
        static Snapshot now;
        take_snapshot(&now, o, st);
        push_snapshot(to, to_n, &now);
        apply_snapshot(&from[--*from_n], o, st);
        return RES_SCENE | RES_HISTORY;
    }
    case CMD_ADD: {
        V3 ahead = v3_norm(v3(g_fwd.x, 0, g_fwd.z));
        V3 pos = v3_add(g_cam, v3_scale(ahead, 1.6f)); /* a step in front of the camera */
        int index = office_add_object(o, (OfficeObjectType)arg, pos, 0.0f);
        if (index < 0) {
            if (arg == OT_LIGHT_PANEL && office_light_count(o, 0) >= INT_MAX_LIGHTS)
                cui_toast(g_ui, "Light limit reached", .description = "The renderer lights a scene with up to 4 light panels.", .tone = CUI_TONE_WARNING);
            else cui_toast(g_ui, "The scene is full", .tone = CUI_TONE_WARNING);
            return 0;
        }
        st->selected = index;
        return RES_SCENE;
    }
    case CMD_DUPLICATE: {
        if (!obj) return 0;
        int index = office_duplicate_object(o, target);
        if (index < 0) {
            if (obj->kind == OBJ_LIGHT) cui_toast(g_ui, "Light limit reached", .description = "The renderer lights a scene with up to 4 light panels.", .tone = CUI_TONE_WARNING);
            else cui_toast(g_ui, "The scene is full", .tone = CUI_TONE_WARNING);
            return 0;
        }
        st->selected = index;
        return RES_SCENE;
    }
    case CMD_DELETE:
        if (!obj) return 0;
        if (obj->kind == OBJ_LIGHT && obj->visible && office_light_count(o, 1) <= 1) {
            cui_toast(g_ui, "The last light stays", .description = "A scene needs at least one visible light.", .tone = CUI_TONE_WARNING);
            return 0;
        }
        office_remove_object(o, target);
        if (st->selected == target) st->selected = -1;
        else if (st->selected > target) st->selected--;
        return RES_SCENE;
    case CMD_HIDE:
        if (!obj) return 0;
        if (obj->kind == OBJ_LIGHT && obj->visible && office_light_count(o, 1) <= 1) {
            cui_toast(g_ui, "The last light stays", .description = "A scene needs at least one visible light.", .tone = CUI_TONE_WARNING);
            return 0;
        }
        obj->visible = !obj->visible;
        return RES_SCENE;
    case CMD_LOCK:
        if (!obj) return 0;
        obj->locked = !obj->locked;
        return RES_META;
    case CMD_FOCUS:
        if (obj) st->focus = target;
        return 0;
    case CMD_RESET:
        if (!obj || obj->locked) return 0;
        obj->pos = obj->home; obj->yaw = obj->home_yaw;
        obj->intensity = 1.0f; obj->warmth = 0.0f;
        return RES_SCENE;
    case CMD_DESELECT: st->selected = -1; return 0;
    case CMD_PLAY: st->play = 1; return 0;
    case CMD_BUILD: return RES_BUILD;
    }
    return 0;
}

/* ---- menu bar and toolbar ----------------------------------------------------------------------- */

static void add_items(void) {
    for (int t = 0; t < OT_COUNT; t++)
        if (cui_menu_item(g_ui, office_type_name((OfficeObjectType)t), .icon = k_type_icons[t])) request(CMD_ADD, t);
}

static void menu_bar(EditorState *st, const Office *o) {
    const OfficeObject *obj = st->selected >= 0 ? &o->objects[st->selected] : NULL;
    cui_menu_bar(g_ui, "menubar") {
        cui_menu_bar_menu(g_ui, "File") {
            if (cui_menu_item(g_ui, "New scene", .icon = CUI_ICON_FILE)) request(CMD_NEW, -1);
            if (cui_menu_item(g_ui, "Save scene", .icon = CUI_ICON_SAVE, .shortcut = "Ctrl S")) request(CMD_SAVE, -1);
            if (cui_menu_item(g_ui, "Revert to saved", .icon = CUI_ICON_HISTORY)) request(CMD_REVERT, -1);
            cui_menu_separator(g_ui);
            if (cui_menu_item(g_ui, "Play", .icon = CUI_ICON_PLAY, .shortcut = "Y")) request(CMD_PLAY, -1);
        }
        cui_menu_bar_menu(g_ui, "Edit") {
            if (cui_menu_item(g_ui, "Undo", .icon = CUI_ICON_UNDO_2, .shortcut = "Ctrl Z", .disabled = !g_undo_n)) request(CMD_UNDO, -1);
            if (cui_menu_item(g_ui, "Redo", .icon = CUI_ICON_REDO_2, .shortcut = "Ctrl Y", .disabled = !g_redo_n)) request(CMD_REDO, -1);
            cui_menu_separator(g_ui);
            if (cui_menu_item(g_ui, "Duplicate", .icon = CUI_ICON_COPY, .shortcut = "Ctrl D", .disabled = !obj)) request(CMD_DUPLICATE, -1);
            if (cui_menu_item(g_ui, "Delete", .icon = CUI_ICON_TRASH_2, .shortcut = "Del", .disabled = !obj, .danger = true)) request(CMD_DELETE, -1);
            cui_menu_separator(g_ui);
            if (cui_menu_item(g_ui, obj && !obj->visible ? "Show" : "Hide", .icon = CUI_ICON_EYE_OFF, .shortcut = "H", .disabled = !obj)) request(CMD_HIDE, -1);
            if (cui_menu_item(g_ui, obj && obj->locked ? "Unlock" : "Lock", .icon = CUI_ICON_LOCK, .disabled = !obj)) request(CMD_LOCK, -1);
            if (cui_menu_item(g_ui, "Focus", .icon = CUI_ICON_TARGET, .shortcut = "F", .disabled = !obj)) request(CMD_FOCUS, -1);
            if (cui_menu_item(g_ui, "Reset transform", .icon = CUI_ICON_ROTATE_CCW, .disabled = !obj)) request(CMD_RESET, -1);
            cui_menu_separator(g_ui);
            if (cui_menu_item(g_ui, "Deselect", .shortcut = "Esc", .disabled = !obj)) request(CMD_DESELECT, -1);
        }
        cui_menu_bar_menu(g_ui, "Add") {
            add_items();
        }
        cui_menu_bar_menu(g_ui, "View") {
            for (int i = 0; i < INT_VIEW_COUNT; i++)
                if (cui_menu_item(g_ui, k_views[i], .checked = st->view == i)) st->view = i;
            cui_menu_separator(g_ui);
            if (cui_menu_item(g_ui, "Ambient obscurance", .checked = st->ao != 0, .shortcut = "F2")) st->ao = !st->ao;
            if (cui_menu_item(g_ui, "Reflections", .checked = st->reflections != 0, .shortcut = "F3")) st->reflections = !st->reflections;
            if (cui_menu_item(g_ui, "Snap to grid", .checked = st->snap != 0)) st->snap = !st->snap;
        }
        cui_menu_bar_menu(g_ui, "Build") {
            if (cui_menu_item(g_ui, "Build lighting", .icon = CUI_ICON_ZAP)) request(CMD_BUILD, -1);
            if (cui_menu_item(g_ui, "Live rebake while editing", .checked = st->live != 0)) st->live = !st->live;
        }
    }
}

static void toolbar(EditorState *st, const Office *o) {
    int has = st->selected >= 0;
    const OfficeObject *obj = has ? &o->objects[st->selected] : NULL;
    cui_toolbar(g_ui, "toolbar") {
        if (cui_toolbar_button(g_ui, CUI_ICON_SAVE, .tooltip = "Save scene", .shortcut = "Ctrl S")) request(CMD_SAVE, -1);
        if (cui_toolbar_button(g_ui, CUI_ICON_UNDO_2, .tooltip = "Undo", .shortcut = "Ctrl Z", .disabled = !g_undo_n)) request(CMD_UNDO, -1);
        if (cui_toolbar_button(g_ui, CUI_ICON_REDO_2, .tooltip = "Redo", .shortcut = "Ctrl Y", .disabled = !g_redo_n)) request(CMD_REDO, -1);
        cui_toolbar_separator(g_ui);
        bool on = st->tool == EDIT_TOOL_SELECT;
        if (cui_toolbar_button(g_ui, CUI_ICON_TARGET, .label = "Select", .tooltip = "Select objects", .shortcut = "Q", .toggle = &on)) st->tool = EDIT_TOOL_SELECT;
        on = st->tool == EDIT_TOOL_MOVE;
        if (cui_toolbar_button(g_ui, CUI_ICON_MOVE, .label = "Move", .tooltip = "Move the selection", .shortcut = "G", .toggle = &on)) st->tool = EDIT_TOOL_MOVE;
        on = st->tool == EDIT_TOOL_ROTATE;
        if (cui_toolbar_button(g_ui, CUI_ICON_ROTATE_CCW, .label = "Rotate", .tooltip = "Rotate the selection", .shortcut = "R", .toggle = &on)) st->tool = EDIT_TOOL_ROTATE;
        cui_toolbar_separator(g_ui);
        on = st->snap != 0;
        cui_toolbar_button(g_ui, CUI_ICON_GRID_3X3, .label = "Snap", .tooltip = "Snap to the grid and to 15 degrees", .toggle = &on);
        st->snap = on;
        cui_toolbar_separator(g_ui);
        if (cui_toolbar_button(g_ui, CUI_ICON_COPY, .tooltip = "Duplicate", .shortcut = "Ctrl D", .disabled = !has)) request(CMD_DUPLICATE, -1);
        if (cui_toolbar_button(g_ui, obj && !obj->visible ? CUI_ICON_EYE_OFF : CUI_ICON_EYE, .tooltip = obj && !obj->visible ? "Show" : "Hide", .shortcut = "H", .disabled = !has)) request(CMD_HIDE, -1);
        if (cui_toolbar_button(g_ui, CUI_ICON_ZOOM_IN, .tooltip = "Focus the camera on the selection", .shortcut = "F", .disabled = !has)) request(CMD_FOCUS, -1);
        if (cui_toolbar_button(g_ui, CUI_ICON_TRASH_2, .tooltip = "Delete", .shortcut = "Del", .disabled = !has)) request(CMD_DELETE, -1);
        cui_toolbar_spacer(g_ui);
        if (cui_toolbar_button(g_ui, CUI_ICON_ZAP, .label = "Build lighting", .tooltip = "Rebuild shadows, probes and reflections from scratch")) request(CMD_BUILD, -1);
        if (cui_toolbar_button(g_ui, CUI_ICON_PLAY, .label = "Play", .tooltip = "Walk around the scene", .shortcut = "Y")) request(CMD_PLAY, -1);
    }
}

/* ---- outliner ----------------------------------------------------------------------------------- */

static int matches_filter(const char *name) {
    if (!g_filter[0]) return 1;
    size_t n = strlen(g_filter);
    for (const char *p = name; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)g_filter[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

static const char *display_name(const OfficeObject *obj) { return obj->name[0] ? obj->name : "(unnamed)"; }

static void outliner(EditorState *st, const Office *o) {
    cui_column(g_ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 10, .gap = 6) {
        cui_row(g_ui, .width = CUI_FILL) {
            cui_label(g_ui, "Outliner");
            cui_spacer(g_ui);
            cui_menu(g_ui, "Add", .icon = CUI_ICON_PLUS, .variant = CUI_BUTTON_GHOST, .placement = CUI_PLACE_BOTTOM_END) {
                add_items();
            }
        }
        cui_search(g_ui, "outliner_search", g_filter, sizeof g_filter, .placeholder = "Search objects", .size = CUI_SIZE_SM, .width = CUI_FILL);
        int shown = 0;
        cui_scroll(g_ui, .id = "outliner_scroll", .height = CUI_FILL, .gap = 0) {
            for (int kind = 0; kind < OBJ_KIND_COUNT; kind++) {
                int members = 0;
                for (int i = 0; i < o->object_count; i++)
                    if ((int)o->objects[i].kind == kind && matches_filter(o->objects[i].name)) members++;
                if (!members) continue;
                cui_push_id_int(g_ui, 1000 + kind);
                cui_tree(g_ui, k_folders[kind], &g_folder_open[kind], .icon = g_folder_open[kind] ? CUI_ICON_FOLDER_OPEN : CUI_ICON_FOLDER) {
                    for (int i = 0; i < o->object_count; i++) {
                        const OfficeObject *obj = &o->objects[i];
                        if ((int)obj->kind != kind || !matches_filter(obj->name)) continue;
                        shown++;
                        cui_push_id_int(g_ui, i);
                        cui_row(g_ui, .width = CUI_FILL, .gap = 2) {
                            if (cui_icon_button(g_ui, obj->visible ? CUI_ICON_EYE : CUI_ICON_EYE_OFF, .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                                                .tooltip = obj->visible ? "Hide" : "Show"))
                                request(CMD_HIDE, i);
                            cui_column(g_ui, .width = CUI_FILL) {
                                const char *trailing = obj->locked ? "locked" : (obj->visible ? office_type_name(obj->type) : "hidden");
                                if (cui_list_item(g_ui, display_name(obj), .icon = k_type_icons[obj->type], .trailing = trailing, .selected = st->selected == i))
                                    st->selected = i;
                                cui_context_menu(g_ui, "object_menu") {
                                    if (cui_menu_item(g_ui, "Focus", .icon = CUI_ICON_TARGET, .shortcut = "F")) { st->selected = i; request(CMD_FOCUS, i); }
                                    if (cui_menu_item(g_ui, "Duplicate", .icon = CUI_ICON_COPY, .shortcut = "Ctrl D")) request(CMD_DUPLICATE, i);
                                    if (cui_menu_item(g_ui, obj->visible ? "Hide" : "Show", .icon = obj->visible ? CUI_ICON_EYE_OFF : CUI_ICON_EYE, .shortcut = "H")) request(CMD_HIDE, i);
                                    if (cui_menu_item(g_ui, obj->locked ? "Unlock" : "Lock", .icon = obj->locked ? CUI_ICON_UNLOCK : CUI_ICON_LOCK)) request(CMD_LOCK, i);
                                    if (cui_menu_item(g_ui, "Reset transform", .icon = CUI_ICON_ROTATE_CCW)) request(CMD_RESET, i);
                                    cui_menu_separator(g_ui);
                                    if (cui_menu_item(g_ui, "Delete", .icon = CUI_ICON_TRASH_2, .shortcut = "Del", .danger = true)) request(CMD_DELETE, i);
                                }
                            }
                        }
                        cui_pop_id(g_ui);
                    }
                }
                cui_pop_id(g_ui);
            }
            if (!shown) cui_muted(g_ui, g_filter[0] ? "No object matches the search." : "The scene is empty. Use Add to place something.");
        }
        char text[96];
        if (g_filter[0]) snprintf(text, sizeof text, "%d of %d objects shown", shown, o->object_count);
        else snprintf(text, sizeof text, "%d objects%s", o->object_count, st->selected >= 0 ? "  (1 selected)" : "");
        cui_muted(g_ui, text);
    }
}

/* ---- details ------------------------------------------------------------------------------------ */

/* The selected object. Returns RES_* for what was edited in place. */
static int details_object(EditorState *st, Office *o) {
    int res = 0;
    if (st->selected < 0) {
        cui_empty_state(g_ui, "Nothing selected", .icon = CUI_ICON_BOX, .description = "Click an object in the viewport or in the outliner to see its details.");
        return 0;
    }
    OfficeObject *obj = &o->objects[st->selected];
    const IntScene *s = &o->scene;
    static bool open_transform = true, open_light = true, open_object = true;
    cui_push_id_int(g_ui, st->selected);
    cui_row(g_ui, .width = CUI_FILL, .gap = 8) {
        cui_icon(g_ui, k_type_icons[obj->type], 16.0f, CUI_COLOR_TEXT_SECONDARY);
        if (cui_input(g_ui, "name", obj->name, sizeof obj->name, .size = CUI_SIZE_SM, .width = CUI_FILL, .placeholder = "Name") & CUI_CHANGED) res |= RES_META;
        cui_badge(g_ui, office_type_name(obj->type));
    }
    if (cui_inspector_begin(g_ui, "object_props")) {
        if (cui_inspector_group(g_ui, "Transform", &open_transform)) {
            float x = obj->pos.x, z = obj->pos.z, deg = obj->yaw * 180.0f / PI_F;
            if (cui_prop_float(g_ui, "Location X", &x, .min = s->room_min.x, .max = s->room_max.x, .step = 0.01f, .format = "%.2f", .suffix = "m", .disabled = obj->locked != 0)) { obj->pos.x = x; res |= RES_SCENE; }
            if (cui_prop_float(g_ui, "Location Z", &z, .min = s->room_min.z, .max = s->room_max.z, .step = 0.01f, .format = "%.2f", .suffix = "m", .disabled = obj->locked != 0)) { obj->pos.z = z; res |= RES_SCENE; }
            if (cui_prop_float(g_ui, "Rotation", &deg, .min = -180.0f, .max = 180.0f, .step = st->snap ? 15.0f : 1.0f, .format = "%.0f", .suffix = "deg", .disabled = obj->locked != 0)) {
                obj->yaw = deg * PI_F / 180.0f; res |= RES_SCENE;
            }
        }
        if (obj->kind == OBJ_LIGHT && cui_inspector_group(g_ui, "Light", &open_light)) {
            if (cui_prop_float(g_ui, "Intensity", &obj->intensity, .min = 0.0f, .max = 3.0f, .step = 0.01f, .format = "%.2f", .suffix = "x")) res |= RES_SCENE;
            if (cui_prop_float(g_ui, "Warmth", &obj->warmth, .min = -1.0f, .max = 1.0f, .step = 0.01f, .format = "%+.2f", .tooltip = "-1 cool, +1 warm")) res |= RES_SCENE;
        }
        if (cui_inspector_group(g_ui, "Object", &open_object)) {
            bool visible = obj->visible != 0, locked = obj->locked != 0;
            if (cui_prop_bool(g_ui, "Visible", &visible)) request(CMD_HIDE, st->selected);
            if (cui_prop_bool(g_ui, "Locked", &locked, .tooltip = "A locked object cannot be picked or moved in the viewport")) { obj->locked = locked; res |= RES_META; }
        }
        cui_inspector_end(g_ui);
    }
    if (res & RES_SCENE) office_clamp_object(o, obj);
    cui_row(g_ui, .width = CUI_FILL, .gap = 6, .wrap = true) {
        if (cui_button(g_ui, "Focus", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_SECONDARY, .icon = CUI_ICON_TARGET)) request(CMD_FOCUS, -1);
        if (cui_button(g_ui, "Duplicate", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_SECONDARY, .icon = CUI_ICON_COPY)) request(CMD_DUPLICATE, -1);
        if (cui_button(g_ui, "Reset", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_SECONDARY, .icon = CUI_ICON_ROTATE_CCW, .disabled = obj->locked != 0)) request(CMD_RESET, -1);
        if (cui_button(g_ui, "Delete", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_DANGER, .icon = CUI_ICON_TRASH_2)) request(CMD_DELETE, -1);
    }
    cui_pop_id(g_ui);
    return res;
}

/* Lighting build and image settings: what belongs to the scene as a whole. Returns EDIT_* flags. */
static int world_settings(EditorState *st, IntLook *look) {
    int flags = 0;
    static bool open_build = true, open_image = true;
    if (cui_inspector_begin(g_ui, "world_props")) {
        if (cui_inspector_group(g_ui, "Lighting build", &open_build)) {
            bool live = st->live != 0;
            if (cui_prop_bool(g_ui, "Live rebake", &live, .tooltip = "Shadows follow at once; bounce light and reflections settle over the next frames")) st->live = live;
            if (cui_prop_float(g_ui, "Shadow softness", &look->shadow_softness, .min = 0.05f, .max = 0.60f, .step = 0.01f, .format = "%.2f", .suffix = "m")) flags |= EDIT_BAKE_LOOK;
            if (cui_prop_float(g_ui, "Bounce colour", &look->bounce_saturation, .min = 0.0f, .max = 1.0f, .step = 0.01f, .format = "%.2f")) flags |= EDIT_BAKE_LOOK;
        }
        if (cui_inspector_group(g_ui, "Image", &open_image)) {
            bool b;
            cui_prop_float(g_ui, "Exposure", &look->exposure, .min = 0.3f, .max = 3.0f, .step = 0.01f, .format = "%.2f");
            b = st->ao != 0;
            if (cui_prop_bool(g_ui, "Ambient obscurance", &b)) st->ao = b;
            if (st->ao) cui_prop_float(g_ui, "Obscurance strength", &st->ao_strength, .min = 0.05f, .max = 1.0f, .step = 0.01f, .format = "%.2f");
            b = st->reflections != 0;
            if (cui_prop_bool(g_ui, "Reflections", &b)) st->reflections = b;
            b = st->dof != 0;
            if (cui_prop_bool(g_ui, "Depth of field", &b, .tooltip = "Blurs what is close to the camera. Shown in play mode only")) st->dof = b;
            b = st->vsync != 0;
            if (cui_prop_bool(g_ui, "VSync", &b)) { st->vsync = b; flags |= EDIT_VSYNC; }
            cui_prop_enum(g_ui, "View mode", k_views, INT_VIEW_COUNT, &st->view);
        }
        cui_inspector_end(g_ui);
    }
    if (cui_button(g_ui, "Build lighting", .full_width = true, .icon = CUI_ICON_ZAP, .variant = st->stale ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY,
                   .tooltip = "Rebuild shadows, probes and reflections from scratch"))
        request(CMD_BUILD, -1);
    return flags;
}

/* ---- status bar --------------------------------------------------------------------------------- */

static void status_bar(const EditorState *st, const Office *o, const IntRenderer *r) {
    char text[128];
    cui_status_bar(g_ui, "status") {
        float progress = interior_bake_progress(r);
        if (st->stale) {
            cui_status_bar_text(g_ui, "Lighting needs to be rebuilt", .icon = CUI_ICON_TRIANGLE_ALERT, .tone = CUI_TONE_WARNING);
        } else if (progress < 1.0f) {
            snprintf(text, sizeof text, "Building lighting  %.1f ms / frame", r->step_ms);
            cui_status_bar_progress(g_ui, progress, .label = text, .width = 120);
        } else {
            cui_status_bar_text(g_ui, "Lighting up to date", .icon = CUI_ICON_CIRCLE_CHECK, .tone = CUI_TONE_SUCCESS);
        }
        if (st->selected >= 0) {
            const OfficeObject *obj = &o->objects[st->selected];
            snprintf(text, sizeof text, "%s   x %.2f  z %.2f  %.0f deg", display_name(obj), obj->pos.x, obj->pos.z, obj->yaw * 180.0f / PI_F);
            cui_status_bar_text(g_ui, text, .icon = k_type_icons[obj->type]);
        } else {
            cui_status_bar_text(g_ui, "Nothing selected");
        }
        cui_status_bar_spacer(g_ui);
        if (g_unsaved) cui_status_bar_text(g_ui, "Unsaved changes", .icon = CUI_ICON_CIRCLE_DOT, .tone = CUI_TONE_WARNING);
        else cui_status_bar_text(g_ui, "Saved", .icon = CUI_ICON_CHECK);
        snprintf(text, sizeof text, "%d objects", o->object_count);
        cui_status_bar_text(g_ui, text);
        snprintf(text, sizeof text, "%.0f fps", st->fps);
        cui_status_bar_text(g_ui, text);
    }
}

/* ---- frame -------------------------------------------------------------------------------------- */

int editor_frame(EditorState *st, Office *o, IntLook *look, const IntRenderer *r, V3 cam, V3 fwd, int width, int height, float dt) {
    int flags = 0, res = 0;
    if (!g_ui || !st->active) {
        g_lmb_pressed = 0; g_wheel = 0.0f; g_dragging = 0; g_gizmo_drag = -1; g_ring_drag = 0; g_view_valid = 0;
        return 0;
    }
    if (st->selected >= o->object_count) st->selected = -1;
    if (!g_prev_valid) { take_snapshot(&g_prev, o, st); g_prev_valid = 1; }
    g_cam = cam; g_fwd = fwd;
    g_cmd = CMD_NONE;

    int rect[4];
    editor_view_rect(st, width, height, rect); /* where the scene was just drawn */
    View view = make_view(cam, fwd, look->fov, rect[0], rect[1], rect[2], rect[3]);

    cui_set_viewport(g_ui, (float)width, (float)height, 1.0f);
    cui_begin_frame(g_ui, dt);
    cui_column(g_ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        cui_column(g_ui, .width = CUI_FILL, .bg = COL_BAR, .gap = 0) {
            menu_bar(st, o);
            toolbar(st, o);
        }
        cui_row(g_ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_STRETCH, .gap = 0) {
            /* the 3D view: a canvas, so CUI decides whether the pointer is on it or on a panel */
            cui_canvas_result vp = cui_canvas(g_ui, "viewport", CUI_FILL, CUI_FILL, paint_viewport, NULL, 0);
            if (vp.rect.w >= 1.0f && vp.rect.h >= 1.0f) { g_vp = vp.rect; g_vp_valid = 1; }
            if (viewport_interact(st, o, &view, vp.hovered || g_dragging || g_gizmo_drag >= 0 || g_ring_drag)) res |= RES_SCENE;
            cui_column(g_ui, .width = CUI_PX(PANEL_W), .height = CUI_FILL, .bg = COL_PANEL, .border = 1, .gap = 0) {
                cui_split(g_ui, "side_split", &g_side_split, .vertical = true, .min_first = 150, .min_second = 200, .width = CUI_FILL, .height = CUI_FILL) {
                    outliner(st, o);
                    cui_split_next(g_ui);
                    cui_column(g_ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 10, .gap = 8) {
                        static const char *tabs[] = {"Details", "World settings"};
                        cui_tabs(g_ui, "side_tabs", tabs, 2, &g_tab, .full_width = true);
                        cui_scroll(g_ui, .id = "details_scroll", .height = CUI_FILL, .gap = 8) {
                            if (g_tab == 0) res |= details_object(st, o);
                            else flags |= world_settings(st, look);
                        }
                    }
                }
            }
        }
        cui_column(g_ui, .width = CUI_FILL, .bg = COL_BAR, .gap = 0) {
            status_bar(st, o, r);
        }
    }

    /* shortcuts, unless a text field is being typed in */
    if (!cui_wants_keyboard(g_ui)) {
        if (cui_shortcut(g_ui, CUI_MOD_CTRL, CUI_KEY_Z)) request(CMD_UNDO, -1);
        if (cui_shortcut(g_ui, CUI_MOD_CTRL, CUI_KEY_Y) || cui_shortcut(g_ui, CUI_MOD_CTRL | CUI_MOD_SHIFT, CUI_KEY_Z)) request(CMD_REDO, -1);
        if (cui_shortcut(g_ui, CUI_MOD_CTRL, CUI_KEY_S)) request(CMD_SAVE, -1);
        if (cui_shortcut(g_ui, CUI_MOD_CTRL, CUI_KEY_D)) request(CMD_DUPLICATE, -1);
        if (cui_shortcut(g_ui, 0, CUI_KEY_DELETE)) request(CMD_DELETE, -1);
        if (cui_shortcut(g_ui, 0, CUI_KEY_F)) request(CMD_FOCUS, -1);
        if (cui_shortcut(g_ui, 0, CUI_KEY_H)) request(CMD_HIDE, -1);
    }
    if (g_ext_cmd) { request(g_ext_cmd, g_ext_arg); g_ext_cmd = CMD_NONE; }
    if (g_cmd != CMD_NONE) res |= run_command(st, o, g_cmd, g_cmd_arg);
    cui_end_frame(g_ui);
    g_lmb_pressed = 0; g_wheel = 0.0f;

    /* one undo step per gesture: remember the scene as it was before the first change of a drag, a
       run of slider edits or a command */
    if (res & (RES_SCENE | RES_META)) {
        if (!(res & RES_HISTORY) && !g_gesture) {
            push_snapshot(g_undo, &g_undo_n, &g_prev);
            g_redo_n = 0;
        }
        g_gesture = g_lmb_down; /* a click is over at once; a drag lasts until the button comes up */
        g_unsaved = 1;
    } else if (!g_lmb_down) {
        g_gesture = 0;
    }
    if (res & RES_CLEAN) g_unsaved = 0;
    take_snapshot(&g_prev, o, st);
    if (res & RES_SCENE) flags |= EDIT_SCENE;
    if (res & RES_BUILD) flags |= EDIT_FULL_BAKE;

    glBindFramebuffer(GL_FRAMEBUFFER, r->out_fbo);
    glViewport(0, 0, width, height);
    cui_gl_render(g_gl, cui_get_draw_data(g_ui), false);

    /* put back what the scene passes expect */
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    return flags;
}
