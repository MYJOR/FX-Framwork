#include "tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f

/* ---- how it is held (eye space: x right, y up, -z ahead; metres, radians) ------------------------ */
#define HOLD_X 0.046f        /* a little right of the middle ... */
#define HOLD_Y (-0.104f)     /* ... low, so the grip runs off the bottom of the picture ... */
#define HOLD_Z (-0.300f)     /* ... at a bent arm's length */
#define HOLD_PITCH (-24.0f * PI / 180.0f) /* leant back: the screen faces up at the eye and a little past it */
#define HOLD_YAW (-10.0f * PI / 180.0f)   /* turned in towards the middle */
#define HOLD_ROLL (-3.0f * PI / 180.0f)
/* lowered: dropped below the frame, tipped forwards, swung out to the side */
#define LOWER_X 0.07f
#define LOWER_Y (-0.36f)
#define LOWER_Z 0.05f
#define LOWER_PITCH (48.0f * PI / 180.0f)
#define LOWER_ROLL (-14.0f * PI / 180.0f)
#define RAISE_RATE 11.0f     /* 1/s: stiffness of the spring that brings it up; higher = snappier */
#define FOCUS_RATE 7.0f      /* 1/s: how quickly the lens goes from the tracker to the room and back */
#define SWAY_RATE 7.0f       /* 1/s: how quickly the hand catches up with the view */
#define SWAY_YAW 0.014f      /* metres the hand trails per radian per second of turning */
#define SWAY_PITCH 0.011f
#define SWAY_MAX 0.035f
#define VIEW_FOV (62.0f * PI / 180.0f) /* its own lens: zooming the room does not magnify the hand */
#define VIEW_PULL 0.40f      /* drawn at this fraction of its real distance: see tracker.vert */

/* ---- the ping ---------------------------------------------------------------------------------- */
#define PING_SPAN 1.25f      /* phase runs 0..1 while the ring crosses the screen, then rests to here */
#define PING_SLOW 1.7f       /* seconds per ping with nothing near ... */
#define PING_FAST 0.75f      /* ... and with a contact right on top of the player */

/* ---- the device (its own space: x right, y up, z out of the screen towards the player; metres) --- */
#define SCREEN_CX 0.0f
#define SCREEN_CY 0.026f
#define SCREEN_HW 0.041f
#define SCREEN_HH 0.038f
#define BODY_HZ 0.019f
#define SCREEN_Z (BODY_HZ + 0.0005f)

enum { MAT_PAINT, MAT_DARK, MAT_RUBBER, MAT_METAL, MAT_SCREEN, MAT_LED, MAT_BUTTON, MAT_PLATE, MAT_KEY };

#define VERT_FLOATS 9 /* position, normal, uv, material */
typedef struct { float *v; int n, cap; } Mesh;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void vert(Mesh *m, V3 p, V3 n, float u, float v, int mat) {
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 1024;
        m->v = (float *)realloc(m->v, (size_t)m->cap * VERT_FLOATS * sizeof(float));
    }
    float *o = m->v + (size_t)m->n++ * VERT_FLOATS;
    o[0] = p.x; o[1] = p.y; o[2] = p.z; o[3] = n.x; o[4] = n.y; o[5] = n.z; o[6] = u; o[7] = v; o[8] = (float)mat;
}

/* A triangle with a normal per corner, wound so that it faces the way its normals point. */
static void tri(Mesh *m, V3 a, V3 b, V3 c, V3 na, V3 nb, V3 nc, const float uv[6], int mat) {
    V3 g = v3_cross(v3_add(b, v3_scale(a, -1.0f)), v3_add(c, v3_scale(a, -1.0f)));
    if (v3_dot(g, v3_add(na, v3_add(nb, nc))) >= 0.0f) {
        vert(m, a, na, uv[0], uv[1], mat); vert(m, b, nb, uv[2], uv[3], mat); vert(m, c, nc, uv[4], uv[5], mat);
    } else {
        vert(m, a, na, uv[0], uv[1], mat); vert(m, c, nc, uv[4], uv[5], mat); vert(m, b, nb, uv[2], uv[3], mat);
    }
}

