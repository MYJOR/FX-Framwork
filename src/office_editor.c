#include "office_editor.h"
#include "gl.h"
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define PI_F 3.14159265f
#define PANEL_W 336
#define PICK_PAD 0.04f     /* ceiling fixtures are flat: give them some thickness to click on */
#define CAM_NEAR 0.05f
#define MAX_SEGMENTS 64

static cui_ctx *g_ui;
static cui_gl_renderer *g_gl;
static EditorState *g_state;

/* raw input since the last frame */
static float g_mouse_x = -1, g_mouse_y = -1, g_wheel;
static int g_lmb_down, g_lmb_pressed;

/* drag in progress */
static int g_dragging;
static float g_grab_y;          /* height of the plane the object slides on */
static float g_grab_dx, g_grab_dz; /* object position minus the grabbed point */

/* Move gizmo on the selected object: one arrow per axis it can travel along (world X and Z; height
   is fixed by what the object is). Dragging an arrow moves the object along that axis only. */
enum { AXIS_X, AXIS_Z, AXIS_COUNT };
typedef struct { cui_vec2 a, b; int visible; } GizmoHandle; /* shaft start and arrow tip, window pixels */
static GizmoHandle g_gizmo[AXIS_COUNT];
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

static const char *k_views[INT_VIEW_COUNT] = {"Final", "Albedo", "Normals", "Lighting", "Direct light", "Indirect light",
                                              "Ambient obscurance", "Shadows", "Roughness / metallic", "Specular"};

static void *loader(const char *name) { return platform_gl_proc(name); }

