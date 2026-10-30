// Platform layer: GLFW window + OpenGL 3 renderer for Dear ImGui.
// Everything application-specific lives in app::Application.

#include "app/Application.h"
#include "ui/Theme.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

void glfwErrorCallback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

}  // namespace

int main() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    GLFWwindow* window = glfwCreateWindow(1280, 800, "Nova Panel", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ui::theme::loadFonts(io);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    {
        app::Application application;
        bool vsync = true;

        while (!glfwWindowShouldClose(window)) {
            const auto frameStart = std::chrono::steady_clock::now();
            glfwPollEvents();
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                ImGui_ImplGlfw_Sleep(10);
                continue;
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            application.frame(io.DeltaTime);
            ImGui::Render();

            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            const ImVec4 bg = ui::theme::palette().background;
            glClearColor(bg.x, bg.y, bg.z, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);

            // Apply platform-level settings coming from the state.
            const core::GeneralSettings& general = application.state().settings.general;
            if (general.vsync != vsync) {
                vsync = general.vsync;
                glfwSwapInterval(vsync ? 1 : 0);
            }
            const int fpsLimit = core::kFpsLimitValues[static_cast<std::size_t>(general.fpsLimit)];
            if (!vsync && fpsLimit > 0)
                std::this_thread::sleep_until(frameStart + std::chrono::microseconds(1'000'000 / fpsLimit));
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