/* Flat quad a-b-c-d. edge: 1 marks a chamfer, where the shader wears the finish through. */
static void quad(Mesh *m, V3 a, V3 b, V3 c, V3 d, V3 n, float edge, int mat) {
    const float uv[6] = {edge, 0, edge, 0, edge, 0};
    tri(m, a, b, c, n, n, n, uv, mat);
    tri(m, a, c, d, n, n, n, uv, mat);
}

/* Box of half size h around c with its edges and corners chamfered by `bevel`. */
static void box(Mesh *m, V3 c, V3 h, float bevel, int mat) {
    const float hs[3] = {h.x, h.y, h.z}, cs[3] = {c.x, c.y, c.z};
    V3 p[8][3]; /* per corner: where it meets each of its three faces */
    for (int k = 0; k < 8; k++)
        for (int f = 0; f < 3; f++) {
            float q[3];
            for (int a = 0; a < 3; a++) q[a] = cs[a] + ((k >> a) & 1 ? 1.0f : -1.0f) * (a == f ? hs[a] : hs[a] - bevel);
            p[k][f] = v3(q[0], q[1], q[2]);
        }
    for (int a = 0; a < 3; a++) {
        int u = (a + 1) % 3, w = (a + 2) % 3;
        for (int side = 0; side < 2; side++) { /* the two faces across axis a */
            float n[3] = {0, 0, 0};
            n[a] = side ? 1.0f : -1.0f;
            int base = side << a;
            quad(m, p[base][a], p[base | (1 << u)][a], p[base | (1 << u) | (1 << w)][a], p[base | (1 << w)][a], v3(n[0], n[1], n[2]), 0.0f, mat);
        }
        if (bevel <= 0.0f) continue;
        for (int e = 0; e < 4; e++) { /* the four edges that run along axis a */
            int k0 = ((e & 1) << u) | ((e >> 1) << w), k1 = k0 | (1 << a);
            float n[3] = {0, 0, 0};
            n[u] = (e & 1) ? 1.0f : -1.0f; n[w] = (e >> 1) ? 1.0f : -1.0f;
            quad(m, p[k0][u], p[k1][u], p[k1][w], p[k0][w], v3_norm(v3(n[0], n[1], n[2])), 1.0f, mat);
        }
    }
    if (bevel <= 0.0f) return;
    for (int k = 0; k < 8; k++) {
        const float uv[6] = {1, 0, 1, 0, 1, 0};
        V3 n = v3_norm(v3((k & 1) ? 1.0f : -1.0f, (k & 2) ? 1.0f : -1.0f, (k & 4) ? 1.0f : -1.0f));
        tri(m, p[k][0], p[k][1], p[k][2], n, n, n, uv, mat);
    }
}

/* Cylinder along `axis` (0 x, 1 y, 2 z), half length hl, with flat ends and a chamfered rim. */
static void cylinder(Mesh *m, V3 c, int axis, float radius, float hl, float bevel, int mat) {
    enum { SEGS = 20 };
    int u = (axis + 1) % 3, w = (axis + 2) % 3;
    const float cs[3] = {c.x, c.y, c.z};
    const float flat[6] = {0, 0, 0, 0, 0, 0}, worn[6] = {1, 0, 1, 0, 1, 0};
    for (int i = 0; i < SEGS; i++) {
        float a0 = (float)i / SEGS * 2.0f * PI, a1 = (float)(i + 1) / SEGS * 2.0f * PI;
        V3 ring[2][4], nrm[2]; /* per angle: bottom, top of the side; top of the chamfer; rim of the cap */
        for (int k = 0; k < 2; k++) {
            float a = k ? a1 : a0, q[3], n[3] = {0, 0, 0};
            n[u] = cosf(a); n[w] = sinf(a);
            nrm[k] = v3(n[0], n[1], n[2]);
            const float along[4] = {-hl, hl - bevel, hl, -hl}, rad[4] = {radius, radius, radius - bevel, radius};
            for (int j = 0; j < 4; j++) {
                q[axis] = cs[axis] + along[j]; q[u] = cs[u] + n[u] * rad[j]; q[w] = cs[w] + n[w] * rad[j];
                ring[k][j] = v3(q[0], q[1], q[2]);
            }
        }
        float ax[3] = {0, 0, 0};
        ax[axis] = 1.0f;
        V3 up = v3(ax[0], ax[1], ax[2]), centre = v3_add(c, v3_scale(up, hl));
        tri(m, ring[0][0], ring[1][0], ring[1][1], nrm[0], nrm[1], nrm[1], flat, mat);
        tri(m, ring[0][0], ring[1][1], ring[0][1], nrm[0], nrm[1], nrm[0], flat, mat);
        if (bevel > 0.0f) {
            V3 n0 = v3_norm(v3_add(nrm[0], up)), n1 = v3_norm(v3_add(nrm[1], up));
            tri(m, ring[0][1], ring[1][1], ring[1][2], n0, n1, n1, worn, mat);
            tri(m, ring[0][1], ring[1][2], ring[0][2], n0, n1, n0, worn, mat);
        }
        tri(m, centre, ring[0][2], ring[1][2], up, up, up, flat, mat);
    }
}

