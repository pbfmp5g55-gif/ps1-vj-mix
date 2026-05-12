// Spike 1 main loop. Owns the GLFW window + ImGui context + an optional
// loaded .vjr file (M2) + a tiny GL renderer for untextured polygons
// (M3). M4 will add textured polys; subsequent milestones blend modes etc.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "mixer/gl_loader.h"
#include "mixer/ipc/ipc_ring.h"
#include "vj/Params.h"
#include "vj/PrimitiveInterceptor.h"
#include "vj/PrimitiveStream.h"

#include <GLFW/glfw3.h>

namespace {

void glfwErrorCallback(int code, const char* msg) {
    std::fprintf(stderr, "[GLFW] %d: %s\n", code, msg);
}

// PS1 native resolution we render at. The window upscales from this.
constexpr int kPS1Width  = 320;
constexpr int kPS1Height = 240;
constexpr int kVRAMWidth  = 1024;
constexpr int kVRAMHeight = 512;

// Unpack the on-wire Primitive layout (matches packPrimitiveForLive in
// the pcsx-redux fork: kind/textured/vc/blend + 8-byte hostTag + N*20).
bool unpackLivePrimitive(const uint8_t* buf, size_t len, vj::Primitive& p) {
    if (len < 12) return false;
    p.kind     = static_cast<vj::PrimitiveKind>(buf[0]);
    p.textured = buf[1] != 0;
    const uint8_t vc = buf[2];
    p.blendMode = static_cast<vj::BlendMode>(buf[3]);
    std::memcpy(&p.hostTag, buf + 4, 8);
    const size_t need = 12 + static_cast<size_t>(vc) * 20;
    if (len < need) return false;
    p.vertices.resize(vc);
    size_t off = 12;
    for (uint8_t i = 0; i < vc; ++i) {
        auto& v = p.vertices[i];
        std::memcpy(&v.x, buf + off, 4); off += 4;
        std::memcpy(&v.y, buf + off, 4); off += 4;
        std::memcpy(&v.u, buf + off, 4); off += 4;
        std::memcpy(&v.v, buf + off, 4); off += 4;
        v.r = buf[off++];
        v.g = buf[off++];
        v.b = buf[off++];
        v.a = buf[off++];
    }
    return true;
}

bool unpackLiveUpload(const uint8_t* buf, size_t len, vj::VRAMUpload& u) {
    if (len < 8) return false;
    uint16_t x = 0, y = 0, w = 0, h = 0;
    std::memcpy(&x, buf + 0, 2);
    std::memcpy(&y, buf + 2, 2);
    std::memcpy(&w, buf + 4, 2);
    std::memcpy(&h, buf + 6, 2);
    u.x = x; u.y = y; u.w = w; u.h = h;
    const size_t pixels = static_cast<size_t>(w) * h;
    const size_t need   = 8 + pixels * 2;
    if (len < need) return false;
    u.data.resize(pixels);
    if (pixels > 0) std::memcpy(u.data.data(), buf + 8, pixels * 2);
    return true;
}

// Convert PS1 5/5/5/mask 16bpp to RGBA8 (we ignore the mask bit; black
// stays black, which the textured shader maps to transparent via alpha=0).
uint32_t convertPSX16toRGBA8(uint16_t px) {
    const uint32_t r = (px >> 0)  & 0x1F;
    const uint32_t g = (px >> 5)  & 0x1F;
    const uint32_t b = (px >> 10) & 0x1F;
    // 5->8 bit expansion: (x << 3) | (x >> 2)
    const uint32_t R = (r << 3) | (r >> 2);
    const uint32_t G = (g << 3) | (g >> 2);
    const uint32_t B = (b << 3) | (b >> 2);
    const uint32_t A = (px == 0) ? 0u : 0xFFu;
    return R | (G << 8) | (B << 16) | (A << 24);
}

struct LoadedRecording {
    std::string                path;
    std::vector<vj::EchoFrame> frames;
    uint64_t                   totalPrimitives = 0;

    void clear() {
        path.clear();
        frames.clear();
        totalPrimitives = 0;
    }
};

struct LoadStatus {
    bool        valid = false;
    std::string text  = "(no file loaded)";
};

bool loadRecording(const std::string& path, LoadedRecording& out, LoadStatus& status) {
    out.clear();
    vj::PrimitiveStreamReader reader;
    if (!reader.open(path)) {
        status.valid = false;
        status.text  = "open failed: " + path;
        return false;
    }
    vj::EchoFrame frame;
    while (reader.readNextFrame(frame)) {
        out.totalPrimitives += frame.primitives.size();
        out.frames.push_back(std::move(frame));
        frame = vj::EchoFrame{};
    }
    out.path     = path;
    status.valid = true;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "loaded %zu frames, %llu prims total",
                  out.frames.size(),
                  static_cast<unsigned long long>(out.totalPrimitives));
    status.text = buf;
    return true;
}

