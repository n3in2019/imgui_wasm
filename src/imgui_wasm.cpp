#include "imgui_wasm.hpp"
#include "imgui_wasm.h"

#include <utility>
#include <vector>

namespace imgui_wasm {

struct RenderSlot {
    std::function<void()> fn;
};

class Server::Impl {
   public:
    bool init(const Config& config) {
        if (initialized) return true;

        imgui_wasm_config_t cfg = {};
        cfg.host = config.host;
        cfg.port = config.port;
        cfg.max_clients = config.max_clients;
        cfg.max_clients_per_ip = config.max_clients_per_ip;
        cfg.pam_service = config.pam_service.empty() ? nullptr : config.pam_service.c_str();
        if (imgui_wasm_init(&cfg) != 0) {
            return false;
        }
        initialized = true;
        backend_initialized = true;

        return true;
    }

    void shutdown() {
        slots.clear();
        if (backend_initialized) {
            imgui_wasm_shutdown();
            backend_initialized = false;
        }
        initialized = false;
    }

    void render() {
        if (!initialized) return;

        imgui_wasm_new_frame();

        for (auto it = slots.begin(); it != slots.end();) {
            if (auto slot = it->lock()) {
                slot->fn();
                ++it;
            } else {
                it = slots.erase(it);
            }
        }

        imgui_wasm_render();
    }

    std::shared_ptr<RenderSlot> add_slot(std::function<void()> fn) {
        auto slot = std::make_shared<RenderSlot>();
        slot->fn = std::move(fn);
        slots.push_back(slot);
        return slot;
    }

    bool initialized = false;
    bool backend_initialized = false;
    std::vector<std::weak_ptr<RenderSlot>> slots;
};

Server::Server() : impl_(new Impl()) {}

Server::~Server() {
    if (impl_) impl_->shutdown();
}

Server::Server(Server&& other) noexcept = default;

Server& Server::operator=(Server&& other) noexcept = default;

bool Server::init(const Config& config) {
    return impl_->init(config);
}

void Server::shutdown() {
    impl_->shutdown();
}

void Server::render() {
    impl_->render();
}

Server::RenderHandle Server::add_render_callback(std::function<void()> fn) {
    return impl_->add_slot(std::move(fn));
}

}  // namespace imgui_wasm
