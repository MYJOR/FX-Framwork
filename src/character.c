#include "character.h"
#include "terrain.h"
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *CLIP_NAMES[CH_CLIPS] = {"Idle_Loop", "Walk_Loop", "Jog_Fwd_Loop", "Sprint_Loop"};
/* Ground speeds (m/s) at which each clip's feet roughly match the ground. Tune to taste. */
static const float CLIP_SPEED[CH_CLIPS] = {0.0f, 1.9f, 4.5f, 7.5f};

typedef struct { float t[CH_MAX_JOINTS][3], q[CH_MAX_JOINTS][4], s[CH_MAX_JOINTS][3]; } Pose;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---------- loading ---------- */

static int joint_index(const cgltf_skin *skin, const cgltf_node *n) {
    for (cgltf_size i = 0; i < skin->joints_count; i++) if (skin->joints[i] == n) return (int)i;
    return -1;
}

static int joint_by_name(const Character *c, const char *name) {
    if (!name) return -1;
    for (int j = 0; j < c->njoints; j++) if (!strcmp(c->jname[j], name)) return j;
    return -1;
}

static void load_clip_named(Character *c, const cgltf_data *d, ChClip *cl, const char *name) {
    const cgltf_animation *an = NULL;
    for (cgltf_size i = 0; i < d->animations_count; i++)
        if (d->animations[i].name && !strcmp(d->animations[i].name, name)) { an = &d->animations[i]; break; }
    if (!an) { fprintf(stderr, "animation '%s' not found\n", name); return; }

    cl->ja = (ChJointAnim *)calloc((size_t)c->njoints, sizeof(ChJointAnim));
    for (cgltf_size k = 0; k < an->channels_count; k++) {
        const cgltf_animation_channel *ch = &an->channels[k];
        int j = ch->target_node ? joint_by_name(c, ch->target_node->name) : -1;
        if (j < 0) continue;
        int slot, comps;
        switch (ch->target_path) {
        case cgltf_animation_path_type_translation: slot = 0; comps = 3; break;
        case cgltf_animation_path_type_rotation: slot = 1; comps = 4; break;
        case cgltf_animation_path_type_scale: slot = 2; comps = 3; break;
        default: continue;
        }
        const cgltf_animation_sampler *sm = ch->sampler;
        size_t n = sm->input->count;
        ChTrack *tr = &cl->ja[j].tr[slot];
        tr->n = (int)n;
        tr->t = (float *)malloc(n * sizeof(float));
        tr->v = (float *)malloc(n * (size_t)comps * sizeof(float));
        cgltf_accessor_unpack_floats(sm->input, tr->t, n);
        cgltf_accessor_unpack_floats(sm->output, tr->v, n * (size_t)comps);
        if (tr->t[n - 1] > cl->dur) cl->dur = tr->t[n - 1];
    }
    cl->loaded = cl->dur > 0.0f;
}

int character_load_slide(Character *c, const char *path) {
    cgltf_options opt = {0};
    cgltf_data *d = NULL;
    if (cgltf_parse_file(&opt, path, &d) != cgltf_result_success) return 0;
    if (cgltf_load_buffers(&opt, d, path) != cgltf_result_success) { cgltf_free(d); return 0; }
    load_clip_named(c, d, &c->slide_start, "Slide_Start");
    load_clip_named(c, d, &c->slide_loop, "Slide_Loop");
    cgltf_free(d);
    return c->slide_loop.loaded;
}

int character_load(Character *c, const char *path) {
    memset(c, 0, sizeof *c);
    cgltf_options opt = {0};
    cgltf_data *d = NULL;
    if (cgltf_parse_file(&opt, path, &d) != cgltf_result_success) { fprintf(stderr, "cannot read %s\n", path); return 0; }
    if (cgltf_load_buffers(&opt, d, path) != cgltf_result_success) { fprintf(stderr, "cannot load buffers of %s\n", path); cgltf_free(d); return 0; }

    const cgltf_node *mn = NULL;
    for (cgltf_size i = 0; i < d->nodes_count; i++)
        if (d->nodes[i].mesh && d->nodes[i].skin) { mn = &d->nodes[i]; break; }
    if (!mn) { fprintf(stderr, "no skinned mesh in %s\n", path); cgltf_free(d); return 0; }
    const cgltf_skin *skin = mn->skin;
    if (skin->joints_count > CH_MAX_JOINTS) { fprintf(stderr, "too many joints\n"); cgltf_free(d); return 0; }
    c->njoints = (int)skin->joints_count;

    /* skeleton */
    float mesh_world[16];
    cgltf_node_transform_world(mn, mesh_world);
    /* inverse of the mesh node's world transform (rigid or uniform-scale affine) */
    {
        const float *m = mesh_world;
        float s2 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
        float inv[16] = {0};
        for (int r = 0; r < 3; r++) for (int col = 0; col < 3; col++) inv[col * 4 + r] = m[r * 4 + col] / s2;
        for (int r = 0; r < 3; r++)
            inv[12 + r] = -(inv[0 + r] * m[12] + inv[4 + r] * m[13] + inv[8 + r] * m[14]);
        inv[15] = 1;
        memcpy(c->inv_mesh, inv, sizeof inv);
    }
    for (int j = 0; j < c->njoints; j++) {
        const cgltf_node *n = skin->joints[j];
        snprintf(c->jname[j], sizeof c->jname[j], "%s", n->name ? n->name : "");
        c->parent[j] = n->parent ? joint_index(skin, n->parent) : -1;
        if (c->parent[j] < 0 && n->parent) cgltf_node_transform_world(n->parent, c->base[j]);
        else { memset(c->base[j], 0, sizeof c->base[j]); c->base[j][0] = c->base[j][5] = c->base[j][10] = c->base[j][15] = 1; }
        for (int k = 0; k < 3; k++) { c->rest_t[j][k] = n->has_translation ? n->translation[k] : 0.0f; c->rest_s[j][k] = n->has_scale ? n->scale[k] : 1.0f; }
        for (int k = 0; k < 4; k++) c->rest_q[j][k] = n->has_rotation ? n->rotation[k] : (k == 3 ? 1.0f : 0.0f);
    }
    if (skin->inverse_bind_matrices)
        cgltf_accessor_unpack_floats(skin->inverse_bind_matrices, &c->inv_bind[0][0], (cgltf_size)c->njoints * 16);

    /* mesh: pos3 normal3 joints4 weights4, all primitives merged */
    size_t vtotal = 0, itotal = 0;
    for (cgltf_size p = 0; p < mn->mesh->primitives_count; p++) {
        const cgltf_primitive *pr = &mn->mesh->primitives[p];
        for (cgltf_size a = 0; a < pr->attributes_count; a++)
            if (pr->attributes[a].type == cgltf_attribute_type_position) vtotal += pr->attributes[a].data->count;
        if (pr->indices) itotal += pr->indices->count;
    }
    float *verts = (float *)calloc(vtotal * 14, sizeof(float));
    unsigned *idx = (unsigned *)malloc(itotal * sizeof(unsigned));
    size_t vo = 0, io = 0;
    float ymin = 1e9f, ymax = -1e9f;
    for (cgltf_size p = 0; p < mn->mesh->primitives_count; p++) {
        const cgltf_primitive *pr = &mn->mesh->primitives[p];
        if (!pr->indices) continue;
        size_t vc = 0;
        for (cgltf_size a = 0; a < pr->attributes_count; a++) {
            const cgltf_attribute *at = &pr->attributes[a];
            if (at->index != 0) continue;
            vc = at->type == cgltf_attribute_type_position ? at->data->count : vc;
        }
        for (cgltf_size a = 0; a < pr->attributes_count; a++) {
            const cgltf_attribute *at = &pr->attributes[a];
            if (at->index != 0) continue;
            for (size_t i = 0; i < at->data->count; i++) {
                float *v = verts + (vo + i) * 14;
                if (at->type == cgltf_attribute_type_position) {
                    cgltf_accessor_read_float(at->data, i, v, 3);
                    if (v[1] < ymin) ymin = v[1];
                    if (v[1] > ymax) ymax = v[1];
                } else if (at->type == cgltf_attribute_type_normal) cgltf_accessor_read_float(at->data, i, v + 3, 3);
                else if (at->type == cgltf_attribute_type_joints) {
                    cgltf_uint j4[4] = {0};
                    cgltf_accessor_read_uint(at->data, i, j4, 4);
                    for (int k = 0; k < 4; k++) v[6 + k] = (float)j4[k];
                } else if (at->type == cgltf_attribute_type_weights) cgltf_accessor_read_float(at->data, i, v + 10, 4);
            }
        }
        for (cgltf_size i = 0; i < pr->indices->count; i++) idx[io++] = (unsigned)(vo + cgltf_accessor_read_index(pr->indices, i));
        vo += vc;
    }
    c->height = ymax - ymin;
    c->index_count = (GLsizei)io;

    glGenVertexArrays(1, &c->vao);
    glBindVertexArray(c->vao);
    glGenBuffers(1, &c->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vo * 14 * sizeof(float)), verts, GL_STATIC_DRAW);
    glGenBuffers(1, &c->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(io * sizeof(unsigned)), idx, GL_STATIC_DRAW);
    const GLsizei stride = 14 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void *)(3 * sizeof(float)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void *)(6 * sizeof(float)));
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void *)(10 * sizeof(float)));
    for (int a = 0; a < 4; a++) glEnableVertexAttribArray((GLuint)a);
    free(verts); free(idx);

    for (int i = 0; i < CH_CLIPS; i++) load_clip_named(c, d, &c->clip[i], CLIP_NAMES[i]);
    c->spring_on = 1;
    c->look_j[0] = joint_by_name(c, "spine_03");
    c->look_j[1] = joint_by_name(c, "neck_01");
    c->look_j[2] = joint_by_name(c, "Head");
    load_clip_named(c, d, &c->jump_start, "Jump_Start");
    load_clip_named(c, d, &c->jump_loop, "Jump_Loop");
    load_clip_named(c, d, &c->jump_land, "Jump_Land");
    { static const char *base[4] = {"thigh_%c", "calf_%c", "foot_%c", "ball_%c"}; for (int l = 0; l < 2; l++) for (int q = 0; q < 4; q++) { char nm[32]; snprintf(nm, sizeof nm, base[q], l ? 'r' : 'l'); c->ik_j[l][q] = joint_by_name(c, nm); } }
    printf("character: %d joints, %zu verts, %zu tris, height %.2f, clips:", c->njoints, vo, io / 3, c->height);
    for (int i = 0; i < CH_CLIPS; i++) printf(" %s=%.2fs", CLIP_NAMES[i], c->clip[i].dur);
    printf("\n");
    cgltf_free(d);
    character_update(c, 0.0f, 0.0f, 0);
    return c->clip[CH_IDLE].loaded;
}

