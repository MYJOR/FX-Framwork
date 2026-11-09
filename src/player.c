#include "player.h"
#include "terrain.h"
#include <stdlib.h>

#define PI_F 3.14159265f

/* Tuning: all feel lives here. */
#define STROLL_SPEED 1.9f       /* gait 2: walk */
#define WALK_SPEED 4.5f         /* gait 0: jog (default) */
#define RUN_SPEED 7.5f
#define GROUND_FOLLOW 7.0f      /* how fast velocity catches up with input */
#define SLIDE_START_NY 0.84f    /* terrain normal.y below this starts sliding (~33 deg, sand repose angle) */
#define SLIDE_FULL_NY 0.55f     /* ... and is full strength here (~57 deg) */
#define SLIDE_ACCEL 24.0f
#define SLIDE_MAX_SPEED 20.0f
#define TURN_RATE 10.0f
#define JUMP_SPEED 5.4f         /* m/s up; ~1 m apex with GRAVITY */
#define GRAVITY 15.0f
#define WORLD_LIMIT 330.0f

static void build_capsule(Player *p) {
    const int ring_half = 8, seg = 24;
    const float r = PLAYER_RADIUS, h = PLAYER_HEIGHT;
    int rings = (ring_half + 1) * 2;
    float *v = (float *)malloc((size_t)rings * (seg + 1) * 6 * sizeof(float));
    unsigned *idx = (unsigned *)malloc((size_t)(rings - 1) * seg * 6 * sizeof(unsigned));
    size_t vi = 0, ii = 0;
    for (int ring = 0; ring < rings; ring++) {
        int top = ring <= ring_half;
        int k = top ? ring : ring - (ring_half + 1);
        float phi = (top ? 0.0f : PI_F * 0.5f) + ((float)k / ring_half) * (PI_F * 0.5f);
        float cy = top ? h - r : r;
        for (int s = 0; s <= seg; s++) {
            float th = (float)s / seg * 2.0f * PI_F;
            float nx = sinf(phi) * cosf(th), ny = cosf(phi), nz = sinf(phi) * sinf(th);
            v[vi++] = nx * r; v[vi++] = cy + ny * r; v[vi++] = nz * r;
            v[vi++] = nx; v[vi++] = ny; v[vi++] = nz;
        }
    }
    for (int ring = 0; ring < rings - 1; ring++)
        for (int s = 0; s < seg; s++) {
            unsigned a = (unsigned)(ring * (seg + 1) + s), b = a + 1;
            unsigned c = a + (unsigned)(seg + 1), d = c + 1;
            idx[ii++] = a; idx[ii++] = c; idx[ii++] = b;
            idx[ii++] = b; idx[ii++] = c; idx[ii++] = d;
        }

    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    glGenBuffers(1, &p->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vi * sizeof(float)), v, GL_STATIC_DRAW);
    glGenBuffers(1, &p->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(ii * sizeof(unsigned)), idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    p->index_count = (GLsizei)ii;
    free(v); free(idx);
}

