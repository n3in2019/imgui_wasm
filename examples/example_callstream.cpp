// Pure-C++ ImGuiWasm core example (call-stream transport): the browser
// replays captured ImGui calls against the WASM twin.

#include <chrono>
#include <cstdio>
#include <thread>

#include "imgui.h"
#include "imgui_wasm.hpp"

int main(int argc, char** argv) {
    imgui_wasm::Config config;
    config.host_port = argc > 1 ? argv[1] : config.host_port;
    // Docking is disabled: it creates an implicit dockspace host window that
    // shifts the main viewport layout; since the dockspace itself isn't part
    // of the captured widget surface, server and twin would diverge in window
    // placement, making clicks miss.
    config.config_flags = ImGuiConfigFlags_NavEnableKeyboard;

    imgui_wasm::Server app;
    if (!app.init(config)) {
        fprintf(stderr, "Failed to initialize ImGuiWasm (C++ core)\n");
        return 1;
    }

    float f = 0.0f;
    int counter = 0;
    bool show_demo = true;
    bool running = true;

    auto main_render = app.on_render([&]() {
        if (show_demo) {
            ImGui::ShowDemoWindow(&show_demo);
        }
        ImGui::Begin("ImGuiWasm Example (C++ core)", &running);
        ImGui::TextUnformatted("Hello from the pure-C++ core!");
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
        ImGui::TextUnformatted("Call-stream transport active (C++ server).");
        ImGui::End();
    });

    while (running && main_render) {
        app.render();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    return 0;
}