/* ---------- sampling and blending ---------- */

static void track_eval(const ChTrack *k, float t, int comps, float *out) {
    if (k->n <= 1 || t <= k->t[0]) { memcpy(out, k->v, (size_t)comps * sizeof(float)); return; }
    if (t >= k->t[k->n - 1]) { memcpy(out, k->v + (size_t)(k->n - 1) * comps, (size_t)comps * sizeof(float)); return; }
    int lo = 0, hi = k->n - 1;
    while (hi - lo > 1) { int mid = (lo + hi) / 2; if (k->t[mid] <= t) lo = mid; else hi = mid; }
    float f = (t - k->t[lo]) / (k->t[hi] - k->t[lo]);
    const float *a = k->v + (size_t)lo * comps, *b = k->v + (size_t)hi * comps;
    float sgn = 1.0f;
    if (comps == 4 && a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0) sgn = -1.0f;
    for (int i = 0; i < comps; i++) out[i] = a[i] + (sgn * b[i] - a[i]) * f;
    if (comps == 4) {
        float l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
        if (l > 1e-8f) for (int i = 0; i < 4; i++) out[i] /= l;
    }
}

static void pose_at(const Character *c, const ChClip *cl, float t, Pose *p);
static void pose_from_clip(const Character *c, const ChClip *cl, float t, Pose *p) {
    if (cl->dur > 0) t = fmodf(t, cl->dur);
    pose_at(c, cl, t, p);
}

/* sample without wrapping: t is clamped to the clip */
static void pose_at(const Character *c, const ChClip *cl, float t, Pose *p) {
    for (int j = 0; j < c->njoints; j++) {
        memcpy(p->t[j], c->rest_t[j], sizeof p->t[j]);
        memcpy(p->q[j], c->rest_q[j], sizeof p->q[j]);
        memcpy(p->s[j], c->rest_s[j], sizeof p->s[j]);
        const ChJointAnim *ja = &cl->ja[j];
        if (ja->tr[0].n) track_eval(&ja->tr[0], t, 3, p->t[j]);
        if (ja->tr[1].n) track_eval(&ja->tr[1], t, 4, p->q[j]);
        if (ja->tr[2].n) track_eval(&ja->tr[2], t, 3, p->s[j]);
    }
}

static void pose_blend(const Character *c, Pose *a, const Pose *b, float f) {
    for (int j = 0; j < c->njoints; j++) {
        for (int k = 0; k < 3; k++) {
            a->t[j][k] += (b->t[j][k] - a->t[j][k]) * f;
            a->s[j][k] += (b->s[j][k] - a->s[j][k]) * f;
        }
        float sgn = (a->q[j][0] * b->q[j][0] + a->q[j][1] * b->q[j][1] + a->q[j][2] * b->q[j][2] + a->q[j][3] * b->q[j][3]) < 0 ? -1.0f : 1.0f;
        float l = 0;
        for (int k = 0; k < 4; k++) { a->q[j][k] += (sgn * b->q[j][k] - a->q[j][k]) * f; l += a->q[j][k] * a->q[j][k]; }
        l = sqrtf(l);
        if (l > 1e-8f) for (int k = 0; k < 4; k++) a->q[j][k] /= l;
    }
}

