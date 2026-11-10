#ifndef GL_H
#define GL_H
#include <windows.h>
#include <stddef.h>
#include <GL/gl.h>

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_STREAM_DRAW 0x88E0
#define GL_PROGRAM_POINT_SIZE 0x8642
#define GL_TIME_ELAPSED 0x88BF
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867

/* textures, samplers and framebuffers (interior scene) */
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_SEAMLESS 0x884F
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_R8 0x8229
#define GL_R16F 0x822D
#define GL_RGBA16F 0x881A
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

/* X(return type, name, args) */
#define GL_FUNCS(X) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, UseProgram, (GLuint)) \
    X(void, DeleteShader, (GLuint)) \
    X(void, DeleteProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, Uniform1f, (GLint, GLfloat)) \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, GenVertexArrays, (GLsizei, GLuint *)) \
    X(void, BindVertexArray, (GLuint)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, Uniform1i, (GLint, GLint)) \
    X(void, GenQueries, (GLsizei, GLuint *)) \
    X(void, DeleteQueries, (GLsizei, const GLuint *)) \
    X(void, BeginQuery, (GLenum, GLuint)) \
    X(void, EndQuery, (GLenum)) \
    X(void, GetQueryObjectiv, (GLuint, GLenum, GLint *)) \
    X(void, GetQueryObjectui64v, (GLuint, GLenum, unsigned long long *)) \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, Uniform3fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, ActiveTexture, (GLenum)) \
    X(void, GenerateMipmap, (GLenum)) \
    X(void, TexImage3D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexImage2DMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum))

#define X(ret, name, args) typedef ret(APIENTRY *PFN_gl##name) args; extern PFN_gl##name gl##name;
GL_FUNCS(X)
#undef X

int gl_load_functions(void);

#endif
