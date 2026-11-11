#include "office_scene.h"
#include "fbx.h"
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f

/* ---- layout (metres; +Y up, the player spawns looking down -Z at the desk) ---------------------- */
#define ROOM_HX 3.0f      /* half width  (x): 10 ceiling tiles */
#define ROOM_HZ 3.6f      /* half length (z): 12 ceiling tiles */
#define ROOM_H 2.7f
/* The hallway runs along the office's +X wall and on past both of its ends; same height, same finishes. */
#define WALL_T 0.3f                 /* thickness of the wall between them */
#define HALL_X0 (ROOM_HX + WALL_T)  /* hallway: x from here ... */
#define HALL_X1 (HALL_X0 + 1.8f)    /* ... to here */
#define HALL_EXTRA 1.8f             /* how far the hallway runs on past each end of the office (3 ceiling tiles) */
#define HALL_HZ (ROOM_HZ + HALL_EXTRA)
#define DOOR_Z 1.2f                 /* centre of the doorway along the wall */
/* The big room lies beyond the hallway's far wall: from the office doorway, turn left (-z) and its
   doorway is on the right. Its walls sit on the ceiling grid. */
#define BIG_X0 (HALL_X1 + WALL_T)
#define BIG_X1 (BIG_X0 + 9.0f)      /* 15 ceiling tiles wide */
#define BIG_Z0 (-7.8f)              /* 18 tiles long */
#define BIG_Z1 3.0f
#define DOOR2_Z (-3.0f)             /* centre of its doorway */
#define DOOR_HW 0.7f                /* half its width = radius of the arch */
#define DOOR_SPRING 1.55f           /* straight sides up to here, then the half circle: 2.25 m at the crown */
#define ARCH_SEGS 24
#define TILE 0.6f         /* suspended ceiling grid */
#define FIXTURE_DROP 0.004f /* panels and vents sit just under the tiles */
#define EMITTER_DROP 0.001f /* and a panel's diffuser just under its frame */
#define PANEL_FRAME_X 0.037f /* width of the frame in the panel texture, along and across the panel */
#define PANEL_FRAME_Z 0.061f

#define DESK_Z (-1.5f)    /* desk centre */
#define DESK_H 0.75f      /* the table model is a low side table: scaled up to desk height */

#define CHAIR_X 0.06f
#define CHAIR_Z (-2.38f)
#define CHAIR_HEIGHT 1.18f
#define CHAIR_YAW (-14.0f * PI / 180.0f)

/* Light panel radiance. Every surface in the room is warm (cream walls, orange wood), so the light
   is a neutral white leaning slightly cool: the colour in the image comes from the materials. */
#define PANEL_R 33.0f
#define PANEL_G 34.0f
#define PANEL_B 35.0f

/* Regions of "room textures.png" (1536 x 1024), in pixels: x0, y0, x1, y1. */
static const int RECT_WALL[4] = {16, 15, 1018, 678};
static const int RECT_FLOOR[4] = {10, 699, 1024, 1012};
static const int RECT_PANEL[4] = {1040, 500, 1528, 657};
static const int RECT_VENT[4] = {1040, 671, 1256, 889};
static const int RECT_BASEBOARD[4] = {1270, 697, 1528, 882};
/* The ceiling sample is 3 x 3 tiles of slightly different sizes; these are the centres of its grid bars. */
static const float CEIL_COLS[4] = {1040.5f, 1199.0f, 1365.5f, 1525.5f};
static const float CEIL_ROWS[4] = {8.5f, 156.5f, 324.5f, 485.0f};

/* M_EMITTER0 + i is the diffuser of light i */
enum { M_CHAIR, M_DESK, M_BASEBOARD, M_PANEL, M_EMITTER0, M_EMITTER_LAST = M_EMITTER0 + INT_MAX_LIGHTS - 1, M_VENT, M_FLOOR, M_WALL, M_CEILING, M_SMUDGE, M_SWITCH, M_DOOR, M_COUNT };

/* ---- textures ----------------------------------------------------------------------------------- */

/* px: top row first, ch = 1 or 3. Uploads with mipmaps and anisotropic filtering. */
static GLuint upload_texture(const unsigned char *px, int w, int h, int ch, int srgb, GLint wrap_s, GLint wrap_t) {
    size_t stride = (size_t)w * ch;
    unsigned char *flipped = (unsigned char *)malloc(stride * h); /* GL wants the bottom row first */
    if (!flipped) return 0;
    for (int y = 0; y < h; y++) memcpy(flipped + (size_t)y * stride, px + (size_t)(h - 1 - y) * stride, stride);
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (ch == 1) glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, flipped);
    else glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, flipped);
    free(flipped);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
    GLfloat aniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &aniso);
    if (glGetError() == GL_NO_ERROR && aniso > 1.0f)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso > 8.0f ? 8.0f : aniso);
    return t;
}

static GLuint upload_region(const unsigned char *atlas, int aw, const int rect[4], GLint wrap_s, GLint wrap_t) {
    int w = rect[2] - rect[0], h = rect[3] - rect[1];
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 3);
    if (!px) return 0;
    for (int y = 0; y < h; y++)
        memcpy(px + (size_t)y * w * 3, atlas + ((size_t)(rect[1] + y) * aw + rect[0]) * 3, (size_t)w * 3);
    GLuint t = upload_texture(px, w, h, 3, 1, wrap_s, wrap_t);
    free(px);
    return t;
}

/* Resamples the 3 x 3 ceiling tiles onto a regular grid so one repeat is exactly 3 tiles (1.8 m). */
static GLuint upload_ceiling(const unsigned char *atlas, int aw, int ah) {
    enum { CELL = 256, N = CELL * 3 };
    unsigned char *px = (unsigned char *)malloc((size_t)N * N * 3);
    if (!px) return 0;
    for (int oy = 0; oy < N; oy++)
        for (int ox = 0; ox < N; ox++) {
            int cx = ox / CELL, cy = oy / CELL;
            float sx = CEIL_COLS[cx] + ((float)(ox % CELL) + 0.5f) / CELL * (CEIL_COLS[cx + 1] - CEIL_COLS[cx]) - 0.5f;
            float sy = CEIL_ROWS[cy] + ((float)(oy % CELL) + 0.5f) / CELL * (CEIL_ROWS[cy + 1] - CEIL_ROWS[cy]) - 0.5f;
            int x0 = (int)sx, y0 = (int)sy;
            float fx = sx - (float)x0, fy = sy - (float)y0;
            int x1 = x0 + 1 < aw ? x0 + 1 : x0, y1 = y0 + 1 < ah ? y0 + 1 : y0;
            for (int c = 0; c < 3; c++) {
                float a = atlas[((size_t)y0 * aw + x0) * 3 + c], b = atlas[((size_t)y0 * aw + x1) * 3 + c];
                float d = atlas[((size_t)y1 * aw + x0) * 3 + c], e = atlas[((size_t)y1 * aw + x1) * 3 + c];
                px[((size_t)oy * N + ox) * 3 + c] = (unsigned char)((a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy + 0.5f);
            }
        }
    GLuint t = upload_texture(px, N, N, 3, 1, GL_REPEAT, GL_REPEAT);
    free(px);
    return t;
}

static GLuint load_png_texture(const char *path, int srgb) {
    int w, h, ch;
    unsigned char *px = png_load(path, &w, &h, &ch);
    if (!px) { fprintf(stderr, "[office] cannot load %s\n", path); return 0; }
    GLuint t = (ch == 3 || ch == 1) ? upload_texture(px, w, h, ch, srgb && ch == 3, GL_REPEAT, GL_REPEAT) : 0;
    free(px);
    return t;
}

/* A cut-out picture (RGB or RGBA): what is transparent is painted with `fill`, so the picture's
   edge does not pick up a dark fringe when it is filtered. The geometry is cut to the outline. */
static GLuint load_cutout_texture(const char *path, const unsigned char fill[3]) {
    int w, h, ch;
    unsigned char *px = png_load(path, &w, &h, &ch);
    if (!px || (ch != 3 && ch != 4)) { fprintf(stderr, "[office] cannot load %s\n", path); free(px); return 0; }
    unsigned char *rgb = (unsigned char *)malloc((size_t)w * h * 3);
    GLuint t = 0;
    if (rgb) {
        for (size_t i = 0; i < (size_t)w * h; i++) {
            int a = ch == 4 ? px[i * 4 + 3] : 255;
            for (int c = 0; c < 3; c++) rgb[i * 3 + c] = (unsigned char)((px[i * ch + c] * a + fill[c] * (255 - a) + 127) / 255);
        }
        t = upload_texture(rgb, w, h, 3, 1, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    }
    free(rgb); free(px);
    return t;
}

/* Turns a mark painted on a transparent background into a normal map: the surface is pressed in
   where the mark is opaque. bump = how steep the sides get. */
static GLuint load_decal_normal(const char *path, float bump) {
    int w, h, ch;
    unsigned char *px = png_load(path, &w, &h, &ch);
    if (!px || (ch != 3 && ch != 4)) { fprintf(stderr, "[office] cannot load %s\n", path); free(px); return 0; }
    float *depth = (float *)malloc((size_t)w * h * 2 * sizeof(float)), *tmp = depth ? depth + (size_t)w * h : NULL;
    unsigned char *rgb = (unsigned char *)malloc((size_t)w * h * 3);
    GLuint t = 0;
    if (depth && rgb) {
        for (size_t i = 0; i < (size_t)w * h; i++) /* no alpha: dark = deep */
            depth[i] = ch == 4 ? (float)px[i * 4 + 3] / 255.0f : 1.0f - (float)(px[i * 3] + px[i * 3 + 1] + px[i * 3 + 2]) / 765.0f;
        for (int pass = 0; pass < 2; pass++) { /* soften it: a 5 x 5 box, twice */
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    float sum = 0.0f;
                    for (int k = -2; k <= 2; k++) { int q = x + k; sum += depth[(size_t)y * w + (q < 0 ? 0 : (q >= w ? w - 1 : q))]; }
                    tmp[(size_t)y * w + x] = sum / 5.0f;
                }
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    float sum = 0.0f;
                    for (int k = -2; k <= 2; k++) { int q = y + k; sum += tmp[(size_t)(q < 0 ? 0 : (q >= h ? h - 1 : q)) * w + x]; }
                    depth[(size_t)y * w + x] = sum / 5.0f;
                }
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float nx = 0.0f, ny = 0.0f; /* the border stays flat */
                if (x > 0 && x < w - 1 && y > 0 && y < h - 1) { /* height = -depth; rows run down, v runs up */
                    nx = (depth[(size_t)y * w + x + 1] - depth[(size_t)y * w + x - 1]) * 0.5f * bump;
                    ny = (depth[(size_t)(y - 1) * w + x] - depth[(size_t)(y + 1) * w + x]) * 0.5f * bump;
                }
                float inv = 1.0f / sqrtf(nx * nx + ny * ny + 1.0f);
                unsigned char *o = rgb + ((size_t)y * w + x) * 3;
                o[0] = (unsigned char)((nx * inv * 0.5f + 0.5f) * 255.0f + 0.5f);
                o[1] = (unsigned char)((ny * inv * 0.5f + 0.5f) * 255.0f + 0.5f);
                o[2] = (unsigned char)((inv * 0.5f + 0.5f) * 255.0f + 0.5f);
            }
        t = upload_texture(rgb, w, h, 3, 0, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    }
    free(depth); free(rgb); free(px);
    return t;
}
/* Packs the separate roughness and metallic maps into one texture: R = roughness, G = metallic. */
static GLuint load_orm_texture(const char *rough_path, const char *metal_path) {
    int rw, rh, rc, mw, mh, mc;
    unsigned char *rough = png_load(rough_path, &rw, &rh, &rc), *metal = png_load(metal_path, &mw, &mh, &mc);
    GLuint t = 0;
    if (rough && metal && rw == mw && rh == mh) {
        unsigned char *px = (unsigned char *)malloc((size_t)rw * rh * 3);
        if (px) {
            for (size_t i = 0; i < (size_t)rw * rh; i++) {
                px[i * 3] = rough[i * rc]; px[i * 3 + 1] = metal[i * mc]; px[i * 3 + 2] = 0;
            }
            t = upload_texture(px, rw, rh, 3, 0, GL_REPEAT, GL_REPEAT);
            free(px);
        }
    } else {
        fprintf(stderr, "[office] chair roughness/metallic maps missing; using constants\n");
    }
    free(rough); free(metal);
    return t;
}

/* ---- geometry ----------------------------------------------------------------------------------- */

typedef struct { float *v; int n, cap; } VertexBuf; /* 8 floats per vertex: position, normal, uv */

static void push_vertex(VertexBuf *b, V3 p, V3 n, float u, float v) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->v = (float *)realloc(b->v, (size_t)b->cap * 8 * sizeof(float));
    }
    float *o = b->v + (size_t)b->n++ * 8;
    o[0] = p.x; o[1] = p.y; o[2] = p.z; o[3] = n.x; o[4] = n.y; o[5] = n.z; o[6] = u; o[7] = v;
}