static void compose(const float t[3], const float q[4], const float s[3], float m[16]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = (1 - 2 * (y * y + z * z)) * s[0]; m[1] = 2 * (x * y + z * w) * s[0]; m[2] = 2 * (x * z - y * w) * s[0]; m[3] = 0;
    m[4] = 2 * (x * y - z * w) * s[1]; m[5] = (1 - 2 * (x * x + z * z)) * s[1]; m[6] = 2 * (y * z + x * w) * s[1]; m[7] = 0;
    m[8] = 2 * (x * z + y * w) * s[2]; m[9] = 2 * (y * z - x * w) * s[2]; m[10] = (1 - 2 * (x * x + y * y)) * s[2]; m[11] = 0;
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
}

static void mul4(const float *a, const float *b, float *r) {
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
}

/* ---------- foot IK ---------- */

static V3 gpos(float G[][16], int j) { return v3(G[j][12], G[j][13], G[j][14]); }
static V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float vlen(V3 a) { return sqrtf(v3_dot(a, a)); }

void character_set_look(Character *c, float x, float y, float z, int on) {
    c->look_t[0] = x; c->look_t[1] = y; c->look_t[2] = z; c->look_on = on;
}
void character_set_air(Character *c, int air) { c->air = air; }
void character_set_ik(Character *c, M4 model, int on) { c->ik_model = model; c->ik_on = on; }

/* rotation matrix (row-major 3x3) taking unit vector a onto unit vector b */
static void rot_from_to(V3 a, V3 b, float R[9]) {
    V3 v = v3_cross(a, b);
    float cs = v3_dot(a, b);
    if (cs < -0.9999f) { /* opposite: half turn about any perpendicular axis */
        V3 ax = fabsf(a.x) < 0.9f ? v3(1, 0, 0) : v3(0, 1, 0);
        V3 u = v3_norm(v3_cross(a, ax));
        R[0] = 2 * u.x * u.x - 1; R[1] = 2 * u.x * u.y;     R[2] = 2 * u.x * u.z;
        R[3] = 2 * u.x * u.y;     R[4] = 2 * u.y * u.y - 1; R[5] = 2 * u.y * u.z;
        R[6] = 2 * u.x * u.z;     R[7] = 2 * u.y * u.z;     R[8] = 2 * u.z * u.z - 1;
        return;
    }
    float k = 1.0f / (1.0f + cs);
    R[0] = cs + v.x * v.x * k;        R[1] = v.x * v.y * k - v.z;  R[2] = v.x * v.z * k + v.y;
    R[3] = v.x * v.y * k + v.z;       R[4] = cs + v.y * v.y * k;   R[5] = v.y * v.z * k - v.x;
    R[6] = v.x * v.z * k - v.y;       R[7] = v.y * v.z * k + v.x;  R[8] = cs + v.z * v.z * k;
}

static V3 rmul(const float R[9], V3 v) {
    return v3(R[0] * v.x + R[1] * v.y + R[2] * v.z, R[3] * v.x + R[4] * v.y + R[5] * v.z, R[6] * v.x + R[7] * v.y + R[8] * v.z);
}

/* rotate joint `root` and all its descendants about pivot P */
static void rotate_subtree(const Character *c, float G[][16], int root, V3 P, const float R[9]) {
    int mark[CH_MAX_JOINTS] = {0};
    mark[root] = 1;
    for (int pass = 0; pass < c->njoints; pass++) {
        int changed = 0;
        for (int j = 0; j < c->njoints; j++)
            if (!mark[j] && c->parent[j] >= 0 && mark[c->parent[j]]) { mark[j] = 1; changed = 1; }
        if (!changed) break;
    }
    for (int j = 0; j < c->njoints; j++) {
        if (!mark[j]) continue;
        for (int col = 0; col < 3; col++) {
            V3 r = rmul(R, v3(G[j][col * 4], G[j][col * 4 + 1], G[j][col * 4 + 2]));
            G[j][col * 4] = r.x; G[j][col * 4 + 1] = r.y; G[j][col * 4 + 2] = r.z;
        }
        V3 t = rmul(R, vsub(gpos(G, j), P));
        G[j][12] = t.x + P.x; G[j][13] = t.y + P.y; G[j][14] = t.z + P.z;
    }
}

/* two-bone solve: put foot at T, keeping the knee bending the way the animation bent it */
static void solve_leg(const Character *c, float G[][16], int th, int ca, int ft, V3 T) {
    V3 H = gpos(G, th), K = gpos(G, ca), F = gpos(G, ft);
    float l1 = vlen(vsub(K, H)), l2 = vlen(vsub(F, K));
    V3 to = vsub(T, H);
    float d = vlen(to);
    if (d < 1e-5f || l1 < 1e-5f || l2 < 1e-5f) return;
    V3 dir = v3_scale(to, 1.0f / d);
    /* soft reach limit: ease into full extension instead of snapping the knee straight */
    float L = (l1 + l2) * 0.995f, r0 = L * 0.9f, dc = d;
    if (d > r0) dc = r0 + (L - r0) * (1.0f - expf(-(d - r0) / (L - r0)));
    if (dc < fabsf(l1 - l2) + 1e-4f) dc = fabsf(l1 - l2) + 1e-4f;
    V3 kv = vsub(K, H);
    V3 pole = vsub(kv, v3_scale(dir, v3_dot(kv, dir)));
    if (vlen(pole) < 1e-5f) pole = v3(0, 0, 1);
    pole = v3_norm(pole);
    float a = (l1 * l1 - l2 * l2 + dc * dc) / (2.0f * dc);
    float h2 = l1 * l1 - a * a;
    float h = h2 > 0 ? sqrtf(h2) : 0;
    V3 K2 = v3_add(H, v3_add(v3_scale(dir, a), v3_scale(pole, h)));
    float R1[9];
    rot_from_to(v3_norm(kv), v3_norm(vsub(K2, H)), R1);
    rotate_subtree(c, G, th, H, R1);
    V3 F1 = gpos(G, ft);
    V3 T2 = v3_add(H, v3_scale(dir, dc));
    float R2[9];
    rot_from_to(v3_norm(vsub(F1, K2)), v3_norm(vsub(T2, K2)), R2);
    rotate_subtree(c, G, ca, K2, R2);
}