// -- Renderer ---------------------------------------------------------------

const char* kVertexSrc = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
uniform vec2 u_psx_size;
out vec4 v_color;
void main() {
    vec2 ndc = vec2(
        (a_pos.x / u_psx_size.x) * 2.0 - 1.0,
        1.0 - (a_pos.y / u_psx_size.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_color = a_color;
}
)";

const char* kFragmentSrc = R"(#version 330 core
in vec4 v_color;
out vec4 frag_color;
void main() { frag_color = v_color; }
)";

// Textured pipeline: per-vertex packs pos / world-VRAM uv / vertex colour
// + a CLUT-kind tag and a per-primitive hash (both derived on the CPU side
// from hostTag's TPage bits and forwarded down the pipeline).
const char* kTexVertexSrc = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in float a_clut_kind;
layout(location = 4) in float a_hash;
uniform vec2 u_psx_size;
uniform vec2 u_vram_size;
out vec2 v_uv;
out vec4 v_color;
flat out int v_clut_kind;
flat out float v_hash;
void main() {
    vec2 ndc = vec2(
        (a_pos.x / u_psx_size.x) * 2.0 - 1.0,
        1.0 - (a_pos.y / u_psx_size.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = a_uv / u_vram_size;
    v_color = a_color;
    v_clut_kind = int(a_clut_kind);
    v_hash = a_hash;
}
)";

// CLUT VJ modes:
//   0 = Direct sample (current behaviour; CLUT prims show VRAM bytes as RGB,
//                      which often looks black/garbage but is "correct" for
//                      15bpp direct-colour textures)
//   1 = Discard CLUT  (CLUT prims drawn fully transparent; silhouette feel)
//   2 = Noise CLUT    (CLUT prims tinted from a per-prim hash; chaos feel)
// A 'clean' palette-aware mode is future work (M5+); the renderer would need
// indirect texture lookup against the CLUT region of VRAM.
const char* kTexFragmentSrc = R"(#version 330 core
in vec2 v_uv;
in vec4 v_color;
flat in int v_clut_kind;     // 0=direct/15bpp, 1=4bpp CLUT, 2=8bpp CLUT
flat in float v_hash;
uniform sampler2D u_vram;
uniform int u_clut_mode;     // 0=direct sample, 1=discard CLUT, 2=noise CLUT
out vec4 frag_color;

vec3 hash3(float h) {
    return fract(vec3(
        sin(h * 12.9898) * 43758.5453,
        sin(h * 78.233 ) * 12345.6789,
        sin(h * 37.719 ) * 91234.5678
    ));
}

void main() {
    vec4 t = texture(u_vram, v_uv);
    if (v_clut_kind != 0) {
        if (u_clut_mode == 1) {
            discard;
        } else if (u_clut_mode == 2) {
            // Noise mode: pure per-prim hash colour, ignore vertex colour
            // (PS1 textured prims often carry v_color=0 or 0x808080 which
            // would attenuate or kill the noise tint). Mix the per-prim
            // hash with the screen-space pixel position so polygons that
            // share a hostTag (common for UI text in PSX games) still
            // get distinct, varying colours.
            float pixHash = fract(sin(dot(gl_FragCoord.xy,
                                          vec2(12.9898, 78.233))) * 43758.5453);
            float h = fract(v_hash + pixHash * 0.5);
            vec3 col = hash3(h);
            // Force at least one channel to saturate so colours don't
            // collapse to grey.
            float m = max(max(col.r, col.g), col.b);
            if (m > 0.0) col /= m;
            frag_color = vec4(col, 1.0);
            return;
        }
        // mode == 0: fall through to direct sampling (current behaviour)
    }
    if (t.a < 0.01) discard;
    frag_color = t * v_color;
}
)";

struct Renderer {
    // Untextured pipeline.
    GLuint program = 0;
    GLuint vao     = 0;
    GLuint vbo     = 0;
    GLint  uPsxSize = -1;
    std::vector<float> verts;  // x,y,r,g,b,a per vertex

    // Textured pipeline.
    GLuint texProgram = 0;
    GLuint texVao     = 0;
    GLuint texVbo     = 0;
    GLint  texUPsxSize  = -1;
    GLint  texUVramSize = -1;
    GLint  texUSampler  = -1;
    GLuint vramTex      = 0;
    std::vector<float> texVerts;  // x,y,u,v,r,g,b,a per vertex
    // Persistent CPU-side mirror so we can re-upload after a GL reset.
    std::vector<uint32_t> vramMirror;  // kVRAMWidth*kVRAMHeight pixels

    GLuint compileShader(GLenum type, const char* src) {
        GLuint sh = vjgl_CreateShader(type);
        vjgl_ShaderSource(sh, 1, &src, nullptr);
        vjgl_CompileShader(sh);
        GLint ok = 0;
        vjgl_GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            vjgl_GetShaderInfoLog(sh, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[shader] compile failed: %s\n", log);
        }
        return sh;
    }