/* Rectangle centred at c spanning +-hu and +-hv. It faces along cross(hu, hv); the texture runs
   u0..u1 along hu and v0..v1 along hv. */
static void quad_uv(VertexBuf *b, V3 c, V3 hu, V3 hv, float u0, float v0, float u1, float v1) {
    V3 n = v3_norm(v3_cross(hu, hv));
    V3 p00 = v3_add(c, v3_add(v3_scale(hu, -1), v3_scale(hv, -1))), p10 = v3_add(c, v3_add(hu, v3_scale(hv, -1)));
    V3 p11 = v3_add(c, v3_add(hu, hv)), p01 = v3_add(c, v3_add(v3_scale(hu, -1), hv));
    push_vertex(b, p00, n, u0, v0); push_vertex(b, p10, n, u1, v0); push_vertex(b, p11, n, u1, v1);
    push_vertex(b, p00, n, u0, v0); push_vertex(b, p11, n, u1, v1); push_vertex(b, p01, n, u0, v1);
}

/* Same rectangle textured in world units: the texture repeats every su metres along hu and sv along
   hv, anchored to the world origin so neighbouring surfaces line up. */
static void quad_tiled(VertexBuf *b, V3 c, V3 hu, V3 hv, float su, float sv) {
    V3 du = v3_norm(hu), dv = v3_norm(hv);
    float cu = v3_dot(c, du), cv = v3_dot(c, dv), lu = v3_dot(hu, du), lv = v3_dot(hv, dv);
    quad_uv(b, c, hu, hv, (cu - lu) / su, (cv - lv) / sv, (cu + lu) / su, (cv + lv) / sv);
}

/* Axis-aligned box, faces outward. On the sides the texture's v runs up. */
static void box(VertexBuf *b, V3 mn, V3 mx, float su, float sv) {
    V3 c = v3_scale(v3_add(mn, mx), 0.5f);
    float hx = (mx.x - mn.x) * 0.5f, hy = (mx.y - mn.y) * 0.5f, hz = (mx.z - mn.z) * 0.5f;
    quad_tiled(b, v3(mx.x, c.y, c.z), v3(0, 0, -hz), v3(0, hy, 0), su, sv);
    quad_tiled(b, v3(mn.x, c.y, c.z), v3(0, 0, hz), v3(0, hy, 0), su, sv);
    quad_tiled(b, v3(c.x, c.y, mx.z), v3(hx, 0, 0), v3(0, hy, 0), su, sv);
    quad_tiled(b, v3(c.x, c.y, mn.z), v3(-hx, 0, 0), v3(0, hy, 0), su, sv);
    quad_tiled(b, v3(c.x, mx.y, c.z), v3(hx, 0, 0), v3(0, 0, -hz), su, sv);
    quad_tiled(b, v3(c.x, mn.y, c.z), v3(hx, 0, 0), v3(0, 0, hz), su, sv);
}

static IntMaterial material(GLuint albedo, float roughness, unsigned flags) {
    IntMaterial m;
    memset(&m, 0, sizeof m);
    m.albedo = albedo;
    m.tint[0] = m.tint[1] = m.tint[2] = 1.0f;
    m.roughness = roughness;
    m.specular = 0.04f;
    m.normal_strength = 1.0f;
    m.flags = flags;
    return m;
}

/* ---- walls with the doorway --------------------------------------------------------------------- */

/* Wall vertices are textured in world units like quad_tiled: 3 m across (along z, or along x for the
   faces inside the doorway), 2 m up. */
static void wall_vertex(VertexBuf *b, V3 p, V3 n, int along_x) { push_vertex(b, p, n, (along_x ? p.x : p.z) / 3.0f, p.y / 2.0f); }
static void wall_tri(VertexBuf *b, V3 a, V3 p, V3 q, V3 n, int along_x) {
    V3 e1 = v3_add(p, v3_scale(a, -1.0f)), e2 = v3_add(q, v3_scale(a, -1.0f));
    if (v3_dot(v3_cross(e1, e2), n) < 0.0f) { V3 t = p; p = q; q = t; } /* wound to face n */
    wall_vertex(b, a, n, along_x); wall_vertex(b, p, n, along_x); wall_vertex(b, q, n, along_x);
}
static void wall_quad(VertexBuf *b, V3 p0, V3 p1, V3 p2, V3 p3, V3 n, int along_x) {
    wall_tri(b, p0, p1, p2, n, along_x);
    wall_tri(b, p0, p2, p3, n, along_x);
}

/* Point i of ARCH_SEGS along the arch of the doorway centred on door_z, from its -z foot over the crown to its +z foot: z and y. */
static void arch_point(int i, float door_z, float *z, float *y) {
    float a = PI * (1.0f - (float)i / (float)ARCH_SEGS);
    *z = door_z + DOOR_HW * cosf(a);
    *y = DOOR_SPRING + DOOR_HW * sinf(a);
}

/* One face of a wall across x with a doorway in it, at x, facing nx, from z_lo to z_hi: everything but the doorway. */
static void arch_wall(VertexBuf *b, float x, float nx, float z_lo, float z_hi, float door_z) {
    V3 n = v3(nx, 0, 0);
    float za = door_z - DOOR_HW, zb = door_z + DOOR_HW;
    /* This face is split at the spring line and the walls it meets at its two ends are not, which
       leaves pinholes down those corners. Run it on a little way behind them: the corner then lies
       inside a triangle. (They show as soon as one room is dark and the next one is lit.) */
    z_lo -= 0.02f; z_hi += 0.02f;
    for (int side = 0; side < 2; side++) { /* beside the doorway, split at the spring line so the arch pieces meet vertices */
        float z0 = side ? zb : z_lo, z1 = side ? z_hi : za;
        wall_quad(b, v3(x, 0, z0), v3(x, 0, z1), v3(x, DOOR_SPRING, z1), v3(x, DOOR_SPRING, z0), n, 0);
        wall_quad(b, v3(x, DOOR_SPRING, z0), v3(x, DOOR_SPRING, z1), v3(x, ROOM_H, z1), v3(x, ROOM_H, z0), n, 0);
    }
    for (int i = 0; i < ARCH_SEGS; i++) { /* from the arch up to the ceiling */
        float z0, y0, z1, y1;
        arch_point(i, door_z, &z0, &y0); arch_point(i + 1, door_z, &z1, &y1);
        wall_quad(b, v3(x, y0, z0), v3(x, y1, z1), v3(x, ROOM_H, z1), v3(x, ROOM_H, z0), n, 0);
    }
}

