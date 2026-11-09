#include "terrain.h"
#include <stdlib.h>

static float hash2(int x, int y) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xffffff) / 16777215.0f;
}

static float vnoise(float x, float y) {
    float fx = floorf(x), fy = floorf(y);
    int ix = (int)fx, iy = (int)fy;
    float tx = x - fx, ty = y - fy;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    float a = hash2(ix, iy), b = hash2(ix + 1, iy);
    float c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
    float top = a + (b - a) * tx, bot = c + (d - c) * tx;
    return top + (bot - top) * ty;
}

static float fbm(float x, float y, int octaves) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int i = 0; i < octaves; i++) {
        sum += vnoise(x, y) * amp;
        norm += amp;
        x = x * 2.03f + 17.1f; y = y * 2.03f - 9.7f;
        amp *= 0.5f;
    }
    return sum / norm; /* 0..1 */
}

float terrain_height(float x, float z) {
    /* broad rolling base */
    float base = (fbm(x * 0.004f, z * 0.004f, 3) - 0.5f) * 60.0f;

    /* main dune field: ridges perpendicular to the wind (wind blows along +dir) */
    float warp = fbm(x * 0.009f + 10.0f, z * 0.009f, 3);
    float u = (x * 0.906f + z * 0.423f) / 75.0f + warp * 2.2f;
    float f = u - floorf(u);
    const float crest = 0.72f;
    float p = f < crest ? f / crest : 1.0f - (f - crest) / (1.0f - crest);
    p = p * p * (3.0f - 2.0f * p);
    float amp = 5.0f + 14.0f * fbm(x * 0.0055f + 50.0f, z * 0.0055f - 20.0f, 2);

    /* secondary, smaller crossing dunes for variety */
    float u2 = (x * -0.35f + z * 0.94f) / 28.0f + warp * 3.0f;
    float f2 = u2 - floorf(u2);
    float p2 = f2 < 0.7f ? f2 / 0.7f : 1.0f - (f2 - 0.7f) / 0.3f;
    p2 = p2 * p2 * (3.0f - 2.0f * p2);
    float amp2 = 1.4f * fbm(x * 0.02f, z * 0.02f + 3.0f, 2);

    float detail = (fbm(x * 0.06f, z * 0.06f, 2) - 0.5f) * 0.5f;
    return base + p * amp + p2 * amp2 + detail;
}

V3 terrain_normal(float x, float z) {
    const float e = 0.4f;
    float dx = terrain_height(x + e, z) - terrain_height(x - e, z);
    float dz = terrain_height(x, z + e) - terrain_height(x, z - e);
    return v3_norm(v3(-dx, 2.0f * e, -dz));
}

void terrain_build(Terrain *t, int cells, float half) {
    int n = cells + 1;
    size_t vcount = (size_t)n * n;
    float *verts = (float *)malloc(vcount * 6 * sizeof(float));
    unsigned *idx = (unsigned *)malloc((size_t)cells * cells * 6 * sizeof(unsigned));
    float step = 2.0f * half / (float)cells;

    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            float x = -half + i * step, z = -half + j * step;
            V3 nr = terrain_normal(x, z);
            float *v = verts + ((size_t)j * n + i) * 6;
            v[0] = x; v[1] = terrain_height(x, z); v[2] = z;
            v[3] = nr.x; v[4] = nr.y; v[5] = nr.z;
        }

    size_t k = 0;
    for (int j = 0; j < cells; j++)
        for (int i = 0; i < cells; i++) {
            unsigned a = (unsigned)(j * n + i), b = a + 1, c = a + (unsigned)n, d = c + 1;
            idx[k++] = a; idx[k++] = c; idx[k++] = b;
            idx[k++] = b; idx[k++] = c; idx[k++] = d;
        }

    glGenVertexArrays(1, &t->vao);
    glBindVertexArray(t->vao);
    glGenBuffers(1, &t->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, t->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vcount * 6 * sizeof(float)), verts, GL_STATIC_DRAW);
    glGenBuffers(1, &t->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, t->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(k * sizeof(unsigned)), idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    t->index_count = (GLsizei)k;
    free(verts); free(idx);
}

void terrain_draw(const Terrain *t) {
    glBindVertexArray(t->vao);
    glDrawElements(GL_TRIANGLES, t->index_count, GL_UNSIGNED_INT, (void *)0);
}
