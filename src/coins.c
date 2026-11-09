#include "coins.h"
#include "terrain.h"
#include <math.h>
#include <stdlib.h>

#define PI_F 3.14159265f
#define PICKUP_DIST 1.3f
#define COLLECT_ANIM 0.45f
#define HOVER_HEIGHT 0.85f

static unsigned rng_next(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}
static float rng_f(unsigned *s) { return (float)(rng_next(s) & 0xffffff) / 16777215.0f; }

/* A coin as a surface of revolution about +Y with a rounded, raised rim. One ring pair per profile
   segment so each segment keeps its own flat normal (crisp edges, smooth around the circumference). */
static void build_mesh(Coins *cs) {
    enum { SEG = 64 };
    const float hr = 0.11f; /* half thickness, in radius units */
    static const float prof[][2] = {
        {0.00f, 1.00f}, {0.88f, 1.00f}, {0.95f, 1.00f}, {0.985f, 0.90f}, {1.00f, 0.60f},
        {1.00f, -0.60f}, {0.985f, -0.90f}, {0.95f, -1.00f}, {0.88f, -1.00f}, {0.00f, -1.00f}};
    const int np = (int)(sizeof prof / sizeof prof[0]);
    int nseg = np - 1;
    size_t vcount = (size_t)nseg * 2 * (SEG + 1);
    float *v = (float *)malloc(vcount * 6 * sizeof(float));
    unsigned *idx = (unsigned *)malloc((size_t)nseg * SEG * 6 * sizeof(unsigned));
    size_t vi = 0, ii = 0;
    for (int s = 0; s < nseg; s++) {
        float r0 = prof[s][0], y0 = prof[s][1] * hr, r1 = prof[s + 1][0], y1 = prof[s + 1][1] * hr;
        float dr = r1 - r0, dy = y1 - y0;
        float nr = -dy, ny = dr; /* outward normal of the segment in the (r, y) half plane */
        float nl = sqrtf(nr * nr + ny * ny);
        nr /= nl; ny /= nl;
        unsigned base = (unsigned)(vi / 6);
        for (int k = 0; k < 2; k++) {
            float r = k ? r1 : r0, y = k ? y1 : y0;
            for (int i = 0; i <= SEG; i++) {
                float a = (float)i / SEG * 2.0f * PI_F, ca = cosf(a), sa = sinf(a);
                v[vi++] = r * ca; v[vi++] = y; v[vi++] = r * sa;
                v[vi++] = nr * ca; v[vi++] = ny; v[vi++] = nr * sa;
            }
        }
        for (int i = 0; i < SEG; i++) {
            unsigned a = base + (unsigned)i, b = a + 1, c = a + (unsigned)(SEG + 1), d = c + 1;
            idx[ii++] = a; idx[ii++] = b; idx[ii++] = c;
            idx[ii++] = b; idx[ii++] = d; idx[ii++] = c;
        }
    }
    glGenVertexArrays(1, &cs->vao);
    glBindVertexArray(cs->vao);
    glGenBuffers(1, &cs->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, cs->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vi * sizeof(float)), v, GL_STATIC_DRAW);
    glGenBuffers(1, &cs->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cs->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(ii * sizeof(unsigned)), idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    cs->index_count = (GLsizei)ii;
    free(v); free(idx);
}

void coins_init(Coins *cs, V3 centre, float radius, int count) {
    cs->n = 0; cs->collected = 0;
    if (count > COIN_MAX) count = COIN_MAX;
    unsigned seed = 0x9e3779b9u;
    /* a small ring right around the start so there's always something to find */
    for (int k = 0; k < 8 && cs->n < count; k++) {
        float a = (float)k * PI_F * 0.25f, r = 5.0f + (float)(k & 1) * 3.0f;
        float x = centre.x + cosf(a) * r, z = centre.z + sinf(a) * r;
        Coin *c = &cs->c[cs->n++];
        c->pos = v3(x, terrain_height(x, z) + HOVER_HEIGHT, z);
        c->phase = rng_f(&seed) * 2.0f * PI_F;
        c->seed = rng_f(&seed);
        c->collect_t = -1.0f;
    }
    for (int tries = 0; cs->n < count && tries < count * 40; tries++) {
        float a = rng_f(&seed) * 2.0f * PI_F, r = 6.0f + sqrtf(rng_f(&seed)) * (radius - 6.0f);
        float x = centre.x + cosf(a) * r, z = centre.z + sinf(a) * r;
        V3 n = terrain_normal(x, z);
        if (n.y < 0.80f) continue; /* keep them off the steep slip faces */
        Coin *c = &cs->c[cs->n++];
        c->pos = v3(x, terrain_height(x, z) + HOVER_HEIGHT, z);
        c->phase = rng_f(&seed) * 2.0f * PI_F;
        c->seed = rng_f(&seed);
        c->collect_t = -1.0f;
    }
    build_mesh(cs);
}

int coins_update(Coins *cs, float dt, V3 pp, Particles *ps) {
    int got = 0;
    for (int i = 0; i < cs->n; i++) {
        Coin *c = &cs->c[i];
        if (c->collect_t >= 0.0f) { c->collect_t += dt; continue; }
        float dx = c->pos.x - pp.x, dz = c->pos.z - pp.z, dy = c->pos.y - (pp.y + 0.9f);
        if (dx * dx + dz * dz + dy * dy < PICKUP_DIST * PICKUP_DIST) {
            c->collect_t = 0.0f;
            cs->collected++;
            got++;
            float gy = terrain_height(c->pos.x, c->pos.z);
            particles_emit_spray(ps, c->pos.x, gy + 0.6f, c->pos.z, 0.0f, 1.0f, 5.0f);
            particles_emit_step(ps, c->pos.x, gy, c->pos.z, 0.0f, 1.0f, 4.0f);
        }
    }
    return got;
}

void coins_draw(const Coins *cs, GLuint program, float time) {
    GLint uModel = glGetUniformLocation(program, "uModel");
    GLint uFade = glGetUniformLocation(program, "uFade");
    GLint uSeed = glGetUniformLocation(program, "uSeed");
    glBindVertexArray(cs->vao);
    for (int i = 0; i < cs->n; i++) {
        const Coin *c = &cs->c[i];
        float scale = COIN_RADIUS, lift = 0.0f, spin = time * 2.3f + c->phase;
        if (c->collect_t >= 0.0f) {
            float t = c->collect_t / COLLECT_ANIM;
            if (t >= 1.0f) continue;
            float e = 1.0f - (1.0f - t) * (1.0f - t);
            lift = e * 1.3f;
            scale *= 1.0f + 0.35f * sinf(t * PI_F) - t; /* pop up, then shrink away */
            spin += t * 14.0f;
            if (scale < 0.001f) continue;
        }
        float bob = 0.07f * sinf(time * 2.0f + c->phase);
        M4 m = m4_mul(m4_translate(v3(c->pos.x, c->pos.y + bob + lift, c->pos.z)), m4_rotate_y(spin));
        m = m4_mul(m, m4_rotate_x(PI_F * 0.5f)); /* stand the disc upright: its faces point along +-Z */
        for (int k = 0; k < 12; k++) m.m[k] *= scale;
        glUniformMatrix4fv(uModel, 1, GL_FALSE, m.m);
        glUniform1f(uFade, 1.0f);
        glUniform1f(uSeed, c->seed);
        glDrawElements(GL_TRIANGLES, cs->index_count, GL_UNSIGNED_INT, (void *)0);
    }
}
