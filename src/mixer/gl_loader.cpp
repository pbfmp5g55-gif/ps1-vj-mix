#include "mixer/gl_loader.h"

#include <cstdio>

#include <GLFW/glfw3.h>

PFNGLCREATESHADERPROC_            vjgl_CreateShader            = nullptr;
PFNGLSHADERSOURCEPROC_            vjgl_ShaderSource            = nullptr;
PFNGLCOMPILESHADERPROC_           vjgl_CompileShader           = nullptr;
PFNGLGETSHADERIVPROC_             vjgl_GetShaderiv             = nullptr;
PFNGLGETSHADERINFOLOGPROC_        vjgl_GetShaderInfoLog        = nullptr;
PFNGLDELETESHADERPROC_            vjgl_DeleteShader            = nullptr;
PFNGLCREATEPROGRAMPROC_           vjgl_CreateProgram           = nullptr;
PFNGLATTACHSHADERPROC_            vjgl_AttachShader            = nullptr;
PFNGLLINKPROGRAMPROC_             vjgl_LinkProgram             = nullptr;
PFNGLGETPROGRAMIVPROC_            vjgl_GetProgramiv            = nullptr;
PFNGLGETPROGRAMINFOLOGPROC_       vjgl_GetProgramInfoLog       = nullptr;
PFNGLDELETEPROGRAMPROC_           vjgl_DeleteProgram           = nullptr;
PFNGLUSEPROGRAMPROC_              vjgl_UseProgram              = nullptr;
PFNGLGETUNIFORMLOCATIONPROC_      vjgl_GetUniformLocation      = nullptr;
PFNGLUNIFORM2FPROC_               vjgl_Uniform2f               = nullptr;
PFNGLGENVERTEXARRAYSPROC_         vjgl_GenVertexArrays         = nullptr;
PFNGLDELETEVERTEXARRAYSPROC_      vjgl_DeleteVertexArrays      = nullptr;
PFNGLBINDVERTEXARRAYPROC_         vjgl_BindVertexArray         = nullptr;
PFNGLGENBUFFERSPROC_              vjgl_GenBuffers              = nullptr;
PFNGLDELETEBUFFERSPROC_           vjgl_DeleteBuffers           = nullptr;
PFNGLBINDBUFFERPROC_              vjgl_BindBuffer              = nullptr;
PFNGLBUFFERDATAPROC_              vjgl_BufferData              = nullptr;
PFNGLVERTEXATTRIBPOINTERPROC_     vjgl_VertexAttribPointer     = nullptr;
PFNGLENABLEVERTEXATTRIBARRAYPROC_ vjgl_EnableVertexAttribArray = nullptr;

namespace {
template <typename T>
T fetch(const char* name, bool& ok) {
    T fn = reinterpret_cast<T>(glfwGetProcAddress(name));
    if (!fn) {
        std::fprintf(stderr, "[vjgl] missing GL entry: %s\n", name);
        ok = false;
    }
    return fn;
}
}  // namespace

bool vjglLoad() {
    bool ok = true;
    vjgl_CreateShader            = fetch<PFNGLCREATESHADERPROC_>("glCreateShader", ok);
    vjgl_ShaderSource            = fetch<PFNGLSHADERSOURCEPROC_>("glShaderSource", ok);
    vjgl_CompileShader           = fetch<PFNGLCOMPILESHADERPROC_>("glCompileShader", ok);
    vjgl_GetShaderiv             = fetch<PFNGLGETSHADERIVPROC_>("glGetShaderiv", ok);
    vjgl_GetShaderInfoLog        = fetch<PFNGLGETSHADERINFOLOGPROC_>("glGetShaderInfoLog", ok);
    vjgl_DeleteShader            = fetch<PFNGLDELETESHADERPROC_>("glDeleteShader", ok);
    vjgl_CreateProgram           = fetch<PFNGLCREATEPROGRAMPROC_>("glCreateProgram", ok);
    vjgl_AttachShader            = fetch<PFNGLATTACHSHADERPROC_>("glAttachShader", ok);
    vjgl_LinkProgram             = fetch<PFNGLLINKPROGRAMPROC_>("glLinkProgram", ok);
    vjgl_GetProgramiv            = fetch<PFNGLGETPROGRAMIVPROC_>("glGetProgramiv", ok);
    vjgl_GetProgramInfoLog       = fetch<PFNGLGETPROGRAMINFOLOGPROC_>("glGetProgramInfoLog", ok);
    vjgl_DeleteProgram           = fetch<PFNGLDELETEPROGRAMPROC_>("glDeleteProgram", ok);
    vjgl_UseProgram              = fetch<PFNGLUSEPROGRAMPROC_>("glUseProgram", ok);
    vjgl_GetUniformLocation      = fetch<PFNGLGETUNIFORMLOCATIONPROC_>("glGetUniformLocation", ok);
    vjgl_Uniform2f               = fetch<PFNGLUNIFORM2FPROC_>("glUniform2f", ok);
    vjgl_GenVertexArrays         = fetch<PFNGLGENVERTEXARRAYSPROC_>("glGenVertexArrays", ok);
    vjgl_DeleteVertexArrays      = fetch<PFNGLDELETEVERTEXARRAYSPROC_>("glDeleteVertexArrays", ok);
    vjgl_BindVertexArray         = fetch<PFNGLBINDVERTEXARRAYPROC_>("glBindVertexArray", ok);
    vjgl_GenBuffers              = fetch<PFNGLGENBUFFERSPROC_>("glGenBuffers", ok);
    vjgl_DeleteBuffers           = fetch<PFNGLDELETEBUFFERSPROC_>("glDeleteBuffers", ok);
    vjgl_BindBuffer              = fetch<PFNGLBINDBUFFERPROC_>("glBindBuffer", ok);
    vjgl_BufferData              = fetch<PFNGLBUFFERDATAPROC_>("glBufferData", ok);
    vjgl_VertexAttribPointer     = fetch<PFNGLVERTEXATTRIBPOINTERPROC_>("glVertexAttribPointer", ok);
    vjgl_EnableVertexAttribArray = fetch<PFNGLENABLEVERTEXATTRIBARRAYPROC_>("glEnableVertexAttribArray", ok);
    return ok;
}