/* The inside of a doorway: two straight sides and the curved underside of the arch. */
static void arch_lining(VertexBuf *b, float x0, float x1, float door_z) {
    float za = door_z - DOOR_HW, zb = door_z + DOOR_HW;
    wall_quad(b, v3(x0, 0, za), v3(x1, 0, za), v3(x1, DOOR_SPRING, za), v3(x0, DOOR_SPRING, za), v3(0, 0, 1), 1);
    wall_quad(b, v3(x0, 0, zb), v3(x1, 0, zb), v3(x1, DOOR_SPRING, zb), v3(x0, DOOR_SPRING, zb), v3(0, 0, -1), 1);
    for (int i = 0; i < ARCH_SEGS; i++) {
        float z[2], y[2], v[2];
        V3 n[2];
        for (int k = 0; k < 2; k++) {
            arch_point(i + k, door_z, &z[k], &y[k]);
            n[k] = v3_norm(v3(0, DOOR_SPRING - y[k], door_z - z[k])); /* towards the arch's centre: smooth shading */
            v[k] = (DOOR_SPRING + DOOR_HW * PI * (float)(i + k) / (float)ARCH_SEGS) / 2.0f; /* the texture carries on up from the side */
        }
        /* two triangles: (x0,0) (x1,0) (x1,1) and (x0,0) (x1,1) (x0,1), facing down into the opening */
        const float xs[6] = {x0, x1, x1, x0, x1, x0};
        const int ks[6] = {0, 0, 1, 0, 1, 1};
        for (int t = 0; t < 6; t += 3) {
            int order[3] = {t, t + 1, t + 2};
            V3 p[3];
            for (int c = 0; c < 3; c++) p[c] = v3(xs[t + c], y[ks[t + c]], z[ks[t + c]]);
            V3 fn = v3_cross(v3_add(p[1], v3_scale(p[0], -1.0f)), v3_add(p[2], v3_scale(p[0], -1.0f)));
            if (v3_dot(fn, n[0]) < 0.0f) { order[1] = t + 2; order[2] = t + 1; }
            for (int c = 0; c < 3; c++) {
                int q = order[c], k = ks[q];
                push_vertex(b, v3(xs[q], y[k], z[k]), n[k], xs[q] / 3.0f, v[k]);
            }
        }
    }
}
/* ---- light switches ------------------------------------------------------------------------------ */

/* "light switch.png" is a photograph of a toggle switch, 1086 x 1448, with its normal map beside it.
   The plate in it is drawn against a chequered background, so the plate is modelled to its outline
   (a rounded rectangle) and the background is never shown. The lever is modelled too, in front of
   the painted one, so it can be thrown. Measured in the picture, in pixels: */
#define SW_TEX_W 1086.0f
#define SW_TEX_H 1448.0f
#define SW_CX 541.5f            /* centre of the plate */
#define SW_CY 722.0f
#define SW_HALF_X 423.5f        /* its half size */
#define SW_HALF_Y 660.0f
#define SW_CORNER 37.0f         /* radius of its corners */
#define SW_LEVER_CY 695.0f      /* the lever: the part of the rocker between the words ON and OFF */
#define SW_LEVER_HALF_X 42.0f
#define SW_LEVER_HALF_Y 71.0f
#define SW_M (0.070f / (2.0f * SW_HALF_X)) /* metres per pixel: the plate is 70 mm wide (and so 109 mm tall) */
#define SW_DEPTH 0.004f         /* how far the plate stands off the wall */
#define SW_HEIGHT 1.22f         /* centre of the plate above the floor */
#define SW_BESIDE 0.28f         /* ... and beside the edge of its doorway */
#define SW_CORNER_SEGS 4
#define SW_LEVER_VERTS 30

/* A point of a switch: x to the right as seen from the room, y up, z out of the wall, from the plate's centre. */
static V3 sw_point(const OfficeSwitch *sw, float x, float y, float z) {
    V3 r = v3_cross(v3(0, 1, 0), sw->normal);
    return v3_add(sw->pos, v3_add(v3_scale(r, x), v3_add(v3(0, y, 0), v3_scale(sw->normal, z))));
}
/* Same, as a direction. */
static V3 sw_dir(const OfficeSwitch *sw, float x, float y, float z) {
    V3 o = sw_point(sw, 0, 0, 0), p = sw_point(sw, x, y, z);
    return v3_norm(v3(p.x - o.x, p.y - o.y, p.z - o.z));
}
/* One triangle of a switch facing n, each corner given as (x, y, z) in the switch's space. With
   plain set, it shows a bare patch of the plate's plastic instead of the picture at that spot. */
static void sw_tri(VertexBuf *b, const OfficeSwitch *sw, const float p[3][3], V3 n, int plain) {
    V3 w[3];
    for (int i = 0; i < 3; i++) w[i] = sw_point(sw, p[i][0], p[i][1], p[i][2]);
    int order[3] = {0, 1, 2};
    V3 e1 = v3_add(w[1], v3_scale(w[0], -1.0f)), e2 = v3_add(w[2], v3_scale(w[0], -1.0f));
    if (v3_dot(v3_cross(e1, e2), n) < 0.0f) { order[1] = 2; order[2] = 1; }
    for (int i = 0; i < 3; i++) {
        int k = order[i];
        /* pulled in a hair so the filter never reaches the background around the plate */
        float px = plain ? 300.0f : SW_CX + p[k][0] / SW_M * 0.985f, py = plain ? 300.0f : SW_CY - p[k][1] / SW_M * 0.990f;
        push_vertex(b, w[k], n, px / SW_TEX_W, 1.0f - py / SW_TEX_H);
    }
}
static void sw_quad(VertexBuf *b, const OfficeSwitch *sw, const float p[4][3], V3 n, int plain) {
    const float t0[3][3] = {{p[0][0], p[0][1], p[0][2]}, {p[1][0], p[1][1], p[1][2]}, {p[2][0], p[2][1], p[2][2]}};
    const float t1[3][3] = {{p[0][0], p[0][1], p[0][2]}, {p[2][0], p[2][1], p[2][2]}, {p[3][0], p[3][1], p[3][2]}};
    sw_tri(b, sw, t0, n, plain);
    sw_tri(b, sw, t1, n, plain);
}

/* The plate: its face with the picture on it, and its rim. */
static void switch_plate(VertexBuf *b, const OfficeSwitch *sw) {
    enum { N = 4 * (SW_CORNER_SEGS + 1) };
    const float hx = SW_HALF_X * SW_M, hy = SW_HALF_Y * SW_M, r = SW_CORNER * SW_M;
    float out[N][2];
    for (int c = 0; c < 4; c++) /* corners, anticlockwise from the right-hand top one */
        for (int i = 0; i <= SW_CORNER_SEGS; i++) {
            float a = ((float)c + (float)i / (float)SW_CORNER_SEGS) * PI * 0.5f;
            float cx = (c == 0 || c == 3) ? hx - r : -hx + r, cy = c < 2 ? hy - r : -hy + r;
            out[c * (SW_CORNER_SEGS + 1) + i][0] = cx + r * cosf(a);
            out[c * (SW_CORNER_SEGS + 1) + i][1] = cy + r * sinf(a);
        }
    for (int i = 0; i < N; i++) {
        const float *p = out[i], *q = out[(i + 1) % N];
        const float face[3][3] = {{0, 0, SW_DEPTH}, {p[0], p[1], SW_DEPTH}, {q[0], q[1], SW_DEPTH}};
        sw_tri(b, sw, face, sw->normal, 0);
        const float rim[4][3] = {{p[0], p[1], SW_DEPTH}, {q[0], q[1], SW_DEPTH}, {q[0], q[1], 0}, {p[0], p[1], 0}};
        sw_quad(b, sw, rim, sw_dir(sw, q[1] - p[1], p[0] - q[0], 0), 0);
    }
}

/* The lever, SW_LEVER_VERTS vertices: a stubby bat hinged in the plate, leaning up when on and down when off. */
static void switch_lever(VertexBuf *b, const OfficeSwitch *sw, int on) {
    const float a = SW_LEVER_HALF_X * SW_M, h = SW_LEVER_HALF_Y * SW_M, yc = (SW_CY - SW_LEVER_CY) * SW_M, s = on ? 1.0f : -1.0f;
    /* its outline from the side, (along the plate towards the tip, out of the plate): heel, toe, the tip's two corners */
    const float prof[4][2] = {{-h, 0.0f}, {h, 0.0f}, {h + 0.0008f, 0.0085f}, {h - 0.0052f, 0.0100f}};
    float y[4], z[4];
    for (int i = 0; i < 4; i++) { y[i] = yc + s * prof[i][0]; z[i] = SW_DEPTH + prof[i][1]; }
    for (int side = 0; side < 2; side++) {
        float x = side ? a : -a;
        const float q[4][3] = {{x, y[0], z[0]}, {x, y[1], z[1]}, {x, y[2], z[2]}, {x, y[3], z[3]}};
        sw_quad(b, sw, q, sw_dir(sw, side ? 1.0f : -1.0f, 0, 0), 1);
    }
    for (int i = 1; i < 4; i++) { /* the three faces across it: beyond the tip, the tip, and the long back it is pushed by */
        int j = (i + 1) % 4;
        const float q[4][3] = {{-a, y[i], z[i]}, {a, y[i], z[i]}, {a, y[j], z[j]}, {-a, y[j], z[j]}};
        float ny = z[j] - z[i], nz = y[i] - y[j]; /* across the edge; pick the way that points off the lever */
        float mid_y = (y[i] + y[j]) * 0.5f - yc, mid_z = (z[i] + z[j]) * 0.5f - (SW_DEPTH + 0.004f);
        if (ny * mid_y + nz * mid_z < 0.0f) { ny = -ny; nz = -nz; }
        sw_quad(b, sw, q, sw_dir(sw, 0, ny, nz), 1);
    }
}

/* ---- the red door ------------------------------------------------------------------------------- */

/* "red door.png" (1024 x 1536, cut out) is a panelled door in its frame, with a cornice over it that
   is wider than the rest; "red door normal map.png" is its relief. It is modelled as two slabs to
   that outline, standing off the hallway's end wall. Measured in the picture, in pixels: */
#define DOOR_TEX_W 1024.0f
#define DOOR_TEX_H 1536.0f
#define DOOR_PX_MID 511.0f      /* its centre line */
#define DOOR_PX_BOTTOM 1512.0f  /* where it stands on the floor */
#define DOOR_PX_TOP 26.0f
#define DOOR_PX_NECK 95.0f      /* the cornice ends here ... */
#define DOOR_PX_TOP_HALF 369.0f /* ... its half width at the top ... */
#define DOOR_PX_NECK_HALF 345.0f /* ... and where it meets the frame */
#define DOOR_PX_HALF 342.0f     /* half width of the frame */
#define DOOR_HEIGHT 2.30f       /* metres, floor to the top of the cornice */
#define DOOR_M (DOOR_HEIGHT / (DOOR_PX_BOTTOM - DOOR_PX_TOP))
#define DOOR_DEPTH 0.045f       /* how far the frame stands off the wall */
#define DOOR_CORNICE 0.075f     /* ... and the cornice */