/* The screen: the one part with real texture coordinates, 0..1 across it. */
static void screen(Mesh *m) {
    V3 n = v3(0, 0, 1);
    V3 a = v3(SCREEN_CX - SCREEN_HW, SCREEN_CY - SCREEN_HH, SCREEN_Z), b = v3(SCREEN_CX + SCREEN_HW, SCREEN_CY - SCREEN_HH, SCREEN_Z);
    V3 c = v3(SCREEN_CX + SCREEN_HW, SCREEN_CY + SCREEN_HH, SCREEN_Z), d = v3(SCREEN_CX - SCREEN_HW, SCREEN_CY + SCREEN_HH, SCREEN_Z);
    const float uv0[6] = {0, 0, 1, 0, 1, 1}, uv1[6] = {0, 0, 1, 1, 0, 1};
    tri(m, a, b, c, n, n, n, uv0, MAT_SCREEN);
    tri(m, a, c, d, n, n, n, uv1, MAT_SCREEN);
}

static void build_device(Mesh *m) {
    const float bar = 0.0045f;                 /* half width of the bezel's bars */
    const float bz = BODY_HZ + 0.0055f;        /* the bezel stands 11 mm proud of the face */
    /* case, and the rubber bumpers down its sides */
    box(m, v3(0, 0, 0), v3(0.056f, 0.082f, BODY_HZ), 0.004f, MAT_PAINT);
    box(m, v3(-0.0585f, 0, 0), v3(0.0045f, 0.074f, 0.022f), 0.003f, MAT_RUBBER);
    box(m, v3(0.0585f, 0, 0), v3(0.0045f, 0.074f, 0.022f), 0.003f, MAT_RUBBER);
    /* screen, sunk inside a bezel, under a hood */
    screen(m);
    box(m, v3(SCREEN_CX, SCREEN_CY + SCREEN_HH + bar, bz), v3(SCREEN_HW + 2.0f * bar, bar, 0.0055f), 0.0018f, MAT_DARK);
    box(m, v3(SCREEN_CX, SCREEN_CY - SCREEN_HH - bar, bz), v3(SCREEN_HW + 2.0f * bar, bar, 0.0055f), 0.0018f, MAT_DARK);
    box(m, v3(SCREEN_CX - SCREEN_HW - bar, SCREEN_CY, bz), v3(bar, SCREEN_HH, 0.0055f), 0.0018f, MAT_DARK);
    box(m, v3(SCREEN_CX + SCREEN_HW + bar, SCREEN_CY, bz), v3(bar, SCREEN_HH, 0.0055f), 0.0018f, MAT_DARK);
    box(m, v3(SCREEN_CX, SCREEN_CY + SCREEN_HH + 2.0f * bar + 0.0025f, BODY_HZ + 0.009f), v3(SCREEN_HW + 2.0f * bar + 0.002f, 0.0025f, 0.009f), 0.0012f, MAT_DARK);
    /* controls under the screen: three keys, a range knob, the ping lamp, a data plate */
    box(m, v3(-0.036f, -0.040f, BODY_HZ + 0.0015f), v3(0.0068f, 0.0046f, 0.0025f), 0.0010f, MAT_BUTTON);
    box(m, v3(-0.018f, -0.040f, BODY_HZ + 0.0015f), v3(0.0068f, 0.0046f, 0.0025f), 0.0010f, MAT_KEY);
    box(m, v3(0.000f, -0.040f, BODY_HZ + 0.0015f), v3(0.0068f, 0.0046f, 0.0025f), 0.0010f, MAT_KEY);
    cylinder(m, v3(0.033f, -0.047f, BODY_HZ + 0.0045f), 2, 0.0120f, 0.0045f, 0.0015f, MAT_METAL);
    cylinder(m, v3(0.033f, -0.047f, BODY_HZ + 0.0100f), 2, 0.0045f, 0.0012f, 0.0006f, MAT_DARK);
    box(m, v3(0.033f, -0.069f, BODY_HZ + 0.0008f), v3(0.0032f, 0.0032f, 0.0012f), 0.0006f, MAT_LED);
    box(m, v3(-0.018f, -0.063f, BODY_HZ + 0.0003f), v3(0.0250f, 0.0075f, 0.0005f), 0.0f, MAT_PLATE);
    /* sensor head on top with its lens, and the aerial beside it */
    box(m, v3(0.014f, 0.0935f, -0.002f), v3(0.036f, 0.0125f, 0.015f), 0.003f, MAT_DARK);
    cylinder(m, v3(0.030f, 0.0940f, 0.0140f), 2, 0.0072f, 0.0020f, 0.0010f, MAT_METAL);
    cylinder(m, v3(0.008f, 0.0940f, 0.0138f), 2, 0.0040f, 0.0015f, 0.0006f, MAT_METAL);
    cylinder(m, v3(-0.040f, 0.1030f, -0.004f), 1, 0.0058f, 0.0220f, 0.0010f, MAT_METAL);
    cylinder(m, v3(-0.040f, 0.1290f, -0.004f), 1, 0.0080f, 0.0045f, 0.0020f, MAT_RUBBER);
    /* collar and grip: the hand is around this, below the bottom of the picture */
    box(m, v3(0.0f, -0.0870f, -0.003f), v3(0.028f, 0.0060f, 0.0180f), 0.002f, MAT_DARK);
    box(m, v3(0.0f, -0.1600f, -0.005f), v3(0.021f, 0.0700f, 0.0160f), 0.005f, MAT_RUBBER);
}

