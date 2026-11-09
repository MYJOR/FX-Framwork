#include "gl.h"
#include <stdio.h>

#define X(ret, name, args) PFN_gl##name gl##name;
GL_FUNCS(X)
#undef X

int gl_load_functions(void) {
    int ok = 1;
#define X(ret, name, args)                                              \
    gl##name = (PFN_gl##name)wglGetProcAddress("gl" #name);             \
    if (!gl##name) { fprintf(stderr, "missing GL function gl" #name "\n"); ok = 0; }
    GL_FUNCS(X)
#undef X
    return ok;
}
