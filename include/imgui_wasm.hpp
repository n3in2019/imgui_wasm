#pragma once

#include <stdint.h>

#ifdef __cplusplus

#include <functional>
#include <memory>
#include <utility>

#include "imgui.h"

namespace imgui_wasm {

struct Config {
    const char* host_port = "127.0.0.1:8888";
    ImGuiConfigFlags config_flags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    bool dark_style = true;
    bool compression = false;
    // Transport is call-stream only (protocol 0x07-0x09): ImGui API calls are
    // streamed to the browser and replayed against a WASM Dear ImGui twin.
    // Supported ImGui:: calls are captured transparently through the public
    // header.
};

class Server {
   public:
    using RenderHandle = std::shared_ptr<void>;

    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&& other) noexcept;
    Server& operator=(Server&& other) noexcept;

    bool init(const Config& config);
    void shutdown();
    void render();

    template <typename DrawFn>
    RenderHandle on_render(DrawFn&& draw) {
        auto fn = std::function<void()>([f = std::forward<DrawFn>(draw)]() mutable { f(); });
        return add_render_callback(std::move(fn));
    }

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    RenderHandle add_render_callback(std::function<void()> fn);
};

}  // namespace imgui_wasm

// Keep the application-facing API identical to Dear ImGui. Supported calls
// are captured transparently for the browser replay twin. Define
// IMGUI_WASM_NO_IMGUI_REDIRECT before including this header to opt out, or
// #undef ImGui after inclusion when raw namespace access is required.
#include "imgui_wasm_capture.hpp"

#endif