    GLuint linkProgram(const char* vs_src, const char* fs_src) {
        GLuint vs = compileShader(GL_VERTEX_SHADER, vs_src);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, fs_src);
        GLuint prog = vjgl_CreateProgram();
        vjgl_AttachShader(prog, vs);
        vjgl_AttachShader(prog, fs);
        vjgl_LinkProgram(prog);
        GLint ok = 0;
        vjgl_GetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            vjgl_GetProgramInfoLog(prog, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[program] link failed: %s\n", log);
            vjgl_DeleteProgram(prog);
            prog = 0;
        }
        vjgl_DeleteShader(vs);
        vjgl_DeleteShader(fs);
        return prog;
    }

    bool init() {
        program = linkProgram(kVertexSrc, kFragmentSrc);
        if (!program) return false;
        uPsxSize = vjgl_GetUniformLocation(program, "u_psx_size");

        vjgl_GenVertexArrays(1, &vao);
        vjgl_GenBuffers(1, &vbo);
        vjgl_BindVertexArray(vao);
        vjgl_BindBuffer(GL_ARRAY_BUFFER, vbo);
        vjgl_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                                 reinterpret_cast<void*>(0));
        vjgl_EnableVertexAttribArray(0);
        vjgl_VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                                 reinterpret_cast<void*>(2 * sizeof(float)));
        vjgl_EnableVertexAttribArray(1);
        vjgl_BindVertexArray(0);

        // Textured pipeline.
        texProgram = linkProgram(kTexVertexSrc, kTexFragmentSrc);
        if (!texProgram) return false;
        texUPsxSize  = vjgl_GetUniformLocation(texProgram, "u_psx_size");
        texUVramSize = vjgl_GetUniformLocation(texProgram, "u_vram_size");
        texUSampler  = vjgl_GetUniformLocation(texProgram, "u_vram");

        vjgl_GenVertexArrays(1, &texVao);
        vjgl_GenBuffers(1, &texVbo);
        vjgl_BindVertexArray(texVao);
        vjgl_BindBuffer(GL_ARRAY_BUFFER, texVbo);
        // Vertex layout: x,y, u,v, r,g,b,a, clut_kind, hash  = 10 floats
        constexpr GLsizei kTexStride = 10 * sizeof(float);
        vjgl_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kTexStride,
                                 reinterpret_cast<void*>(0));
        vjgl_EnableVertexAttribArray(0);
        vjgl_VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kTexStride,
                                 reinterpret_cast<void*>(2 * sizeof(float)));
        vjgl_EnableVertexAttribArray(1);
        vjgl_VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, kTexStride,
                                 reinterpret_cast<void*>(4 * sizeof(float)));
        vjgl_EnableVertexAttribArray(2);
        vjgl_VertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, kTexStride,
                                 reinterpret_cast<void*>(8 * sizeof(float)));
        vjgl_EnableVertexAttribArray(3);
        vjgl_VertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, kTexStride,
                                 reinterpret_cast<void*>(9 * sizeof(float)));
        vjgl_EnableVertexAttribArray(4);
        vjgl_BindVertexArray(0);

        // VRAM texture (RGBA8 mirror of PS1 VRAM). Init to all-zero.
        vramMirror.assign(static_cast<size_t>(kVRAMWidth) * kVRAMHeight, 0);
        glGenTextures(1, &vramTex);
        glBindTexture(GL_TEXTURE_2D, vramTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kVRAMWidth, kVRAMHeight, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, vramMirror.data());

        return true;
    }

    void applyUploads(const std::vector<vj::VRAMUpload>& uploads,
                      int xRelocate = 0) {
        if (uploads.empty()) return;
        glBindTexture(GL_TEXTURE_2D, vramTex);
        std::vector<uint32_t> patch;
        for (const auto& u : uploads) {
            const int ux = u.x + xRelocate;
            if (u.w <= 0 || u.h <= 0) continue;
            if (ux < 0 || u.y < 0) continue;
            if (ux + u.w > kVRAMWidth || u.y + u.h > kVRAMHeight) continue;
            const size_t n = static_cast<size_t>(u.w) * u.h;
            patch.resize(n);
            for (size_t i = 0; i < n && i < u.data.size(); ++i) {
                patch[i] = convertPSX16toRGBA8(u.data[i]);
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, ux, u.y, u.w, u.h,
                            GL_RGBA, GL_UNSIGNED_BYTE, patch.data());
            for (int row = 0; row < u.h; ++row) {
                const size_t srcOff = static_cast<size_t>(row) * u.w;
                const size_t dstOff = static_cast<size_t>(u.y + row) * kVRAMWidth + ux;
                std::memcpy(&vramMirror[dstOff], &patch[srcOff],
                            static_cast<size_t>(u.w) * sizeof(uint32_t));
            }
        }
    }

    void pushTri(const vj::Vertex& a, const vj::Vertex& b, const vj::Vertex& c) {
        auto push = [&](const vj::Vertex& v) {
            verts.push_back(v.x);
            verts.push_back(v.y);
            verts.push_back(v.r / 255.0f);
            verts.push_back(v.g / 255.0f);
            verts.push_back(v.b / 255.0f);
            verts.push_back(v.a / 255.0f);
        };
        push(a);
        push(b);
        push(c);
    }

    // Pack one triangle into `out` using the untextured (6 floats per vtx)
    // layout. Optional colorMul for the Twin Self ghost.
    static void pushTriUntex(std::vector<float>& out,
                             const vj::Vertex& a, const vj::Vertex& b,
                             const vj::Vertex& c, float colorMul) {
        auto push = [&](const vj::Vertex& v) {
            out.push_back(v.x);
            out.push_back(v.y);
            out.push_back((v.r / 255.0f) * colorMul);
            out.push_back((v.g / 255.0f) * colorMul);
            out.push_back((v.b / 255.0f) * colorMul);
            // For semi-transparent prims the fragment shader uses this
            // alpha for blending; opaque prims ignore it.
            out.push_back(v.a / 255.0f);
        };
        push(a); push(b); push(c);
    }

    // Submit a buffer of untextured triangles to the GPU.
    void submitUntex(const std::vector<float>& buf, int viewportX, int viewportY,
                     int viewportW, int viewportH) {
        if (buf.empty()) return;
        glViewport(viewportX, viewportY, viewportW, viewportH);
        vjgl_UseProgram(program);
        vjgl_Uniform2f(uPsxSize, static_cast<float>(kPS1Width),
                       static_cast<float>(kPS1Height));
        vjgl_BindVertexArray(vao);
        vjgl_BindBuffer(GL_ARRAY_BUFFER, vbo);
        vjgl_BufferData(GL_ARRAY_BUFFER,
                        static_cast<GLsizeiptr_compat>(buf.size() * sizeof(float)),
                        buf.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(buf.size() / 6));
        vjgl_BindVertexArray(0);
    }

    // Scratch buffers per blend bucket (Opaque + SemiTransparent).
    std::vector<float> vertsSemi;

    int drawUntextured(const std::vector<vj::Primitive>& prims,
                       int viewportX, int viewportY,
                       int viewportW, int viewportH,
                       float colorMul = 1.0f) {
        verts.clear();
        vertsSemi.clear();
        int drawn = 0;
        for (const auto& p : prims) {
            if (p.textured) continue;
            std::vector<float>& bucket =
                (p.blendMode == vj::BlendMode::Opaque) ? verts : vertsSemi;
            if (p.kind == vj::PrimitiveKind::Triangle &&
                p.vertices.size() >= 3) {
                pushTriUntex(bucket, p.vertices[0], p.vertices[1],
                             p.vertices[2], colorMul);
                ++drawn;
            } else if (p.kind == vj::PrimitiveKind::Quad &&
                       p.vertices.size() >= 4) {
                pushTriUntex(bucket, p.vertices[0], p.vertices[1],
                             p.vertices[2], colorMul);
                pushTriUntex(bucket, p.vertices[1], p.vertices[3],
                             p.vertices[2], colorMul);
                ++drawn;
            }
        }
        // Opaque pass first.
        glDisable(GL_BLEND);
        submitUntex(verts, viewportX, viewportY, viewportW, viewportH);
        // Semi-transparent pass. PS1 Average mode = (B + F) / 2; emulate
        // by drawing F with alpha 0.5 over B. The fragment colour
        // already carries vertex alpha; we just need the right blend
        // function and a forced alpha of 0.5.
        if (!vertsSemi.empty()) {
            for (size_t i = 5; i < vertsSemi.size(); i += 6) {
                vertsSemi[i] = 0.5f;
            }
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            submitUntex(vertsSemi, viewportX, viewportY, viewportW, viewportH);
            glDisable(GL_BLEND);
        }
        return drawn;
    }

    static void pushTriTex(std::vector<float>& out,
                           const vj::Vertex& a, const vj::Vertex& b,
                           const vj::Vertex& c,
                           float TPageBaseX, float TPageBaseY,
                           float colorMul,
                           float clutKind, float primHash) {
        auto push = [&](const vj::Vertex& v) {
            out.push_back(v.x);
            out.push_back(v.y);
            out.push_back(TPageBaseX + v.u);
            out.push_back(TPageBaseY + v.v);
            out.push_back((v.r / 255.0f) * colorMul);
            out.push_back((v.g / 255.0f) * colorMul);
            out.push_back((v.b / 255.0f) * colorMul);
            out.push_back(v.a / 255.0f);
            out.push_back(clutKind);
            out.push_back(primHash);
        };
        push(a); push(b); push(c);
    }

    GLint texUClutMode = -1;
    int   clutMode = 0;  // 0=Direct, 1=Discard, 2=Noise

    void submitTex(const std::vector<float>& buf, int viewportX, int viewportY,
                   int viewportW, int viewportH) {
        if (buf.empty()) return;
        glViewport(viewportX, viewportY, viewportW, viewportH);
        vjgl_UseProgram(texProgram);
        vjgl_Uniform2f(texUPsxSize, static_cast<float>(kPS1Width),
                       static_cast<float>(kPS1Height));
        vjgl_Uniform2f(texUVramSize, static_cast<float>(kVRAMWidth),
                       static_cast<float>(kVRAMHeight));
        if (texUClutMode < 0) {
            texUClutMode = vjgl_GetUniformLocation(texProgram, "u_clut_mode");
        }
        vjgl_Uniform1i(texUClutMode, clutMode);
        vjgl_ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, vramTex);
        vjgl_Uniform1i(texUSampler, 0);
        vjgl_BindVertexArray(texVao);
        vjgl_BindBuffer(GL_ARRAY_BUFFER, texVbo);
        vjgl_BufferData(GL_ARRAY_BUFFER,
                        static_cast<GLsizeiptr_compat>(buf.size() * sizeof(float)),
                        buf.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(buf.size() / 10));
        vjgl_BindVertexArray(0);
    }

    std::vector<float> texVertsSemi;

    // Per-frame statistics on textured primitives by TP mode. Reset by
    // drawTextured() each call.
    int statDirectPrims = 0;
    int stat4bppPrims   = 0;
    int stat8bppPrims   = 0;

    int drawTextured(const std::vector<vj::Primitive>& prims,
                     int viewportX, int viewportY,
                     int viewportW, int viewportH,
                     float colorMul = 1.0f,
                     float uvRelocateX = 0.0f) {
        texVerts.clear();
        texVertsSemi.clear();
        statDirectPrims = stat4bppPrims = stat8bppPrims = 0;
        int drawn = 0;
        for (const auto& p : prims) {
            if (!p.textured) continue;
            const uint64_t tpageRaw = (p.hostTag >> 24) & 0xFFFF;
            const float TPageBaseX = static_cast<float>((tpageRaw & 0xF) * 64) + uvRelocateX;
            const float TPageBaseY = static_cast<float>(((tpageRaw >> 4) & 0x1) * 256);
            // PS1 TP (bits 7-8 of TPage): 0=4bpp CLUT, 1=8bpp CLUT, 2=15bpp direct.
            const uint32_t tpField = static_cast<uint32_t>((tpageRaw >> 7) & 0x3);
            float clutKind = 0.0f;  // shader: 0=direct, 1=4bpp, 2=8bpp
            if (tpField == 0)      { clutKind = 1.0f; ++stat4bppPrims; }
            else if (tpField == 1) { clutKind = 2.0f; ++stat8bppPrims; }
            else                   { clutKind = 0.0f; ++statDirectPrims; }
            const float primHash =
                static_cast<float>(static_cast<uint32_t>(p.hostTag * 2654435761u) & 0xFFFF) /
                65535.0f;
            std::vector<float>& bucket =
                (p.blendMode == vj::BlendMode::Opaque) ? texVerts : texVertsSemi;
            if (p.kind == vj::PrimitiveKind::Triangle && p.vertices.size() >= 3) {
                pushTriTex(bucket, p.vertices[0], p.vertices[1], p.vertices[2],
                           TPageBaseX, TPageBaseY, colorMul, clutKind, primHash);
                ++drawn;
            } else if (p.kind == vj::PrimitiveKind::Quad && p.vertices.size() >= 4) {
                pushTriTex(bucket, p.vertices[0], p.vertices[1], p.vertices[2],
                           TPageBaseX, TPageBaseY, colorMul, clutKind, primHash);
                pushTriTex(bucket, p.vertices[1], p.vertices[3], p.vertices[2],
                           TPageBaseX, TPageBaseY, colorMul, clutKind, primHash);
                ++drawn;
            }
        }
        glDisable(GL_BLEND);
        submitTex(texVerts, viewportX, viewportY, viewportW, viewportH);
        if (!texVertsSemi.empty()) {
            // Force alpha 0.5 on the semi-transparent bucket (PS1 Average).
            // Stride = 10 floats; alpha index within each vertex is 7.
            for (size_t i = 7; i < texVertsSemi.size(); i += 10) {
                texVertsSemi[i] = 0.5f;
            }
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            submitTex(texVertsSemi, viewportX, viewportY, viewportW, viewportH);
            glDisable(GL_BLEND);
        }
        return drawn;
    }

    void shutdown() {
        if (vbo) vjgl_DeleteBuffers(1, &vbo);
        if (vao) vjgl_DeleteVertexArrays(1, &vao);
        if (program) vjgl_DeleteProgram(program);
        if (texVbo) vjgl_DeleteBuffers(1, &texVbo);
        if (texVao) vjgl_DeleteVertexArrays(1, &texVao);
        if (texProgram) vjgl_DeleteProgram(texProgram);
        if (vramTex) glDeleteTextures(1, &vramTex);
        vbo = vao = texVbo = texVao = 0;
        program = texProgram = 0;
        vramTex = 0;
    }
};

}  // namespace

