#pragma once

#include <stdint.h>

#ifdef __cplusplus

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "imgui.h"

namespace imgui_wasm {

struct Config {
    const char* host = "127.0.0.1";  // IPv4 dotted quad
    uint16_t port = 8888;
    unsigned max_clients = 0;       // connection caps; 0 = unlimited
    unsigned max_clients_per_ip = 0;
    // PAM-backed HTTP Basic login: a non-empty pam_service makes static
    // pages and WebSocket upgrades require username/password verified via
    // PAM (libpam loaded at runtime). Deploy /etc/pam.d/<service> first.
    // Unprivileged verification covers SSSD/LDAP domains and the process's
    // own user; other local /etc/shadow accounts need root, the shadow
    // group, or a setuid helper (see README). Linux only.
    std::string pam_service;
    // Transport is call-stream only (protocol 0x07-0x09): ImGui API calls are
    // streamed to the browser and replayed against a WASM Dear ImGui twin.
    // Supported ImGui:: calls are captured transparently through the public
    // header. ImGui settings (io.ConfigFlags, styles) belong to the
    // application: set them on the context right after init(), as with any
    // Dear ImGui backend.
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
