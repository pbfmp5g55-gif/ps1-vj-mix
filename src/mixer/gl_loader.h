// Minimal GL 3.3 loader for the mixer. Only the functions we actually use
// in the polygon renderer are pulled in via glfwGetProcAddress. Lets us
// avoid bringing in GLAD as a build-time dependency.

#pragma once

// On Windows MinGW, <GL/gl.h> provides GL 1.1 types/decls; GL 3.3 core
// types and entry points are declared as typedef + extern function
// pointers here.

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

// Types from glcorearb.h that aren't in legacy <GL/gl.h>.
#ifndef GL_VERSION_2_0
typedef char GLchar;
#endif
typedef ptrdiff_t GLsizeiptr_compat;

// Constants we need (values from OpenGL spec).
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER       0x8892
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW       0x88E8
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER    0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER      0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS     0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS        0x8B82
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0           0x84C0
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE      0x812F
#endif
#ifndef GL_R16UI
#define GL_R16UI              0x8234
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER        0x8D94
#endif

// Function-pointer typedefs.
#ifdef _WIN32
#define VJGL_APIENTRY __stdcall
#else
#define VJGL_APIENTRY
#endif

typedef GLuint (VJGL_APIENTRY *PFNGLCREATESHADERPROC_)(GLenum);
typedef void   (VJGL_APIENTRY *PFNGLSHADERSOURCEPROC_)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void   (VJGL_APIENTRY *PFNGLCOMPILESHADERPROC_)(GLuint);
typedef void   (VJGL_APIENTRY *PFNGLGETSHADERIVPROC_)(GLuint, GLenum, GLint*);
typedef void   (VJGL_APIENTRY *PFNGLGETSHADERINFOLOGPROC_)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void   (VJGL_APIENTRY *PFNGLDELETESHADERPROC_)(GLuint);
typedef GLuint (VJGL_APIENTRY *PFNGLCREATEPROGRAMPROC_)(void);
typedef void   (VJGL_APIENTRY *PFNGLATTACHSHADERPROC_)(GLuint, GLuint);
typedef void   (VJGL_APIENTRY *PFNGLLINKPROGRAMPROC_)(GLuint);
typedef void   (VJGL_APIENTRY *PFNGLGETPROGRAMIVPROC_)(GLuint, GLenum, GLint*);
typedef void   (VJGL_APIENTRY *PFNGLGETPROGRAMINFOLOGPROC_)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void   (VJGL_APIENTRY *PFNGLDELETEPROGRAMPROC_)(GLuint);
typedef void   (VJGL_APIENTRY *PFNGLUSEPROGRAMPROC_)(GLuint);
typedef GLint  (VJGL_APIENTRY *PFNGLGETUNIFORMLOCATIONPROC_)(GLuint, const GLchar*);
typedef void   (VJGL_APIENTRY *PFNGLUNIFORM2FPROC_)(GLint, GLfloat, GLfloat);
typedef void   (VJGL_APIENTRY *PFNGLGENVERTEXARRAYSPROC_)(GLsizei, GLuint*);
typedef void   (VJGL_APIENTRY *PFNGLDELETEVERTEXARRAYSPROC_)(GLsizei, const GLuint*);
typedef void   (VJGL_APIENTRY *PFNGLBINDVERTEXARRAYPROC_)(GLuint);
typedef void   (VJGL_APIENTRY *PFNGLGENBUFFERSPROC_)(GLsizei, GLuint*);
typedef void   (VJGL_APIENTRY *PFNGLDELETEBUFFERSPROC_)(GLsizei, const GLuint*);
typedef void   (VJGL_APIENTRY *PFNGLBINDBUFFERPROC_)(GLenum, GLuint);
typedef void   (VJGL_APIENTRY *PFNGLBUFFERDATAPROC_)(GLenum, GLsizeiptr_compat, const void*, GLenum);
typedef void   (VJGL_APIENTRY *PFNGLVERTEXATTRIBPOINTERPROC_)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void   (VJGL_APIENTRY *PFNGLENABLEVERTEXATTRIBARRAYPROC_)(GLuint);
typedef void   (VJGL_APIENTRY *PFNGLACTIVETEXTUREPROC_)(GLenum);
typedef void   (VJGL_APIENTRY *PFNGLUNIFORM1IPROC_)(GLint, GLint);

extern PFNGLCREATESHADERPROC_            vjgl_CreateShader;
extern PFNGLSHADERSOURCEPROC_            vjgl_ShaderSource;
extern PFNGLCOMPILESHADERPROC_           vjgl_CompileShader;
extern PFNGLGETSHADERIVPROC_             vjgl_GetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC_        vjgl_GetShaderInfoLog;
extern PFNGLDELETESHADERPROC_            vjgl_DeleteShader;
extern PFNGLCREATEPROGRAMPROC_           vjgl_CreateProgram;
extern PFNGLATTACHSHADERPROC_            vjgl_AttachShader;
extern PFNGLLINKPROGRAMPROC_             vjgl_LinkProgram;
extern PFNGLGETPROGRAMIVPROC_            vjgl_GetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC_       vjgl_GetProgramInfoLog;
extern PFNGLDELETEPROGRAMPROC_           vjgl_DeleteProgram;
extern PFNGLUSEPROGRAMPROC_              vjgl_UseProgram;
extern PFNGLGETUNIFORMLOCATIONPROC_      vjgl_GetUniformLocation;
extern PFNGLUNIFORM2FPROC_               vjgl_Uniform2f;
extern PFNGLGENVERTEXARRAYSPROC_         vjgl_GenVertexArrays;
extern PFNGLDELETEVERTEXARRAYSPROC_      vjgl_DeleteVertexArrays;
extern PFNGLBINDVERTEXARRAYPROC_         vjgl_BindVertexArray;
extern PFNGLGENBUFFERSPROC_              vjgl_GenBuffers;
extern PFNGLDELETEBUFFERSPROC_           vjgl_DeleteBuffers;
extern PFNGLBINDBUFFERPROC_              vjgl_BindBuffer;
extern PFNGLBUFFERDATAPROC_              vjgl_BufferData;
extern PFNGLVERTEXATTRIBPOINTERPROC_     vjgl_VertexAttribPointer;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC_ vjgl_EnableVertexAttribArray;
extern PFNGLACTIVETEXTUREPROC_           vjgl_ActiveTexture;
extern PFNGLUNIFORM1IPROC_               vjgl_Uniform1i;

// Load all extern pointers via glfwGetProcAddress. Returns false if any
// look-up fails.
bool vjglLoad();