/* One triangle of the door facing n. Corners are (x, y, depth): pixels of the picture and metres off
   the wall. plain: show the door's bare paint instead of the picture at that spot (the slabs' edges). */
static void door_tri(VertexBuf *b, V3 foot, const float p[3][3], V3 n, int plain) {
    V3 w[3];
    for (int i = 0; i < 3; i++) w[i] = v3(foot.x + (p[i][0] - DOOR_PX_MID) * DOOR_M, (DOOR_PX_BOTTOM - p[i][1]) * DOOR_M, foot.z + p[i][2]);
    int order[3] = {0, 1, 2};
    V3 e1 = v3_add(w[1], v3_scale(w[0], -1.0f)), e2 = v3_add(w[2], v3_scale(w[0], -1.0f));
    if (v3_dot(v3_cross(e1, e2), n) < 0.0f) { order[1] = 2; order[2] = 1; }
    for (int i = 0; i < 3; i++) {
        int k = order[i];
        float px = plain ? 512.0f : p[k][0], py = plain ? 1440.0f : p[k][1];
        push_vertex(b, w[k], n, px / DOOR_TEX_W, 1.0f - py / DOOR_TEX_H);
    }
}
static void door_quad(VertexBuf *b, V3 foot, float x0, float y0, float d0, float x1, float y1, float d1,
                      float x2, float y2, float d2, float x3, float y3, float d3, V3 n, int plain) {
    const float t0[3][3] = {{x0, y0, d0}, {x1, y1, d1}, {x2, y2, d2}}, t1[3][3] = {{x0, y0, d0}, {x2, y2, d2}, {x3, y3, d3}};
    door_tri(b, foot, t0, n, plain);
    door_tri(b, foot, t1, n, plain);
}

/* foot: the middle of its threshold, on the wall. It faces +z, down the hallway. */
static void red_door(VertexBuf *b, V3 foot) {
    const float l = DOOR_PX_MID - DOOR_PX_HALF, r = DOOR_PX_MID + DOOR_PX_HALF, top = DOOR_PX_NECK, bot = DOOR_PX_BOTTOM;
    const float cl0 = DOOR_PX_MID - DOOR_PX_TOP_HALF, cr0 = DOOR_PX_MID + DOOR_PX_TOP_HALF; /* the cornice: wide at the top ... */
    const float cl1 = DOOR_PX_MID - DOOR_PX_NECK_HALF, cr1 = DOOR_PX_MID + DOOR_PX_NECK_HALF; /* ... narrower where it sits on the frame */
    const float d = DOOR_DEPTH, c = DOOR_CORNICE, ct = DOOR_PX_TOP;
    V3 out = v3(0, 0, 1);
    door_quad(b, foot, l, bot, d, r, bot, d, r, top, d, l, top, d, out, 0);                 /* the door in its frame */
    door_quad(b, foot, l, bot, 0, l, bot, d, l, top, d, l, top, 0, v3(-1, 0, 0), 1);       /* its two sides */
    door_quad(b, foot, r, bot, 0, r, bot, d, r, top, d, r, top, 0, v3(1, 0, 0), 1);
    door_quad(b, foot, cl1, top, c, cr1, top, c, cr0, ct, c, cl0, ct, c, out, 0);           /* the cornice */
    door_quad(b, foot, cl1, top, 0, cl1, top, c, cl0, ct, c, cl0, ct, 0, v3_norm(v3(-(top - ct), 0, 0)), 1); /* its ends, ... */
    door_quad(b, foot, cr1, top, 0, cr1, top, c, cr0, ct, c, cr0, ct, 0, v3_norm(v3(top - ct, 0, 0)), 1);
    door_quad(b, foot, cl0, ct, 0, cr0, ct, 0, cr0, ct, c, cl0, ct, c, v3(0, 1, 0), 1);     /* ... its top, and underneath */
    door_quad(b, foot, cl1, top, 0, cr1, top, 0, cr1, top, c, cl1, top, c, v3(0, -1, 0), 1);
}

int office_room_at(float x) { return x < ROOM_HX + WALL_T * 0.5f ? 0 : (x < HALL_X1 + WALL_T * 0.5f ? 1 : 2); }

void office_set_room_lights(Office *o, int room, int on) {
    if (room < 0 || room >= OFFICE_ROOMS) return;
    o->room_on[room] = on != 0;
    for (int i = 0; i < o->switch_count; i++) {
        const OfficeSwitch *sw = &o->switches[i];
        if (sw->room != room) continue;
        VertexBuf lever = {0};
        switch_lever(&lever, sw, o->room_on[room]);
        if (lever.n == SW_LEVER_VERTS) {
            size_t bytes = (size_t)SW_LEVER_VERTS * 8 * sizeof(float);
            memcpy(o->room + (size_t)sw->lever * 8, lever.v, bytes);
            if (o->world && o->scene.vbo) { /* ... and in the buffer the renderer draws from */
                size_t at = (size_t)(o->room_base + sw->lever) * 8;
                memcpy(o->world + at, lever.v, bytes);
                glBindBuffer(GL_ARRAY_BUFFER, o->scene.vbo);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)o->scene.vertex_count * 8 * sizeof(float)), o->world, GL_STATIC_DRAW);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }
        }
        free(lever.v);
    }
}

int office_switch_at(const Office *o, V3 eye, V3 dir) {
    const float reach = 1.5f, aim = 0.11f; /* metres: arm's length; how far off the plate's centre the look may pass */
    int best = -1;
    float best_off = aim;
    for (int i = 0; i < o->switch_count; i++) {
        const OfficeSwitch *sw = &o->switches[i];
        V3 to = v3_add(sw->pos, v3_scale(eye, -1.0f));
        if (v3_dot(to, sw->normal) >= 0.0f) continue; /* from behind its wall */
        float t = v3_dot(to, dir);
        if (t < 0.05f || t > reach) continue;
        V3 miss = v3_add(to, v3_scale(dir, -t));
        float off = sqrtf(v3_dot(miss, miss));
        if (off < best_off) { best_off = off; best = i; }
    }
    return best;
}

/* ---- movable objects ---------------------------------------------------------------------------- */

static const char *TYPE_NAMES[OT_COUNT] = {"Chair", "Desk", "Light panel", "Vent"};
static const char *TYPE_KEYS[OT_COUNT] = {"chair", "desk", "light", "vent"};

const char *office_type_name(OfficeObjectType type) { return type >= 0 && type < OT_COUNT ? TYPE_NAMES[type] : "?"; }

V3 office_object_to_world(const OfficeObject *obj, V3 p) {
    float cs = cosf(obj->yaw), sn = sinf(obj->yaw);
    return v3(obj->pos.x + p.x * cs + p.z * sn, obj->pos.y + p.y, obj->pos.z - p.x * sn + p.z * cs);
}

/* World-space bounds of an object-space box after placement. */
static void placed_bounds(const OfficeObject *obj, V3 mn, V3 mx, V3 *out_min, V3 *out_max) {
    V3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
    for (int i = 0; i < 4; i++) {
        V3 p = office_object_to_world(obj, v3(i & 1 ? mx.x : mn.x, 0, i & 2 ? mx.z : mn.z));
        if (p.x < lo.x) lo.x = p.x;
        if (p.z < lo.z) lo.z = p.z;
        if (p.x > hi.x) hi.x = p.x;
        if (p.z > hi.z) hi.z = p.z;
    }
    lo.y = obj->pos.y + mn.y; hi.y = obj->pos.y + mx.y;
    *out_min = lo; *out_max = hi;
}

void office_clamp_object(const Office *o, OfficeObject *obj) {
    (void)o;
    const float margin = 0.02f;
    V3 lo, hi;
    placed_bounds(obj, obj->bmin, obj->bmax, &lo, &hi);
    /* an object stays in the room it is in: the office, the hallway, or the big room */
    static const float rooms[3][4] = { /* x0, z0, x1, z1 */
        {-ROOM_HX, -ROOM_HZ, ROOM_HX, ROOM_HZ}, {HALL_X0, -HALL_HZ, HALL_X1, HALL_HZ}, {BIG_X0, BIG_Z0, BIG_X1, BIG_Z1}};
    const float *rm = rooms[obj->pos.x < ROOM_HX + WALL_T * 0.5f ? 0 : (obj->pos.x < HALL_X1 + WALL_T * 0.5f ? 1 : 2)];
    if (lo.x < rm[0] + margin) obj->pos.x += rm[0] + margin - lo.x;
    if (hi.x > rm[2] - margin) obj->pos.x -= hi.x - (rm[2] - margin);
    if (lo.z < rm[1] + margin) obj->pos.z += rm[1] + margin - lo.z;
    if (hi.z > rm[3] - margin) obj->pos.z -= hi.z - (rm[3] - margin);
}
/* The material a part of an object is drawn with, or -1 if it is not drawn (the diffuser of a light
   that did not get one of the renderer's light slots). */
static int part_material(const OfficeObject *obj, const OfficePart *part) {
    if (!part->emitter) return part->material;
    return obj->light >= 0 ? M_EMITTER0 + obj->light : -1;
}

