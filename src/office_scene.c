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
enum { M_CHAIR, M_DESK, M_BASEBOARD, M_PANEL, M_EMITTER0, M_EMITTER_LAST = M_EMITTER0 + INT_MAX_LIGHTS - 1, M_VENT, M_FLOOR, M_WALL, M_CEILING, M_COUNT };

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
    const IntScene *s = &o->scene;
    const float margin = 0.02f;
    V3 lo, hi;
    placed_bounds(obj, obj->bmin, obj->bmax, &lo, &hi);
    if (lo.x < s->room_min.x + margin) obj->pos.x += s->room_min.x + margin - lo.x;
    if (hi.x > s->room_max.x - margin) obj->pos.x -= hi.x - (s->room_max.x - margin);
    if (lo.z < s->room_min.z + margin) obj->pos.z += s->room_min.z + margin - lo.z;
    if (hi.z > s->room_max.z - margin) obj->pos.z -= hi.z - (s->room_max.z - margin);
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
        box(&vb, v3(ROOM_HX - t, 0, -ROOM_HZ + t), v3(ROOM_HX, h, ROOM_HZ - t), su, h);
    }
    end_room_draw(o, &vb);

    begin_room_draw(o, &vb, M_FLOOR); /* planks run across the room (along x) */
    quad_tiled(&vb, v3(0, 0, 0), v3(ROOM_HX, 0, 0), v3(0, 0, -ROOM_HZ), 4.0f, 1.25f);
    end_room_draw(o, &vb);

    begin_room_draw(o, &vb, M_WALL);
    {
        const float su = 3.0f, sv = 2.0f, hy = ROOM_H * 0.5f;
        quad_tiled(&vb, v3(0, hy, -ROOM_HZ), v3(ROOM_HX, 0, 0), v3(0, hy, 0), su, sv);
        quad_tiled(&vb, v3(0, hy, ROOM_HZ), v3(-ROOM_HX, 0, 0), v3(0, hy, 0), su, sv);
        quad_tiled(&vb, v3(-ROOM_HX, hy, 0), v3(0, 0, -ROOM_HZ), v3(0, hy, 0), su, sv);
        quad_tiled(&vb, v3(ROOM_HX, hy, 0), v3(0, 0, ROOM_HZ), v3(0, hy, 0), su, sv);
    }
    end_room_draw(o, &vb);

    begin_room_draw(o, &vb, M_CEILING); /* one texture repeat = 3 tiles, so the grid lands on the fixtures */
    quad_tiled(&vb, v3(0, ROOM_H, 0), v3(ROOM_HX, 0, 0), v3(0, 0, ROOM_HZ), 3.0f * TILE, 3.0f * TILE);
    end_room_draw(o, &vb);
    o->room = vb.v;
    o->room_count = vb.n;

    s->room_min = v3(-ROOM_HX, 0, -ROOM_HZ);
    s->room_max = v3(ROOM_HX, ROOM_H, ROOM_HZ);
    s->env_pos = v3(0.0f, 1.5f, 0.6f);

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
