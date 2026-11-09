#include "character.h"
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

static void load_clip(Character *c, const cgltf_data *d, const cgltf_skin *skin, int ci) {
    const cgltf_animation *an = NULL;
    for (cgltf_size i = 0; i < d->animations_count; i++)
        if (d->animations[i].name && !strcmp(d->animations[i].name, CLIP_NAMES[ci])) { an = &d->animations[i]; break; }
    if (!an) { fprintf(stderr, "animation '%s' not found\n", CLIP_NAMES[ci]); return; }

    ChClip *cl = &c->clip[ci];
    cl->ja = (ChJointAnim *)calloc((size_t)c->njoints, sizeof(ChJointAnim));
    for (cgltf_size k = 0; k < an->channels_count; k++) {
        const cgltf_animation_channel *ch = &an->channels[k];
        int j = ch->target_node ? joint_index(skin, ch->target_node) : -1;
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

    for (int i = 0; i < CH_CLIPS; i++) load_clip(c, d, skin, i);
    printf("character: %d joints, %zu verts, %zu tris, height %.2f, clips:", c->njoints, vo, io / 3, c->height);
    for (int i = 0; i < CH_CLIPS; i++) printf(" %s=%.2fs", CLIP_NAMES[i], c->clip[i].dur);
    printf("\n");
    cgltf_free(d);
    character_update(c, 0.0f, 0.0f);
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

static void pose_from_clip(const Character *c, const ChClip *cl, float t, Pose *p) {
    if (cl->dur > 0) t = fmodf(t, cl->dur);
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

static void build_bones(Character *c, const Pose *p) {
    float G[CH_MAX_JOINTS][16];
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
    for (int j = 0; j < c->njoints; j++) {
        float tmp[16];
        mul4(G[j], c->inv_bind[j], tmp);
        mul4(c->inv_mesh, tmp, c->bones[j].m);
    }
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

void character_update(Character *c, float dt, float speed) {
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
    for (int i = 0; i < CH_CLIPS; i++) {
        if (c->w[i] < 1e-3f || !c->clip[i].loaded) continue;
        float t = i == CH_IDLE ? c->t_idle : c->phase * c->clip[i].dur;
        if (acc == 0) { pose_from_clip(c, &c->clip[i], t, &pose); acc = c->w[i]; }
        else { pose_from_clip(c, &c->clip[i], t, &tmp); pose_blend(c, &pose, &tmp, c->w[i] / (acc + c->w[i])); acc += c->w[i]; }
    }
    if (acc == 0) pose_from_clip(c, &c->clip[CH_IDLE], 0, &pose);
    build_bones(c, &pose);
}

void character_draw(const Character *c, GLuint program) {
    glUniformMatrix4fv(glGetUniformLocation(program, "uBones[0]"), c->njoints, GL_FALSE, c->bones[0].m);
    glBindVertexArray(c->vao);
    glDrawElements(GL_TRIANGLES, c->index_count, GL_UNSIGNED_INT, (void *)0);
}