void office_update(Office *o) {
    IntScene *s = &o->scene;
    s->proxy_count = 0;
    s->light_count = 0;
    o->collider_count = 0;
    /* the two walls with doorways, either side of their openings */
    o->colliders[o->collider_count++] = (OfficeCollider){ROOM_HX, -ROOM_HZ, HALL_X0, DOOR_Z - DOOR_HW};
    o->colliders[o->collider_count++] = (OfficeCollider){ROOM_HX, DOOR_Z + DOOR_HW, HALL_X0, ROOM_HZ};
    o->colliders[o->collider_count++] = (OfficeCollider){HALL_X1, -HALL_HZ, BIG_X0, DOOR2_Z - DOOR_HW};
    o->colliders[o->collider_count++] = (OfficeCollider){HALL_X1, DOOR2_Z + DOOR_HW, BIG_X0, BIG_Z1};
    /* ... and the solid ground around the rooms, inside the level's bounding box: beyond the office's
       two ends, beyond the hallway's -z end, and beyond the big room's +z end */
    o->colliders[o->collider_count++] = (OfficeCollider){-ROOM_HX - 1.0f, BIG_Z0 - 1.0f, HALL_X0, -ROOM_HZ};
    o->colliders[o->collider_count++] = (OfficeCollider){-ROOM_HX - 1.0f, ROOM_HZ, HALL_X0, HALL_HZ + 1.0f};
    o->colliders[o->collider_count++] = (OfficeCollider){ROOM_HX, BIG_Z0 - 1.0f, BIG_X0, -HALL_HZ};
    o->colliders[o->collider_count++] = (OfficeCollider){HALL_X1, BIG_Z1, BIG_X1 + 1.0f, HALL_HZ + 1.0f};
    /* hand out the light slots and size the vertex buffer */
    int total = o->room_count;
    for (int k = 0; k < o->object_count; k++) {
        OfficeObject *obj = &o->objects[k];
        obj->light = -1;
        if (!obj->visible) continue;
        if (obj->kind == OBJ_LIGHT && s->light_count < INT_MAX_LIGHTS) obj->light = s->light_count++;
        const OfficeProto *proto = &o->protos[obj->type];
        for (int p = 0; p < proto->part_count; p++) total += proto->parts[p].n;
    }
    if (total > o->world_cap) {
        float *grown = (float *)realloc(o->world, (size_t)total * 8 * sizeof(float));
        if (!grown) return;
        o->world = grown; o->world_cap = total;
    }

    /* objects first, one draw per material, so they fill the depth buffer before the room */
    int n = 0;
    s->draw_count = 0;
    for (int m = 0; m < M_COUNT; m++) {
        int first = n;
        for (int k = 0; k < o->object_count; k++) {
            const OfficeObject *obj = &o->objects[k];
            if (!obj->visible) continue;
            const OfficeProto *proto = &o->protos[obj->type];
            float cs = cosf(obj->yaw), sn = sinf(obj->yaw);
            for (int p = 0; p < proto->part_count; p++) {
                const OfficePart *part = &proto->parts[p];
                if (part_material(obj, part) != m) continue;
                for (int i = 0; i < part->n; i++) {
                    const float *v = part->v + (size_t)i * 8;
                    float *w = o->world + (size_t)n++ * 8;
                    w[0] = obj->pos.x + v[0] * cs + v[2] * sn;
                    w[1] = obj->pos.y + v[1];
                    w[2] = obj->pos.z - v[0] * sn + v[2] * cs;
                    w[3] = v[3] * cs + v[5] * sn;
                    w[4] = v[4];
                    w[5] = -v[3] * sn + v[5] * cs;
                    w[6] = v[6]; w[7] = v[7];
                }
            }
        }
        if (n > first && s->draw_count < INT_MAX_DRAWS) {
            IntDraw d = {first, n - first, m};
            s->draws[s->draw_count++] = d;
        }
    }
    o->room_base = n;
    memcpy(o->world + (size_t)n * 8, o->room, (size_t)o->room_count * 8 * sizeof(float));
    for (int i = 0; i < o->room_draw_count && s->draw_count < INT_MAX_DRAWS; i++) {
        IntDraw d = o->room_draws[i];
        d.first += n;
        s->draws[s->draw_count++] = d;
    }
    n += o->room_count;
    s->vertex_count = n;

    for (int k = 0; k < o->object_count; k++) {
        const OfficeObject *obj = &o->objects[k];
        if (!obj->visible) continue;
        float cs = cosf(obj->yaw), sn = sinf(obj->yaw);
        if (obj->light >= 0) {
            IntLight *l = &s->lights[obj->light];
            /* neutral white leaning slightly cool by design; warmth trades blue against red */
            V3 rad = v3(PANEL_R * (1.0f + 0.22f * obj->warmth), PANEL_G * (1.0f + 0.03f * obj->warmth),
                        PANEL_B * (1.0f - 0.30f * obj->warmth));
            rad = v3_scale(rad, obj->intensity);
            l->pos = v3(obj->pos.x, obj->pos.y - EMITTER_DROP, obj->pos.z);
            l->half_u = v3(obj->light_half.x * cs, 0, -obj->light_half.x * sn);
            l->half_v = v3(obj->light_half.z * sn, 0, obj->light_half.z * cs);
            l->normal = v3(0, -1, 0);
            l->radiance = rad;
            IntMaterial *em = &s->mats[M_EMITTER0 + obj->light];
            em->emissive[0] = rad.x; em->emissive[1] = rad.y; em->emissive[2] = rad.z;
        }
        for (int i = 0; i < obj->box_count; i++) {
            V3 lo, hi;
            placed_bounds(obj, obj->boxes[i].min, obj->boxes[i].max, &lo, &hi);
            if (s->proxy_count < INT_MAX_PROXIES) { s->proxies[s->proxy_count].min = lo; s->proxies[s->proxy_count].max = hi; s->proxy_count++; }
        }
        if (obj->solid && o->collider_count < OFFICE_MAX_COLLIDERS) {
            V3 lo, hi;
            placed_bounds(obj, obj->bmin, obj->bmax, &lo, &hi);
            OfficeCollider c = {lo.x, lo.z, hi.x, hi.z};
            o->colliders[o->collider_count++] = c;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)s->vertex_count * 8 * sizeof(float)), o->world, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* ---- adding and removing objects ---------------------------------------------------------------- */

static int name_taken(const Office *o, const char *name, int except) {
    for (int i = 0; i < o->object_count; i++)
        if (i != except && !strcmp(o->objects[i].name, name)) return 1;
    return 0;
}

/* "Chair", then "Chair 2", "Chair 3", ... */
static void unique_name(const Office *o, const char *base, char out[OFFICE_NAME_LEN]) {
    char stem[OFFICE_NAME_LEN];
    snprintf(stem, sizeof stem, "%.24s", base);
    int len = (int)strlen(stem);
    while (len > 0 && stem[len - 1] >= '0' && stem[len - 1] <= '9') len--; /* "Vent 2" continues as "Vent 3" */
    while (len > 0 && stem[len - 1] == ' ') len--;
    stem[len] = 0;
    if (!len) snprintf(stem, sizeof stem, "Object");
    snprintf(out, OFFICE_NAME_LEN, "%s", stem);
    for (int n = 2; name_taken(o, out, -1) && n < 1000; n++) snprintf(out, OFFICE_NAME_LEN, "%s %d", stem, n);
}

int office_light_count(const Office *o, int visible_only) {
    int n = 0;
    for (int i = 0; i < o->object_count; i++)
        if (o->objects[i].kind == OBJ_LIGHT && (!visible_only || o->objects[i].visible)) n++;
    return n;
}

int office_add_object(Office *o, OfficeObjectType type, V3 pos, float yaw) {
    if (type < 0 || type >= OT_COUNT || o->object_count >= OFFICE_MAX_OBJECTS) return -1;
    if (type == OT_LIGHT_PANEL && office_light_count(o, 0) >= INT_MAX_LIGHTS) return -1;
    OfficeObject obj = o->protos[type].tmpl;
    obj.pos.x = pos.x; obj.pos.z = pos.z;
    if (obj.kind != OBJ_FURNITURE) { /* ceiling fixtures sit in the grid */
        obj.pos.x = -ROOM_HX + (floorf((obj.pos.x + ROOM_HX) / TILE) + (type == OT_LIGHT_PANEL ? 1.0f : 0.5f)) * TILE;
        obj.pos.z = -ROOM_HZ + (floorf((obj.pos.z + ROOM_HZ) / TILE) + 0.5f) * TILE;
    }
    obj.yaw = yaw;
    office_clamp_object(o, &obj);
    obj.home = obj.pos; obj.home_yaw = obj.yaw;
    unique_name(o, TYPE_NAMES[type], obj.name);
    o->objects[o->object_count] = obj;
    return o->object_count++;
}

int office_duplicate_object(Office *o, int index) {
    if (index < 0 || index >= o->object_count || o->object_count >= OFFICE_MAX_OBJECTS) return -1;
    OfficeObject obj = o->objects[index];
    if (obj.kind == OBJ_LIGHT && office_light_count(o, 0) >= INT_MAX_LIGHTS) return -1;
    float step = obj.kind == OBJ_FURNITURE ? 0.4f : (obj.type == OT_LIGHT_PANEL ? 2.0f * TILE : TILE);
    obj.pos.x += step;
    office_clamp_object(o, &obj);
    if (fabsf(obj.pos.x - o->objects[index].pos.x) < step * 0.5f) { /* against the wall: go the other way */
        obj.pos.x = o->objects[index].pos.x - step;
        office_clamp_object(o, &obj);
    }
    obj.home = obj.pos; obj.home_yaw = obj.yaw;
    obj.locked = 0;
    unique_name(o, o->objects[index].name, obj.name);
    o->objects[o->object_count] = obj;
    return o->object_count++;
}

void office_remove_object(Office *o, int index) {
    if (index < 0 || index >= o->object_count) return;
    memmove(&o->objects[index], &o->objects[index + 1], (size_t)(o->object_count - index - 1) * sizeof(OfficeObject));
    o->object_count--;
}

/* Centre of the fixture that occupies grid cells [cx0, cx1) x [cz0, cz1); cell 0 starts at the -x / -z wall. */
static V3 fixture_pos(int cx0, int cz0, int cx1, int cz1) {
    return v3(-ROOM_HX + (float)(cx0 + cx1) * 0.5f * TILE, 0, -ROOM_HZ + (float)(cz0 + cz1) * 0.5f * TILE);
}

static void add_named(Office *o, OfficeObjectType type, V3 pos, float yaw, const char *name) {
    int i = office_add_object(o, type, pos, yaw);
    if (i < 0) return;
    OfficeObject *obj = &o->objects[i];
    obj->pos.x = obj->home.x = pos.x; obj->pos.z = obj->home.z = pos.z; /* exactly as designed, no grid rounding */
    snprintf(obj->name, sizeof obj->name, "%s", name);
}

void office_reset(Office *o) {
    o->object_count = 0;
    add_named(o, OT_CHAIR, v3(CHAIR_X, 0, CHAIR_Z), CHAIR_YAW, "Chair");
    add_named(o, OT_DESK, v3(0, 0, DESK_Z), 0.0f, "Desk");
    /* The key light sits in front of the desk and to its right, so the desk and chair are lit from the
       player's side with shadows falling away; the second fills the entry end. */
    add_named(o, OT_LIGHT_PANEL, fixture_pos(6, 5, 8, 6), 0.0f, "Key light");
    add_named(o, OT_LIGHT_PANEL, fixture_pos(2, 9, 4, 10), 0.0f, "Fill light");
    add_named(o, OT_VENT, fixture_pos(7, 4, 8, 5), 0.0f, "Vent 1");
    add_named(o, OT_VENT, fixture_pos(2, 2, 3, 3), 0.0f, "Vent 2");
    add_named(o, OT_LIGHT_PANEL, fixture_pos(11, 5, 13, 6), 0.0f, "Hall light"); /* mid hallway, centred across it */
    add_named(o, OT_LIGHT_PANEL, fixture_pos(17, -3, 19, -2), 0.0f, "Big room light 1"); /* four, evenly over the big room */
    add_named(o, OT_LIGHT_PANEL, fixture_pos(24, -3, 26, -2), 0.0f, "Big room light 2");
    add_named(o, OT_LIGHT_PANEL, fixture_pos(17, 6, 19, 7), 0.0f, "Big room light 3");
    add_named(o, OT_LIGHT_PANEL, fixture_pos(24, 6, 26, 7), 0.0f, "Big room light 4");
    office_update(o);
}

/* ---- save / load -------------------------------------------------------------------------------- */

int office_save(const Office *o, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "office-scene 1\n# type x z yaw(deg) visible locked intensity warmth name\n");
    for (int i = 0; i < o->object_count; i++) {
        const OfficeObject *obj = &o->objects[i];
        fprintf(f, "object %s %.4f %.4f %.3f %d %d %.3f %.3f %s\n", TYPE_KEYS[obj->type], obj->pos.x, obj->pos.z,
                obj->yaw * 180.0f / PI, obj->visible, obj->locked, obj->intensity, obj->warmth, obj->name);
    }
    int ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

int office_load(Office *o, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    static OfficeObject loaded[OFFICE_MAX_OBJECTS];
    int count = 0, lights = 0, version = 0, ok = 1;
    char line[256];
    if (!fgets(line, sizeof line, f) || sscanf(line, "office-scene %d", &version) != 1 || version != 1) ok = 0;
    while (ok && fgets(line, sizeof line, f)) {
        char key[16], name[OFFICE_NAME_LEN];
        float x, z, deg, intensity, warmth;
        int visible, locked;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (sscanf(line, "object %15s %f %f %f %d %d %f %f %31[^\r\n]", key, &x, &z, &deg, &visible, &locked, &intensity, &warmth, name) != 9) { ok = 0; break; }
        int type = -1;
        for (int t = 0; t < OT_COUNT; t++) if (!strcmp(key, TYPE_KEYS[t])) type = t;
        if (type < 0 || count >= OFFICE_MAX_OBJECTS) { ok = 0; break; }
        if (type == OT_LIGHT_PANEL && ++lights > INT_MAX_LIGHTS) { ok = 0; break; }
        OfficeObject obj = o->protos[type].tmpl;
        obj.pos.x = x; obj.pos.z = z; obj.yaw = deg * PI / 180.0f;
        obj.visible = visible != 0; obj.locked = locked != 0;
        obj.intensity = intensity < 0.0f ? 0.0f : intensity; obj.warmth = warmth;
        office_clamp_object(o, &obj);
        obj.home = obj.pos; obj.home_yaw = obj.yaw;
        snprintf(obj.name, sizeof obj.name, "%s", name);
        loaded[count++] = obj;
    }
    fclose(f);
    if (!ok) return 0;
    memcpy(o->objects, loaded, (size_t)count * sizeof(OfficeObject));
    o->object_count = count;
    office_update(o);
    return 1;
}

/* ---- build -------------------------------------------------------------------------------------- */

/* Closes a part of a prototype: takes the vertices gathered so far. */
static void end_part(OfficeProto *proto, VertexBuf *b, int material, int emitter) {
    OfficePart *part = &proto->parts[proto->part_count++];
    part->v = b->v; part->n = b->n; part->material = material; part->emitter = emitter;
    memset(b, 0, sizeof *b);
}

/* Fills in the template object every instance of the prototype starts from, and measures it. */
static OfficeObject *end_proto(OfficeProto *proto, OfficeObjectType type, OfficeObjectKind kind, float y) {
    OfficeObject *obj = &proto->tmpl;
    memset(obj, 0, sizeof *obj);
    obj->type = type; obj->kind = kind;
    obj->visible = 1;
    obj->pos = v3(0, y, 0);
    obj->light = -1;
    obj->intensity = 1.0f;
    obj->snap = kind == OBJ_FURNITURE ? 0.1f : TILE;
    obj->bmin = v3(1e9f, 1e9f, 1e9f); obj->bmax = v3(-1e9f, -1e9f, -1e9f);
    for (int p = 0; p < proto->part_count; p++)
        for (int i = 0; i < proto->parts[p].n; i++) {
            const float *v = proto->parts[p].v + (size_t)i * 8;
            if (v[0] < obj->bmin.x) obj->bmin.x = v[0];
            if (v[1] < obj->bmin.y) obj->bmin.y = v[1];
            if (v[2] < obj->bmin.z) obj->bmin.z = v[2];
            if (v[0] > obj->bmax.x) obj->bmax.x = v[0];
            if (v[1] > obj->bmax.y) obj->bmax.y = v[1];
            if (v[2] > obj->bmax.z) obj->bmax.z = v[2];
        }
    return obj;
}

static void object_box(OfficeObject *obj, V3 mn, V3 mx) {
    if (obj->box_count < OFFICE_OBJECT_BOXES) { obj->boxes[obj->box_count].min = mn; obj->boxes[obj->box_count].max = mx; obj->box_count++; }
}

/* A ceiling fixture of nx x nz grid cells, in object space: centred on the origin, facing down. */
static void fixture(VertexBuf *b, int nx, int nz) {
    quad_uv(b, v3(0, 0, 0), v3((float)nx * TILE * 0.5f, 0, 0), v3(0, 0, (float)nz * TILE * 0.5f), 0, 0, 1, 1);
}

static void begin_room_draw(Office *o, const VertexBuf *b, int material) {
    IntDraw *d = &o->room_draws[o->room_draw_count++];
    d->first = b->n; d->count = 0; d->material = material;
}
static void end_room_draw(Office *o, const VertexBuf *b) {
    IntDraw *d = &o->room_draws[o->room_draw_count - 1];
    d->count = b->n - d->first;
}

int office_build(Office *o, const char *asset_dir) {
    memset(o, 0, sizeof *o);
    IntScene *s = &o->scene;
    char path[512], path2[512];

    /* room surfaces: one sheet with wall, floor, ceiling, light panel, vent and baseboard */
    int aw, ah, ach;
    snprintf(path, sizeof path, "%s/room textures.png", asset_dir);
    unsigned char *atlas = png_load(path, &aw, &ah, &ach);
    if (!atlas || ach != 3 || aw != 1536 || ah != 1024) {
        fprintf(stderr, "[office] %s: missing or not the expected 1536x1024 RGB sheet\n", path);
        free(atlas);
        return 0;
    }
    GLuint wall_tex = upload_region(atlas, aw, RECT_WALL, GL_MIRRORED_REPEAT, GL_MIRRORED_REPEAT);
    GLuint floor_tex = upload_region(atlas, aw, RECT_FLOOR, GL_MIRRORED_REPEAT, GL_MIRRORED_REPEAT);
    GLuint panel_tex = upload_region(atlas, aw, RECT_PANEL, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    GLuint vent_tex = upload_region(atlas, aw, RECT_VENT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    GLuint base_tex = upload_region(atlas, aw, RECT_BASEBOARD, GL_REPEAT, GL_CLAMP_TO_EDGE);
    GLuint ceil_tex = upload_ceiling(atlas, aw, ah);
    free(atlas);

    /* chair: mesh + PBR maps */
    const char *chair = "chair/Meshy_AI_Black_Office_Chair_1110045910_texture_fbx/Meshy_AI_Black_Office_Chair_1110045910_texture";
    FbxMesh mesh;
    snprintf(path, sizeof path, "%s/%s.fbx", asset_dir, chair);
    if (!fbx_load_mesh(path, &mesh)) {
        fprintf(stderr, "[office] cannot load the chair model %s\n", path);
        return 0;
    }
    snprintf(path, sizeof path, "%s/%s.png", asset_dir, chair);
    GLuint chair_albedo = load_png_texture(path, 1);
    snprintf(path, sizeof path, "%s/%s_normal.png", asset_dir, chair);
    GLuint chair_normal = load_png_texture(path, 0);
    snprintf(path, sizeof path, "%s/%s_roughness.png", asset_dir, chair);
    snprintf(path2, sizeof path2, "%s/%s_metallic.png", asset_dir, chair);
    GLuint chair_orm = load_orm_texture(path, path2);

    /* desk: mesh + PBR maps (the 2k PNGs are conversions of the 4k JPG / EXR originals next to them) */
    FbxMesh table;
    snprintf(path, sizeof path, "%s/table/gallinera_table_4k.fbx", asset_dir);
    if (!fbx_load_mesh(path, &table)) {
        fprintf(stderr, "[office] cannot load the desk model %s\n", path);
        return 0;
    }
    snprintf(path, sizeof path, "%s/table/textures/gallinera_table_diff_2k.png", asset_dir);
    GLuint table_albedo = load_png_texture(path, 1);
    snprintf(path, sizeof path, "%s/table/textures/gallinera_table_nor_gl_2k.png", asset_dir);
    GLuint table_normal = load_png_texture(path, 0);
    snprintf(path, sizeof path, "%s/table/textures/gallinera_table_orm_2k.png", asset_dir);
    GLuint table_orm = load_png_texture(path, 0); /* R = roughness, G = metallic (none) */

    snprintf(path, sizeof path, "%s/smudge decal.png", asset_dir);
    GLuint smudge_normal = load_decal_normal(path, 6.0f);
    snprintf(path, sizeof path, "%s/light switch.png", asset_dir);
    GLuint switch_albedo = load_png_texture(path, 1);
    snprintf(path, sizeof path, "%s/light switch normal map.png", asset_dir);
    GLuint switch_normal = load_png_texture(path, 0);
    snprintf(path, sizeof path, "%s/red door.png", asset_dir);
    const unsigned char door_red[3] = {120, 22, 22};
    GLuint door_albedo = load_cutout_texture(path, door_red);
    snprintf(path, sizeof path, "%s/red door normal map.png", asset_dir);
    GLuint door_normal = load_png_texture(path, 0);

    /* ---- materials ---- */
    s->mat_count = M_COUNT;
    s->mats[M_CHAIR] = material(chair_albedo, 1.0f, MAT_TWO_SIDED | MAT_CASTS_SHADOW);
    s->mats[M_CHAIR].normal = chair_normal;
    s->mats[M_CHAIR].orm = chair_orm;
    s->mats[M_CHAIR].metallic = chair_orm ? 1.0f : 0.0f;
    if (!chair_orm) s->mats[M_CHAIR].roughness = 0.55f;
    /* the leather is authored almost pure black, which swallows all shading: lift it to charcoal and
       give it a leather sheen so the cushions read */
    s->mats[M_CHAIR].tint[0] = s->mats[M_CHAIR].tint[1] = s->mats[M_CHAIR].tint[2] = 2.3f;
    s->mats[M_CHAIR].roughness = chair_orm ? 0.85f : 0.5f;
    s->mats[M_CHAIR].specular = 0.055f;
    /* desk: an oiled dark wood table */
    s->mats[M_DESK] = material(table_albedo, 1.0f, MAT_TWO_SIDED | MAT_CASTS_SHADOW);
    s->mats[M_DESK].normal = table_normal;
    s->mats[M_DESK].orm = table_orm;
    if (!table_orm) s->mats[M_DESK].roughness = 0.6f;
    s->mats[M_DESK].specular = 0.05f;
    s->mats[M_BASEBOARD] = material(base_tex, 0.4f, MAT_SHELL);
    s->mats[M_PANEL] = material(panel_tex, 0.5f, MAT_SHELL);
    for (int i = 0; i < INT_MAX_LIGHTS; i++) /* one per light: office_update sets their radiance */
        s->mats[M_EMITTER0 + i] = material(0, 1.0f, MAT_SHELL);
    s->mats[M_VENT] = material(vent_tex, 0.55f, MAT_SHELL);
    s->mats[M_FLOOR] = material(floor_tex, 0.34f, MAT_SHELL); /* sealed laminate: a soft sheen, not a mirror */
    s->mats[M_FLOOR].rough_variation = 0.30f;
    s->mats[M_FLOOR].specular = 0.07f;
    s->mats[M_WALL] = material(wall_tex, 0.92f, MAT_SHELL);
    s->mats[M_CEILING] = material(ceil_tex, 0.95f, MAT_SHELL);
    s->mats[M_SMUDGE] = material(0, 1.0f, MAT_DECAL); /* relief only: no stain */
    s->mats[M_SMUDGE].normal = smudge_normal;
    s->mats[M_SWITCH] = material(switch_albedo, 0.42f, 0); /* moulded plastic, worn to a soft gloss */
    s->mats[M_SWITCH].normal = switch_normal;
    s->mats[M_SWITCH].specular = 0.05f;
    /* the photograph is exposed darker than the room's other textures: lift it to the ivory it is */
    s->mats[M_SWITCH].tint[0] = s->mats[M_SWITCH].tint[1] = s->mats[M_SWITCH].tint[2] = 1.3f;
    s->mats[M_DOOR] = material(door_albedo, 0.5f, MAT_SHELL); /* old gloss paint, gone dull where hands have been */
    s->mats[M_DOOR].normal = door_normal;
    s->mats[M_DOOR].normal_strength = 1.4f;
    s->mats[M_DOOR].rough_variation = 0.25f;
    s->mats[M_DOOR].specular = 0.05f;
    if (!door_albedo) { s->mats[M_DOOR].tint[0] = 0.22f; s->mats[M_DOOR].tint[1] = 0.012f; s->mats[M_DOOR].tint[2] = 0.012f; }
    if (!switch_albedo) { s->mats[M_SWITCH].tint[0] = 0.50f; s->mats[M_SWITCH].tint[1] = 0.36f; s->mats[M_SWITCH].tint[2] = 0.22f; }

    /* ---- prototypes: what an object of each type is made of, in its own space ---- */
    VertexBuf vb = {0};
    OfficeObject *obj;

    {   /* the model is Y-up with its back toward -Z and arbitrary units: stand it on the floor at chair height */
        float scale = CHAIR_HEIGHT / (mesh.max[1] - mesh.min[1]);
        float mx = (mesh.min[0] + mesh.max[0]) * 0.5f, mz = (mesh.min[2] + mesh.max[2]) * 0.5f;
        for (int i = 0; i < mesh.vertex_count; i++) {
            const float *v = mesh.v + (size_t)i * 8;
            push_vertex(&vb, v3((v[0] - mx) * scale, (v[1] - mesh.min[1]) * scale, (v[2] - mz) * scale),
                        v3(v[3], v[4], v[5]), v[6], v[7]);
        }
    }
    end_part(&o->protos[OT_CHAIR], &vb, M_CHAIR, 0);
    obj = end_proto(&o->protos[OT_CHAIR], OT_CHAIR, OBJ_FURNITURE, 0.0f);
    obj->solid = 1;
    fbx_free(&mesh);

    {   /* The table model is Z-up, in metres, centred on its footprint: turn it Y-up and scale it to desk height. */
        float scale = DESK_H / (table.max[2] - table.min[2]);
        float cx = (table.min[0] + table.max[0]) * 0.5f, cy = (table.min[1] + table.max[1]) * 0.5f;
        for (int i = 0; i < table.vertex_count; i++) {
            const float *v = table.v + (size_t)i * 8;
            push_vertex(&vb, v3((v[0] - cx) * scale, (v[2] - table.min[2]) * scale, -(v[1] - cy) * scale),
                        v3(v[3], v[5], -v[4]), v[6], v[7]);
        }
        float hx = (table.max[0] - table.min[0]) * 0.5f * scale, hz = (table.max[1] - table.min[1]) * 0.5f * scale;
        end_part(&o->protos[OT_DESK], &vb, M_DESK, 0);
        obj = end_proto(&o->protos[OT_DESK], OT_DESK, OBJ_FURNITURE, 0.0f);
        obj->solid = 1;
        /* reflection stand-ins: the overhanging top, and the slatted body under it as one block */
        object_box(obj, v3(-hx, DESK_H - 0.05f, -hz), v3(hx, DESK_H, hz));
        object_box(obj, v3(-hx + 0.04f, 0.03f, -hz + 0.04f), v3(hx - 0.04f, DESK_H - 0.05f, hz - 0.04f));
        fbx_free(&table);
    }
    s->proxy_albedo = v3(0.035f, 0.022f, 0.015f); /* the table's wood, roughly */

    {   /* 0.6 x 1.2 m light panel: a framed fixture plus its diffuser, the lit area, drawn as its own
           piece of geometry so its outline is antialiased like any other edge */
        V3 half = v3(TILE - PANEL_FRAME_X, 0, TILE * 0.5f - PANEL_FRAME_Z);
        fixture(&vb, 2, 1);
        end_part(&o->protos[OT_LIGHT_PANEL], &vb, M_PANEL, 0);
        quad_uv(&vb, v3(0, -EMITTER_DROP, 0), v3(half.x, 0, 0), v3(0, 0, half.z), 0, 0, 1, 1);
        end_part(&o->protos[OT_LIGHT_PANEL], &vb, M_EMITTER0, 1);
        obj = end_proto(&o->protos[OT_LIGHT_PANEL], OT_LIGHT_PANEL, OBJ_LIGHT, ROOM_H - FIXTURE_DROP);
        obj->light_half = half;
    }

    fixture(&vb, 1, 1);
    end_part(&o->protos[OT_VENT], &vb, M_VENT, 0);
    end_proto(&o->protos[OT_VENT], OT_VENT, OBJ_FIXTURE, ROOM_H - FIXTURE_DROP);

    /* ---- the room itself: fixed ---- */
    begin_room_draw(o, &vb, M_BASEBOARD);
    {   /* the texture's height is the board's height; it repeats along the wall */
        const float h = 0.10f, t = 0.012f, su = 0.5f;
        box(&vb, v3(-ROOM_HX, 0, -ROOM_HZ), v3(ROOM_HX, h, -ROOM_HZ + t), su, h);
        box(&vb, v3(-ROOM_HX, 0, ROOM_HZ - t), v3(ROOM_HX, h, ROOM_HZ), su, h);
        box(&vb, v3(-ROOM_HX, 0, -ROOM_HZ + t), v3(-ROOM_HX + t, h, ROOM_HZ - t), su, h);
        const float za = DOOR_Z - DOOR_HW, zb = DOOR_Z + DOOR_HW;
        box(&vb, v3(ROOM_HX - t, 0, -ROOM_HZ + t), v3(ROOM_HX, h, za), su, h); /* office side of the shared wall, around the doorway */
        box(&vb, v3(ROOM_HX - t, 0, zb), v3(ROOM_HX, h, ROOM_HZ - t), su, h);
        box(&vb, v3(ROOM_HX, 0, za), v3(HALL_X0, h, za + t), su, h);           /* through the doorway */
        box(&vb, v3(ROOM_HX, 0, zb - t), v3(HALL_X0, h, zb), su, h);
        box(&vb, v3(HALL_X0, 0, -HALL_HZ + t), v3(HALL_X0 + t, h, za), su, h); /* hallway */
        box(&vb, v3(HALL_X0, 0, zb), v3(HALL_X0 + t, h, HALL_HZ - t), su, h);
        const float zc = DOOR2_Z - DOOR_HW, zd = DOOR2_Z + DOOR_HW;
        box(&vb, v3(HALL_X1 - t, 0, -HALL_HZ + t), v3(HALL_X1, h, zc), su, h); /* its far wall, around the big room's doorway */
        box(&vb, v3(HALL_X1 - t, 0, zd), v3(HALL_X1, h, HALL_HZ - t), su, h);
        box(&vb, v3(HALL_X1, 0, zc), v3(BIG_X0, h, zc + t), su, h);            /* through that doorway */
        box(&vb, v3(HALL_X1, 0, zd - t), v3(BIG_X0, h, zd), su, h);
        box(&vb, v3(BIG_X0, 0, BIG_Z0 + t), v3(BIG_X0 + t, h, zc), su, h);     /* big room */
        box(&vb, v3(BIG_X0, 0, zd), v3(BIG_X0 + t, h, BIG_Z1 - t), su, h);
        box(&vb, v3(BIG_X1 - t, 0, BIG_Z0 + t), v3(BIG_X1, h, BIG_Z1 - t), su, h);
        box(&vb, v3(BIG_X0, 0, BIG_Z0), v3(BIG_X1, h, BIG_Z0 + t), su, h);
        box(&vb, v3(BIG_X0, 0, BIG_Z1 - t), v3(BIG_X1, h, BIG_Z1), su, h);
        box(&vb, v3(HALL_X0, 0, -HALL_HZ), v3(HALL_X1, h, -HALL_HZ + t), su, h);
        box(&vb, v3(HALL_X0, 0, HALL_HZ - t), v3(HALL_X1, h, HALL_HZ), su, h);
    }
    end_room_draw(o, &vb);

    begin_room_draw(o, &vb, M_FLOOR); /* planks run across the room (along x) */
    {   /* office, then hallway and doorway: both anchored to the world, so the boards run straight through */
        float cx = (ROOM_HX + HALL_X1) * 0.5f, hx = (HALL_X1 - ROOM_HX) * 0.5f;
        quad_tiled(&vb, v3(0, 0, 0), v3(ROOM_HX, 0, 0), v3(0, 0, -ROOM_HZ), 4.0f, 1.25f);
        quad_tiled(&vb, v3(cx, 0, 0), v3(hx, 0, 0), v3(0, 0, -HALL_HZ), 4.0f, 1.25f);
        quad_tiled(&vb, v3((HALL_X1 + BIG_X1) * 0.5f, 0, (BIG_Z0 + BIG_Z1) * 0.5f), v3((BIG_X1 - HALL_X1) * 0.5f, 0, 0),
                   v3(0, 0, -(BIG_Z1 - BIG_Z0) * 0.5f), 4.0f, 1.25f); /* big room and its doorway */
    }
    end_room_draw(o, &vb);

    begin_room_draw(o, &vb, M_WALL);
    {
        const float su = 3.0f, sv = 2.0f, hy = ROOM_H * 0.5f;
        quad_tiled(&vb, v3(0, hy, -ROOM_HZ), v3(ROOM_HX, 0, 0), v3(0, hy, 0), su, sv);
        quad_tiled(&vb, v3(0, hy, ROOM_HZ), v3(-ROOM_HX, 0, 0), v3(0, hy, 0), su, sv);
        quad_tiled(&vb, v3(-ROOM_HX, hy, 0), v3(0, 0, -ROOM_HZ), v3(0, hy, 0), su, sv);
        arch_wall(&vb, ROOM_HX, -1.0f, -ROOM_HZ, ROOM_HZ, DOOR_Z); /* the wall shared with the hallway: its office face, ... */
        arch_wall(&vb, HALL_X0, 1.0f, -HALL_HZ, HALL_HZ, DOOR_Z);  /* ... its hallway face, which runs on past the office ... */
        arch_lining(&vb, ROOM_HX, HALL_X0, DOOR_Z); /* ... and the doorway through it */
        float hcx = (HALL_X0 + HALL_X1) * 0.5f, hhx = (HALL_X1 - HALL_X0) * 0.5f;
        arch_wall(&vb, HALL_X1, -1.0f, -HALL_HZ, HALL_HZ, DOOR2_Z); /* the hallway's far wall, with the doorway to the big room */
        arch_wall(&vb, BIG_X0, 1.0f, BIG_Z0, BIG_Z1, DOOR2_Z);
        arch_lining(&vb, HALL_X1, BIG_X0, DOOR2_Z);
        float bcx = (BIG_X0 + BIG_X1) * 0.5f, bhx = (BIG_X1 - BIG_X0) * 0.5f, bcz = (BIG_Z0 + BIG_Z1) * 0.5f, bhz = (BIG_Z1 - BIG_Z0) * 0.5f;
        quad_tiled(&vb, v3(BIG_X1, hy, bcz), v3(0, 0, bhz), v3(0, hy, 0), su, sv); /* the big room's other three walls */
        quad_tiled(&vb, v3(bcx, hy, BIG_Z0), v3(bhx, 0, 0), v3(0, hy, 0), su, sv);
        quad_tiled(&vb, v3(bcx, hy, BIG_Z1), v3(-bhx, 0, 0), v3(0, hy, 0), su, sv);
        quad_tiled(&vb, v3(hcx, hy, -HALL_HZ), v3(hhx, 0, 0), v3(0, hy, 0), su, sv);
        quad_tiled(&vb, v3(hcx, hy, HALL_HZ), v3(-hhx, 0, 0), v3(0, hy, 0), su, sv);
    }
    end_room_draw(o, &vb);

    begin_room_draw(o, &vb, M_CEILING); /* one texture repeat = 3 tiles, so the grid lands on the fixtures */
    {   /* the grid carries on over the hallway: half tiles along both of its long walls */
        float cx = (ROOM_HX + HALL_X1) * 0.5f, hx = (HALL_X1 - ROOM_HX) * 0.5f;
        quad_tiled(&vb, v3(0, ROOM_H, 0), v3(ROOM_HX, 0, 0), v3(0, 0, ROOM_HZ), 3.0f * TILE, 3.0f * TILE);
        quad_tiled(&vb, v3(cx, ROOM_H, 0), v3(hx, 0, 0), v3(0, 0, HALL_HZ), 3.0f * TILE, 3.0f * TILE);
        quad_tiled(&vb, v3((HALL_X1 + BIG_X1) * 0.5f, ROOM_H, (BIG_Z0 + BIG_Z1) * 0.5f), v3((BIG_X1 - HALL_X1) * 0.5f, 0, 0),
                   v3(0, 0, (BIG_Z1 - BIG_Z0) * 0.5f), 3.0f * TILE, 3.0f * TILE);
    }
    end_room_draw(o, &vb);

    begin_room_draw(o, &vb, M_SMUDGE);
    /* gouges dragged across the plaster of the hallway wall facing the office doorway, at about hand height */
    quad_uv(&vb, v3(HALL_X1 - 0.002f, 1.15f, DOOR_Z + 0.25f), v3(0, 0, 0.85f), v3(0, 0.425f, 0), 0, 0, 1, 1);
    end_room_draw(o, &vb);

    /* a light switch for every room, on the wall beside the doorway where a hand looks for it coming
       in; the hallway has a doorway at each end of its run, so it gets one by each */
    begin_room_draw(o, &vb, M_SWITCH);
    {
        const struct { float x, nx, z; int room; } at[OFFICE_MAX_SWITCHES] = {
            {ROOM_HX, -1.0f, DOOR_Z + DOOR_HW + SW_BESIDE, 0},
            {HALL_X0, 1.0f, DOOR_Z - DOOR_HW - SW_BESIDE, 1},
            {HALL_X1, -1.0f, DOOR2_Z + DOOR_HW + SW_BESIDE, 1},
            {BIG_X0, 1.0f, DOOR2_Z + DOOR_HW + SW_BESIDE, 2}};
        for (int i = 0; i < OFFICE_ROOMS; i++) o->room_on[i] = 1;
        for (int i = 0; i < OFFICE_MAX_SWITCHES; i++) {
            OfficeSwitch *sw = &o->switches[o->switch_count++];
            sw->pos = v3(at[i].x, SW_HEIGHT, at[i].z);
            sw->normal = v3(at[i].nx, 0, 0);
            sw->room = at[i].room;
            switch_plate(&vb, sw);
            sw->lever = vb.n;
            switch_lever(&vb, sw, 1);
        }
    }
    end_room_draw(o, &vb);

    /* the red door: in the middle of the hallway's end wall, the end to the left on coming out of the office */
    begin_room_draw(o, &vb, M_DOOR);
    {
        V3 foot = v3((HALL_X0 + HALL_X1) * 0.5f, 0, -HALL_HZ);
        red_door(&vb, foot);
        o->door = v3(foot.x, 1.15f, foot.z + DOOR_DEPTH);
    }
    end_room_draw(o, &vb);
    o->room = vb.v;
    o->room_count = vb.n;

    s->room_min = v3(-ROOM_HX, 0, BIG_Z0);
    s->room_max = v3(BIG_X1, ROOM_H, HALL_HZ);
    s->env_count = 3; /* a reflection probe each for the office, the hallway and the big room */
    s->envs[0].pos = v3(0.0f, 1.5f, 0.6f);
    s->envs[0].box_min = v3(-ROOM_HX, 0, -ROOM_HZ); s->envs[0].box_max = v3(ROOM_HX, ROOM_H, ROOM_HZ);
    s->envs[1].pos = v3((HALL_X0 + HALL_X1) * 0.5f, 1.5f, 0.0f);
    s->envs[1].box_min = v3(HALL_X0, 0, -HALL_HZ); s->envs[1].box_max = v3(HALL_X1, ROOM_H, HALL_HZ);
    s->envs[2].pos = v3((BIG_X0 + BIG_X1) * 0.5f, 1.5f, (BIG_Z0 + BIG_Z1) * 0.5f);
    s->envs[2].box_min = v3(BIG_X0, 0, BIG_Z0); s->envs[2].box_max = v3(BIG_X1, ROOM_H, BIG_Z1);
    s->portal_count = 2;
    s->portals[0] = (IntPortal){ROOM_HX, HALL_X0, DOOR_Z, DOOR_HW, DOOR_SPRING};
    s->portals[1] = (IntPortal){HALL_X1, BIG_X0, DOOR2_Z, DOOR_HW, DOOR_SPRING};
    /* ---- vertex buffer: filled by office_update ---- */
    glGenVertexArrays(1, &s->vao);
    glBindVertexArray(s->vao);
    glGenBuffers(1, &s->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)(3 * sizeof(float)));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)(6 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    office_reset(o); /* the designed layout: places the objects and fills the buffer */

    o->spawn = v3(0.0f, 0.0f, 2.3f);
    o->spawn_yaw = 0.0f;
    return 1;
}
