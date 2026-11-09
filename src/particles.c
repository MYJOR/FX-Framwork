#include "particles.h"
#include "terrain.h"
#include "wind.h"
#include <stdlib.h>

static void kick_update(Particles *p, float dt, float time);

static float frand(Particles *p) {
    p->rng ^= p->rng << 13; p->rng ^= p->rng >> 17; p->rng ^= p->rng << 5;
    return (float)(p->rng & 0xffffff) / 16777215.0f;
}

/* Crest score: how far this point sits above its neighbours along the wind. */
static float crest_score(float x, float z) {
    float h = terrain_height(x, z);
    float a = terrain_height(x + WIND_X * 7.0f, z + WIND_Z * 7.0f);
    float b = terrain_height(x - WIND_X * 7.0f, z - WIND_Z * 7.0f);
    return h - 0.5f * (a + b);
}

static void spawn(Particles *p, int i, V3 cam, int initial) {
    float x = 0, z = 0;
    int plume = 0;
    /* try to start on a crest: that is where real dunes shed sand */
    for (int tries = 0; tries < 8; tries++) {
        float ang = frand(p) * 6.2831853f, r = sqrtf(frand(p)) * PARTICLE_RADIUS;
        /* bias the spawn disc upwind so grains drift across the view */
        x = cam.x + cosf(ang) * r - WIND_X * 18.0f;
        z = cam.z + sinf(ang) * r - WIND_Z * 18.0f;
        if (crest_score(x, z) > 0.35f) { plume = 1; break; }
    }
    float ground = terrain_height(x, z);
    p->px[i] = x;
    p->pz[i] = z;
    p->lift[i] = plume ? 1.0f : 0.0f;
    p->py[i] = ground + (plume ? 0.1f + frand(p) * 0.6f : 0.05f + frand(p) * frand(p) * 1.8f);
    p->vx[i] = p->vy[i] = p->vz[i] = 0;
    p->hgt[i] = p->py[i] - ground;
    p->maxlife[i] = 2.0f + frand(p) * 4.0f;
    p->life[i] = initial ? p->maxlife[i] * frand(p) : p->maxlife[i];
}

void particles_init(Particles *p, V3 cam) {
    p->rng = 2463534242u;
    for (int i = 0; i < PARTICLE_COUNT; i++) spawn(p, i, cam, 1);

    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    glGenBuffers(1, &p->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)PARTICLE_COUNT * 2 * 4 * sizeof(float), NULL, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    /* dust sprites: pos(3), alpha, size in metres */
    for (int i = 0; i < KICK_COUNT; i++) p->klife[i] = 0;
    glGenVertexArrays(1, &p->dvao);
    glBindVertexArray(p->dvao);
    glGenBuffers(1, &p->dvbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->dvbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)KICK_COUNT * 5 * sizeof(float), NULL, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
}

/* ---- kick-up: sand thrown by the character ------------------------------------ */

static int kick_alloc(Particles *p) {
    int i = p->khead;
    p->khead = (p->khead + 1) % KICK_COUNT; /* ring buffer: oldest is overwritten */
    return i;
}

static void kick_grain(Particles *p, float x, float y, float z, float vx, float vy, float vz) {
    int i = kick_alloc(p);
    p->kx[i] = x + (frand(p) - 0.5f) * 0.12f; p->ky[i] = y + 0.04f; p->kz[i] = z + (frand(p) - 0.5f) * 0.12f;
    p->kvx[i] = vx; p->kvy[i] = vy; p->kvz[i] = vz;
    p->kmax[i] = p->klife[i] = 0.5f + frand(p) * 0.9f;
    p->ksize[i] = 0.035f + frand(p) * 0.035f; p->kkind[i] = 0;
}

static void kick_dust(Particles *p, float x, float y, float z, float vx, float vy, float vz) {
    int i = kick_alloc(p);
    p->kx[i] = x + (frand(p) - 0.5f) * 0.2f; p->ky[i] = y + 0.05f; p->kz[i] = z + (frand(p) - 0.5f) * 0.2f;
    p->kvx[i] = vx; p->kvy[i] = vy; p->kvz[i] = vz;
    p->kmax[i] = p->klife[i] = 1.3f + frand(p) * 1.3f;
    p->ksize[i] = 1.0f + frand(p) * 0.9f; p->kkind[i] = 1;
}

void particles_emit_step(Particles *p, float x, float y, float z, float dx, float dz, float speed) {
    float px = -dz, pz = dx; /* perpendicular */
    float sp = 0.4f + 0.1f * speed;
    int grains = 8 + (int)(speed * 1.6f);
    for (int n = 0; n < grains; n++) {
        float back = (0.4f + 2.0f * frand(p)) * sp, side = (frand(p) - 0.5f) * 1.6f * sp;
        kick_grain(p, x, y, z, -dx * back + px * side, (0.7f + 2.0f * frand(p)) * (0.5f + 0.07f * speed),
                   -dz * back + pz * side);
    }
    int puffs = 3 + (int)(speed * 0.6f);
    for (int n = 0; n < puffs; n++)
        kick_dust(p, x, y, z, -dx * (0.3f + 0.6f * frand(p)) * sp + px * (frand(p) - 0.5f) * 0.5f,
                  0.3f + 0.6f * frand(p), -dz * (0.3f + 0.6f * frand(p)) * sp + pz * (frand(p) - 0.5f) * 0.5f);
}

