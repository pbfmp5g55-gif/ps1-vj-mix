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
#include "vj/AutoMode.h"
#include "vj/FilterPresetBank.h"
#include "vj/Params.h"
#include "vj/PrimitiveInterceptor.h"
#include "vj/PrimitiveStream.h"
#include "vj/RtMidiController.h"

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
[[maybe_unused]] uint32_t convertPSX16toRGBA8(uint16_t px) {
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

// Textured pipeline: per-vertex packs pos / raw PS1 texel u,v / vertex colour
// + a CLUT-kind tag, per-primitive hash, TPage base (VRAM pixel coords) and
// CLUT base (VRAM pixel coords). The fragment shader does its own bpp-aware
// indexing into VRAM (which is now an unsigned 16-bit texture so we can pull
// raw bytes for palette lookup).
const char* kTexVertexSrc = R"(#version 330 core
layout(location = 0) in vec2  a_pos;
layout(location = 1) in vec2  a_uv_raw;     // raw PS1 texel coords (0..255)
layout(location = 2) in vec4  a_color;
layout(location = 3) in float a_clut_kind;
layout(location = 4) in float a_hash;
layout(location = 5) in vec2  a_tpage_base; // VRAM pixel coords
layout(location = 6) in vec2  a_clut_base;  // VRAM pixel coords
uniform vec2 u_psx_size;
out vec2 v_uv_raw;
out vec4 v_color;
flat out int   v_clut_kind;
flat out float v_hash;
flat out vec2  v_tpage_base;
flat out vec2  v_clut_base;
void main() {
    vec2 ndc = vec2(
        (a_pos.x / u_psx_size.x) * 2.0 - 1.0,
        1.0 - (a_pos.y / u_psx_size.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv_raw     = a_uv_raw;
    v_color      = a_color;
    v_clut_kind  = int(a_clut_kind);
    v_hash       = a_hash;
    v_tpage_base = a_tpage_base;
    v_clut_base  = a_clut_base;
}
)";

// CLUT VJ modes:
//   0 = Direct sample (legacy "VJ" mode; CLUT prims read raw VRAM bytes as if
//                      15bpp colour. Looks broken on purpose for 4/8bpp CLUT
//                      sprites; matches the older renderer's behaviour.)
//   1 = Discard CLUT  (CLUT prims drawn fully transparent; silhouette feel)
//   2 = Noise CLUT    (CLUT prims tinted from a per-prim hash; chaos feel)
//   3 = Clean CLUT    (proper PS1 palette indirect lookup: read the nibble/
//                      byte index from the texture page, then read the actual
//                      RGB from the CLUT region of VRAM. This is the only
//                      mode that reproduces the original game graphics.)
const char* kTexFragmentSrc = R"(#version 330 core
in vec2 v_uv_raw;
in vec4 v_color;
flat in int   v_clut_kind;     // 0=direct/15bpp, 1=4bpp CLUT, 2=8bpp CLUT
flat in float v_hash;
flat in vec2  v_tpage_base;
flat in vec2  v_clut_base;
uniform usampler2D u_vram;     // raw 16-bit PS1 VRAM
uniform int u_clut_mode;       // 0=direct, 1=discard, 2=noise, 3=clean
out vec4 frag_color;

vec3 hash3(float h) {
    return fract(vec3(
        sin(h * 12.9898) * 43758.5453,
        sin(h * 78.233 ) * 12345.6789,
        sin(h * 37.719 ) * 91234.5678
    ));
}

vec4 decodePSX(uint p) {
    float r = float((p >>  0u) & 0x1Fu) / 31.0;
    float g = float((p >>  5u) & 0x1Fu) / 31.0;
    float b = float((p >> 10u) & 0x1Fu) / 31.0;
    // PS1 convention: a palette / texture word of exactly 0x0000 means
    // fully transparent. Anything else is opaque (the stp / mask bit
    // governs semi-transparency, which we ignore for now).
    float a = (p == 0u) ? 0.0 : 1.0;
    return vec4(r, g, b, a);
}

void main() {
    int psxU = int(v_uv_raw.x);
    int psxV = int(v_uv_raw.y);
    int tpx  = int(v_tpage_base.x);
    int tpy  = int(v_tpage_base.y);

    // 15bpp direct-colour: no CLUT, sample VRAM at TPage + raw u,v.
    if (v_clut_kind == 0) {
        uint raw = texelFetch(u_vram, ivec2(tpx + psxU, tpy + psxV), 0).r;
        vec4 c = decodePSX(raw);
        if (c.a < 0.5) discard;
        // PS1 modulation: vertex colour 0x80 means "unmodulated" (texture
        // passes through), not 50%. Multiply by 2 and clamp so 0x80 → 1.0×.
        frag_color = vec4(min(c.rgb * v_color.rgb * 2.0, vec3(1.0)), c.a * v_color.a);
        return;
    }

    // CLUT-indexed (4bpp or 8bpp). Mode dictates how we colour.
    if (u_clut_mode == 1) discard;
    if (u_clut_mode == 2) {
        // Noise: per-prim hash mixed with screen-space hash, saturated.
        float pixHash = fract(sin(dot(gl_FragCoord.xy,
                                      vec2(12.9898, 78.233))) * 43758.5453);
        float h = fract(v_hash + pixHash * 0.5);
        vec3 col = hash3(h);
        float m = max(max(col.r, col.g), col.b);
        if (m > 0.0) col /= m;
        frag_color = vec4(col, 1.0);
        return;
    }
    if (u_clut_mode == 0) {
        // Legacy Direct: read VRAM at PSX-texel offset (which is wrong for
        // 4/8bpp — that's the point: it produces the "garbage" look).
        uint raw = texelFetch(u_vram, ivec2(tpx + psxU, tpy + psxV), 0).r;
        vec4 c = decodePSX(raw);
        if (c.a < 0.5) discard;
        frag_color = vec4(min(c.rgb * v_color.rgb * 2.0, vec3(1.0)),
                          c.a * v_color.a);
        return;
    }

    // u_clut_mode == 3: Clean palette lookup.
    uint idx;
    if (v_clut_kind == 1) {
        // 4bpp: 4 nibbles per 16-bit VRAM pixel.
        int vramX = tpx + (psxU >> 2);
        int shift = (psxU & 3) * 4;
        uint raw = texelFetch(u_vram, ivec2(vramX, tpy + psxV), 0).r;
        idx = (raw >> uint(shift)) & 0xFu;
    } else {
        // 8bpp: 2 bytes per 16-bit VRAM pixel.
        int vramX = tpx + (psxU >> 1);
        int shift = (psxU & 1) * 8;
        uint raw = texelFetch(u_vram, ivec2(vramX, tpy + psxV), 0).r;
        idx = (raw >> uint(shift)) & 0xFFu;
    }
    int cx = int(v_clut_base.x);
    int cy = int(v_clut_base.y);
    uint pal = texelFetch(u_vram, ivec2(cx + int(idx), cy), 0).r;
    vec4 c = decodePSX(pal);
    if (c.a < 0.5) discard;
    frag_color = vec4(min(c.rgb * v_color.rgb * 2.0, vec3(1.0)),
                      c.a * v_color.a);
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
    std::vector<uint16_t> vramMirror;  // kVRAMWidth*kVRAMHeight raw 16-bit pixels

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
        constexpr GLsizei kTexStride = 14 * sizeof(float);
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
        vjgl_VertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, kTexStride,
                                 reinterpret_cast<void*>(10 * sizeof(float)));
        vjgl_EnableVertexAttribArray(5);
        vjgl_VertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, kTexStride,
                                 reinterpret_cast<void*>(12 * sizeof(float)));
        vjgl_EnableVertexAttribArray(6);
        vjgl_BindVertexArray(0);

        // VRAM texture: raw 16-bit PS1 VRAM as R16UI so the shader can pull
        // bytes / nibbles for 8bpp / 4bpp CLUT palette indexing.
        vramMirror.assign(static_cast<size_t>(kVRAMWidth) * kVRAMHeight, 0);
        glGenTextures(1, &vramTex);
        glBindTexture(GL_TEXTURE_2D, vramTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, kVRAMWidth, kVRAMHeight, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_SHORT, vramMirror.data());

        return true;
    }

    void applyUploads(const std::vector<vj::VRAMUpload>& uploads,
                      int xRelocate = 0) {
        if (uploads.empty()) return;
        glBindTexture(GL_TEXTURE_2D, vramTex);
        for (const auto& u : uploads) {
            const int ux = u.x + xRelocate;
            if (u.w <= 0 || u.h <= 0) continue;
            if (ux < 0 || u.y < 0) continue;
            if (ux + u.w > kVRAMWidth || u.y + u.h > kVRAMHeight) continue;
            const size_t n = static_cast<size_t>(u.w) * u.h;
            if (u.data.size() < n) continue;
            glTexSubImage2D(GL_TEXTURE_2D, 0, ux, u.y, u.w, u.h,
                            GL_RED_INTEGER, GL_UNSIGNED_SHORT, u.data.data());
            for (int row = 0; row < u.h; ++row) {
                const size_t srcOff = static_cast<size_t>(row) * u.w;
                const size_t dstOff = static_cast<size_t>(u.y + row) * kVRAMWidth + ux;
                std::memcpy(&vramMirror[dstOff], &u.data[srcOff],
                            static_cast<size_t>(u.w) * sizeof(uint16_t));
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
                           float clutBaseX, float clutBaseY,
                           float colorMul,
                           float clutKind, float primHash) {
        auto push = [&](const vj::Vertex& v) {
            out.push_back(v.x);
            out.push_back(v.y);
            // RAW PS1 texel coords; fragment shader applies TPage offset
            // and the appropriate bpp-aware stride itself.
            out.push_back(v.u);
            out.push_back(v.v);
            out.push_back((v.r / 255.0f) * colorMul);
            out.push_back((v.g / 255.0f) * colorMul);
            out.push_back((v.b / 255.0f) * colorMul);
            out.push_back(v.a / 255.0f);
            out.push_back(clutKind);
            out.push_back(primHash);
            out.push_back(TPageBaseX);
            out.push_back(TPageBaseY);
            out.push_back(clutBaseX);
            out.push_back(clutBaseY);
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
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(buf.size() / 14));
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
            // CLUT base lives in hostTag bits 0..15 (the PS1 'clutraw' field).
            //   bits 0..5  = clutX / 16    (X is a multiple of 16, 0..1008)
            //   bits 6..14 = clutY         (0..511)
            const uint32_t clutRaw = static_cast<uint32_t>(p.hostTag & 0xFFFF);
            const float clutBaseX = static_cast<float>((clutRaw & 0x3F) * 16);
            const float clutBaseY = static_cast<float>((clutRaw >> 6) & 0x1FF);
            const float primHash =
                static_cast<float>(static_cast<uint32_t>(p.hostTag * 2654435761u) & 0xFFFF) /
                65535.0f;
            std::vector<float>& bucket =
                (p.blendMode == vj::BlendMode::Opaque) ? texVerts : texVertsSemi;
            if (p.kind == vj::PrimitiveKind::Triangle && p.vertices.size() >= 3) {
                pushTriTex(bucket, p.vertices[0], p.vertices[1], p.vertices[2],
                           TPageBaseX, TPageBaseY, clutBaseX, clutBaseY,
                           colorMul, clutKind, primHash);
                ++drawn;
            } else if (p.kind == vj::PrimitiveKind::Quad && p.vertices.size() >= 4) {
                pushTriTex(bucket, p.vertices[0], p.vertices[1], p.vertices[2],
                           TPageBaseX, TPageBaseY, clutBaseX, clutBaseY,
                           colorMul, clutKind, primHash);
                pushTriTex(bucket, p.vertices[1], p.vertices[3], p.vertices[2],
                           TPageBaseX, TPageBaseY, clutBaseX, clutBaseY,
                           colorMul, clutKind, primHash);
                ++drawn;
            }
        }
        glDisable(GL_BLEND);
        submitTex(texVerts, viewportX, viewportY, viewportW, viewportH);
        if (!texVertsSemi.empty()) {
            // Force alpha 0.5 on the semi-transparent bucket (PS1 Average).
            // Stride = 14 floats; alpha index within each vertex is 7.
            for (size_t i = 7; i < texVertsSemi.size(); i += 14) {
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

    // MIDI controller (libvj's RtMidi-backed). When a port is open and
    // m_midiOverride is on, every frame we poll CC values and overwrite the
    // Twin Self params (and any future MIDI-bound mixer params) from MIDI.
    std::unique_ptr<vj::RtMidiController> midi;
    int  midiPortIndex = -1;          // currently-open port, -1 = none
    bool midiOverrideEnabled = true;  // master switch
    std::vector<std::string> midiPortListCache;
    bool midiPortListCached = false;
    auto refreshMidiPorts = [&]() {
        midiPortListCache = vj::RtMidiController::listPorts();
        midiPortListCached = true;
    };
    // CC assignments. Defaults picked so they don't collide with the 8-axis
    // glitch CCs (20..27) that pcsx-redux uses.
    int twinEnableCC = 13;
    int twinDelayCC  = 14;
    int twinAlphaCC  = 15;
    // (filterPresetCC=16 is declared in the Filter Preset Bank block below.)
    int clutModeCC   = 17;
    int crossfadeCC  = 18;
    int relocateCC   = 19;
    int midiLearnTarget = -1;  // 0..2 = twin, 3 = clutMode, 4 = crossfade, 5 = relocate
    int midiLearnSeenCC = -1;
    auto applyMidiOverrides = [&]() {
        if (!midi || !midiOverrideEnabled) return;
        const int e = midi->getCC(twinEnableCC);
        if (e >= 0) twinEnabled = (e >= 64);
        const int d = midi->getCC(twinDelayCC);
        if (d >= 0) twinDelayFrames = 1 + (d * 299) / 127;  // 1..300
        const int a = midi->getCC(twinAlphaCC);
        if (a >= 0) twinAlpha = static_cast<float>(a) / 127.0f;
    };

    // Live IPC mode: two channels A and B for Phase B mixing. history holds
    // the last few hundred frames so Twin Self / echo effects can overlay a
    // delayed copy of the live stream.
    struct LiveChannel {
        char                 nameBuf[128];
        vjmix::IpcRingReader reader;
        vj::EchoFrame        building;
        vj::EchoFrame        latest;
        bool                 hasFrame = false;
        int                  framesSeen = 0;
        vj::PrimitiveRingbuffer history{300};  // 5 s at 60 fps
    };
    LiveChannel chA, chB;
    std::strncpy(chA.nameBuf, "Local\\vj-mix-prim-A", sizeof(chA.nameBuf));
    std::strncpy(chB.nameBuf, "Local\\vj-mix-prim-B", sizeof(chB.nameBuf));
    std::vector<uint8_t> liveRecBuf;
    // 2 MB receive buffer — fits a full 1024x512 VRAM snapshot (which
    // pcsx-redux v0.7.4+ sends on live::start as up to four 1024x128
    // VRAMUpload records, ~262 KB each). The previous 64 KB cap silently
    // dropped any such record (readRecord sets outLen to the real size
    // and drainChannel skips records where len > buffer size), so the
    // mixer's VRAM mirror never saw the initial palette state and Clean
    // CLUT mode came up blank for games whose palette is uploaded once
    // (Quake, Nekketsu, etc.).
    liveRecBuf.resize(2 * 1024 * 1024);

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

    // Phase B / C MIDI overrides — kept separate from applyMidiOverrides()
    // because their target variables (crossfade, relocateBX, renderer.clutMode)
    // are declared in this scope and Renderer's clutMode lives on the Renderer
    // instance. Same master switch (midiOverrideEnabled).
    auto applyPhaseBmidi = [&]() {
        if (!midi || !midiOverrideEnabled) return;
        const int c = midi->getCC(clutModeCC);
        if (c >= 0) {
            int m = c / 32;            // 0..31 -> 0 ... 96..127 -> 3
            if (m > 3) m = 3;          // 127/32 == 3, but guard anyway
            renderer.clutMode = m;
        }
        const int x = midi->getCC(crossfadeCC);
        if (x >= 0) crossfade = static_cast<float>(x) / 127.0f;
        const int r = midi->getCC(relocateCC);
        if (r >= 0) relocateBX = static_cast<float>(r) / 127.0f * 512.0f;
    };

    // VJ output preferences: hide the GUI + window chrome so the mixer
    // window is a pure VJ surface. Hotkeys:
    //   F1   toggle Controls panel + all ImGui windows
    //   F11  toggle borderless fullscreen on the current monitor
    bool showUI       = true;
    bool borderless   = false;
    bool prevF1Down   = false;
    bool prevF11Down  = false;
    int  saveWinX = 0, saveWinY = 0, saveWinW = 0, saveWinH = 0;

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

    // Filter Preset Bank — 16 slots of FilterParams selectable via a single
    // MIDI CC. Saved/loaded as a .vjbank file alongside the exe.
    vj::FilterPresetBank presetBank;
    int  filterPresetCC        = 16;     // default CC for preset select
    bool filterMidiEnabled     = false;
    bool filterInterpolation   = true;
    int  filterEditingSlot     = 0;
    bool filterLearnArmed      = false;
    int  filterLearnSeenCC     = -1;
    char filterBankPath[256]   = "mixer-filter-bank.vjbank";
    char filterBankStatus[64]  = "";

    // AutoMode — sinewave LFOs that modulate the 8 effect axes on top of the
    // base values from the sliders. Pure function in libvj.
    vj::AutoModeParams autoMode;  // enabled=false by default

    // Extend applyMidiOverrides to also drive the filter preset CC when
    // Filter MIDI is on.
    auto applyFilterMidi = [&]() {
        if (!midi || !filterMidiEnabled) return;
        const int v = midi->getCC(filterPresetCC);
        if (v < 0) return;
        vjEffectParams.filter = filterInterpolation
            ? presetBank.selectInterpolated(v)
            : presetBank.selectSnap(v);
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // VJ hotkeys: F1 hides the GUI overlay, F11 toggles a borderless
        // fullscreen window. Both edge-triggered (act on press, not hold).
        {
            const bool f1Now  = glfwGetKey(window, GLFW_KEY_F1)  == GLFW_PRESS;
            const bool f11Now = glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS;
            if (f1Now && !prevF1Down) showUI = !showUI;
            if (f11Now && !prevF11Down) {
                borderless = !borderless;
                if (borderless) {
                    glfwGetWindowPos(window, &saveWinX, &saveWinY);
                    glfwGetWindowSize(window, &saveWinW, &saveWinH);
                    GLFWmonitor* mon = glfwGetPrimaryMonitor();
                    const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
                    glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
                    if (vm) {
                        glfwSetWindowPos(window, 0, 0);
                        glfwSetWindowSize(window, vm->width, vm->height);
                    }
                } else {
                    glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
                    glfwSetWindowPos(window, saveWinX, saveWinY);
                    glfwSetWindowSize(window,
                                      saveWinW > 0 ? saveWinW : 1024,
                                      saveWinH > 0 ? saveWinH : 720);
                }
            }
            prevF1Down  = f1Now;
            prevF11Down = f11Now;
        }

        // Pull any pending MIDI CC values into Twin Self params before any
        // UI / draw code reads them this frame. Manual UI edits made later
        // in the same frame override these.
        applyMidiOverrides();
        applyFilterMidi();
        applyPhaseBmidi();

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
                    // Push into history for Twin Self / live echo overlay.
                    ch.history.beginFrame(ch.latest.frameIndex);
                    for (const auto& p : ch.latest.primitives) {
                        ch.history.recordPrimitive(p);
                    }
                }
            }
        };
        drainChannel(chA);
        drainChannel(chB);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (showUI) {
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
                "Noise CLUT (per-prim hash color)",
                "Clean CLUT (proper palette lookup)"
            };
            ImGui::Combo("CLUT mode", &renderer.clutMode, clutModes, 4);
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

            // ---------------------------------------------------------------
            // AutoMode: drive the 8 effect axes by sine LFOs.
            // ---------------------------------------------------------------
            if (ImGui::CollapsingHeader("AutoMode (LFO-driven effect axes)")) {
                ImGui::Checkbox("AutoMode enabled", &autoMode.enabled);
                ImGui::SliderFloat("LFO depth", &autoMode.depth, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("LFO rate",  &autoMode.rate,  0.05f, 4.0f, "%.2fx");
                ImGui::TextDisabled("(modulates on top of the slider values above)");
            }

            // ---------------------------------------------------------------
            // Filter Preset Bank: 16 named slots of FilterParams, optionally
            // selected by a single MIDI CC (interpolated or snap mode).
            // ---------------------------------------------------------------
            if (ImGui::CollapsingHeader("Filter Preset Bank")) {
                // Save / Load row.
                ImGui::SetNextItemWidth(220);
                ImGui::InputText("Bank file", filterBankPath, sizeof(filterBankPath));
                if (ImGui::Button("Save bank")) {
                    const bool ok = presetBank.saveTo(filterBankPath);
                    std::snprintf(filterBankStatus, sizeof(filterBankStatus),
                                  "Save: %s", ok ? "OK" : "FAILED");
                }
                ImGui::SameLine();
                if (ImGui::Button("Reload bank")) {
                    const bool ok = presetBank.loadFrom(filterBankPath);
                    std::snprintf(filterBankStatus, sizeof(filterBankStatus),
                                  "Load: %s", ok ? "OK" : "FAILED");
                }
                if (filterBankStatus[0]) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", filterBankStatus);
                }

                ImGui::Separator();
                ImGui::Checkbox("Filter MIDI enabled", &filterMidiEnabled);
                ImGui::SameLine();
                ImGui::Checkbox("Interpolate", &filterInterpolation);

                if (filterLearnArmed && midi) {
                    const int latest = midi->lastReceivedCC();
                    if (latest >= 0 && latest != filterLearnSeenCC) {
                        filterPresetCC = latest;
                        filterLearnArmed = false;
                    }
                }
                ImGui::SetNextItemWidth(90);
                if (ImGui::InputInt("Preset CC", &filterPresetCC, 1, 8)) {
                    if (filterPresetCC < 0) filterPresetCC = 0;
                    if (filterPresetCC > 127) filterPresetCC = 127;
                }
                ImGui::SameLine();
                if (filterLearnArmed) {
                    if (ImGui::Button("Cancel##bank")) filterLearnArmed = false;
                } else {
                    if (ImGui::Button("Learn##bank")) {
                        filterLearnArmed = true;
                        filterLearnSeenCC = midi ? midi->lastReceivedCC() : -1;
                    }
                }
                ImGui::SameLine();
                const int liveCC = midi ? midi->getCC(filterPresetCC) : -1;
                char vbuf[32];
                if (liveCC < 0) std::snprintf(vbuf, sizeof(vbuf), "--");
                else            std::snprintf(vbuf, sizeof(vbuf), "%d", liveCC);
                ImGui::ProgressBar(liveCC < 0 ? 0.0f
                                              : static_cast<float>(liveCC) / 127.0f,
                                   ImVec2(130, 0), vbuf);
                if (liveCC >= 0) {
                    const int s = vj::FilterPresetBank::slotForCC(liveCC);
                    ImGui::TextDisabled("Hot slot: %d \"%s\"", s,
                                        presetBank.slot(s).name.c_str());
                }

                ImGui::Separator();
                ImGui::TextUnformatted("Slots (click to edit):");
                if (ImGui::BeginChild("##slotlist", ImVec2(180, 220), true)) {
                    for (int i = 0; i < vj::FilterPresetBank::slotCount(); ++i) {
                        char label[64];
                        std::snprintf(label, sizeof(label), "%2d  %s", i,
                                      presetBank.slot(i).name.c_str());
                        const bool sel = filterEditingSlot == i;
                        if (ImGui::Selectable(label, sel)) filterEditingSlot = i;
                    }
                }
                ImGui::EndChild();
                ImGui::SameLine();
                ImGui::BeginGroup();
                {
                    auto& slot = presetBank.slot(filterEditingSlot);
                    ImGui::Text("Editing slot %d", filterEditingSlot);
                    char nameBuf[64];
                    std::snprintf(nameBuf, sizeof(nameBuf), "%s", slot.name.c_str());
                    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
                        slot.name = nameBuf;
                    }
                    ImGui::Checkbox("Textured only", &slot.params.texturedOnly);
                    ImGui::SliderFloat("Min area", &slot.params.minArea, 0.0f, 50000.0f, "%.0f");
                    ImGui::SliderFloat("Max area", &slot.params.maxArea, 0.0f, 50000.0f, "%.0f");
                    float region[4] = {
                        slot.params.regionX0, slot.params.regionY0,
                        slot.params.regionX1, slot.params.regionY1,
                    };
                    if (ImGui::SliderFloat4("Region", region, 0.0f, 1024.0f, "%.0f")) {
                        slot.params.regionX0 = region[0];
                        slot.params.regionY0 = region[1];
                        slot.params.regionX1 = region[2];
                        slot.params.regionY1 = region[3];
                    }
                    ImGui::SliderInt("Every N", &slot.params.everyN, 0, 16);

                    if (ImGui::Button("Capture live -> slot")) {
                        slot.params = vjEffectParams.filter;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Apply slot -> live")) {
                        vjEffectParams.filter = slot.params;
                    }
                    if (ImGui::Button("Clear slot")) {
                        slot.params = vj::FilterParams{};
                    }
                }
                ImGui::EndGroup();
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
            } else {
                ImGui::TextDisabled("(open a .vjr file)");
            }

            // Twin Self works against either a loaded file or a live channel's
            // history ringbuffer, so the UI is shown unconditionally.
            ImGui::Separator();
            ImGui::Checkbox("Twin Self (overlay past frame)", &twinEnabled);
            if (twinEnabled) {
                ImGui::SliderInt("Delay frames", &twinDelayFrames, 1, 300);
                ImGui::Text("Delay: %.2f s (at 60 fps)", twinDelayFrames / 60.0f);
                ImGui::SliderFloat("Ghost brightness", &twinAlpha, 0.0f, 1.0f, "%.2f");
                const bool anyAttached = chA.reader.isOpen() || chB.reader.isOpen();
                if (anyAttached) {
                    ImGui::TextDisabled("Live history: A=%d frames, B=%d frames",
                                        chA.history.sizeFrames(),
                                        chB.history.sizeFrames());
                }
            }

            // ---------------------------------------------------------------
            // MIDI section — port selection + Twin Self CC bindings.
            // ---------------------------------------------------------------
            ImGui::Separator();
            ImGui::TextUnformatted("MIDI input:");
            if (!midiPortListCached) refreshMidiPorts();
            const char* portLabel = midi ? midi->portName().c_str()
                                          : "(not open)";
            ImGui::Text("Open: %s", portLabel);
            if (midiPortListCache.empty()) {
                ImGui::TextDisabled("  (no MIDI input ports detected)");
            } else {
                for (size_t i = 0; i < midiPortListCache.size(); ++i) {
                    const int idx = static_cast<int>(i);
                    const bool isOpen = midi && idx == midiPortIndex;
                    char label[256];
                    std::snprintf(label, sizeof(label), "%s [%d] %s",
                                  isOpen ? "[OPEN]" : "      ", idx,
                                  midiPortListCache[i].c_str());
                    if (ImGui::Selectable(label, isOpen)) {
                        if (isOpen) { midi.reset(); midiPortIndex = -1; }
                        else {
                            midi = std::make_unique<vj::RtMidiController>(
                                static_cast<unsigned>(idx), "ps1-vj-mix");
                            midiPortIndex = idx;
                        }
                    }
                }
            }
            if (ImGui::Button("Refresh MIDI")) refreshMidiPorts();
            ImGui::SameLine();
            ImGui::Checkbox("MIDI overrides Twin Self", &midiOverrideEnabled);

            // CC mapping + learn for the three Twin Self params.
            ImGui::TextUnformatted("Twin Self CC bindings:");
            auto bindingRow = [&](const char* label, int* cc, int learnIdx,
                                  const char* hint) {
                ImGui::PushID(label);
                ImGui::Text("%-22s", label);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                if (ImGui::InputInt("CC", cc, 1, 8)) {
                    if (*cc < 0) *cc = 0;
                    if (*cc > 127) *cc = 127;
                }
                ImGui::SameLine();
                const bool armed = midiLearnTarget == learnIdx;
                if (armed) {
                    if (ImGui::Button("Cancel")) midiLearnTarget = -1;
                } else {
                    if (ImGui::Button("Learn")) {
                        midiLearnTarget = learnIdx;
                        midiLearnSeenCC = midi ? midi->lastReceivedCC() : -1;
                    }
                }
                ImGui::SameLine();
                const int v = midi ? midi->getCC(*cc) : -1;
                char vbuf[16];
                if (v < 0) std::snprintf(vbuf, sizeof(vbuf), "--");
                else       std::snprintf(vbuf, sizeof(vbuf), "%d", v);
                ImGui::ProgressBar(v < 0 ? 0.0f : static_cast<float>(v) / 127.0f,
                                   ImVec2(110, 0), vbuf);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", hint);
                ImGui::PopID();
            };
            bindingRow("Twin Self enable",   &twinEnableCC, 0, "(>=64 = on)");
            bindingRow("Twin delay frames",  &twinDelayCC,  1, "(maps 0..127 -> 1..300)");
            bindingRow("Twin ghost bright",  &twinAlphaCC,  2, "(maps 0..127 -> 0..1)");
            ImGui::TextUnformatted("Phase B / C CC bindings:");
            bindingRow("CLUT mode",          &clutModeCC,   3, "(0..31=Dir,32..63=Disc,64..95=Noi,96..127=Cln)");
            bindingRow("Crossfade A<->B",    &crossfadeCC,  4, "(0..127 -> 0..1)");
            bindingRow("B VRAM relocate",    &relocateCC,   5, "(0..127 -> 0..512, B chaos vs C clean)");

            // Process learn: poll lastReceivedCC; when it changes commit it.
            if (midi && midiLearnTarget >= 0) {
                const int latest = midi->lastReceivedCC();
                if (latest >= 0 && latest != midiLearnSeenCC) {
                    switch (midiLearnTarget) {
                        case 0: twinEnableCC = latest; break;
                        case 1: twinDelayCC  = latest; break;
                        case 2: twinAlphaCC  = latest; break;
                        case 3: clutModeCC   = latest; break;
                        case 4: crossfadeCC  = latest; break;
                        case 5: relocateCC   = latest; break;
                    }
                    midiLearnTarget = -1;
                }
                ImGui::TextDisabled("Move a MIDI control to assign...");
            }
        }
        ImGui::End();
        }  // if (showUI)

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
                    // Twin Self ghost: draw delayed history first, dimmed, so
                    // the live frame paints over it.
                    if (twinEnabled && keepProb > 0.0f) {
                        const vj::EchoFrame* gh =
                            ch.history.getDelayed(twinDelayFrames);
                        if (gh && !gh->primitives.empty()) {
                            renderer.drawUntextured(gh->primitives,
                                                    vpX, vpY, vpW, vpH, twinAlpha);
                            renderer.drawTextured(gh->primitives,
                                                  vpX, vpY, vpW, vpH,
                                                  twinAlpha, xRelocate);
                        }
                    }
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
                        const vj::Params effective = autoMode.enabled
                            ? vj::applyAutoMode(vjEffectParams, autoMode, vjFrameCounter)
                            : vjEffectParams;
                        vjInterceptor.beginFrame(effective, vjFrameCounter);
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
