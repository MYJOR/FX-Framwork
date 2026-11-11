#include "handprints.h"
#include <stdlib.h>
#include <string.h>

#define FLOOR_LIFT 0.003f /* metres above the floor; the renderer also sets a polygon offset */
#define VERT_FLOATS 9     /* position, uv (-1..1 across the quad), age, age / life, seed, side */

static GLint ul(GLuint p, const char *name) { return glGetUniformLocation(p, name); }

int handprints_init(Handprints *h) {
    memset(h, 0, sizeof *h);
    for (int i = 0; i < HANDPRINT_MAX; i++) h->p[i].age = HANDPRINT_LIFE;
    if (!shader_program_load(&h->prog, "handprint.vert", "handprint.frag")) return 0;
    glGenVertexArrays(1, &h->vao);
    glBindVertexArray(h->vao);
    glGenBuffers(1, &h->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, h->vbo);
    const GLsizei stride = VERT_FLOATS * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)(3 * sizeof(float)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void *)(5 * sizeof(float)));
    for (GLuint i = 0; i < 3; i++) glEnableVertexAttribArray(i);
    glBindVertexArray(0);
    return 1;
}

void handprints_reload_shaders(Handprints *h) { shader_reload_if_changed(&h->prog); }

void handprints_add(Handprints *h, float x, float z, float yaw, int left, float age) {
    Handprint *p = &h->p[h->next];
    h->next = (h->next + 1) % HANDPRINT_MAX;
    p->x = x; p->z = z; p->yaw = yaw; p->left = left; p->age = age;
    p->seed = (float)rand() / (float)RAND_MAX;
}

void handprints_update(Handprints *h, float dt) {
    for (int i = 0; i < HANDPRINT_MAX; i++)
        if (h->p[i].age < HANDPRINT_LIFE) h->p[i].age += dt;
}

void handprints_draw(void *user, const IntScene *s, const IntOverlayView *v) {
    Handprints *h = (Handprints *)user;
    (void)s;
    static float verts[HANDPRINT_MAX * 6 * VERT_FLOATS];
    static const float corner[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    int n = 0;
    for (int i = 0; i < HANDPRINT_MAX; i++) {
        const Handprint *p = &h->p[i];
        if (p->age >= HANDPRINT_LIFE) continue;
        V3 ahead = cam_forward(p->yaw, 0.0f), right = v3(-ahead.z, 0.0f, ahead.x);
        for (int k = 0; k < 6; k++) {
            float *o = verts + (size_t)(n * 6 + k) * VERT_FLOATS, u = corner[k][0], w = corner[k][1];
            o[0] = p->x + (right.x * u + ahead.x * w) * HANDPRINT_HALF;
            o[1] = FLOOR_LIFT;
            o[2] = p->z + (right.z * u + ahead.z * w) * HANDPRINT_HALF;
            o[3] = u; o[4] = w;
            o[5] = p->age; o[6] = p->age / HANDPRINT_LIFE; o[7] = p->seed; o[8] = p->left ? -1.0f : 1.0f;
        }
        n++;
    }
    h->live = n;
    if (!n) return;

    GLuint prog = h->prog.id;
    glUseProgram(prog);
    glUniformMatrix4fv(ul(prog, "uViewProj"), 1, GL_FALSE, v->view_proj.m);
    glUniform3f(ul(prog, "uCam"), v->cam.x, v->cam.y, v->cam.z);
    glUniform1f(ul(prog, "uSceneLight"), v->scene_light);
    glUniform1f(ul(prog, "uEnvMaxLod"), v->env_max_lod);
    glUniform4f(ul(prog, "uFlashPos"), v->flash_pos.x, v->flash_pos.y, v->flash_pos.z, v->flash);
    glUniform4f(ul(prog, "uFlashDir"), v->flash_dir.x, v->flash_dir.y, v->flash_dir.z, v->flash_cone);
    glUniform3f(ul(prog, "uFlashColor"), v->flash_color.x, v->flash_color.y, v->flash_color.z);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, v->env_tex);
    glUniform1i(ul(prog, "uEnvTex"), 0);

    glBindVertexArray(h->vao);
    glBindBuffer(GL_ARRAY_BUFFER, h->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * 6 * VERT_FLOATS * sizeof(float)), verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, n * 6);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}