void particles_emit_spray(Particles *p, float x, float y, float z, float dx, float dz, float speed) {
    float px = -dz, pz = dx;
    int grains = 3 + (int)(speed * 0.15f);
    for (int n = 0; n < grains; n++) {
        float side = (frand(p) < 0.5f ? -1.0f : 1.0f) * (1.2f + 3.0f * frand(p));
        float back = (0.3f + 1.2f * frand(p));
        kick_grain(p, x, y, z, -dx * back + px * side, (1.2f + 3.0f * frand(p)) * (0.5f + 0.05f * speed),
                   -dz * back + pz * side);
    }
    if (frand(p) < 0.9f)
        kick_dust(p, x, y, z, -dx * 0.8f + px * (frand(p) - 0.5f) * 1.5f, 0.5f + 0.7f * frand(p),
                  -dz * 0.8f + pz * (frand(p) - 0.5f) * 1.5f);
}

static void kick_update(Particles *p, float dt, float time) {
    float drag_g = expf(-1.3f * dt), drag_d = expf(-2.2f * dt);
    for (int i = 0; i < KICK_COUNT; i++) {
        if (p->klife[i] <= 0) continue;
        p->klife[i] -= dt;
        float g = wind_gust(p->kx[i], p->kz[i], time);
        float wind = (1.5f + 9.0f * g) * 0.6f;
        if (p->kkind[i] == 0) {
            p->kvy[i] -= 9.0f * dt;
            p->kvx[i] = p->kvx[i] * drag_g + WIND_X * wind * dt * 0.8f;
            p->kvz[i] = p->kvz[i] * drag_g + WIND_Z * wind * dt * 0.8f;
        } else {
            /* dust hangs in the air, is carried by the wind and billows */
            p->kvy[i] += 0.25f * dt;
            p->kvx[i] += (WIND_X * wind - p->kvx[i]) * (1.0f - drag_d);
            p->kvz[i] += (WIND_Z * wind - p->kvz[i]) * (1.0f - drag_d);
            p->kvy[i] *= drag_d;
        }
        p->kx[i] += p->kvx[i] * dt; p->ky[i] += p->kvy[i] * dt; p->kz[i] += p->kvz[i] * dt;

        float ground = terrain_height(p->kx[i], p->kz[i]) + 0.02f;
        if (p->ky[i] < ground) {
            p->ky[i] = ground;
            if (p->kkind[i] == 0 && p->kvy[i] < -1.2f) { /* small bounce, then it settles */
                p->kvy[i] = -p->kvy[i] * 0.22f; p->kvx[i] *= 0.5f; p->kvz[i] *= 0.5f;
            } else if (p->kkind[i] == 0) {
                p->klife[i] = 0;
            } else {
                p->kvy[i] = 0;
            }
        }
    }
}
void particles_update(Particles *p, float dt, V3 cam, float time) {
    kick_update(p, dt, time);
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        p->life[i] -= dt;
        float dx = p->px[i] - cam.x, dz = p->pz[i] - cam.z;
        if (p->life[i] <= 0 || dx * dx + dz * dz > PARTICLE_RADIUS * PARTICLE_RADIUS * 1.2f) {
            spawn(p, i, cam, 0);
            continue;
        }

        float g = wind_gust(p->px[i], p->pz[i], time);
        float ground = terrain_height(p->px[i], p->pz[i]);
        float above = p->py[i] - ground;

        /* wind is slower near the ground and stronger higher up */
        float speed = (2.0f + 15.0f * g) * (0.45f + 0.55f * (above / (above + 1.5f)));
        float tx = WIND_X * speed, tz = WIND_Z * speed;
        float follow = dt * 2.5f; if (follow > 1) follow = 1;
        p->vx[i] += (tx - p->vx[i]) * follow;
        p->vz[i] += (tz - p->vz[i]) * follow;

        /* gravity, countered by turbulence lift during gusts */
        /* lift fades out with height so sand stays a low ground-hugging layer */
        float room = 1.0f - above / 3.5f; if (room < 0) room = 0;
        float lift = p->lift[i] > 0 ? (1.0f + 3.0f * g) * room : 0.0f;
        p->vy[i] += (-4.0f + lift) * dt;
        if (p->lift[i] > 0 && g > 0.15f) p->vy[i] += (frand(p) - 0.3f) * 6.0f * g * dt;

        p->px[i] += p->vx[i] * dt;
        p->py[i] += p->vy[i] * dt;
        p->pz[i] += p->vz[i] * dt;

        /* saltation: grains hit the sand and hop again, more energetically in gusts */
        float gnew = terrain_height(p->px[i], p->pz[i]) + 0.03f;
        if (p->py[i] < gnew) {
            p->py[i] = gnew;
            p->vy[i] = (0.6f + frand(p) * 1.6f) * (0.3f + g);
        }
        p->hgt[i] = p->py[i] - gnew;
        if (p->hgt[i] > 4.0f) p->life[i] = 0;
    }
}

