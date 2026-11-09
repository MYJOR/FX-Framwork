#include "shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char g_dir[260] = "shaders";

void shader_set_dir(const char *dir) { snprintf(g_dir, sizeof g_dir, "%s", dir); }

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) { fclose(f); return NULL; }
    size_t got = fread(s, 1, (size_t)n, f);
    s[got] = 0;
    fclose(f);
    return s;
}

static time_t mtime_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static GLuint compile(GLenum type, const char *common, const char *src, const char *name) {
    GLuint s = glCreateShader(type);
    const char *parts[2] = {common, src};
    glShaderSource(s, 2, parts, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[shader] %s failed:\n%s\n", name, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint build(const Program *p, time_t *out_stamp) {
    char common_path[300];
    snprintf(common_path, sizeof common_path, "%s/common.glsl", g_dir);
    char *common = read_file(common_path);
    char *vs = read_file(p->vs_path);
    char *fs = read_file(p->fs_path);
    GLuint prog = 0;
    if (!common || !vs || !fs) {
        fprintf(stderr, "[shader] cannot read %s / %s / %s\n", common_path, p->vs_path, p->fs_path);
    } else {
        GLuint v = compile(GL_VERTEX_SHADER, common, vs, p->vs_path);
        GLuint f = compile(GL_FRAGMENT_SHADER, common, fs, p->fs_path);
        if (v && f) {
            prog = glCreateProgram();
            glAttachShader(prog, v);
            glAttachShader(prog, f);
            glLinkProgram(prog);
            GLint ok = 0;
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[2048];
                glGetProgramInfoLog(prog, sizeof log, NULL, log);
                fprintf(stderr, "[shader] link failed:\n%s\n", log);
                glDeleteProgram(prog);
                prog = 0;
            }
        }
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
    }
    free(common); free(vs); free(fs);
    time_t a = mtime_of(common_path), b = mtime_of(p->vs_path), c = mtime_of(p->fs_path);
    *out_stamp = a > b ? (a > c ? a : c) : (b > c ? b : c);
    return prog;
}

int shader_program_load(Program *p, const char *vs_file, const char *fs_file) {
    snprintf(p->vs_path, sizeof p->vs_path, "%s/%s", g_dir, vs_file);
    snprintf(p->fs_path, sizeof p->fs_path, "%s/%s", g_dir, fs_file);
    p->id = build(p, &p->stamp);
    return p->id != 0;
}

void shader_reload_if_changed(Program *p) {
    char common_path[300];
    snprintf(common_path, sizeof common_path, "%s/common.glsl", g_dir);
    time_t a = mtime_of(common_path), b = mtime_of(p->vs_path), c = mtime_of(p->fs_path);
    time_t latest = a > b ? (a > c ? a : c) : (b > c ? b : c);
    if (latest == p->stamp) return;
    time_t stamp;
    GLuint np = build(p, &stamp);
    p->stamp = stamp;
    if (np) {
        if (p->id) glDeleteProgram(p->id);
        p->id = np;
        fprintf(stderr, "[shader] reloaded %s\n", p->fs_path);
    }
}