static float smooth01(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* Gm: joint matrices in mesh space. Plants both feet on the terrain and lowers the body to reach the lower one. */
static void foot_ik(Character *c, float dt, float Gm[][16]) {
    if (!c->ik_on || c->ik_j[0][2] < 0 || c->ik_j[1][2] < 0) return;
    const float *M = c->ik_model.m;
    float s2 = M[0] * M[0] + M[1] * M[1] + M[2] * M[2];
    if (s2 < 1e-8f) return;
    V3 t0 = v3(M[12], M[13], M[14]);
    /* world <-> mesh space (the model matrix is rotation * uniform scale) */
#define TO_WORLD(p) v3(M[0]*(p).x + M[4]*(p).y + M[8]*(p).z + t0.x, M[1]*(p).x + M[5]*(p).y + M[9]*(p).z + t0.y, M[2]*(p).x + M[6]*(p).y + M[10]*(p).z + t0.z)
#define DIR_TO_MESH(v) v3((M[0]*(v).x + M[1]*(v).y + M[2]*(v).z) / s2, (M[4]*(v).x + M[5]*(v).y + M[6]*(v).z) / s2, (M[8]*(v).x + M[9]*(v).y + M[10]*(v).z) / s2)

    float k = (1.0f - c->slide_w) * (1.0f - c->air_w); /* no IK while sliding or jumping: those poses own the legs */
    float dsm[2], hanim[2];
    V3 fw[2];
    for (int l = 0; l < 2; l++) {
        V3 fm = gpos(Gm, c->ik_j[l][2]);
        fw[l] = TO_WORLD(fm);
        float gh = terrain_height(fw[l].x, fw[l].z);
        float want = clampf(gh - t0.y, -0.6f, 0.6f);
        c->ik_d[l] += (want - c->ik_d[l]) * clampf(dt * 30.0f, 0.0f, 1.0f);
        dsm[l] = c->ik_d[l] * k;
        hanim[l] = fw[l].y - t0.y;
    }
    /* Only planted feet drive the body height: a swinging foot reaches ahead over higher ground when
       climbing, and letting it steer the hips made them pump with every step. */
    float cw[2], csum = 0, cacc = 0;
    for (int l = 0; l < 2; l++) {
        cw[l] = 1.0f - smooth01(0.08f, 0.28f, hanim[l]);
        csum += cw[l]; cacc += cw[l] * dsm[l];
    }
    float tgt = c->ik_off;
    if (csum > 0.05f) tgt = clampf(cacc / csum, -0.5f, 0.25f);
    c->ik_off += (tgt - c->ik_off) * clampf(dt * 6.0f, 0.0f, 1.0f);
    float off = c->ik_off;
    /* swinging feet follow the body offset (smoothly), planted feet follow the ground exactly */
    for (int l = 0; l < 2; l++) dsm[l] = off + (dsm[l] - off) * cw[l];
    V3 offm = DIR_TO_MESH(v3(0, off, 0));
    for (int j = 0; j < c->njoints; j++) { Gm[j][12] += offm.x; Gm[j][13] += offm.y; Gm[j][14] += offm.z; }

    for (int l = 0; l < 2; l++) {
        const int *J = c->ik_j[l];
        if (J[0] < 0 || J[1] < 0) continue;
        V3 tw = v3(fw[l].x, fw[l].y + dsm[l], fw[l].z);
        V3 Tm = v3_add(DIR_TO_MESH(vsub(tw, t0)), v3(0, 0, 0));
        /* DIR_TO_MESH gives mesh-space position of a world point relative to t0 */
        solve_leg(c, Gm, J[0], J[1], J[2], Tm);

        /* tilt the foot toward the ground normal while it's planted */
        V3 n = terrain_normal(fw[l].x, fw[l].z);
        V3 nm = v3_norm(DIR_TO_MESH(n));
        float w = (1.0f - smooth01(0.12f, 0.30f, hanim[l])) * k;
        V3 up = v3(0, 1, 0);
        V3 tgt = v3_norm(v3_add(up, v3_scale(vsub(nm, up), w)));
        /* low-pass the tilt so ankles don't flick as the foot swings over bumpy ground */
        float *st = c->ik_tilt[l];
        if (st[0] == 0 && st[1] == 0 && st[2] == 0) { st[0] = tgt.x; st[1] = tgt.y; st[2] = tgt.z; }
        float ka = clampf(dt * 10.0f, 0.0f, 1.0f);
        st[0] += (tgt.x - st[0]) * ka; st[1] += (tgt.y - st[1]) * ka; st[2] += (tgt.z - st[2]) * ka;
        tgt = v3_norm(v3(st[0], st[1], st[2]));
        float R[9];
        rot_from_to(up, tgt, R);
        rotate_subtree(c, Gm, J[2], gpos(Gm, J[2]), R);
    }
#undef TO_WORLD
#undef DIR_TO_MESH
}

static void fk_globals(const Character *c, const Pose *p, float G[][16]) {
    int done[CH_MAX_JOINTS] = {0};
    /* parents are not guaranteed to precede children: iterate until every joint is placed */
    for (int pass = 0, left = c->njoints; left > 0 && pass < c->njoints; pass++) {
        for (int j = 0; j < c->njoints; j++) {
            if (done[j]) continue;
            int par = c->parent[j];
            if (par >= 0 && !done[par]) continue;
            float local[16];
            compose(p->t[j], p->q[j], p->s[j], local);
            mul4(par >= 0 ? G[par] : c->base[j], local, G[j]);
            done[j] = 1; left--;
        }
    }
}

/* Head tracking: turn spine_03 / neck / Head toward the look target, clamped to a believable range and
   faded out when the target is behind the character. Works in mesh space on the joint matrices. */
static void head_look(Character *c, float dt, float Gm[][16]) {
    const int *J = c->look_j;
    if (J[0] < 0 || J[1] < 0 || J[2] < 0) return;
    const float *M = c->ik_model.m;
    float s2 = M[0] * M[0] + M[1] * M[1] + M[2] * M[2];
    if (s2 < 1e-8f) return;
    float want_yaw = 0, want_pitch = 0;
    if (c->look_on) {
        V3 d = v3(c->look_t[0] - M[12], c->look_t[1] - M[13], c->look_t[2] - M[14]);
        V3 pm = v3((M[0] * d.x + M[1] * d.y + M[2] * d.z) / s2, (M[4] * d.x + M[5] * d.y + M[6] * d.z) / s2,
                   (M[8] * d.x + M[9] * d.y + M[10] * d.z) / s2); /* target in mesh space */
        V3 hp = gpos(Gm, J[2]);
        V3 v = vsub(pm, hp);
        float hl = sqrtf(v.x * v.x + v.z * v.z);
        if (hl > 1e-4f || fabsf(v.y) > 1e-4f) {
            float yaw = atan2f(v.x, v.z), pitch = atan2f(v.y, hl);
            float behind = smooth01(1.9f, 2.6f, fabsf(yaw));       /* >~110 deg: look straight ahead again */
            want_yaw = clampf(yaw, -1.15f, 1.15f) * (1.0f - behind);
            want_pitch = clampf(pitch, -0.6f, 0.7f) * (1.0f - behind);
        }
    }
    float k = clampf(dt * 8.0f, 0.0f, 1.0f);
    c->look_yaw += (want_yaw - c->look_yaw) * k;
    c->look_pitch += (want_pitch - c->look_pitch) * k;
    static const float share[3] = {0.20f, 0.35f, 0.45f};
    for (int i = 0; i < 3; i++) {
        float y = c->look_yaw * share[i], p = c->look_pitch * share[i];
        float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p);
        /* R = Ry(y) * Rx(-p): yaw about up, then pitch the nose up/down */
        float R[9] = {cy, -sy * sp, sy * cp,
                      0, cp, sp,
                      -sy, -cy * sp, cy * cp};
        rotate_subtree(c, Gm, J[i], gpos(Gm, J[i]), R);
    }
}

static void build_bones(Character *c, const Pose *p, float dt) {
    float G[CH_MAX_JOINTS][16];
    fk_globals(c, p, G);
    float Gm[CH_MAX_JOINTS][16];
    for (int j = 0; j < c->njoints; j++) mul4(c->inv_mesh, G[j], Gm[j]);
    foot_ik(c, dt, Gm);
    head_look(c, dt, Gm);
    for (int j = 0; j < c->njoints; j++) mul4(Gm[j], c->inv_bind[j], c->bones[j].m);
}

