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

struct Renderer {
    GLuint program = 0;
    GLuint vao     = 0;
    GLuint vbo     = 0;
    GLint  uPsxSize = -1;

    std::vector<float> verts;  // x,y,r,g,b,a per vertex

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

    bool init() {
        GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentSrc);
        program = vjgl_CreateProgram();
        vjgl_AttachShader(program, vs);
        vjgl_AttachShader(program, fs);
        vjgl_LinkProgram(program);
        GLint ok = 0;
        vjgl_GetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            vjgl_GetProgramInfoLog(program, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[program] link failed: %s\n", log);
            return false;
        }
        vjgl_DeleteShader(vs);
        vjgl_DeleteShader(fs);

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
        return true;
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

    int drawUntextured(const std::vector<vj::Primitive>& prims,
                       int viewportX, int viewportY,
                       int viewportW, int viewportH,
                       float colorMul = 1.0f) {
        verts.clear();
        int drawn = 0;
        for (const auto& p : prims) {
            if (p.textured) continue;
            auto pushDim = [&](const vj::Vertex& a, const vj::Vertex& b,
                               const vj::Vertex& c) {
                auto push = [&](const vj::Vertex& v) {
                    verts.push_back(v.x);
                    verts.push_back(v.y);
                    verts.push_back((v.r / 255.0f) * colorMul);
                    verts.push_back((v.g / 255.0f) * colorMul);
                    verts.push_back((v.b / 255.0f) * colorMul);
                    verts.push_back(v.a / 255.0f);
                };
                push(a); push(b); push(c);
            };
            if (p.kind == vj::PrimitiveKind::Triangle &&
                p.vertices.size() >= 3) {
                pushDim(p.vertices[0], p.vertices[1], p.vertices[2]);
                ++drawn;
            } else if (p.kind == vj::PrimitiveKind::Quad &&
                       p.vertices.size() >= 4) {
                pushDim(p.vertices[0], p.vertices[1], p.vertices[2]);
                pushDim(p.vertices[1], p.vertices[3], p.vertices[2]);
                ++drawn;
            }
        }
        if (verts.empty()) return drawn;

        glViewport(viewportX, viewportY, viewportW, viewportH);
        vjgl_UseProgram(program);
        vjgl_Uniform2f(uPsxSize, static_cast<float>(kPS1Width),
                       static_cast<float>(kPS1Height));
        vjgl_BindVertexArray(vao);
        vjgl_BindBuffer(GL_ARRAY_BUFFER, vbo);
        vjgl_BufferData(GL_ARRAY_BUFFER,
                        static_cast<GLsizeiptr_compat>(verts.size() * sizeof(float)),
                        verts.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0,
                     static_cast<GLsizei>(verts.size() / 6));
        vjgl_BindVertexArray(0);
        return drawn;
    }

    void shutdown() {
        if (vbo) vjgl_DeleteBuffers(1, &vbo);
        if (vao) vjgl_DeleteVertexArrays(1, &vao);
        if (program) vjgl_DeleteProgram(program);
        vbo = vao = 0;
        program = 0;
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
                ImGui::Text("Drawn (untextured): %d", lastDrawn);

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

            // Draw the ghost (past frame) first so the live frame overdraws
            // it. Wraps around for files shorter than the requested delay.
            if (twinEnabled) {
                const int n = static_cast<int>(recording.frames.size());
                int ghostFrame = currentFrame - twinDelayFrames;
                while (ghostFrame < 0) ghostFrame += n;
                ghostFrame %= n;
                const auto& gh = recording.frames[static_cast<size_t>(ghostFrame)];
                renderer.drawUntextured(gh.primitives, vpX, vpY, vpW, vpH, twinAlpha);
            }
            const auto& fr =
                recording.frames[static_cast<size_t>(currentFrame)];
            lastDrawn = renderer.drawUntextured(fr.primitives, vpX, vpY, vpW, vpH);
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