int editor_init(EditorState *st, const IntLook *look) {
    memset(st, 0, sizeof *st);
    st->selected = -1;
    st->live = 1;
    st->snap = 0;
    st->view = INT_VIEW_FINAL;
    st->ao = look->ao_strength > 0.0f;
    st->ao_strength = st->ao ? look->ao_strength : interior_default_look().ao_strength;
    st->reflections = 1;
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
int editor_view_width(const EditorState *st, int window_width) {
    int w = st->active ? window_width - PANEL_W : window_width;
    return w < 64 ? 64 : w;
}
int editor_wants_keyboard(const EditorState *st) { return st->active && g_ui && cui_wants_keyboard(g_ui); }

/* ---- camera maths: the same projection interior_render uses -------------------------------------- */

typedef struct { V3 pos, fwd, right, up; float tan_y, tan_x; int w, h; } View;

static View make_view(V3 cam, V3 fwd, float fov, int w, int h) {
    View v;
    v.pos = cam; v.fwd = v3_norm(fwd);
    v.right = v3_norm(v3_cross(v.fwd, v3(0, 1, 0)));
    v.up = v3_cross(v.right, v.fwd);
    v.tan_y = tanf(fov * 0.5f);
    v.tan_x = v.tan_y * (float)w / (float)h;
    v.w = w; v.h = h;
    return v;
}

static V3 view_ray(const View *v, float mx, float my) {
    float x = (2.0f * mx / (float)v->w - 1.0f) * v->tan_x, y = (1.0f - 2.0f * my / (float)v->h) * v->tan_y;
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
    s->a.x = (v3_dot(ra, v->right) / (za * v->tan_x) + 1.0f) * 0.5f * (float)v->w;
    s->a.y = (1.0f - v3_dot(ra, v->up) / (za * v->tan_y)) * 0.5f * (float)v->h;
    s->b.x = (v3_dot(rb, v->right) / (zb * v->tan_x) + 1.0f) * 0.5f * (float)v->w;
    s->b.y = (1.0f - v3_dot(rb, v->up) / (zb * v->tan_y)) * 0.5f * (float)v->h;
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
    out->x = (v3_dot(r, v->right) / (z * v->tan_x) + 1.0f) * 0.5f * (float)v->w;
    out->y = (1.0f - v3_dot(r, v->up) / (z * v->tan_y)) * 0.5f * (float)v->h;
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
    (void)rect; (void)user;
    for (int i = 0; i < g_segment_count; i++) {
        const Segment *s = &g_segments[i];
        if (s->strong) cui_draw_line(ui, s->a, s->b, 3.5f, CUI_RGBA(0x00000055));
        cui_draw_line(ui, s->a, s->b, s->strong ? 1.75f : 1.0f, s->strong ? CUI_RGB(0xFFB23E) : CUI_RGBA(0xFFFFFF90));
    }
    if (g_state && g_state->selected >= 0) paint_gizmo(ui);
}

/* Selecting, dragging and rotating objects with the mouse. Returns 1 if an object moved. */
static int viewport_interact(EditorState *st, Office *o, const View *v, int hovered) {
    int changed = 0;
    int have_mouse = g_mouse_x >= 0.0f && g_mouse_y >= 0.0f;
    V3 ray = view_ray(v, g_mouse_x, g_mouse_y);
    float hit_t = 0.0f;
    int over = hovered && have_mouse ? pick(o, v->pos, ray, &hit_t) : -1;

    /* the gizmo comes first: it lies over the object and whatever is behind it */
    g_gizmo_hot = -1;
    g_ring_hot = 0;
    g_view = *v; g_view_valid = 1;
    if (st->selected >= 0 && st->tool == EDIT_TOOL_ROTATE) {
        OfficeObject *obj = &o->objects[st->selected];
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
    } else if (st->selected >= 0) {
        g_ring_drag = 0;
        gizmo_layout(v, &o->objects[st->selected]);
        if (hovered && have_mouse && !g_dragging) g_gizmo_hot = gizmo_hit(g_mouse_x, g_mouse_y);
    } else {
        g_gizmo_drag = -1; g_ring_drag = 0;
    }
    if (g_lmb_pressed && hovered && have_mouse && g_gizmo_hot >= 0) {
        const OfficeObject *obj = &o->objects[st->selected];
        g_gizmo_origin = gizmo_origin(obj);
        if (axis_param(g_gizmo_origin, axis_dir(g_gizmo_hot), v->pos, ray, &g_gizmo_s0)) {
            g_gizmo_drag = g_gizmo_hot;
            g_gizmo_start = obj->pos;
        }
        g_lmb_pressed = 0; /* the press is used up: it must not reselect what is under the arrow */
    }
    if (g_gizmo_drag >= 0 && have_mouse) {
        OfficeObject *obj = &o->objects[st->selected];
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
        st->selected = over;
        if (over >= 0) {
            const OfficeObject *obj = &o->objects[over];
            V3 p = v3_add(v->pos, v3_scale(ray, hit_t));
            g_dragging = 1;
            g_grab_y = p.y;
            g_grab_dx = obj->pos.x - p.x;
            g_grab_dz = obj->pos.z - p.z;
        }
    }
    if (st->selected < 0) g_dragging = 0;

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

    if (g_wheel != 0.0f && hovered && st->selected >= 0) {
        OfficeObject *obj = &o->objects[st->selected];
        float step = (st->snap ? 15.0f : 5.0f) * PI_F / 180.0f;
        obj->yaw = wrap_angle(obj->yaw + g_wheel * step);
        if (st->snap) obj->yaw = floorf(obj->yaw / step + 0.5f) * step;
        office_clamp_object(o, obj);
        changed = 1;
    }

    g_segment_count = 0;
    if (over >= 0 && over != st->selected && !g_dragging) add_outline(v, &o->objects[over], 0);
    if (st->selected >= 0) {
        add_outline(v, &o->objects[st->selected], 1);
        /* lay the gizmo out again: the object may have moved or just been selected */
        if (st->tool == EDIT_TOOL_ROTATE) ring_layout(v, &o->objects[st->selected]);
        else gizmo_layout(v, &o->objects[st->selected]);
    }
    return changed;
}

/* ---- panel -------------------------------------------------------------------------------------- */

static const char *kind_name(OfficeObjectKind k) { return k == OBJ_LIGHT ? "light" : (k == OBJ_FIXTURE ? "ceiling" : "furniture"); }

static int panel_object(EditorState *st, Office *o) {
    int changed = 0;
    OfficeObject *obj = &o->objects[st->selected];
    const IntScene *s = &o->scene;
    cui_card(g_ui, .pad = 12, .gap = 6) {
        cui_row(g_ui, .width = CUI_FILL) {
            cui_label(g_ui, obj->name);
            cui_spacer(g_ui);
            if (cui_button(g_ui, "Reset", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) {
                obj->pos = obj->home; obj->yaw = obj->home_yaw;
                obj->intensity = 1.0f; obj->warmth = 0.0f;
                changed = 1;
            }
        }
        float x = obj->pos.x, z = obj->pos.z, deg = obj->yaw * 180.0f / PI_F;
        if (cui_slider(g_ui, "Position X", &x, s->room_min.x, s->room_max.x, .show_value = true, .format = "%.2f m")) { obj->pos.x = x; changed = 1; }
        if (cui_slider(g_ui, "Position Z", &z, s->room_min.z, s->room_max.z, .show_value = true, .format = "%.2f m")) { obj->pos.z = z; changed = 1; }
        if (cui_slider(g_ui, "Rotation", &deg, -180.0f, 180.0f, .show_value = true, .format = "%.0f deg", .step = st->snap ? 15.0f : 1.0f)) {
            obj->yaw = deg * PI_F / 180.0f; changed = 1;
        }
        if (obj->kind == OBJ_LIGHT) {
            if (cui_slider(g_ui, "Intensity", &obj->intensity, 0.0f, 3.0f, .show_value = true, .format = "%.2f x")) changed = 1;
            if (cui_slider(g_ui, "Warmth", &obj->warmth, -1.0f, 1.0f, .show_value = true, .format = "%+.2f")) changed = 1;
        }
        if (changed) office_clamp_object(o, obj);
    }
    return changed;
}

static int panel(EditorState *st, Office *o, IntLook *look, const IntRenderer *r) {
    int flags = 0;
    bool b;
    cui_row(g_ui, .width = CUI_FILL) {
        cui_heading(g_ui, "Scene editor", 3);
        cui_spacer(g_ui);
        cui_kbd(g_ui, "Y");
        cui_muted(g_ui, " play");
    }
    cui_muted(g_ui, "Click an object, then drag it, pull an arrow (Move) or turn the ring (Rotate). Hold the right mouse button to look, WASD to walk.");
    {
        static const char *tools[] = {"Move  (G)", "Rotate  (R)"};
        cui_segmented(g_ui, "tool", tools, 2, &st->tool, .full_width = true);
    }

    cui_scroll(g_ui, .id = "editor_scroll", .height = CUI_FILL, .gap = 10) {
        cui_label(g_ui, "Objects");
        cui_column(g_ui, .gap = 0) {
            for (int i = 0; i < o->object_count; i++) {
                cui_push_id_int(g_ui, i);
                if (cui_list_item(g_ui, o->objects[i].name, .selected = st->selected == i, .trailing = kind_name(o->objects[i].kind)))
                    st->selected = st->selected == i ? -1 : i;
                cui_pop_id(g_ui);
            }
        }
        if (st->selected >= 0 && panel_object(st, o)) flags |= EDIT_SCENE;
        cui_row(g_ui, .width = CUI_FILL, .gap = 8) {
            b = st->snap != 0;
            if (cui_switch(g_ui, "Snap to grid", &b)) st->snap = b;
            cui_spacer(g_ui);
            if (cui_button(g_ui, "Reset layout", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_OUTLINE)) {
                for (int i = 0; i < o->object_count; i++) {
                    OfficeObject *obj = &o->objects[i];
                    obj->pos = obj->home; obj->yaw = obj->home_yaw; obj->intensity = 1.0f; obj->warmth = 0.0f;
                }
                flags |= EDIT_SCENE;
            }
        }

        cui_separator(g_ui);
        cui_label(g_ui, "Lighting bake");
        b = st->live != 0;
        if (cui_switch(g_ui, "Live rebake", &b, .description = "Shadows follow at once; bounce light and reflections settle over the next frames.")) st->live = b;
        if (cui_button(g_ui, "Full rebake", .full_width = true, .variant = st->stale ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY,
                       .tooltip = "Rebuild shadows, probes and reflections from scratch"))
            flags |= EDIT_FULL_BAKE;
        if (cui_slider(g_ui, "Shadow softness", &look->shadow_softness, 0.05f, 0.60f, .show_value = true, .format = "%.2f m")) flags |= EDIT_BAKE_LOOK;
        if (cui_slider(g_ui, "Bounce colour", &look->bounce_saturation, 0.0f, 1.0f, .show_value = true, .format = "%.2f")) flags |= EDIT_BAKE_LOOK;

        cui_separator(g_ui);
        cui_label(g_ui, "Image");
        cui_slider(g_ui, "Exposure", &look->exposure, 0.3f, 3.0f, .show_value = true, .format = "%.2f");
        b = st->ao != 0;
        if (cui_switch(g_ui, "Ambient obscurance", &b)) st->ao = b;
        if (st->ao) cui_slider(g_ui, "Obscurance strength", &st->ao_strength, 0.05f, 1.0f, .show_value = true, .format = "%.2f");
        b = st->reflections != 0;
        if (cui_switch(g_ui, "Reflections", &b)) st->reflections = b;
        b = st->vsync != 0;
        if (cui_switch(g_ui, "VSync", &b)) { st->vsync = b; flags |= EDIT_VSYNC; }
        cui_select(g_ui, "view", k_views, INT_VIEW_COUNT, &st->view, .label = "Debug view");
    }
    /* always in sight: what the bake is doing */
    float progress = interior_bake_progress(r);
    char lab[80];
    if (st->stale) {
        cui_progress(g_ui, 0.0f, .height = 6, .tone = CUI_TONE_WARNING, .label = "Lighting is out of date");
    } else if (progress < 1.0f) {
        snprintf(lab, sizeof lab, "Rebaking bounce light   %.1f ms / frame", r->step_ms);
        cui_progress(g_ui, progress, .height = 6, .tone = CUI_TONE_ACCENT, .label = lab);
    } else {
        snprintf(lab, sizeof lab, "Lighting is up to date   %.0f fps", st->fps);
        cui_progress(g_ui, 1.0f, .height = 6, .tone = CUI_TONE_SUCCESS, .label = lab);
    }
    return flags;
}

/* ---- frame -------------------------------------------------------------------------------------- */

int editor_frame(EditorState *st, Office *o, IntLook *look, const IntRenderer *r, V3 cam, V3 fwd, int width, int height, float dt) {
    int flags = 0;
    if (!g_ui || !st->active) {
        g_lmb_pressed = 0; g_wheel = 0.0f; g_dragging = 0; g_gizmo_drag = -1; g_ring_drag = 0; g_view_valid = 0;
        return 0;
    }
    View view = make_view(cam, fwd, look->fov, editor_view_width(st, width), height); /* the scene is drawn beside the panel */

    cui_set_viewport(g_ui, (float)width, (float)height, 1.0f);
    cui_begin_frame(g_ui, dt);
    cui_row(g_ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_STRETCH) {
        /* the 3D view: a canvas, so CUI decides whether the pointer is on it or on the panel */
        cui_canvas_result vp = cui_canvas(g_ui, "viewport", CUI_FILL, CUI_FILL, paint_viewport, NULL, 0);
        if (viewport_interact(st, o, &view, vp.hovered || g_dragging || g_gizmo_drag >= 0 || g_ring_drag)) flags |= EDIT_SCENE;
        cui_column(g_ui, .width = CUI_PX(PANEL_W), .height = CUI_FILL, .pad = 14, .gap = 10,
                   .bg = CUI_RGB(0x0E1014), .border = 1) {
            flags |= panel(st, o, look, r);
        }
    }
    cui_end_frame(g_ui);
    g_lmb_pressed = 0; g_wheel = 0.0f;

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