int tracker_init(Tracker *t) {
    memset(t, 0, sizeof *t);
    t->nearest = -1.0f;
    if (!shader_program_load(&t->prog, "tracker.vert", "tracker.frag")) return 0;
    Mesh m = {0};
    build_device(&m);
    t->vertex_count = m.n;
    glGenVertexArrays(1, &t->vao);
    glBindVertexArray(t->vao);
    glGenBuffers(1, &t->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, t->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)m.n * VERT_FLOATS * sizeof(float)), m.v, GL_STATIC_DRAW);
    const GLsizei stride = VERT_FLOATS * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void *)(3 * sizeof(float)));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void *)(6 * sizeof(float)));
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void *)(8 * sizeof(float)));
    for (GLuint i = 0; i < 4; i++) glEnableVertexAttribArray(i);
    glBindVertexArray(0);
    free(m.v);
    return 1;
}

void tracker_reload_shaders(Tracker *t) { shader_reload_if_changed(&t->prog); }

int tracker_visible(const Tracker *t) { return t->raise > 0.004f; }

void tracker_update(Tracker *t, const TrackerInput *in, float dt) {
    int settle = dt <= 0.0f;
    float want = in->raised ? 1.0f : 0.0f;
    if (settle) { t->raise = want; t->raise_vel = 0.0f; }
    else { /* a critically damped spring: quick off the mark, no overshoot */
        t->raise_vel += (RAISE_RATE * RAISE_RATE * (want - t->raise) - 2.0f * RAISE_RATE * t->raise_vel) * dt;
        t->raise = clampf(t->raise + t->raise_vel * dt, 0.0f, 1.0f);
        if (!in->raised && t->raise < 0.004f) { t->raise = 0.0f; t->raise_vel = 0.0f; }
    }
    float k = settle ? 1.0f : 1.0f - expf(-FOCUS_RATE * dt);
    t->room_focus += ((in->focus_room ? 1.0f : 0.0f) - t->room_focus) * k;

    /* the hand trails the view, and swings with the stride */
    k = settle ? 1.0f : 1.0f - expf(-SWAY_RATE * dt);
    t->sway_x += (clampf(-in->yaw_rate * SWAY_YAW, -SWAY_MAX, SWAY_MAX) - t->sway_x) * k;
    t->sway_y += (clampf(-in->pitch_rate * SWAY_PITCH, -SWAY_MAX, SWAY_MAX) - t->sway_y) * k;
    t->bob_x = cosf(in->bob_phase) * 0.0050f * in->bob_amount;
    t->bob_y = -fabsf(sinf(in->bob_phase)) * 0.0060f * in->bob_amount;

    /* contacts, and how near the nearest moving one is */
    t->origin = in->origin; t->yaw = in->yaw;
    t->contact_count = in->contact_count < TRACKER_MAX_CONTACTS ? in->contact_count : TRACKER_MAX_CONTACTS;
    t->nearest = -1.0f;
    k = settle ? 1.0f : 1.0f - expf(-3.0f * dt);
    for (int i = 0; i < TRACKER_MAX_CONTACTS; i++) {
        if (i >= t->contact_count) { t->level[i] = 0.0f; continue; }
        t->contacts[i] = in->contacts[i];
        t->level[i] += (clampf(in->contacts[i].motion, 0.0f, 1.0f) - t->level[i]) * k;
        float dx = in->contacts[i].x - in->origin.x, dz = in->contacts[i].z - in->origin.z, d = sqrtf(dx * dx + dz * dz);
        if (t->level[i] > 0.15f && d <= TRACKER_RANGE && (t->nearest < 0.0f || d < t->nearest)) t->nearest = d;
    }

    /* the ping comes quicker the closer something is */
    t->time += dt;
    float period = t->nearest < 0.0f ? PING_SLOW : PING_FAST + (PING_SLOW - PING_FAST) * t->nearest / TRACKER_RANGE;
    if (settle) t->ping = 0.62f; /* a still picture: the ring part of the way out */
    else {
        t->ping += dt / period;
        if (t->ping >= PING_SPAN) t->ping -= PING_SPAN;
    }
}

