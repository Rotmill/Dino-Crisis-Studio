// Минимальный загрузчик OpenGL 3.3 core для своего 3D-вида (ImGui грузит свои функции сам).
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <GL/gl.h>

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE0 0x84C0
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_MULTISAMPLE 0x809D

#define DCGL_FUNCS(X)                                                                                         \
    X(void, GenVertexArrays, (GLsizei n, GLuint * a))                                                      \
    X(void, BindVertexArray, (GLuint a))                                                                   \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint* a))                                              \
    X(void, GenBuffers, (GLsizei n, GLuint * b))                                                           \
    X(void, BindBuffer, (GLenum t, GLuint b))                                                              \
    X(void, BufferData, (GLenum t, GLsizeiptr s, const void* d, GLenum u))                                 \
    X(void, BufferSubData, (GLenum t, GLintptr o, GLsizeiptr s, const void* d))                            \
    X(void, DeleteBuffers, (GLsizei n, const GLuint* b))                                                   \
    X(void, VertexAttribPointer, (GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void* p))    \
    X(void, EnableVertexAttribArray, (GLuint i))                                                           \
    X(GLuint, CreateShader, (GLenum t))                                                                    \
    X(void, ShaderSource, (GLuint s, GLsizei c, const GLchar* const* str, const GLint* len))               \
    X(void, CompileShader, (GLuint s))                                                                     \
    X(void, GetShaderiv, (GLuint s, GLenum p, GLint * v))                                                  \
    X(void, GetShaderInfoLog, (GLuint s, GLsizei m, GLsizei * l, GLchar * log))                            \
    X(void, DeleteShader, (GLuint s))                                                                      \
    X(GLuint, CreateProgram, (void))                                                                       \
    X(void, AttachShader, (GLuint p, GLuint s))                                                            \
    X(void, LinkProgram, (GLuint p))                                                                       \
    X(void, GetProgramiv, (GLuint p, GLenum n, GLint * v))                                                 \
    X(void, GetProgramInfoLog, (GLuint p, GLsizei m, GLsizei * l, GLchar * log))                           \
    X(void, UseProgram, (GLuint p))                                                                        \
    X(void, BindAttribLocation, (GLuint p, GLuint i, const GLchar* n))                                     \
    X(GLint, GetUniformLocation, (GLuint p, const GLchar* n))                                              \
    X(void, UniformMatrix4fv, (GLint l, GLsizei c, GLboolean t, const GLfloat* v))                         \
    X(void, UniformMatrix3fv, (GLint l, GLsizei c, GLboolean t, const GLfloat* v))                         \
    X(void, Uniform1i, (GLint l, GLint v))                                                                 \
    X(void, Uniform1f, (GLint l, GLfloat v))                                                               \
    X(void, Uniform4f, (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d))                              \
    X(void, GenFramebuffers, (GLsizei n, GLuint * f))                                                      \
    X(void, BindFramebuffer, (GLenum t, GLuint f))                                                         \
    X(void, FramebufferTexture2D, (GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l))                    \
    X(void, GenRenderbuffers, (GLsizei n, GLuint * r))                                                     \
    X(void, BindRenderbuffer, (GLenum t, GLuint r))                                                        \
    X(void, RenderbufferStorage, (GLenum t, GLenum f, GLsizei w, GLsizei h))                               \
    X(void, FramebufferRenderbuffer, (GLenum t, GLenum a, GLenum rt, GLuint r))                            \
    X(GLenum, CheckFramebufferStatus, (GLenum t))                                                          \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint* f))                                              \
    X(void, DeleteRenderbuffers, (GLsizei n, const GLuint* r))                                             \
    X(void, ActiveTexture, (GLenum t))                                                                     \
    X(void, BlendEquation, (GLenum m))

namespace gl {
#define DCGL_DECL(ret, name, args) typedef ret(APIENTRY* PFN_##name) args; extern PFN_##name name;
DCGL_FUNCS(DCGL_DECL)
#undef DCGL_DECL
bool load();   // через SDL_GL_GetProcAddress
}  // namespace gl
