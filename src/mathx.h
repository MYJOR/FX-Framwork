#ifndef MATHX_H
#define MATHX_H
#include <math.h>

typedef struct { float x, y, z; } V3;
typedef struct { float m[16]; } M4; /* column-major, OpenGL convention */

static inline V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static inline V3 v3_add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 v3_scale(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 v3_cross(V3 a, V3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline V3 v3_norm(V3 a) {
    float l = sqrtf(v3_dot(a, a));
    return l > 1e-8f ? v3_scale(a, 1.0f / l) : a;
}

/* Yaw 0 looks down -Z. Pitch positive looks up. */
static inline V3 cam_forward(float yaw, float pitch) {
    return v3(sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch));
}

static inline M4 m4_perspective(float fovy, float aspect, float n, float f) {
    M4 r = {{0}};
    float t = 1.0f / tanf(fovy * 0.5f);
    r.m[0] = t / aspect;
    r.m[5] = t;
    r.m[10] = (f + n) / (n - f);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * f * n / (n - f);
    return r;
}

static inline M4 m4_view(V3 pos, V3 fwd) {
    V3 r = v3_norm(v3_cross(fwd, v3(0, 1, 0)));
    V3 u = v3_cross(r, fwd);
    M4 m = {{0}};
    m.m[0] = r.x;  m.m[4] = r.y;  m.m[8]  = r.z;  m.m[12] = -v3_dot(r, pos);
    m.m[1] = u.x;  m.m[5] = u.y;  m.m[9]  = u.z;  m.m[13] = -v3_dot(u, pos);
    m.m[2] = -fwd.x; m.m[6] = -fwd.y; m.m[10] = -fwd.z; m.m[14] = v3_dot(fwd, pos);
    m.m[15] = 1.0f;
    return m;
}

static inline M4 m4_mul(M4 a, M4 b) {
    M4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

static inline M4 m4_identity(void) {
    M4 r = {{0}};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}
static inline M4 m4_translate(V3 t) {
    M4 r = m4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}
static inline M4 m4_rotate_y(float a) {
    M4 r = m4_identity();
    float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[8] = s; r.m[2] = -s; r.m[10] = c;
    return r;
}
static inline M4 m4_rotate_x(float a) {
    M4 r = m4_identity();
    float c = cosf(a), s = sinf(a);
    r.m[5] = c; r.m[9] = -s; r.m[6] = s; r.m[10] = c;
    return r;
}

#endif