void tracker_apply_look(const Tracker *t, IntLook *look) {
    /* the lens only goes to the tracker once it is most of the way up */
    float up = clampf((t->raise - 0.35f) / 0.6f, 0.0f, 1.0f);
    look->dof_hand = up * up * (3.0f - 2.0f * up) * (1.0f - t->room_focus);
    look->dof_hand_dist = -HOLD_Z;
}

static M4 rotate_z(float a) {
    M4 r = m4_identity();
    float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
    return r;
}

static GLint ul(GLuint p, const char *name) { return glGetUniformLocation(p, name); }

void tracker_draw(void *user, const IntScene *s, const IntHeldView *v) {
    Tracker *t = (Tracker *)user;
    if (!tracker_visible(t)) return;
    GLuint p = t->prog.id;

    /* the hand, in eye space: eased between lowered and held, plus sway and bob */
    float e = t->raise * t->raise * (3.0f - 2.0f * t->raise), down = 1.0f - e;
    V3 pos = v3(HOLD_X + LOWER_X * down + t->sway_x + t->bob_x,
                HOLD_Y + LOWER_Y * down + t->sway_y + t->bob_y,
                HOLD_Z + LOWER_Z * down);
    M4 hand = m4_mul(m4_translate(pos),
              m4_mul(m4_rotate_y(HOLD_YAW - t->sway_x * 2.5f),
              m4_mul(m4_rotate_x(HOLD_PITCH + LOWER_PITCH * down + t->sway_y * 2.0f),
                     rotate_z(HOLD_ROLL + LOWER_ROLL * down - t->sway_x * 1.5f + t->bob_x * 3.0f))));
    /* eye -> world: the view matrix is a rotation and a translation */
    M4 eye_to_world = m4_identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) eye_to_world.m[i * 4 + j] = v->view.m[j * 4 + i];
    eye_to_world.m[12] = v->cam.x; eye_to_world.m[13] = v->cam.y; eye_to_world.m[14] = v->cam.z;
    M4 model = m4_mul(eye_to_world, hand);
    M4 proj = m4_perspective(VIEW_FOV, v->aspect, v->near_z, v->far_z);

    glUseProgram(p);
    glUniformMatrix4fv(ul(p, "uModel"), 1, GL_FALSE, model.m);
    glUniformMatrix4fv(ul(p, "uView"), 1, GL_FALSE, v->view.m);
    glUniformMatrix4fv(ul(p, "uProj"), 1, GL_FALSE, proj.m);
    glUniform1f(ul(p, "uPull"), VIEW_PULL);
    glUniform3f(ul(p, "uCam"), v->cam.x, v->cam.y, v->cam.z);
    glUniform3f(ul(p, "uBack"), v->view.m[2], v->view.m[6], v->view.m[10]);

    /* the room's light */
    glUniform4fv(ul(p, "uSH"), 3, &v->sh[0][0]);
    glUniform1f(ul(p, "uSceneLight"), v->scene_light);
    int n = s->light_count;
    float lp[INT_MAX_LIGHTS * 3], lu[INT_MAX_LIGHTS * 3], lv[INT_MAX_LIGHTS * 3], ln[INT_MAX_LIGHTS * 3], lr[INT_MAX_LIGHTS * 3];
    for (int i = 0; i < n; i++) {
        const IntLight *l = &s->lights[i];
        memcpy(lp + i * 3, &l->pos, 12); memcpy(lu + i * 3, &l->half_u, 12); memcpy(lv + i * 3, &l->half_v, 12);
        memcpy(ln + i * 3, &l->normal, 12); memcpy(lr + i * 3, &l->radiance, 12);
    }
    glUniform1i(ul(p, "uLightCount"), n);
    if (n) {
        glUniform3fv(ul(p, "uLightPos"), n, lp);
        glUniform3fv(ul(p, "uLightU"), n, lu);
        glUniform3fv(ul(p, "uLightV"), n, lv);
        glUniform3fv(ul(p, "uLightN"), n, ln);
        glUniform3fv(ul(p, "uLightRadiance"), n, lr);
    }
    interior_set_portals(p, s);
    glActiveTexture(GL_TEXTURE0);     glBindTexture(GL_TEXTURE_CUBE_MAP, v->env_tex);
    glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, v->exposure_tex);
    glUniform1i(ul(p, "uEnvTex"), 0);
    glUniform1i(ul(p, "uExposureTex"), 1);
    glUniform1f(ul(p, "uEnvMaxLod"), v->env_max_lod);

    /* the screen */
    glUniform4f(ul(p, "uScreenRect"), SCREEN_CX, SCREEN_CY, SCREEN_HW, SCREEN_HH);
    glUniform1f(ul(p, "uScreenZ"), SCREEN_Z);
    glUniform4f(ul(p, "uPing"), t->ping, PING_SPAN, t->time, 1.0f);
    float contacts[TRACKER_MAX_CONTACTS * 4] = {0};
    V3 ahead = cam_forward(t->yaw, 0.0f);
    for (int i = 0; i < t->contact_count; i++) {
        float dx = t->contacts[i].x - t->origin.x, dz = t->contacts[i].z - t->origin.z;
        contacts[i * 4] = (dx * -ahead.z + dz * ahead.x) / TRACKER_RANGE;    /* to the player's right */
        contacts[i * 4 + 1] = (dx * ahead.x + dz * ahead.z) / TRACKER_RANGE; /* ahead */
        contacts[i * 4 + 2] = t->level[i];
    }
    glUniform4fv(ul(p, "uContact"), TRACKER_MAX_CONTACTS, contacts);
    glUniform1i(ul(p, "uContactCount"), t->contact_count);
    glUniform2f(ul(p, "uReadout"), t->nearest, TRACKER_RANGE);

    glEnable(GL_CULL_FACE);
    glBindVertexArray(t->vao);
    glDrawArrays(GL_TRIANGLES, 0, t->vertex_count);
    glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);     glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}
