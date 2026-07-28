// Minimal OpenGL 3.3 loader for Windows.
//
// mingw ships a GL 1.1 header, so everything past that is resolved at runtime
// through wglGetProcAddress. Only the entry points NotClon actually uses are
// declared, which keeps this to one file instead of pulling in glad/GLEW.
#pragma once

#include <windows.h>
#include <GL/gl.h>
#include <cstdio>

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GL_ARRAY_BUFFER                   0x8892
#define GL_STATIC_DRAW                    0x88E4
#define GL_STREAM_DRAW                    0x88E0
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_FRAMEBUFFER                    0x8D40
#define GL_RENDERBUFFER                   0x8D41
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_DEPTH_COMPONENT24              0x81A6
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_TEXTURE0                       0x84C0
#define GL_ACTIVE_UNIFORMS                0x8B86
// Uniform types, for setting a knob at its declared type and for spotting the
// extra sampler2Ds a shader chain binds buffers to.
#define GL_BOOL                           0x8B56
#define GL_SAMPLER_2D                     0x8B5E
// Half-float colour, for an ActorFrameTexture asked to EnableFloat. A
// feedback target that accumulates in 8 bits bands visibly.
#define GL_RGBA16F                        0x881A
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_MULTISAMPLE                    0x809D
#define GL_MIRRORED_REPEAT                0x8370

#define NC_GL_FUNCS \
    X(PFNGLCREATESHADERPROC,             glCreateShader) \
    X(PFNGLSHADERSOURCEPROC,             glShaderSource) \
    X(PFNGLCOMPILESHADERPROC,            glCompileShader) \
    X(PFNGLGETSHADERIVPROC,              glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,         glGetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC,             glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC,            glCreateProgram) \
    X(PFNGLATTACHSHADERPROC,             glAttachShader) \
    X(PFNGLLINKPROGRAMPROC,              glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,             glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC,        glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC,               glUseProgram) \
    X(PFNGLGENVERTEXARRAYSPROC,          glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,          glBindVertexArray) \
    X(PFNGLGENBUFFERSPROC,               glGenBuffers) \
    X(PFNGLBINDBUFFERPROC,               glBindBuffer) \
    X(PFNGLBUFFERDATAPROC,               glBufferData) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,      glVertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC,  glEnableVertexAttribArray) \
    X(PFNGLGETUNIFORMLOCATIONPROC,       glGetUniformLocation) \
    X(PFNGLUNIFORM1FPROC,                glUniform1f) \
    X(PFNGLUNIFORM2FPROC,                glUniform2f) \
    X(PFNGLUNIFORM3FPROC,                glUniform3f) \
    X(PFNGLUNIFORM1IPROC,                glUniform1i) \
    X(PFNGLUNIFORMMATRIX4FVPROC,         glUniformMatrix4fv) \
    X(PFNGLGENFRAMEBUFFERSPROC,          glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC,          glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC,     glFramebufferTexture2D) \
    X(PFNGLGENRENDERBUFFERSPROC,         glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC,         glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC,      glRenderbufferStorage) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC,  glFramebufferRenderbuffer) \
    X(PFNGLDELETERENDERBUFFERSPROC,      glDeleteRenderbuffers) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,   glCheckFramebufferStatus) \
    X(PFNGLDELETEFRAMEBUFFERSPROC,       glDeleteFramebuffers) \
    X(PFNGLACTIVETEXTUREPROC,            glActiveTexture) \
    X(PFNGLGENERATEMIPMAPPROC,           glGenerateMipmap) \
    X(PFNGLDELETEPROGRAMPROC,            glDeleteProgram) \
    X(PFNGLBINDATTRIBLOCATIONPROC,       glBindAttribLocation) \
    X(PFNGLGETACTIVEUNIFORMPROC,         glGetActiveUniform)

typedef GLuint (APIENTRY *PFNGLCREATESHADERPROC)(GLenum);
typedef void   (APIENTRY *PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void   (APIENTRY *PFNGLCOMPILESHADERPROC)(GLuint);
typedef void   (APIENTRY *PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void   (APIENTRY *PFNGLDELETESHADERPROC)(GLuint);
typedef GLuint (APIENTRY *PFNGLCREATEPROGRAMPROC)(void);
typedef void   (APIENTRY *PFNGLATTACHSHADERPROC)(GLuint, GLuint);
typedef void   (APIENTRY *PFNGLLINKPROGRAMPROC)(GLuint);
typedef void   (APIENTRY *PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void   (APIENTRY *PFNGLUSEPROGRAMPROC)(GLuint);
typedef void   (APIENTRY *PFNGLGENVERTEXARRAYSPROC)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFNGLBINDVERTEXARRAYPROC)(GLuint);
typedef void   (APIENTRY *PFNGLGENBUFFERSPROC)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void   (APIENTRY *PFNGLBUFFERDATAPROC)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void   (APIENTRY *PFNGLVERTEXATTRIBPOINTERPROC)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void   (APIENTRY *PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef GLint  (APIENTRY *PFNGLGETUNIFORMLOCATIONPROC)(GLuint, const GLchar*);
typedef void   (APIENTRY *PFNGLUNIFORM1FPROC)(GLint, GLfloat);
typedef void   (APIENTRY *PFNGLUNIFORM2FPROC)(GLint, GLfloat, GLfloat);
typedef void   (APIENTRY *PFNGLUNIFORM3FPROC)(GLint, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFNGLUNIFORM1IPROC)(GLint, GLint);
typedef void   (APIENTRY *PFNGLUNIFORMMATRIX4FVPROC)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void   (APIENTRY *PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef void   (APIENTRY *PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void   (APIENTRY *PFNGLGENRENDERBUFFERSPROC)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFNGLDELETERENDERBUFFERSPROC)(GLsizei, const GLuint*);
typedef void   (APIENTRY *PFNGLBINDRENDERBUFFERPROC)(GLenum, GLuint);
typedef void   (APIENTRY *PFNGLRENDERBUFFERSTORAGEPROC)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY *PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef void   (APIENTRY *PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint*);
typedef void   (APIENTRY *PFNGLACTIVETEXTUREPROC)(GLenum);
typedef void   (APIENTRY *PFNGLGENERATEMIPMAPPROC)(GLenum);
typedef void   (APIENTRY *PFNGLDELETEPROGRAMPROC)(GLuint);
typedef void   (APIENTRY *PFNGLBINDATTRIBLOCATIONPROC)(GLuint, GLuint, const GLchar*);
typedef void   (APIENTRY *PFNGLGETACTIVEUNIFORMPROC)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*);

#define X(type, name) extern type name;
NC_GL_FUNCS
#undef X

bool nc_load_gl();