void player_init(Player *p, float x, float z) {
    p->pos = v3(x, terrain_height(x, z), z);
    p->vx = p->vz = 0;
    p->yaw = 0;
    p->lean = p->lean_vel = p->fwd_prev = p->accel_f = 0;
    p->speed = 0;
    p->des_vx = p->des_vz = 0;
    p->vy = 0; p->air = 0; p->jump_req = 0; p->yaw_drive = 0; p->yaw_rate = 0;
    p->sliding = 0;
    build_capsule(p);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void player_update(Player *p, float dt, float mx, float mz, int run) {
    V3 n = terrain_normal(p->pos.x, p->pos.z);
    /* horizontal part of the normal points downhill */
    float hl = sqrtf(n.x * n.x + n.z * n.z);
    float dhx = hl > 1e-5f ? n.x / hl : 0, dhz = hl > 1e-5f ? n.z / hl : 0;

    float mag = sqrtf(mx * mx + mz * mz);
    if (mag > 1.0f) { mx /= mag; mz /= mag; mag = 1.0f; }

    /* steepness 0 (flat) .. 1 (slide at full strength) */
    float steep = clampf((SLIDE_START_NY - n.y) / (SLIDE_START_NY - SLIDE_FULL_NY), 0.0f, 1.0f);
    p->sliding = steep > 0.02f;

    /* walking uphill is slower, and impossible on slip faces */
    float uphill = mag > 0 ? -(mx * dhx + mz * dhz) / mag : 0.0f; /* +1 straight up the slope */
    float slope_slow = 1.0f - clampf(uphill, 0.0f, 1.0f) * (0.35f + 0.65f * steep);
    float top_speed = (run == 1 ? RUN_SPEED : (run == 2 ? STROLL_SPEED : WALK_SPEED)) * slope_slow;

    float tx = mx * top_speed, tz = mz * top_speed;
    p->des_vx = tx; p->des_vz = tz;
    /* control fades as the slope takes over */
    float follow = GROUND_FOLLOW * (1.0f - 0.8f * steep);
    if (p->air) { follow *= 0.25f; steep = 0.0f; } /* little steering and no slope pull in the air */
    float k = clampf(dt * follow, 0.0f, 1.0f);
    p->vx += (tx - p->vx) * k;
    p->vz += (tz - p->vz) * k;

    p->vx += dhx * SLIDE_ACCEL * steep * dt;
    p->vz += dhz * SLIDE_ACCEL * steep * dt;

    float sp = sqrtf(p->vx * p->vx + p->vz * p->vz);
    float cap = steep > 0 ? SLIDE_MAX_SPEED : RUN_SPEED * 1.05f;
    if (sp > cap) { p->vx *= cap / sp; p->vz *= cap / sp; sp = cap; }
    p->speed = sp;
    /* only "sliding" when actually carried downhill; climbing up a slope doesn't count */
    p->sliding = steep > 0.02f && (p->vx * dhx + p->vz * dhz) > 1.0f;

    p->pos.x = clampf(p->pos.x + p->vx * dt, -WORLD_LIMIT, WORLD_LIMIT);
    p->pos.z = clampf(p->pos.z + p->vz * dt, -WORLD_LIMIT, WORLD_LIMIT);
    float ground_y = terrain_height(p->pos.x, p->pos.z);
    if (p->jump_req && !p->air) { p->air = 1; p->vy = JUMP_SPEED; p->pos.y = ground_y + 0.01f; }
    p->jump_req = 0;
    if (p->air) {
        p->vy -= GRAVITY * dt;
        p->pos.y += p->vy * dt;
        if (p->pos.y <= ground_y && p->vy < 0) { p->pos.y = ground_y; p->air = 0; p->vy = 0; }
        p->sliding = 0;
    } else {
        p->pos.y = ground_y;
    }

    /* face the direction of travel, taking the shortest turn */
    if (p->yaw_drive) {
        p->yaw += p->yaw_rate * dt; /* a turn clip is rotating the body */
    } else if (sp > 0.4f) {
        float target = atan2f(p->vx, -p->vz);
        float d = target - p->yaw;
        while (d > PI_F) d -= 2.0f * PI_F;
        while (d < -PI_F) d += 2.0f * PI_F;
        p->yaw += d * clampf(dt * TURN_RATE, 0.0f, 1.0f);
    }

    /* Tilt target (+ forward, - backward):
     *  - speed: lean into the run
     *  - acceleration along the facing: pitch forward when pushing off, rock back when braking
     *  - slope along the facing: lean into climbs, lean back descending */
    float fx = sinf(p->yaw), fz = -cosf(p->yaw);
    float fwd = p->vx * fx + p->vz * fz;               /* signed speed along facing */
    float acc = dt > 1e-5f ? (fwd - p->fwd_prev) / dt : 0.0f;
    p->fwd_prev = fwd;
    p->accel_f += (acc - p->accel_f) * clampf(dt * 10.0f, 0.0f, 1.0f);

    float rise = -(n.x * fx + n.z * fz) / (n.y > 0.2f ? n.y : 0.2f); /* + = uphill ahead */
    float speed_lean = clampf(fwd * 0.03f, 0.0f, 0.24f);
    float accel_lean = clampf(p->accel_f * 0.028f, -0.32f, 0.32f);
    float slope_lean = clampf(rise * 0.7f, -0.30f, 0.30f) * clampf(sp / 2.0f, 0.0f, 1.0f);
    float lean_target = clampf(speed_lean + accel_lean + slope_lean, -0.5f, 0.55f);

    /* underdamped spring so the body overshoots a little and settles */
    p->lean_vel += (170.0f * (lean_target - p->lean) - 16.0f * p->lean_vel) * dt;
    p->lean += p->lean_vel * dt;
    p->lean = clampf(p->lean, -0.6f, 0.65f);
}

M4 player_model(const Player *p) {
    /* forward is local -Z; Ry(-yaw) maps it to (sin yaw, -cos yaw) */
    M4 m = m4_mul(m4_translate(p->pos), m4_rotate_y(-p->yaw));
    return m4_mul(m, m4_rotate_x(-p->lean));
}

void player_draw(const Player *p) {
    glBindVertexArray(p->vao);
    glDrawElements(GL_TRIANGLES, p->index_count, GL_UNSIGNED_INT, (void *)0);
}
