#include "particles.h"
#include "terrain.h"
#include "wind.h"
#include <stdlib.h>

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
}

void particles_update(Particles *p, float dt, V3 cam, float time) {
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
    static float verts[PARTICLE_COUNT * 2 * 4];
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
    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof verts, verts, GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, PARTICLE_COUNT * 2);
}