/* ---------- motion matching ----------
 * A database of locomotion clips (with root motion, from the *_RM files) is sampled at 30 Hz. Every
 * frame gets a feature vector: where the root will be 0.2/0.4/0.6 s later and which way it will face,
 * current velocity, and foot positions/velocities. Each 0.1 s the controller's predicted trajectory is
 * matched against all frames; playback jumps to the best one and the pose difference is inertialised
 * away. Root motion is stripped from the pose: the player capsule owns the world position. */

#define MM_MAX_CLIPS 40
#define MM_HZ 30.0f
#define MM_DIM 23
#define MM_PI 3.14159265f
#define MM_TURN_SPEEDUP 2.6f /* turn clips play this much faster, and rotate the body this much faster */

typedef struct {
    ChClip clip;
    int loop, N, first;
    float dur;
    float *rx, *rz, *ry;          /* root x, z, unwrapped yaw at N+1 samples */
    float dx, dz, dy;             /* displacement over one loop */
    float (*fp)[6];               /* foot positions (stripped pose, mesh space) at N+1 samples */
    char name[40];
} MMClip;

typedef struct MotionDB {
    MMClip clips[MM_MAX_CLIPS];
    int nclips, nframes, root;
    float *feat;                  /* nframes * MM_DIM, already weighted */
    float *fspeed;                /* root speed per frame, file units / s */
    float wscale[MM_DIM];
    /* playback */
    int cur_clip; float cur_t, timer;
    Pose out; int have_out;
    float off_t[CH_MAX_JOINTS][3], off_q[CH_MAX_JOINTS][4];
    /* controller input */
    float vx, vz, dvx, dvz, yaw;
    float sc;                     /* metres per file unit */
    int last_clip, last_frame;    /* diagnostics */
    float last_dvx, last_dvz, react;
    int excl_clip; float excl_timer; /* a one-shot clip we just left can't be re-entered immediately */
} MotionDB;

static float wrap_pi(float a) { while (a > MM_PI) a -= 2 * MM_PI; while (a < -MM_PI) a += 2 * MM_PI; return a; }

