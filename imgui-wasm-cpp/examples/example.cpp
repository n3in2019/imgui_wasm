#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "imgui.h"
#include "imgui_wasm.hpp"

int main(int argc, char** argv) {
    imgui_wasm::Config config;
    config.host_port = argc > 1 ? argv[1] : config.host_port;

#ifdef IMGUI_WASM_EXAMPLE_CALLSTREAM
    // Call-stream mode: disable docking. Docking creates an implicit dockspace
    // host window that shifts the main viewport layout; since the dockspace
    // itself isn't part of the captured widget surface, server and twin would
    // diverge in window placement, making clicks miss. Without docking both
    // contexts lay out windows identically.
    config.config_flags = ImGuiConfigFlags_NavEnableKeyboard;
#endif

    // Enable call-stream transport when the example is built with
    // -DIMGUI_WASM_EXAMPLE_CALLSTREAM. Application drawing code keeps the
    // original ImGui:: API in both modes; the browser replays captured calls.
#ifdef IMGUI_WASM_EXAMPLE_CALLSTREAM
    config.callstream = true;
#endif

    imgui_wasm::Server app;
    if (!app.init(config)) {
        fprintf(stderr, "Failed to initialize ImGuiWasm\n");
        return 1;
    }

    ImGuiIO& io = ImGui::GetIO();

    float f = 0.0f;
    int counter = 0;
    bool show_demo = true;
    bool running = true;
    char buf[256] = "Hello, ImGuiWasm!";

    auto main_render = app.on_render([&]() {
#ifdef IMGUI_WASM_EXAMPLE_CALLSTREAM
        // Call-stream mode uses the same ImGui:: API as ordinary Dear ImGui.
        if (show_demo) {
            ImGui::ShowDemoWindow(&show_demo);
        }
        ImGui::Begin("ImGuiWasm Example (call-stream)", &running);
        ImGui::TextUnformatted("Hello from ImGuiWasm!");
        ImGui::Checkbox("Show Demo Window", &show_demo);
        ImGui::SliderFloat("float", &f, 0.0f, 1.0f);
        if (ImGui::Button("Button")) {
            counter++;
        }
        ImGui::SameLine();
        {
            char label[64];
            snprintf(label, sizeof(label), "counter = %d", counter);
            ImGui::TextUnformatted(label);
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Call-stream transport active.");
        ImGui::End();
#else
        ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        (void)dockspace_id;

        if (show_demo) {
            ImGui::ShowDemoWindow(&show_demo);
        }

        ImGui::Begin("ImGuiWasm Example", &running);
        ImGui::Text("Hello from ImGuiWasm!");
        ImGui::Checkbox("Show Demo Window", &show_demo);

        ImGui::SliderFloat("float", &f, 0.0f, 1.0f);
        ImGui::InputText("string", buf, sizeof(buf));

        if (ImGui::Button("Button")) {
            counter++;
        }
        ImGui::SameLine();
        ImGui::Text("counter = %d", counter);

        ImGui::ColorEdit3("clear color", (float*)&ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);

        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate,
                    io.Framerate);
        ImGui::End();
#endif
    });

    while (running && main_render) {
        app.render();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    return 0;
}