int main() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* window =
        glfwCreateWindow(1024, 720, "ps1-vj-mix — Spike 1", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!vjglLoad()) {
        std::fprintf(stderr, "GL loader failed (need GL 3.3)\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    Renderer renderer;
    if (!renderer.init()) {
        std::fprintf(stderr, "renderer init failed\n");
    }

    char            pathBuf[512] = "vj-record.vjr";
    LoadedRecording recording;
    LoadStatus      status;
    int             currentFrame = 0;
    bool            playing      = false;
    float           playSpeed    = 1.0f;
    double          lastTickTime = glfwGetTime();
    double          frameAccum   = 0.0;
    int             lastDrawn    = 0;

    // Twin Self: overlay an N-frame-delayed copy of the current frame.
    bool   twinEnabled = false;
    int    twinDelayFrames = 60;  // ~1 second at 60 fps
    float  twinAlpha = 0.5f;      // colour-mul factor for the ghost

    // Live IPC mode: two channels A and B for Phase B mixing.
    struct LiveChannel {
        char                 nameBuf[128];
        vjmix::IpcRingReader reader;
        vj::EchoFrame        building;
        vj::EchoFrame        latest;
        bool                 hasFrame = false;
        int                  framesSeen = 0;
    };
    LiveChannel chA, chB;
    std::strncpy(chA.nameBuf, "Local\\vj-mix-prim-A", sizeof(chA.nameBuf));
    std::strncpy(chB.nameBuf, "Local\\vj-mix-prim-B", sizeof(chB.nameBuf));
    std::vector<uint8_t> liveRecBuf;
    liveRecBuf.resize(64 * 1024);

    // Phase B crossfader: 0 = 100% A, 1 = 100% B. Sources at the midpoint
    // each keep half their primitives via the random gate below.
    float crossfade = 0.0f;
    std::mt19937 cfRng(0x5EED5EED);
    auto rng01 = [&cfRng]() {
        return std::uniform_real_distribution<float>(0.0f, 1.0f)(cfRng);
    };

    // Phase C: VRAM relocation amount for channel B's uploads + UVs.
    // 0   = Phase B (shared VRAM, sources collide → glitches)
    // 512 = Phase C (right half of VRAM for B, clean co-existence)
    // 0..512 interpolated = continuous "collision amount" knob.
    float relocateBX = 0.0f;

    // libvj effects on the mixed stream. The interceptor lives across frames
    // (its internal RandomController + DepthDelayQueue need persistence);
    // each frame we beginFrame with current params, push primitives through,
    // collect them in a thread_local-free scratch vector via the callback,
    // and feed the result to the renderer.
    vj::Params       vjEffectParams;
    vj::PrimitiveInterceptor vjInterceptor;
    bool             vjEffectsEnabled = false;
    std::vector<vj::Primitive> vjPassThruScratch;
    vjInterceptor.setSubmitCallback([&vjPassThruScratch](const vj::Primitive& p) {
        vjPassThruScratch.push_back(p);
    });
    int vjFrameCounter = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        const double now = glfwGetTime();
        const double dt  = now - lastTickTime;
        lastTickTime = now;
        if (playing && !recording.frames.empty()) {
            frameAccum += dt * 60.0 * static_cast<double>(playSpeed);
            while (frameAccum >= 1.0) {
                currentFrame = (currentFrame + 1) %
                               static_cast<int>(recording.frames.size());
                frameAccum -= 1.0;
            }
        }

        // Drain each live channel; commit a frame each time a FrameEnd arrives.
        auto drainChannel = [&](LiveChannel& ch) {
            if (!ch.reader.isOpen()) return;
            for (int safety = 0; safety < 50000; ++safety) {
                vjmix::IpcRecordType type;
                size_t len = 0;
                if (!ch.reader.readRecord(type, liveRecBuf.data(),
                                          liveRecBuf.size(), len)) {
                    break;
                }
                if (len > liveRecBuf.size()) continue;
                if (type == vjmix::IpcRecordType::Primitive) {
                    vj::Primitive p;
                    if (unpackLivePrimitive(liveRecBuf.data(), len, p)) {
                        ch.building.primitives.push_back(std::move(p));
                    }
                } else if (type == vjmix::IpcRecordType::VRAMUpload) {
                    vj::VRAMUpload u;
                    if (unpackLiveUpload(liveRecBuf.data(), len, u)) {
                        ch.building.uploads.push_back(std::move(u));
                    }
                } else if (type == vjmix::IpcRecordType::FrameEnd) {
                    uint32_t fi = 0;
                    if (len >= 4) std::memcpy(&fi, liveRecBuf.data(), 4);
                    ch.building.frameIndex = static_cast<int>(fi);
                    ch.latest = std::move(ch.building);
                    ch.building.primitives.clear();
                    ch.building.uploads.clear();
                    ch.hasFrame = true;
                    ++ch.framesSeen;
                }
            }
        };
        drainChannel(chA);
        drainChannel(chB);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (ImGui::Begin("Controls")) {
            ImGui::TextUnformatted("Live IPC sources (Phase B):");
            auto channelRow = [&](LiveChannel& ch, const char* label) {
                ImGui::PushID(label);
                ImGui::Text("Channel %s:", label);
                ImGui::SetNextItemWidth(-160);
                ImGui::InputText("##name", ch.nameBuf, sizeof(ch.nameBuf),
                                 ch.reader.isOpen() ? ImGuiInputTextFlags_ReadOnly : 0);
                ImGui::SameLine();
                if (ch.reader.isOpen()) {
                    if (ImGui::Button("Detach")) {
                        ch.reader.close();
                        ch.building = vj::EchoFrame{};
                        ch.latest   = vj::EchoFrame{};
                        ch.hasFrame = false;
                        ch.framesSeen = 0;
                    }
                } else {
                    if (ImGui::Button("Attach")) {
                        if (!ch.reader.open(ch.nameBuf)) {
                            std::fprintf(stderr,
                                "[mixer] attach failed for %s\n", ch.nameBuf);
                        }
                    }
                }
                if (ch.reader.isOpen()) {
                    ImGui::Text("  %d frames | dropped=%u | hb=%u",
                                ch.framesSeen, ch.reader.droppedCount(),
                                ch.reader.writerHeartbeat());
                } else {
                    ImGui::TextDisabled("  (idle)");
                }
                ImGui::PopID();
            };
            channelRow(chA, "A");
            channelRow(chB, "B");
            ImGui::SliderFloat("Crossfade A<->B", &crossfade, 0.0f, 1.0f, "%.2f");
            ImGui::TextDisabled("(0=A only, 0.5=both half-density, 1=B only)");
            ImGui::SliderFloat("B VRAM relocate X", &relocateBX, 0.0f, 512.0f, "%.0f");
            ImGui::TextDisabled("(0 = Phase B chaos, 512 = Phase C clean, mid = partial collision)");
            if (ImGui::Button("Phase B (collide)")) relocateBX = 0.0f;
            ImGui::SameLine();
            if (ImGui::Button("Phase C (clean)")) relocateBX = 512.0f;
            ImGui::Separator();

            ImGui::TextUnformatted("CLUT texture handling (PS1 sprites/UI):");
            const char* clutModes[] = {
                "Direct sample (CLUT looks black)",
                "Discard CLUT (silhouette)",
                "Noise CLUT (per-prim hash color)"
            };
            ImGui::Combo("CLUT mode", &renderer.clutMode, clutModes, 3);
            ImGui::Text("Last frame: direct=%d  4bpp=%d  8bpp=%d",
                        renderer.statDirectPrims,
                        renderer.stat4bppPrims,
                        renderer.stat8bppPrims);
            ImGui::Separator();

            ImGui::TextUnformatted("libvj effects on the mixed stream:");
            ImGui::Checkbox("Enable glitch effects", &vjEffectsEnabled);
            if (vjEffectsEnabled) {
                ImGui::SliderFloat("MASTER",   &vjEffectParams.master,   0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("CHANCE",   &vjEffectParams.chance,   0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("GEOMETRY", &vjEffectParams.geometry, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("TEXTURE",  &vjEffectParams.texture,  0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("MISSING",  &vjEffectParams.missing,  0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("COLOR",    &vjEffectParams.color,    0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("DEPTH",    &vjEffectParams.depth,    0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("CHAOS",    &vjEffectParams.chaos,    0.0f, 1.0f, "%.2f");
                if (ImGui::Button("Reset effects")) vjEffectParams = vj::Params{};
            }
            ImGui::Separator();

            ImGui::TextDisabled("Recorded .vjr file:");

            ImGui::SetNextItemWidth(-180);
            ImGui::InputText("##path", pathBuf, sizeof(pathBuf));
            ImGui::SameLine();
            if (ImGui::Button("Open")) {
                if (loadRecording(pathBuf, recording, status)) {
                    currentFrame = 0;
                    playing      = false;
                    frameAccum   = 0.0;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Close") && !recording.frames.empty()) {
                recording.clear();
                status.valid = false;
                status.text  = "(no file loaded)";
                currentFrame = 0;
                playing      = false;
            }

            if (status.valid) {
                ImGui::Text("%s", status.text.c_str());
            } else {
                ImGui::TextDisabled("%s", status.text.c_str());
            }

            ImGui::Separator();

            if (!recording.frames.empty()) {
                const int frameCount = static_cast<int>(recording.frames.size());
                ImGui::SliderInt("Frame", &currentFrame, 0, frameCount - 1);
                if (ImGui::Button(playing ? "Pause" : "Play")) {
                    playing = !playing;
                    frameAccum = 0.0;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120);
                ImGui::SliderFloat("Speed", &playSpeed, 0.1f, 4.0f, "%.2fx");

                const auto& fr = recording.frames[static_cast<size_t>(currentFrame)];
                ImGui::Separator();
                ImGui::Text("Source frameIndex: %d", fr.frameIndex);
                ImGui::Text("Primitives:        %zu", fr.primitives.size());
                ImGui::Text("VRAM uploads:      %zu", fr.uploads.size());
                ImGui::Text("Drawn (total):     %d", lastDrawn);

                ImGui::Separator();
                ImGui::Checkbox("Twin Self (overlay past frame)", &twinEnabled);
                if (twinEnabled) {
                    ImGui::SliderInt("Delay frames", &twinDelayFrames, 1, 300);
                    ImGui::Text("Delay: %.2f s (at 60 fps)", twinDelayFrames / 60.0f);
                    ImGui::SliderFloat("Ghost brightness", &twinAlpha, 0.0f, 1.0f, "%.2f");
                }
            } else {
                ImGui::TextDisabled("(open a .vjr file)");
            }
        }
        ImGui::End();

        int displayW = 0;
        int displayH = 0;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        const bool liveActive =
            (chA.reader.isOpen() && chA.hasFrame) ||
            (chB.reader.isOpen() && chB.hasFrame);
        const bool haveSource = liveActive || !recording.frames.empty();
        if (haveSource && renderer.program) {
            const float aspectPSX = static_cast<float>(kPS1Width) /
                                    static_cast<float>(kPS1Height);
            int vpW = displayW;
            int vpH = static_cast<int>(static_cast<float>(displayW) / aspectPSX);
            if (vpH > displayH) {
                vpH = displayH;
                vpW = static_cast<int>(static_cast<float>(displayH) * aspectPSX);
            }
            const int vpX = (displayW - vpW) / 2;
            const int vpY = (displayH - vpH) / 2;

            lastDrawn = 0;
            if (liveActive) {
                // For each channel, take its frame's primitives, apply the
                // crossfade keep-gate, then optionally run them through
                // libvj's PrimitiveInterceptor for glitch effects, then
                // draw with the right VRAM x-relocation offset.
                auto submitChan = [&](const LiveChannel& ch, float keepProb,
                                      float xRelocate) {
                    if (!ch.hasFrame) return;
                    renderer.applyUploads(ch.latest.uploads,
                                          static_cast<int>(xRelocate));
                    if (keepProb <= 0.0f) return;
                    std::vector<vj::Primitive> kept;
                    kept.reserve(ch.latest.primitives.size());
                    if (keepProb >= 0.999f) {
                        kept = ch.latest.primitives;
                    } else {
                        for (const auto& p : ch.latest.primitives) {
                            if (rng01() < keepProb) kept.push_back(p);
                        }
                    }
                    const std::vector<vj::Primitive>* drawn = &kept;
                    if (vjEffectsEnabled) {
                        vjPassThruScratch.clear();
                        vjInterceptor.beginFrame(vjEffectParams, vjFrameCounter);
                        for (auto& p : kept) vjInterceptor.interceptAndSubmit(p);
                        drawn = &vjPassThruScratch;
                    }
                    lastDrawn += renderer.drawUntextured(*drawn, vpX, vpY, vpW, vpH);
                    lastDrawn += renderer.drawTextured(*drawn, vpX, vpY, vpW, vpH,
                                                       1.0f, xRelocate);
                };
                submitChan(chA, 1.0f - crossfade, 0.0f);
                submitChan(chB, crossfade,        relocateBX);
                ++vjFrameCounter;
            } else {
                // File mode (existing behaviour).
                const vj::EchoFrame& fr =
                    recording.frames[static_cast<size_t>(currentFrame)];
                renderer.applyUploads(fr.uploads);
                if (twinEnabled && !recording.frames.empty()) {
                    const int n = static_cast<int>(recording.frames.size());
                    int ghostFrame = currentFrame - twinDelayFrames;
                    while (ghostFrame < 0) ghostFrame += n;
                    ghostFrame %= n;
                    const auto& gh = recording.frames[static_cast<size_t>(ghostFrame)];
                    renderer.drawUntextured(gh.primitives, vpX, vpY, vpW, vpH, twinAlpha);
                    renderer.drawTextured  (gh.primitives, vpX, vpY, vpW, vpH, twinAlpha);
                }
                lastDrawn  = renderer.drawUntextured(fr.primitives, vpX, vpY, vpW, vpH);
                lastDrawn += renderer.drawTextured  (fr.primitives, vpX, vpY, vpW, vpH);
            }
        } else {
            lastDrawn = 0;
        }

        ImGui::Render();
        glViewport(0, 0, displayW, displayH);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    renderer.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