static void quat_mul(const float *a, const float *b, float *r) {
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/* remove the root's planar motion: the capsule carries the character instead */
static void strip_root(const Character *c, int root, Pose *p) {
    memcpy(p->t[root], c->rest_t[root], sizeof p->t[root]);
    memcpy(p->q[root], c->rest_q[root], sizeof p->q[root]);
}

static void mm_sample(const MMClip *m, int k, float *x, float *z, float *yw) {
    int q = 0, r = k;
    if (m->loop) { q = k >= 0 ? k / m->N : -((-k + m->N - 1) / m->N); r = k - q * m->N; }
    else r = k < 0 ? 0 : (k > m->N ? m->N : k);
    *x = m->rx[r] + (float)q * m->dx; *z = m->rz[r] + (float)q * m->dz; *yw = m->ry[r] + (float)q * m->dy;
}

static int mm_add_clip(Character *c, MotionDB *db, const cgltf_data *d, const char *name) {
    if (db->nclips >= MM_MAX_CLIPS) return 0;
    MMClip *m = &db->clips[db->nclips];
    memset(m, 0, sizeof *m);
    load_clip_named(c, d, &m->clip, name);
    if (!m->clip.loaded) return 0;
    snprintf(m->name, sizeof m->name, "%s", name);
    m->loop = strstr(name, "_Loop") != NULL;
    m->dur = m->clip.dur;
    m->N = (int)(m->dur * MM_HZ + 0.5f);
    if (m->N < 4) { free(m->clip.ja); memset(m, 0, sizeof *m); return 0; }
    m->rx = (float *)malloc((size_t)(m->N + 1) * sizeof(float));
    m->rz = (float *)malloc((size_t)(m->N + 1) * sizeof(float));
    m->ry = (float *)malloc((size_t)(m->N + 1) * sizeof(float));
    m->fp = (float (*)[6])malloc((size_t)(m->N + 1) * sizeof *m->fp);
    float prev = 0;
    for (int i = 0; i <= m->N; i++) {
        float t = i == m->N ? m->dur : (float)i / MM_HZ;
        Pose p;
        pose_at(c, &m->clip, t, &p);
        float *q = p.q[db->root], *rq = c->rest_q[db->root];
        float conj[4] = {-rq[0], -rq[1], -rq[2], rq[3]}, D[4];
        quat_mul(q, conj, D);
        float raw = 2.0f * atan2f(D[1], D[3]);
        if (i == 0) prev = wrap_pi(raw); else prev += wrap_pi(raw - prev);
        m->rx[i] = p.t[db->root][0] - c->rest_t[db->root][0];
        m->rz[i] = p.t[db->root][2] - c->rest_t[db->root][2];
        m->ry[i] = prev;
        strip_root(c, db->root, &p);
        float G[CH_MAX_JOINTS][16];
        fk_globals(c, &p, G);
        for (int l = 0; l < 2; l++) {
            int fj = c->ik_j[l][2];
            float tmp[16];
            if (fj < 0) { memset(m->fp[i] + l * 3, 0, 3 * sizeof(float)); continue; }
            mul4(c->inv_mesh, G[fj], tmp);
            m->fp[i][l * 3] = tmp[12]; m->fp[i][l * 3 + 1] = tmp[13]; m->fp[i][l * 3 + 2] = tmp[14];
        }
    }
    m->dx = m->rx[m->N] - m->rx[0]; m->dz = m->rz[m->N] - m->rz[0]; m->dy = m->ry[m->N] - m->ry[0];
    db->nclips++;
    return 1;
}

static void mm_foot_at(const MMClip *m, int k, float *out) {
    if (m->loop) { k %= m->N; if (k < 0) k += m->N; } else k = k < 0 ? 0 : (k > m->N ? m->N : k);
    memcpy(out, m->fp[k], 6 * sizeof(float));
}

static void mm_build_features(MotionDB *db) {
    int total = 0;
    for (int i = 0; i < db->nclips; i++) { db->clips[i].first = total; total += db->clips[i].N; }
    db->nframes = total;
    db->feat = (float *)calloc((size_t)total * MM_DIM, sizeof(float));
    db->fspeed = (float *)calloc((size_t)total, sizeof(float));
    const int H[3] = {6, 12, 18};
    for (int ci = 0; ci < db->nclips; ci++) {
        const MMClip *m = &db->clips[ci];
        for (int i = 0; i < m->N; i++) {
            float *f = db->feat + (size_t)(m->first + i) * MM_DIM;
            float x0, z0, y0;
            mm_sample(m, i, &x0, &z0, &y0);
            float cs = cosf(y0), sn = sinf(y0);
            for (int h = 0; h < 3; h++) {
                float x, z, y;
                mm_sample(m, i + H[h], &x, &z, &y);
                float dx = x - x0, dz = z - z0;
                f[h * 2] = dx * cs - dz * sn;
                f[h * 2 + 1] = dx * sn + dz * cs;
                f[6 + h] = y - y0;
            }
            float xa, za, ya, xb, zb, yb;
            mm_sample(m, i + 3, &xa, &za, &ya);
            mm_sample(m, i - 3, &xb, &zb, &yb);
            float vx = (xa - xb) / 0.2f, vz = (za - zb) / 0.2f;
            f[9] = vx * cs - vz * sn; f[10] = vx * sn + vz * cs;
            db->fspeed[m->first + i] = sqrtf(vx * vx + vz * vz);
            float fa[6], fb[6];
            mm_foot_at(m, i + 1, fa); mm_foot_at(m, i - 1, fb);
            for (int k = 0; k < 6; k++) { f[11 + k] = m->fp[i][k]; f[17 + k] = (fa[k] - fb[k]) * (MM_HZ / 2.0f); }
        }
    }
    /* weight groups so each contributes comparably: weight / pooled std */
    static const int g0[5] = {0, 6, 9, 11, 17}, g1[5] = {6, 9, 11, 17, 23};
    static const float gw[5] = {1.0f, 0.6f, 1.2f, 0.5f, 0.3f};
    for (int g = 0; g < 5; g++) {
        double var = 0;
        for (int d = g0[g]; d < g1[g]; d++) {
            double mean = 0, v = 0;
            for (int i = 0; i < total; i++) mean += db->feat[(size_t)i * MM_DIM + d];
            mean /= total;
            for (int i = 0; i < total; i++) { double e = db->feat[(size_t)i * MM_DIM + d] - mean; v += e * e; }
            var += v / total;
        }
        float sd = (float)sqrt(var / (g1[g] - g0[g]));
        if (sd < 1e-3f) sd = 1e-3f;
        for (int d = g0[g]; d < g1[g]; d++) db->wscale[d] = gw[g] / sd;
    }
    for (int i = 0; i < total; i++)
        for (int d = 0; d < MM_DIM; d++) db->feat[(size_t)i * MM_DIM + d] *= db->wscale[d];
}

static void mm_load_file(Character *c, MotionDB *db, const char *path, const char *const *names) {
    cgltf_options opt = {0};
    cgltf_data *d = NULL;
    if (cgltf_parse_file(&opt, path, &d) != cgltf_result_success) { fprintf(stderr, "motion matching: cannot read %s\n", path); return; }
    if (cgltf_load_buffers(&opt, d, path) != cgltf_result_success) { cgltf_free(d); return; }
    for (int i = 0; names[i]; i++) mm_add_clip(c, db, d, names[i]);
    cgltf_free(d);
}

int character_load_motion(Character *c, const char *rm1, const char *rm2) {
    static const char *const n1[] = {"Idle_Loop", "Walk_Loop", "Jog_Fwd_Loop", "Jog_Fwd_L_Loop", "Jog_Fwd_R_Loop",
        "Jog_Bwd_Loop", "Jog_Bwd_L_Loop", "Jog_Bwd_R_Loop", "Jog_Left_Loop",
        "Jog_Right_Loop", "Sprint_Loop", "Turn90_L", "Turn90_R", NULL};
    static const char *const n2[] = {"Walk_Fwd_L_Loop", "Walk_Fwd_R_Loop", "Walk_Bwd_Loop", "Walk_Bwd_L_Loop", "Walk_Bwd_R_Loop",
        "Walk_L_Loop", "Walk_R_Loop", "Turn180_L", "Turn180_R", NULL};
    int root = joint_by_name(c, "root");
    if (root < 0 || c->ik_j[0][2] < 0) return 0;
    MotionDB *db = (MotionDB *)calloc(1, sizeof *db);
    db->root = root;
    db->sc = c->height > 0.1f ? 1.75f / c->height : 1.0f;
    mm_load_file(c, db, rm1, n1);
    mm_load_file(c, db, rm2, n2);
    if (db->nclips < 4) { free(db); return 0; }
    mm_build_features(db);
    int idle = 0;
    for (int i = 0; i < db->nclips; i++) if (!strcmp(db->clips[i].name, "Idle_Loop")) idle = i;
    db->cur_clip = idle;
    c->mdb = db;
    printf("motion matching: %d clips, %d frames\n", db->nclips, db->nframes);
    return 1;
}

/* While a Turn clip plays, its root yaw rate (rad/s, in capsule-yaw convention) should rotate the capsule. */
int character_mm_turn(const Character *c, float *yaw_rate) {
    if (!c->mdb) return 0;
    const MotionDB *db = c->mdb;
    const MMClip *m = &db->clips[db->cur_clip];
    if (m->loop || strncmp(m->name, "Turn", 4) != 0) return 0;
    int i = (int)(db->cur_t * MM_HZ);
    if (i < 1) i = 1;
    if (i > m->N - 1) i = m->N - 1;
    *yaw_rate = -(m->ry[i + 1] - m->ry[i - 1]) * (MM_HZ / 2.0f) * MM_TURN_SPEEDUP;
    return 1;
}

const char *character_mm_clip(const Character *c, float *t) {
    if (!c->mdb) return NULL;
    if (t) *t = c->mdb->cur_t;
    return c->mdb->clips[c->mdb->cur_clip].name;
}

void character_set_motion(Character *c, float vx, float vz, float dvx, float dvz, float yaw) {
    if (!c->mdb) return;
    c->mdb->vx = vx; c->mdb->vz = vz; c->mdb->dvx = dvx; c->mdb->dvz = dvz; c->mdb->yaw = yaw;
}

/* the controller's predicted trajectory as a feature vector (pose part left zero) */
static void mm_query(const MotionDB *db, float q[MM_DIM]) {
    const float T = 0.16f;                       /* velocity response time of the controller */
    const float taus[3] = {0.2f, 0.4f, 0.6f};
    float th = MM_PI - db->yaw;                  /* world heading of the character's +Z axis */
    float cs = cosf(th), sn = sinf(th);
    float dsp = sqrtf(db->dvx * db->dvx + db->dvz * db->dvz);
    float th_des = dsp > 0.4f ? atan2f(db->dvx, db->dvz) : th;
    /* the capsule yaw uses atan2(vx, -vz) = pi - theta; convert desired heading the same way */
    if (dsp > 0.4f) th_des = MM_PI - atan2f(db->dvx, -db->dvz);
    float inv = 1.0f / db->sc;
    memset(q, 0, MM_DIM * sizeof(float));
    for (int h = 0; h < 3; h++) {
        float t = taus[h], e = 1.0f - expf(-t / T);
        float px = db->dvx * t + (db->vx - db->dvx) * T * e;
        float pz = db->dvz * t + (db->vz - db->dvz) * T * e;
        q[h * 2] = (px * cs - pz * sn) * inv;
        q[h * 2 + 1] = (px * sn + pz * cs) * inv;
        q[6 + h] = wrap_pi(th_des - th) * (1.0f - expf(-t * 3.0f));
    }
    q[9] = (db->vx * cs - db->vz * sn) * inv;
    q[10] = (db->vx * sn + db->vz * cs) * inv;
}

static int mm_frame_of(const MotionDB *db, int clip, float t) {
    const MMClip *m = &db->clips[clip];
    int f = (int)(t * MM_HZ + 0.5f);
    if (f < 0) f = 0;
    if (f > m->N - 1) f = m->N - 1;
    return m->first + f;
}

static float mm_cost(const MotionDB *db, int frame, const float *q, int dims) {
    const float *f = db->feat + (size_t)frame * MM_DIM;
    float s = 0;
    for (int d = 0; d < dims; d++) { float e = f[d] - q[d]; s += e * e; }
    return s;
}

static void mm_sample_pose(const Character *c, const MotionDB *db, int clip, float t, Pose *p) {
    const MMClip *m = &db->clips[clip];
    pose_at(c, &m->clip, t < m->dur ? t : m->dur, p);
    strip_root(c, db->root, p);
}

/* returns 1 and fills *out with the (root-stripped) pose */
static int mm_update(Character *c, float dt, Pose *out) {
    MotionDB *db = c->mdb;
    MMClip *cur = &db->clips[db->cur_clip];
    int force = 0;

    /* advance, nudging the rate toward the real ground speed so feet don't skate */
    int cf = mm_frame_of(db, db->cur_clip, db->cur_t);
    float cs_fu = sqrtf(db->vx * db->vx + db->vz * db->vz) / db->sc;
    float rate = 1.0f;
    if (db->fspeed[cf] > 0.5f && cs_fu > 0.5f) rate = clampf(cs_fu / db->fspeed[cf], 0.85f, 1.15f);
    if (!cur->loop && !strncmp(cur->name, "Turn", 4)) rate = MM_TURN_SPEEDUP; /* turns are snappy */
    db->cur_t += dt * rate;
    if (cur->loop) { if (db->cur_t >= cur->dur) db->cur_t = fmodf(db->cur_t, cur->dur); }
    else if (db->cur_t >= cur->dur - 1.0f / MM_HZ) { db->cur_t = cur->dur - 1.0f / MM_HZ; force = 1; }

    db->timer -= dt;
    if (db->excl_timer > 0) db->excl_timer -= dt;
    /* a change of input (stop, start, turn) is searched for right away instead of at the next tick,
       and for a short while the current clip is easier to leave */
    {
        float ddx = db->dvx - db->last_dvx, ddz = db->dvz - db->last_dvz;
        if (ddx * ddx + ddz * ddz > 1.0f) { db->timer = 0; db->react = 0.35f; }
        db->last_dvx = db->dvx; db->last_dvz = db->dvz;
        if (db->react > 0) db->react -= dt;
    }
    if (db->timer <= 0.0f || force || !db->have_out) {
        db->timer = 0.05f;
        float q[MM_DIM];
        mm_query(db, q);
        for (int d = 0; d < 11; d++) q[d] *= db->wscale[d];
        cf = mm_frame_of(db, db->cur_clip, db->cur_t);
        for (int d = 11; d < MM_DIM; d++) q[d] = db->feat[(size_t)cf * MM_DIM + d]; /* pose part: keep what we're doing */
        /* standing still and asking for nothing: match on trajectory alone, otherwise the foot features of the
           clip we are already playing (e.g. a walk) keep it looking like the best match forever */
        float dsp = sqrtf(db->dvx * db->dvx + db->dvz * db->dvz);
        int dims = (cs_fu * db->sc < 0.3f && dsp < 0.1f) ? 11 : MM_DIM;
        float cur_cost = mm_cost(db, cf, q, dims), best = 1e30f;
        int bf = cf, bc = db->cur_clip;
        for (int ci = 0; ci < db->nclips; ci++) {
            const MMClip *m = &db->clips[ci];
            for (int i = 0; i < m->N; i++) {
                /* non-looping clips need room to play; don't start in their last moments */
                if (!m->loop && i > m->N - 6) continue;
                if (ci == db->excl_clip && db->excl_timer > 0) continue;
                float cost = mm_cost(db, m->first + i, q, dims);
                if (cost < best) { best = cost; bf = m->first + i; bc = ci; }
            }
        }
        float ratio = cur->loop ? 0.85f : 0.98f; /* one-shots give way more easily to a loop that fits */
        if (db->react > 0) ratio = 0.6f;         /* input just changed: commit to the new motion quickly */
        int better = force || !db->have_out || best < cur_cost * ratio - 0.02f;
        if (better && bf != cf) {
            const MMClip *m = &db->clips[bc];
            float nt = (float)(bf - m->first) / MM_HZ;
            Pose np;
            mm_sample_pose(c, db, bc, nt, &np);
            if (db->have_out) { /* inertialisation offsets: old pose expressed relative to the new one */
                for (int j = 0; j < c->njoints; j++) {
                    for (int k = 0; k < 3; k++) db->off_t[j][k] = db->out.t[j][k] - np.t[j][k];
                    float conj[4] = {-np.q[j][0], -np.q[j][1], -np.q[j][2], np.q[j][3]};
                    quat_mul(db->out.q[j], conj, db->off_q[j]);
                }
            }
            if (!cur->loop && bc != db->cur_clip) { db->excl_clip = db->cur_clip; db->excl_timer = 0.8f; }
            db->cur_clip = bc; db->cur_t = nt;
            db->last_clip = bc; db->last_frame = bf;
        }
    }

    Pose p;
    mm_sample_pose(c, db, db->cur_clip, db->cur_t, &p);
    if (db->have_out) { /* decay the offsets with a 0.08 s half-life */
        float w = expf(-dt * 0.6931f / 0.08f);
        for (int j = 0; j < c->njoints; j++) {
            for (int k = 0; k < 3; k++) { db->off_t[j][k] *= w; p.t[j][k] += db->off_t[j][k]; }
            float *o = db->off_q[j];
            if (o[3] < 0) { o[0] = -o[0]; o[1] = -o[1]; o[2] = -o[2]; o[3] = -o[3]; }
            float id[4] = {o[0] * w, o[1] * w, o[2] * w, 1.0f + (o[3] - 1.0f) * w}, l = sqrtf(id[0] * id[0] + id[1] * id[1] + id[2] * id[2] + id[3] * id[3]), r[4];
            for (int k = 0; k < 4; k++) id[k] /= l;
            memcpy(o, id, sizeof id); /* keep the decayed offset for the next frame (already normalised) */
            quat_mul(id, p.q[j], r);
            memcpy(p.q[j], r, sizeof r);
        }
    }
    db->out = p; db->have_out = 1;
    *out = p;
    return 1;
}

/* ---------- per-frame update ---------- */

static void loco_weights(float s, float w[CH_CLIPS]) {
    memset(w, 0, CH_CLIPS * sizeof(float));
    if (s >= CLIP_SPEED[CH_SPRINT]) { w[CH_SPRINT] = 1; return; }
    int i = 0;
    while (s > CLIP_SPEED[i + 1]) i++;
    float f = clampf((s - CLIP_SPEED[i]) / (CLIP_SPEED[i + 1] - CLIP_SPEED[i]), 0.0f, 1.0f);
    f = f * f * (3.0f - 2.0f * f);
    w[i] = 1.0f - f; w[i + 1] = f;
}

void character_update(Character *c, float dt, float speed, int sliding) {
    c->spd += (speed - c->spd) * clampf(dt * 12.0f, 0.0f, 1.0f);
    loco_weights(c->spd, c->w);

    /* the walk/jog/sprint cycles share one phase so the feet stay in step through blends;
       the rate follows ground speed so strides keep up with the capsule */
    float wl = c->w[CH_WALK] + c->w[CH_JOG] + c->w[CH_SPRINT], rate = 0;
    if (wl > 1e-4f) {
        for (int i = CH_WALK; i <= CH_SPRINT; i++) {
            if (!c->clip[i].loaded || c->w[i] <= 0) continue;
            float scale = clampf(c->spd / CLIP_SPEED[i], 0.75f, 1.35f);
            rate += c->w[i] * scale / c->clip[i].dur;
        }
        rate /= wl;
    }
    c->phase = fmodf(c->phase + dt * rate, 1.0f);
    c->t_idle += dt;

    Pose pose, tmp;
    float acc = 0;
    int used_mm = c->mdb ? mm_update(c, dt, &pose) : 0;
    if (!used_mm) for (int i = 0; i < CH_CLIPS; i++) {
        if (c->w[i] < 1e-3f || !c->clip[i].loaded) continue;
        float t = i == CH_IDLE ? c->t_idle : c->phase * c->clip[i].dur;
        if (acc == 0) { pose_from_clip(c, &c->clip[i], t, &pose); acc = c->w[i]; }
        else { pose_from_clip(c, &c->clip[i], t, &tmp); pose_blend(c, &pose, &tmp, c->w[i] / (acc + c->w[i])); acc += c->w[i]; }
    }
    if (!used_mm && acc == 0) pose_from_clip(c, &c->clip[CH_IDLE], 0, &pose);

    /* slide: blend toward Slide_Start then Slide_Loop while carried down a slope */
    if (c->slide_loop.loaded) {
        int on = sliding && speed > 1.0f;
        if (on && !c->was_sliding) c->slide_t = 0;
        c->was_sliding = on;
        c->slide_t += dt;
        c->slide_w += ((on ? 1.0f : 0.0f) - c->slide_w) * clampf(dt * 10.0f, 0.0f, 1.0f);
        if (c->slide_w > 1e-3f) {
            const ChClip *sc = &c->slide_loop;
            float t = c->slide_t;
            if (c->slide_start.loaded) {
                if (t < c->slide_start.dur) { sc = &c->slide_start; }
                else t -= c->slide_start.dur;
            }
            if (sc == &c->slide_loop) t = fmodf(t, sc->dur);
            else if (t >= sc->dur) t = sc->dur - 1e-4f;
            pose_from_clip(c, sc, t, &tmp);
            pose_blend(c, &pose, &tmp, c->slide_w);
        }
    }
    /* spring smoothing: every joint chases the animated pose through a critically damped spring, which
       rounds off pops and gives limbs a touch of follow-through. Extremities are softer than the trunk. */
    if (c->spring_on && dt > 1e-5f) {
        if (!c->sp_init) {
            for (int j = 0; j < c->njoints; j++) {
                const char *n = c->jname[j];
                int soft = strstr(n, "hand") || strstr(n, "lowerarm") || strstr(n, "index") || strstr(n, "middle") ||
                           strstr(n, "pinky") || strstr(n, "ring") || strstr(n, "thumb") || !strcmp(n, "Head") || strstr(n, "neck");
                int foot = strstr(n, "foot") || strstr(n, "ball");
                c->sp_hl[j] = foot ? 0.02f : (soft ? 0.07f : 0.035f);
            }
        }
        for (int j = 0; j < c->njoints; j++) {
            if (!c->sp_init) {
                memcpy(c->sp_t[j], pose.t[j], sizeof c->sp_t[j]);
                memcpy(c->sp_q[j], pose.q[j], sizeof c->sp_q[j]);
                memset(c->sv_t[j], 0, sizeof c->sv_t[j]);
                memset(c->sv_q[j], 0, sizeof c->sv_q[j]);
                continue;
            }
            float y = 2.7726f / c->sp_hl[j] * 0.5f;   /* 4 ln2 / halflife, halved */
            float e = expf(-y * dt);
            float *gq = pose.q[j];
            float sgn = (c->sp_q[j][0] * gq[0] + c->sp_q[j][1] * gq[1] + c->sp_q[j][2] * gq[2] + c->sp_q[j][3] * gq[3]) < 0 ? -1.0f : 1.0f;
            for (int k = 0; k < 7; k++) {
                float *x = k < 3 ? &c->sp_t[j][k] : &c->sp_q[j][k - 3];
                float *v = k < 3 ? &c->sv_t[j][k] : &c->sv_q[j][k - 3];
                float goal = k < 3 ? pose.t[j][k] : sgn * gq[k - 3];
                float j0 = *x - goal, j1 = *v + j0 * y;
                *x = e * (j0 + j1 * dt) + goal;
                *v = e * (*v - j1 * y * dt);
            }
            float l = sqrtf(c->sp_q[j][0] * c->sp_q[j][0] + c->sp_q[j][1] * c->sp_q[j][1] + c->sp_q[j][2] * c->sp_q[j][2] + c->sp_q[j][3] * c->sp_q[j][3]);
            if (l > 1e-8f) for (int k = 0; k < 4; k++) c->sp_q[j][k] /= l;
            memcpy(pose.t[j], c->sp_t[j], sizeof pose.t[j]);
            memcpy(pose.q[j], c->sp_q[j], sizeof pose.q[j]);
        }
        c->sp_init = 1;
    }

    /* jump: Jump_Start, then Jump_Loop while airborne, Jump_Land after touching down */
    if (c->jump_loop.loaded) {
        if (c->air && !c->was_air) c->air_t = 0;
        if (!c->air && c->was_air) c->land_t = 0;
        c->was_air = c->air;
        c->air_t += dt;
        c->land_t += dt;
        int landing = !c->air && c->jump_land.loaded && c->land_t < c->jump_land.dur * 0.8f;
        float target = (c->air || landing) ? 1.0f : 0.0f;
        c->air_w += (target - c->air_w) * clampf(dt * 16.0f, 0.0f, 1.0f);
        if (c->air_w > 1e-3f) {
            const ChClip *jc;
            float t;
            if (c->air) {
                if (c->jump_start.loaded && c->air_t < c->jump_start.dur) { jc = &c->jump_start; t = c->air_t; }
                else { jc = &c->jump_loop; t = c->air_t - (c->jump_start.loaded ? c->jump_start.dur : 0.0f); }
            } else { jc = &c->jump_land; t = c->land_t; }
            if (jc == &c->jump_loop) t = fmodf(t, jc->dur);
            pose_at(c, jc, t, &tmp);
            pose_blend(c, &pose, &tmp, c->air_w);
        }
    }
    build_bones(c, &pose, dt);
}

void character_draw(const Character *c, GLuint program) {
    glUniformMatrix4fv(glGetUniformLocation(program, "uBones[0]"), c->njoints, GL_FALSE, c->bones[0].m);
    glBindVertexArray(c->vao);
    glDrawElements(GL_TRIANGLES, c->index_count, GL_UNSIGNED_INT, (void *)0);
}