void particles_draw(Particles *p, V3 cam, float time) {
    static float verts[(PARTICLE_COUNT + KICK_COUNT) * 2 * 4];
    static float dverts[KICK_COUNT * 5];
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        float dx = p->px[i] - cam.x, dz = p->pz[i] - cam.z;
        float d = sqrtf(dx * dx + dz * dz) / PARTICLE_RADIUS;
        float radial = 1.0f - d * d; if (radial < 0) radial = 0;
        float t = p->life[i] / p->maxlife[i];
        float fade = sinf(t * 3.14159265f);
        float g = wind_gust(p->px[i], p->pz[i], time);
        float hf = 1.0f - (p->hgt[i] - 0.4f) / 2.0f; hf = hf < 0 ? 0 : (hf > 1 ? 1 : hf);
        float dy = p->py[i] - cam.y;
        float d3 = sqrtf(dx * dx + dz * dz + dy * dy);
        float nearK = (d3 - 3.0f) / 12.0f; nearK = nearK < 0 ? 0 : (nearK > 1 ? 1 : nearK);
        float alpha = fade * radial * hf * nearK * (0.18f + 0.82f * g);
        /* streaks stretch with speed, giving the motion blur of fast grains */
        float k = 0.03f + 0.035f * g;
        float *v = verts + (size_t)i * 8;
        v[0] = p->px[i]; v[1] = p->py[i]; v[2] = p->pz[i]; v[3] = alpha;
        v[4] = p->px[i] - p->vx[i] * k; v[5] = p->py[i] - p->vy[i] * k; v[6] = p->pz[i] - p->vz[i] * k;
        v[7] = 0.0f;
    }
    /* character kick-up: grains join the streak buffer, dust goes to the sprite buffer */
    int nv = PARTICLE_COUNT * 2, nd = 0;
    for (int i = 0; i < KICK_COUNT; i++) {
        if (p->klife[i] <= 0) continue;
        float t = p->klife[i] / p->kmax[i]; /* 1 -> 0 over the particle's life */
        float dx = p->kx[i] - cam.x, dy = p->ky[i] - cam.y, dz = p->kz[i] - cam.z;
        float d3 = sqrtf(dx * dx + dy * dy + dz * dz);
        float nearK = (d3 - 0.8f) / 2.5f; nearK = nearK < 0 ? 0 : (nearK > 1 ? 1 : nearK);
        if (p->kkind[i] == 0) {
            float *v = verts + (size_t)nv * 4;
            float alpha = (t > 0.25f ? 1.0f : t * 4.0f) * 0.95f * nearK;
            v[0] = p->kx[i]; v[1] = p->ky[i]; v[2] = p->kz[i]; v[3] = alpha;
            v[4] = p->kx[i] - p->kvx[i] * 0.03f; v[5] = p->ky[i] - p->kvy[i] * 0.03f;
            v[6] = p->kz[i] - p->kvz[i] * 0.03f; v[7] = 0.0f;
            nv += 2;
            /* a round, lit speck on top of the streak so grains read at any distance */
            float *g = dverts + (size_t)nd * 5;
            g[0] = p->kx[i]; g[1] = p->ky[i]; g[2] = p->kz[i];
            g[3] = (t > 0.25f ? 1.0f : t * 4.0f) * 0.9f * nearK; g[4] = p->ksize[i];
            nd++;
        } else {
            float age = 1.0f - t;
            float a = (age < 0.12f ? age / 0.12f : 1.0f) * powf(t, 0.8f) * 0.62f * nearK;
            float *v = dverts + (size_t)nd * 5;
            v[0] = p->kx[i]; v[1] = p->ky[i]; v[2] = p->kz[i]; v[3] = a;
            v[4] = p->ksize[i] * (0.35f + 0.9f * age); /* billows outward as it ages */
            nd++;
        }
    }
    p->dust_count = nd;

    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)nv * 4 * sizeof(float)), verts, GL_STREAM_DRAW);
    p->line_count = nv / 2;
    p->kick_grains = (nv - PARTICLE_COUNT * 2) / 2;
    if (!p->skip_streaks) glDrawArrays(GL_LINES, 0, nv);

    glBindVertexArray(p->dvao);
    glBindBuffer(GL_ARRAY_BUFFER, p->dvbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)nd * 5 * sizeof(float)), dverts, GL_STREAM_DRAW);
}

void particles_draw_dust(const Particles *p) {
    if (p->dust_count <= 0) return;
    glBindVertexArray(p->dvao);
    glDrawArrays(GL_POINTS, 0, p->dust_count);
}