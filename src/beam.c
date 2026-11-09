#include <math.h>
#include "beam.h"

void beams_init(Beams *b) {
    static const float q[8] = {0, -1, 0, 1, 1, -1, 1, 1};
    glGenVertexArrays(1, &b->vao);
    glGenBuffers(1, &b->vbo);
    glBindVertexArray(b->vao);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glBindVertexArray(0);
}

static void draw_quad(const Beams *b, GLuint prog, V3 a, V3 e, V3 cam, float width, float strength, int mode) {
    V3 d = v3_add(e, v3_scale(a, -1.0f));
    glUniform3f(glGetUniformLocation(prog, "uA"), a.x, a.y, a.z);
    glUniform3f(glGetUniformLocation(prog, "uB"), e.x, e.y, e.z);
    glUniform3f(glGetUniformLocation(prog, "uCam"), cam.x, cam.y, cam.z);
    glUniform1f(glGetUniformLocation(prog, "uWidth"), width);
    glUniform1f(glGetUniformLocation(prog, "uStrength"), strength);
    glUniform1f(glGetUniformLocation(prog, "uLen"), sqrtf(v3_dot(d, d)));
    glUniform1i(glGetUniformLocation(prog, "uMode"), mode);
    glBindVertexArray(b->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void beam_draw(const Beams *b, GLuint prog, V3 a, V3 b_end, V3 cam, float width, float strength) {
    draw_quad(b, prog, a, b_end, cam, width, strength, 0);
}

void beam_flare(const Beams *b, GLuint prog, V3 centre, V3 cam, V3 cam_right, float radius, float strength) {
    V3 o = v3_scale(cam_right, radius);
    draw_quad(b, prog, v3_add(centre, v3_scale(o, -1.0f)), v3_add(centre, o), cam, radius, strength, 1);
}
