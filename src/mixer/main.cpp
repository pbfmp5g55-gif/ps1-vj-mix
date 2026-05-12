// Spike 1 main loop. Owns the GLFW window + ImGui context + an optional
// loaded .vjr file (M2). M3+ will hang the polygon renderer off the same
// loop.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "vj/PrimitiveStream.h"

#include <GLFW/glfw3.h>

namespace {

void glfwErrorCallback(int code, const char* msg) {
    std::fprintf(stderr, "[GLFW] %d: %s\n", code, msg);
}

struct LoadedRecording {
    std::string                  path;
    std::vector<vj::EchoFrame>   frames;
    uint64_t                     totalPrimitives = 0;

    void clear() {
        path.clear();
        frames.clear();
        totalPrimitives = 0;
    }
};

// Status line shown in the UI; gets filled in after a load attempt.
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
        frame = vj::EchoFrame{};  // reset before next read
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
        glfwCreateWindow(960, 640, "ps1-vj-mix — Spike 1", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    char            pathBuf[512] = "vj-record.vjr";
    LoadedRecording recording;
    LoadStatus      status;
    int             currentFrame = 0;
    bool            playing      = false;
    float           playSpeed    = 1.0f;
    double          lastTickTime = glfwGetTime();
    double          frameAccum   = 0.0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Advance playback timing.
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

        if (ImGui::Begin("ps1-vj-mix — Spike 1")) {
            ImGui::TextDisabled("M2: .vjr reader + scrubber");
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
                ImGui::Text("Primitives:         %zu", fr.primitives.size());

                // Quick per-kind histogram for visibility.
                int triCount = 0, quadCount = 0, spriteCount = 0;
                int texturedCount = 0;
                for (const auto& p : fr.primitives) {
                    switch (p.kind) {
                        case vj::PrimitiveKind::Triangle: ++triCount; break;
                        case vj::PrimitiveKind::Quad:     ++quadCount; break;
                        case vj::PrimitiveKind::Sprite:   ++spriteCount; break;
                    }
                    if (p.textured) ++texturedCount;
                }
                ImGui::Text("  triangles: %d", triCount);
                ImGui::Text("  quads:     %d", quadCount);
                ImGui::Text("  sprites:   %d", spriteCount);
                ImGui::Text("  textured:  %d", texturedCount);
            } else {
                ImGui::TextDisabled("(open a .vjr file to scrub through it)");
            }
        }
        ImGui::End();

        ImGui::Render();
        int displayW = 0;
        int displayH = 0;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
