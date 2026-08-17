// lib.cpp — public lifecycle C ABI + the core api table consumed by the
// shared ImGui backend (src/imgui_backend.cpp).

#include "core.hpp"
#include "pam_auth.hpp"
#include "server.hpp"

#include <arpa/inet.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include "imgui_wasm.h"
#include "imgui_wasm_internal.h"

namespace imgui_wasm_core {
namespace {

struct GlobalCtx {
    std::shared_ptr<State> state;
    std::shared_ptr<ServerHandle> server;
};

std::mutex g_mutex;
std::unique_ptr<GlobalCtx> g_global;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_backend_initialized{false};
// Guarded by g_mutex: set for the duration of an in-flight init so a racing
// second init cannot start a duplicate server. g_mutex is NOT held across the
// backend init below (backend callbacks call global(), which locks it).
bool g_initializing = false;

GlobalCtx* global() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_global ? g_global.get() : nullptr;
}

// --- backend state ------------------------------------------------------------

struct Backend {
    double time = 0.0;
    uint64_t next_texture_id = 1;
    float display_w = 1280.0f;
    float display_h = 720.0f;
    float display_scale = 1.0f;
};

// --- core api table implementation ---------------------------------------------

imgui_wasm_backend_t* backend_create() {
    float size[2] = {1280.0f, 720.0f};
    if (GlobalCtx* g = global()) g->state->get_display_size(size);
    auto* b = new Backend();
    b->display_w = size[0];
    b->display_h = size[1];
    return reinterpret_cast<imgui_wasm_backend_t*>(b);
}

void backend_destroy(imgui_wasm_backend_t* backend) { delete reinterpret_cast<Backend*>(backend); }

void backend_new_frame(imgui_wasm_backend_t* backend_ptr, double current_time,
                       imgui_wasm_frame_info_t* out_info) {
    if (backend_ptr == nullptr || out_info == nullptr) return;
    Backend* backend = reinterpret_cast<Backend*>(backend_ptr);

    float delta_time =
        backend->time > 0.0 ? float(current_time - backend->time) : 1.0f / 60.0f;
    backend->time = current_time;
    if (delta_time <= 0.0f) delta_time = 1.0f / 10000.0f;

    float size[2] = {backend->display_w, backend->display_h};
    if (GlobalCtx* g = global()) g->state->get_display_size(size);
    if (size[0] > 0.0f && size[1] > 0.0f) {
        backend->display_w = size[0];
        backend->display_h = size[1];
    }
    float scale = 1.0f;
    if (GlobalCtx* g = global()) scale = g->state->get_display_scale();
    if (scale > 0.0f) backend->display_scale = scale;

    out_info->delta_time = delta_time;
    out_info->display_w = backend->display_w;
    out_info->display_h = backend->display_h;
    out_info->display_scale = backend->display_scale;
}

uint64_t backend_alloc_texture_id(imgui_wasm_backend_t* backend_ptr) {
    if (backend_ptr == nullptr) return 0;
    Backend* backend = reinterpret_cast<Backend*>(backend_ptr);
    return backend->next_texture_id++;
}

int begin_frame(float dpx, float dpy, float dsw, float dsh, float fbsx, float fbsy) {
    GlobalCtx* g = global();
    if (g == nullptr) return 0;
    // Call-stream gating: stash the frame header and proceed only when
    // clients are connected.
    return g->state->begin_frame(dpx, dpy, dsw, dsh, fbsx, fbsy) ? 1 : 0;
}

void add_draw_list(const void*, uint32_t, const void*, uint32_t, int, const imgui_wasm_draw_cmd_t*,
                   uint32_t) {
    // Unused: the browser regenerates draw data from the captured calls.
}

void end_frame() {
    GlobalCtx* g = global();
    if (g == nullptr) return;
    // Drain the captured calls and broadcast a 0x07 frame (plus a 0x09
    // string update if new labels were interned this frame).
    g->state->end_callstream_frame();
}

void send_texture(uint64_t id, const uint8_t* pixels, uint32_t len, uint32_t width,
                  uint32_t height) {
    if (pixels == nullptr || len == 0) return;
    GlobalCtx* g = global();
    if (g == nullptr) return;
    std::vector<uint8_t> msg = make_texture_msg(id, width, height, pixels, len);
    g->state->send_texture(id, msg);
}

int backend_poll_event(imgui_wasm_backend_t* backend_ptr, imgui_wasm_event_t* out) {
    GlobalCtx* g = global();
    if (g == nullptr || out == nullptr) return 0;

    auto ev = g->state->try_poll_input();
    if (!ev.has_value()) return 0;

    memset(out, 0, sizeof(*out));
    out->type = ev->ev_type;
    out->display_w = ev->display_w;
    out->display_h = ev->display_h;
    switch (ev->ev_type) {
        case 0:
            out->mouse_move.x = ev->x;
            out->mouse_move.y = ev->y;
            break;
        case 1:
        case 2:
            out->mouse_button.button = ev->button;
            break;
        case 3:
            out->mouse_wheel.dx = ev->wheel_x;
            out->mouse_wheel.dy = ev->wheel_y;
            break;
        case 4:
        case 5:
            out->key.key = ev->key;
            break;
        case 6:
            out->text.ch = ev->character;
            break;
        default:
            break;
    }
    if (backend_ptr != nullptr && out->display_w > 0.0f && out->display_h > 0.0f) {
        Backend* backend = reinterpret_cast<Backend*>(backend_ptr);
        backend->display_w = out->display_w;
        backend->display_h = out->display_h;
    }
    return 1;
}

int get_clipboard_text(char* buf, int buf_size) {
    GlobalCtx* g = global();
    if (g == nullptr) {
        if (buf != nullptr && buf_size > 0) buf[0] = '\0';
        return 0;
    }
    std::string text = g->state->get_clipboard_text();
    int copy_len = int(std::min(text.size(), size_t(buf_size > 0 ? buf_size - 1 : 0)));
    if (copy_len > 0 && buf != nullptr) memcpy(buf, text.data(), size_t(copy_len));
    if (buf != nullptr && buf_size > 0) buf[copy_len] = '\0';
    return copy_len;
}

void set_clipboard_text(const char* text) {
    if (text == nullptr) return;
    GlobalCtx* g = global();
    if (g == nullptr) return;
    g->state->set_clipboard_text(text);
}

const imgui_wasm_core_api_t kCoreApi = {
    backend_create,
    backend_destroy,
    backend_new_frame,
    backend_alloc_texture_id,
    backend_poll_event,
    begin_frame,
    add_draw_list,
    end_frame,
    send_texture,
    get_clipboard_text,
    set_clipboard_text,
};

}  // namespace
}  // namespace imgui_wasm_core

