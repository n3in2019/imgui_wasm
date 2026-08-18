// Pure-C++ ImGuiWasm core example (call-stream transport): the browser
// replays captured ImGui calls against the WASM twin.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "imgui.h"
#include "imgui_wasm.hpp"

int main(int argc, char** argv) {
    imgui_wasm::Config config;
    // Accept "[host:]port" — e.g. "0.0.0.0:9000" or just "9000".
    std::string host_arg;
    if (argc > 1) {
        if (const char* sep = std::strrchr(argv[1], ':')) {
            host_arg.assign(argv[1], size_t(sep - argv[1]));
            config.host = host_arg.c_str();
            config.port = uint16_t(std::atoi(sep + 1));
        } else {
            config.port = uint16_t(std::atoi(argv[1]));
        }
    }
    // Optional PAM-backed Basic auth: IMGUI_WASM_PAM=<service> ./example ...
    // ("1" selects the default "imgui_wasm" service).
    if (const char* pam = std::getenv("IMGUI_WASM_PAM")) {
        config.pam_service = (pam[0] != '\0' && strcmp(pam, "1") != 0) ? pam : "imgui_wasm";
    }

    imgui_wasm::Server app;
    if (!app.init(config)) {
        fprintf(stderr, "Failed to initialize ImGuiWasm (C++ core)\n");
        return 1;
    }

    // Keyboard nav + docking. Docking works over the call stream:
    // DockSpaceOverViewport() and SetNextWindowDockID() are captured and
    // replayed, the effective ConfigFlags ride the 0x07 frame header (the
    // twin mirrors the docking bit), and docking layout changes re-sync
    // through the authoritative INI snapshot.
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;

    float f = 0.0f;
    int counter = 0;
    bool show_demo = true;
    bool running = true;

    auto main_render = app.on_render([&]() {
        // Submit the dockspace before any window it may host.
        ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();
        if (show_demo) {
            ImGui::ShowDemoWindow(&show_demo);
        }
        // Initial layout: dock the example window into the central node once
        // (FirstUseEver); after that the user rearranges freely and the INI
        // snapshot carries the layout to every client.
        ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);
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
