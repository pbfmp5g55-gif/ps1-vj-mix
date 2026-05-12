// Spike 1 main loop. Owns the GLFW window + ImGui context + an optional
// loaded .vjr file (M2) + a tiny GL renderer for untextured polygons
// (M3). M4 will add textured polys; subsequent milestones blend modes etc.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "mixer/gl_loader.h"
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

// Textured pipeline: per-vertex packs pos / world-VRAM uv / vertex colour.
const char* kTexVertexSrc = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;     // world-VRAM coords (with TPage base added)
layout(location = 2) in vec4 a_color;
uniform vec2 u_psx_size;
uniform vec2 u_vram_size;
out vec2 v_uv;
out vec4 v_color;
void main() {
    vec2 ndc = vec2(
        (a_pos.x / u_psx_size.x) * 2.0 - 1.0,
        1.0 - (a_pos.y / u_psx_size.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = a_uv / u_vram_size;
    v_color = a_color;
}
)";

const char* kTexFragmentSrc = R"(#version 330 core
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_vram;
out vec4 frag_color;
void main() {
    vec4 t = texture(u_vram, v_uv);
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
        vjgl_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                                 reinterpret_cast<void*>(0));
        vjgl_EnableVertexAttribArray(0);
        vjgl_VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                                 reinterpret_cast<void*>(2 * sizeof(float)));
        vjgl_EnableVertexAttribArray(1);
        vjgl_VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                                 reinterpret_cast<void*>(4 * sizeof(float)));
        vjgl_EnableVertexAttribArray(2);
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

    void applyUploads(const std::vector<vj::VRAMUpload>& uploads) {
        if (uploads.empty()) return;
        glBindTexture(GL_TEXTURE_2D, vramTex);
        std::vector<uint32_t> patch;
        for (const auto& u : uploads) {
            if (u.w <= 0 || u.h <= 0) continue;
            if (u.x < 0 || u.y < 0) continue;
            if (u.x + u.w > kVRAMWidth || u.y + u.h > kVRAMHeight) continue;
            const size_t n = static_cast<size_t>(u.w) * u.h;
            patch.resize(n);
            for (size_t i = 0; i < n && i < u.data.size(); ++i) {
                patch[i] = convertPSX16toRGBA8(u.data[i]);
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, u.x, u.y, u.w, u.h,
                            GL_RGBA, GL_UNSIGNED_BYTE, patch.data());
            // Mirror the patch so we never lose state.
            for (int row = 0; row < u.h; ++row) {
                const size_t srcOff = static_cast<size_t>(row) * u.w;
                const size_t dstOff = static_cast<size_t>(u.y + row) * kVRAMWidth + u.x;
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
                           float colorMul) {
        auto push = [&](const vj::Vertex& v) {
            out.push_back(v.x);
            out.push_back(v.y);
            out.push_back(TPageBaseX + v.u);
            out.push_back(TPageBaseY + v.v);
            out.push_back((v.r / 255.0f) * colorMul);
            out.push_back((v.g / 255.0f) * colorMul);
            out.push_back((v.b / 255.0f) * colorMul);
            out.push_back(v.a / 255.0f);
        };
        push(a); push(b); push(c);
    }

    void submitTex(const std::vector<float>& buf, int viewportX, int viewportY,
                   int viewportW, int viewportH) {
        if (buf.empty()) return;
        glViewport(viewportX, viewportY, viewportW, viewportH);
        vjgl_UseProgram(texProgram);
        vjgl_Uniform2f(texUPsxSize, static_cast<float>(kPS1Width),
                       static_cast<float>(kPS1Height));
        vjgl_Uniform2f(texUVramSize, static_cast<float>(kVRAMWidth),
                       static_cast<float>(kVRAMHeight));
        vjgl_ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, vramTex);
        vjgl_Uniform1i(texUSampler, 0);
        vjgl_BindVertexArray(texVao);
        vjgl_BindBuffer(GL_ARRAY_BUFFER, texVbo);
        vjgl_BufferData(GL_ARRAY_BUFFER,
                        static_cast<GLsizeiptr_compat>(buf.size() * sizeof(float)),
                        buf.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(buf.size() / 8));
        vjgl_BindVertexArray(0);
    }

    std::vector<float> texVertsSemi;

    int drawTextured(const std::vector<vj::Primitive>& prims,
                     int viewportX, int viewportY,
                     int viewportW, int viewportH,
                     float colorMul = 1.0f) {
        texVerts.clear();
        texVertsSemi.clear();
        int drawn = 0;
        for (const auto& p : prims) {
            if (!p.textured) continue;
            const uint64_t tpageRaw = (p.hostTag >> 24) & 0xFFFF;
            const float TPageBaseX = static_cast<float>((tpageRaw & 0xF) * 64);
            const float TPageBaseY = static_cast<float>(((tpageRaw >> 4) & 0x1) * 256);
            std::vector<float>& bucket =
                (p.blendMode == vj::BlendMode::Opaque) ? texVerts : texVertsSemi;
            if (p.kind == vj::PrimitiveKind::Triangle && p.vertices.size() >= 3) {
                pushTriTex(bucket, p.vertices[0], p.vertices[1], p.vertices[2],
                           TPageBaseX, TPageBaseY, colorMul);
                ++drawn;
            } else if (p.kind == vj::PrimitiveKind::Quad && p.vertices.size() >= 4) {
                pushTriTex(bucket, p.vertices[0], p.vertices[1], p.vertices[2],
                           TPageBaseX, TPageBaseY, colorMul);
                pushTriTex(bucket, p.vertices[1], p.vertices[3], p.vertices[2],
                           TPageBaseX, TPageBaseY, colorMul);
                ++drawn;
            }
        }
        glDisable(GL_BLEND);
        submitTex(texVerts, viewportX, viewportY, viewportW, viewportH);
        if (!texVertsSemi.empty()) {
            // Force alpha 0.5 on the semi-transparent bucket (PS1 Average).
            for (size_t i = 7; i < texVertsSemi.size(); i += 8) {
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

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (ImGui::Begin("Controls")) {
            ImGui::TextDisabled("M3: untextured polygon rendering");
            ImGui::Separator();

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

        if (!recording.frames.empty() && renderer.program) {
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

            // Apply VRAM uploads from the current frame BEFORE any draws.
            const auto& fr =
                recording.frames[static_cast<size_t>(currentFrame)];
            renderer.applyUploads(fr.uploads);

            // Draw the ghost (past frame) first so the live frame overdraws
            // it. Wraps around for files shorter than the requested delay.
            if (twinEnabled) {
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
