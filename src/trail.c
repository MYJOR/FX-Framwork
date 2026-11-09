#include "trail.h"
#include "wind.h"
#include <stdlib.h>
#include <string.h>

#define GL_RG 0x8227
#define GL_RG8 0x822B

#define TEXEL (TRAIL_SIZE / TRAIL_N)

/* Tuning */
#define FOOT_STRIDE 0.70f      /* metres between footprints (alternating feet) */
#define DRAG_SPACING 0.06f     /* scuff stamps along each foot lane */
#define SLUMP_RADIUS 512       /* texels around the player that settle/slump (16 m) */
#define FOOT_HALF_W 0.11f
#define FOOT_HALF_L 0.25f
#define FOOT_SIDE_OFFSET 0.17f
#define FOOT_DEPTH 0.80f       /* 0..1 of the displayable depth */
#define GROOVE_SPACING 0.09f
#define FILL_INTERVAL_CALM 0.30f   /* seconds per fill-in step with no wind */
#define FILL_INTERVAL_GUST 0.06f   /* ... and in a full gust */

static int pmod(int a, int n) { int m = a % n; return m < 0 ? m + n : m; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void mark_row(Trail *t, int row) {
    if (t->dirty_row_max < t->dirty_row_min) t->dirty_row_min = t->dirty_row_max = row;
    if (row < t->dirty_row_min) t->dirty_row_min = row;
    if (row > t->dirty_row_max) t->dirty_row_max = row;
}

void trail_init(Trail *t, V3 start) {
    memset(t, 0, sizeof *t);
    t->data = (unsigned char *)calloc((size_t)TRAIL_N * TRAIL_N * 2, 1);
    t->dirty_row_min = 1; t->dirty_row_max = 0;
    t->last_pos = start;

    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, TRAIL_N, TRAIL_N, 0, GL_RG, GL_UNSIGNED_BYTE, t->data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

static float rnd(Trail *t) {
    t->rng ^= t->rng << 13; t->rng ^= t->rng >> 17; t->rng ^= t->rng << 5;
    return (float)(t->rng & 0xffffff) / 16777215.0f;
}
static float rnd_s(Trail *t) { return rnd(t) * 2.0f - 1.0f; }

/* Smooth 0..1 variation along the path, different per lane. */
static float path_noise(float d, float lane) {
    return 0.5f + 0.30f * sinf(d * 4.3f + lane * 1.7f) + 0.20f * sinf(d * 9.1f + lane * 3.9f);
}

static void clear_column(Trail *t, int wx) {
    int c = pmod(wx, TRAIL_N);
    for (int y = 0; y < TRAIL_N; y++) {
        size_t i = ((size_t)y * TRAIL_N + c) * 2;
        t->data[i] = t->data[i + 1] = 0;
    }
    t->dirty_full = 1;
}

static void clear_row(Trail *t, int wz) {
    int r = pmod(wz, TRAIL_N);
    memset(t->data + (size_t)r * TRAIL_N * 2, 0, (size_t)TRAIL_N * 2);
    mark_row(t, r);
}

/* Soft mound of displaced sand (raises G only). */
static void bump(Trail *t, float x, float z, float yaw, float half_w, float half_l, float amt) {
    float rx = cosf(yaw), rz = sinf(yaw), fx = sinf(yaw), fz = -cosf(yaw);
    float reach = (half_w > half_l ? half_w : half_l) + TEXEL;
    int x0 = (int)floorf((x - reach) / TEXEL), x1 = (int)floorf((x + reach) / TEXEL);
    int z0 = (int)floorf((z - reach) / TEXEL), z1 = (int)floorf((z + reach) / TEXEL);
    for (int wz = z0; wz <= z1; wz++) {
        int row = pmod(wz, TRAIL_N);
        for (int wx = x0; wx <= x1; wx++) {
            float dx = (wx + 0.5f) * TEXEL - x, dz = (wz + 0.5f) * TEXEL - z;
            float u = (dx * rx + dz * rz) / half_w, v = (dx * fx + dz * fz) / half_l;
            float q = u * u + v * v;
            if (q >= 1.0f) continue;
            float s = 1.0f - q;
            unsigned char b = (unsigned char)(clampf(amt * s * s, 0, 1) * 255.0f);
            size_t i = ((size_t)row * TRAIL_N + pmod(wx, TRAIL_N)) * 2 + 1;
            if (b > t->data[i]) t->data[i] = b;
        }
        mark_row(t, row);
    }
}

/* Sand settles: crisp edges relax into soft slopes. Runs near the player only. */
static void slump(Trail *t) {
    enum { R = SLUMP_RADIUS, W = 2 * R + 1 };
    static unsigned char tmp[(size_t)W * W * 2];
    for (int dz = -R; dz <= R; dz++)
        for (int dx = -R; dx <= R; dx++) {
            int xi = pmod(t->cx + dx, TRAIL_N), zi = pmod(t->cz + dz, TRAIL_N);
            int xm = pmod(xi - 1, TRAIL_N), xp = pmod(xi + 1, TRAIL_N);
            int zm = pmod(zi - 1, TRAIL_N), zp = pmod(zi + 1, TRAIL_N);
            size_t o = ((size_t)(dz + R) * W + (dx + R)) * 2;
            for (int c = 0; c < 2; c++) {
                int self = t->data[((size_t)zi * TRAIL_N + xi) * 2 + c];
                int sum = t->data[((size_t)zi * TRAIL_N + xm) * 2 + c] + t->data[((size_t)zi * TRAIL_N + xp) * 2 + c]
                        + t->data[((size_t)zm * TRAIL_N + xi) * 2 + c] + t->data[((size_t)zp * TRAIL_N + xi) * 2 + c];
                tmp[o + c] = (unsigned char)((self * 12 + sum + 8) / 16);
            }
        }
    for (int dz = -R; dz <= R; dz++) {
        int zi = pmod(t->cz + dz, TRAIL_N);
        for (int dx = -R; dx <= R; dx++) {
            int xi = pmod(t->cx + dx, TRAIL_N);
            size_t o = ((size_t)(dz + R) * W + (dx + R)) * 2;
            size_t d = ((size_t)zi * TRAIL_N + xi) * 2;
            t->data[d] = tmp[o]; t->data[d + 1] = tmp[o + 1];
        }
    }
    t->dirty_full = 1;
}

static void recenter(Trail *t, float x, float z) {
    int tx = (int)floorf(x / TEXEL), tz = (int)floorf(z / TEXEL);
    if (!t->started) { t->cx = tx; t->cz = tz; t->started = 1; return; }
    if (abs(tx - t->cx) >= TRAIL_N || abs(tz - t->cz) >= TRAIL_N) { /* teleport */
        memset(t->data, 0, (size_t)TRAIL_N * TRAIL_N * 2);
        t->cx = tx; t->cz = tz; t->dirty_full = 1;
        return;
    }
    /* window covers world texels [c - N/2, c + N/2) */
    while (t->cx < tx) { t->cx++; clear_column(t, t->cx + TRAIL_N / 2 - 1); }
    while (t->cx > tx) { t->cx--; clear_column(t, t->cx - TRAIL_N / 2); }
    while (t->cz < tz) { t->cz++; clear_row(t, t->cz + TRAIL_N / 2 - 1); }
    while (t->cz > tz) { t->cz--; clear_row(t, t->cz - TRAIL_N / 2); }
}

/* Writes one oriented elliptical dent with a raised rim. */
static void stamp(Trail *t, float x, float z, float yaw, float half_w, float half_l,
                  float depth, float rim) {
    float rx = cosf(yaw), rz = sinf(yaw);   /* right */
    float fx = sinf(yaw), fz = -cosf(yaw);  /* forward */
    float reach = (half_w > half_l ? half_w : half_l) * 1.75f + TEXEL;
    int x0 = (int)floorf((x - reach) / TEXEL), x1 = (int)floorf((x + reach) / TEXEL);
    int z0 = (int)floorf((z - reach) / TEXEL), z1 = (int)floorf((z + reach) / TEXEL);

    for (int wz = z0; wz <= z1; wz++) {
        int row = pmod(wz, TRAIL_N);
        for (int wx = x0; wx <= x1; wx++) {
            float dx = (wx + 0.5f) * TEXEL - x, dz = (wz + 0.5f) * TEXEL - z;
            float u = (dx * rx + dz * rz) / half_w;
            float v = (dx * fx + dz * fz) / half_l;
            float q = u * u + v * v;
            size_t i = ((size_t)row * TRAIL_N + pmod(wx, TRAIL_N)) * 2;

            if (q < 1.0f) {
                float s = 1.0f - q;
                float d = depth * s * (1.35f - 0.35f * s); /* smooth bowl: soft walls, rounded floor */
                unsigned char b = (unsigned char)(clampf(d, 0, 1) * 255.0f);
                if (b > t->data[i]) t->data[i] = b;
                /* a dent pushes its own rim down */
                if (t->data[i + 1] > 0 && q < 0.8f) t->data[i + 1] = (unsigned char)(t->data[i + 1] * 0.6f);
            } else if (q < 3.0f) {
                float s = 1.0f - fabsf(q - 1.5f) / 1.5f;
                float r = rim * s * s;
                unsigned char b = (unsigned char)(clampf(r, 0, 1) * 255.0f);
                if (b > t->data[i + 1]) t->data[i + 1] = b;
            }
        }
        mark_row(t, row);
    }
}

static void push_event(Trail *t, const Player *p, float x, float z, int kind) {
    if (t->ev_count >= TRAIL_MAX_EVENTS) return;
    float sp = p->speed > 1e-3f ? p->speed : 1.0f;
    TrailEvent *e = &t->ev[t->ev_count++];
    e->x = x; e->z = z; e->dx = p->vx / sp; e->dz = p->vz / sp; e->speed = p->speed; e->kind = kind;
}

/* One footprint:
 heel and toe pressed separately, a little random variation,
   and a small mound of sand pushed up behind the toe-off. */
static void footprint(Trail *t, const Player *p, float px, float pz, int side) {
    float run = clampf(p->speed / 7.5f, 0.0f, 1.0f);
    float yaw = p->yaw + rnd_s(t) * 0.12f + (float)side * 0.10f; /* slight toe-out */
    float fx = sinf(yaw), fz = -cosf(yaw);
    float rx = cosf(yaw), rz = sinf(yaw);
    float lat = (float)side * FOOT_SIDE_OFFSET + rnd_s(t) * 0.02f;
    float cx = px + rx * lat + fx * rnd_s(t) * 0.03f;
    float cz = pz + rz * lat + fz * rnd_s(t) * 0.03f;
    float size = 1.0f + rnd_s(t) * 0.08f;
    float press = (0.75f + 0.25f * run) * (0.9f + 0.2f * rnd(t));

    /* heel: rounder, deeper */
    stamp(t, cx - fx * 0.10f, cz - fz * 0.10f, yaw, FOOT_HALF_W * 0.80f * size, FOOT_HALF_L * 0.50f * size,
          FOOT_DEPTH * press, 0.35f);
    /* ball/toe: wider, lighter */
    stamp(t, cx + fx * 0.11f, cz + fz * 0.11f, yaw, FOOT_HALF_W * 0.95f * size, FOOT_HALF_L * 0.55f * size,
          FOOT_DEPTH * 0.65f * press, 0.35f);
    push_event(t, p, cx, cz, 0);
    /* sand shoved backwards by the push-off */
    bump(t, cx - fx * (0.30f + 0.10f * run), cz - fz * (0.30f + 0.10f * run), yaw,
         0.09f * size, 0.10f + 0.10f * run, 0.45f + 0.35f * run);
}

/* Feet drag through loose sand between steps: broken scuff lines along each foot lane. */
static void scuff(Trail *t, const Player *p, float px, float pz, float path) {
    float run = clampf(p->speed / 7.5f, 0.0f, 1.0f);
    float rx = cosf(p->yaw), rz = sinf(p->yaw);
    for (int lane = -1; lane <= 1; lane += 2) {
        float n = path_noise(path, (float)lane);
        float gate = clampf((n - 0.42f) * 3.5f, 0.0f, 1.0f); /* breaks up the line */
        if (gate <= 0.01f) continue;
        float meander = 0.02f * sinf(path * 1.3f + (float)lane);
        float lat = (float)lane * (FOOT_SIDE_OFFSET + meander);
        stamp(t, px + rx * lat, pz + rz * lat, p->yaw, 0.055f + 0.03f * run, 0.07f,
              (0.14f + 0.18f * run) * gate, 0.22f * gate);
    }
}

/* Sliding: a ragged channel whose depth and banks vary along its length. */
static void groove(Trail *t, const Player *p, float px, float pz, float path) {
    float yaw = atan2f(p->vx, -p->vz);
    float n = path_noise(path, 0.0f);
    float w = 0.24f + 0.06f * n + 0.02f * rnd_s(t);
    stamp(t, px, pz, yaw, w, 0.14f, 0.42f + 0.18f * n, 0.40f + 0.30f * n);
    if (rnd(t) < 0.7f) push_event(t, p, px, pz, 1);
}

typedef void (*StampFn)(Trail *, const Player *, float, float, float);

/* Calls fn every `spacing` metres along the segment travelled this frame. */
static void walk_segment(Trail *t, const Player *p, float sx, float sz, float ex, float ez,
                         float path0, float *accum, float spacing, StampFn fn) {
    float seg = sqrtf((ex - sx) * (ex - sx) + (ez - sz) * (ez - sz));
    if (seg < 1e-5f) return;
    float travelled = seg, pos = 0;
    while (*accum + travelled >= spacing) {
        float step = spacing - *accum;
        pos += step; travelled -= step; *accum = 0;
        float k = pos / seg;
        fn(t, p, sx + (ex - sx) * k, sz + (ez - sz) * k, path0 + pos);
    }
    *accum += travelled;
}

static void foot_fn(Trail *t, const Player *p, float x, float z, float path) {
    (void)path;
    t->foot_side = (t->foot_side >= 0) ? -1 : 1; /* alternate left / right */
    footprint(t, p, x, z, t->foot_side);
}

void trail_update(Trail *t, const Player *p, float dt, float time) {
    if (!t->rng) t->rng = 2463534242u;
    t->ev_count = 0;
    recenter(t, p->pos.x, p->pos.z);

    /* wind fills the tracks back in, much faster inside a gust; sand also slumps */
    float g = wind_gust(p->pos.x, p->pos.z, time);
    t->decay_acc += dt;
    float interval = FILL_INTERVAL_CALM + (FILL_INTERVAL_GUST - FILL_INTERVAL_CALM) * g;
    if (t->decay_acc >= interval) {
        t->decay_acc = 0;
        size_t n = (size_t)TRAIL_N * TRAIL_N * 2;
        for (size_t i = 0; i < n; i++) if (t->data[i]) t->data[i]--;
        slump(t);
        t->dirty_full = 1;
    }

    float sx = t->last_pos.x, sz = t->last_pos.z, ex = p->pos.x, ez = p->pos.z;
    float seg = sqrtf((ex - sx) * (ex - sx) + (ez - sz) * (ez - sz));
    int sliding = p->sliding && p->speed > 1.5f;
    int moving = p->speed > 0.3f;

    if (p->air) {
        /* airborne: leave no tracks */
    } else if (seg > 1e-5f && moving) {
        if (sliding) {
            walk_segment(t, p, sx, sz, ex, ez, t->path, &t->groove_accum, GROOVE_SPACING, groove);
        } else {
            /* scuffs first so the footprints press on top of them */
            walk_segment(t, p, sx, sz, ex, ez, t->path, &t->drag_accum, DRAG_SPACING, scuff);
            walk_segment(t, p, sx, sz, ex, ez, t->path, &t->foot_accum, FOOT_STRIDE, foot_fn);
        }
        t->path += seg;
    } else if (!moving) {
        t->foot_accum = FOOT_STRIDE * 0.5f; /* first step after standing still comes quickly */
        t->groove_accum = 0;
        t->drag_accum = 0;
    }
    t->last_pos = p->pos;

    /* upload only what changed */
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    t->upload_bytes = 0;
    if (t->dirty_full) {
        t->upload_bytes = TRAIL_N * TRAIL_N * 2;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TRAIL_N, TRAIL_N, GL_RG, GL_UNSIGNED_BYTE, t->data);
    } else if (t->dirty_row_max >= t->dirty_row_min) {
        int rows = t->dirty_row_max - t->dirty_row_min + 1;
        t->upload_bytes = rows * TRAIL_N * 2;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, t->dirty_row_min, TRAIL_N, rows, GL_RG, GL_UNSIGNED_BYTE,
                        t->data + (size_t)t->dirty_row_min * TRAIL_N * 2);
    }
    t->dirty_full = 0;
    t->dirty_row_min = 1; t->dirty_row_max = 0;
}
void trail_bind(const Trail *t) { glBindTexture(GL_TEXTURE_2D, t->tex); }