// The shared ImGui backend (src/imgui_backend.cpp) is compiled into this
// library verbatim.
extern "C" {
void imgui_wasm_imgui_backend_set_core_api(const imgui_wasm_core_api_t* api);
bool imgui_wasm_imgui_backend_init(void);
void imgui_wasm_imgui_backend_shutdown();
void imgui_wasm_imgui_backend_begin_frame();
void imgui_wasm_imgui_backend_render();
}

extern "C" int imgui_wasm_init(const imgui_wasm_config_t* config) {
    const char* host = "127.0.0.1";
    uint16_t port = 8888;
    if (config != nullptr) {
        if (config->host != nullptr) host = config->host;
        if (config->port != 0) port = config->port;
    }

    sockaddr_in probe{};
    if (inet_pton(AF_INET, host, &probe.sin_addr) != 1) {
        fprintf(stderr, "[imgui_wasm] Invalid host '%s' (IPv4 dotted quad expected)\n", host);
        return -1;
    }

    {
        // Serialize init start: two threads racing past validation would
        // otherwise start two servers and leak one.
        std::lock_guard<std::mutex> lk(imgui_wasm_core::g_mutex);
        if (imgui_wasm_core::g_initialized.load(std::memory_order_seq_cst) ||
            imgui_wasm_core::g_initializing) {
            fprintf(stderr, "[imgui_wasm] Already initialized\n");
            return 0;
        }
        imgui_wasm_core::g_initializing = true;
    }
    auto initializing_done = [&] {
        std::lock_guard<std::mutex> lk(imgui_wasm_core::g_mutex);
        imgui_wasm_core::g_initializing = false;
    };

    auto state = std::make_shared<imgui_wasm_core::State>();

    imgui_wasm_core::AuthConfig auth;
    if (config != nullptr) {
        auth.max_clients = config->max_clients;
        auth.max_clients_per_ip = config->max_clients_per_ip;
        if (config->pam_service != nullptr && config->pam_service[0] != '\0') {
            std::string err;
            auth.pam = imgui_wasm_core::PamAuth::load(&err);
            if (!auth.pam) {
                fprintf(stderr, "[imgui_wasm] PAM auth requested but unavailable: %s\n",
                        err.c_str());
                initializing_done();
                return -4;
            }
            auth.pam_service = config->pam_service;
        }
    }
    state->set_auth(auth);

    auto server = imgui_wasm_core::run_server(state, host, port);
    if (server == nullptr) {
        initializing_done();
        return -2;
    }

    {
        std::lock_guard<std::mutex> lk(imgui_wasm_core::g_mutex);
        imgui_wasm_core::g_global =
            std::make_unique<imgui_wasm_core::GlobalCtx>(imgui_wasm_core::GlobalCtx{state, server});
    }

    // Enable the capture buffer BEFORE the backend's first frame so host
    // wrapper calls start recording immediately.
    imgui_wasm_core::capture::set_enabled(true);

    imgui_wasm_imgui_backend_set_core_api(&imgui_wasm_core::kCoreApi);
    if (!imgui_wasm_imgui_backend_init()) {
        imgui_wasm_core::capture::set_enabled(false);
        imgui_wasm_core::stop_server(server);
        {
            std::lock_guard<std::mutex> lk(imgui_wasm_core::g_mutex);
            imgui_wasm_core::g_global.reset();
        }
        initializing_done();
        return -3;
    }
    imgui_wasm_core::g_backend_initialized.store(true, std::memory_order_seq_cst);
    imgui_wasm_core::g_initialized.store(true, std::memory_order_seq_cst);
    initializing_done();

    if (!auth.pam_service.empty()) {
        fprintf(stderr, "[imgui_wasm] PAM Basic auth enabled (service '%s')\n",
                auth.pam_service.c_str());
    }
    fprintf(stderr, "[imgui_wasm] Initialized (C++ core, call-stream), open http://%s:%u in your browser\n",
            host, unsigned(port));
    return 0;
}

extern "C" void imgui_wasm_shutdown() {
    if (!imgui_wasm_core::g_initialized.load(std::memory_order_seq_cst)) return;
    if (imgui_wasm_core::g_backend_initialized.exchange(false, std::memory_order_seq_cst)) {
        imgui_wasm_imgui_backend_shutdown();
    }
    imgui_wasm_core::capture::set_enabled(false);
    {
        std::lock_guard<std::mutex> lk(imgui_wasm_core::g_mutex);
        if (imgui_wasm_core::g_global) imgui_wasm_core::stop_server(imgui_wasm_core::g_global->server);
        imgui_wasm_core::g_global.reset();
    }
    imgui_wasm_core::g_initialized.store(false, std::memory_order_seq_cst);
    fprintf(stderr, "[imgui_wasm] Shutdown complete\n");
}

extern "C" void imgui_wasm_new_frame() {
    // Reset the capture buffer BEFORE ImGui::NewFrame() and the host render
    // callback run, so host widget calls are recorded into a fresh buffer.
    imgui_wasm_core::capture::frame_begin();
    imgui_wasm_imgui_backend_begin_frame();
}

extern "C" void imgui_wasm_render() { imgui_wasm_imgui_backend_render(); }
