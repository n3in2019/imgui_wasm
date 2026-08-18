// core.hpp — shared internal declarations for the pure-C++ ImGuiWasm core.
//
// The server captures ImGui API calls and streams them (messages
// 0x07/0x09) for the browser's WASM replay twin.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace imgui_wasm_core {

using ClientId = uint32_t;

struct InputEvent {
    int32_t ev_type = 0;
    float x = 0.0f, y = 0.0f;
    int32_t button = 0;
    int32_t key = 0;
    uint32_t character = 0;
    float wheel_x = 0.0f, wheel_y = 0.0f;
    float display_w = 0.0f, display_h = 0.0f;
};

struct ClientMsg {
    enum class Kind { HelloAck, Input, Resize, ClipboardText };
    Kind kind = Kind::Input;
    uint32_t capabilities = 0;              // HelloAck
    InputEvent input;                       // Input
    float resize_w = 0.0f;                  // Resize
    float resize_h = 0.0f;                  // Resize
    float resize_scale = 0.0f;              // Resize (devicePixelRatio; 0 = absent, legacy 13-byte messages omit it)
    std::string clipboard_text;             // ClipboardText
};

// --- Client message parsing (state.cpp) ------------------------------------

std::optional<std::vector<std::pair<ClientId, ClientMsg>>> parse_client_msgs(
    const uint8_t* data, size_t len);
std::optional<uint32_t> leading_hello_ack(
    const std::vector<std::pair<ClientId, ClientMsg>>& msgs, ClientId expected);
std::vector<uint8_t> make_clipboard_write_msg(const std::string& text);

// --- Call-stream wire format (callstream_protocol.cpp) ----------------------

struct FrameHeader {
    float dpx = 0.0f, dpy = 0.0f, dsw = 0.0f, dsh = 0.0f, fbsx = 1.0f, fbsy = 1.0f;
    // Effective ImGuiIO::ConfigFlags the frame was produced with. The replay
    // twin mirrors the docking bit so DockSpace* calls replay with identical
    // semantics (off server-side = off twin-side, and vice versa). Other bits
    // are reserved.
    uint32_t imgui_flags = 0;
};

std::vector<uint8_t> serialize_callstream_frame(FrameHeader header, uint32_t frame_id,
                                                const uint8_t* call_bytes, size_t call_len,
                                                uint32_t call_count);
std::vector<uint8_t> serialize_string_update(
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& strings);
uint32_t callstream_frame_hash(FrameHeader header, const uint8_t* call_bytes, size_t call_len);
std::vector<uint8_t> make_texture_msg(uint64_t id, uint32_t width, uint32_t height,
                                      const uint8_t* pixels, size_t len);

// --- Capture buffer helpers (capture.cpp) -----------------------------------
// The imgui_wasm_capture_* C ABI (consumed by the generated host wrappers in
// imgui_wasm_capture.hpp) is implemented in capture.cpp.

namespace capture {
void set_enabled(bool enabled);
bool is_enabled();
void frame_begin();  // reset the thread-local buffer at frame start
std::vector<uint8_t> frame_drain();
std::vector<std::pair<uint32_t, std::vector<uint8_t>>> drain_new_strings();
std::vector<std::pair<uint32_t, std::vector<uint8_t>>> snapshot_all_strings();
}  // namespace capture

// --- Per-client outbound mailbox --------------------------------------------
//
// Control messages (textures, string tables, clipboard) are prerequisites
// of frames, so the sender thread always drains them first. The frame slot
// keeps only the newest frame, shared immutably across clients (one
// serialization, refcounted fan-out); `unobserved` is cleared by the socket
// sender before the send, so the coalescing detector only trips on frames
// truly replaced unobserved.

struct OutBox {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> control;
    std::shared_ptr<const std::vector<uint8_t>> frame;
    uint64_t frame_seq = 0;
    uint64_t sent_seq = 0;
    std::atomic<bool> unobserved{false};
    std::atomic<bool> closed{false};
};

struct ClientState {
    float display_size[2] = {1280.0f, 720.0f};
    float display_scale = 1.0f;  // client devicePixelRatio (layout space stays CSS pixels)
    size_t force_frames = 3;
    uint32_t last_call_hash = 0;  // hash of the last frame this client received
    std::string clipboard_text;
    uint32_t capabilities = 0;
    uint32_t peer_addr = 0;  // network byte order; 0 = unknown
    // Pending input with global arrival sequence numbers; the poller takes
    // the lowest sequence across clients, preserving cross-client FIFO.
    std::deque<std::pair<uint64_t, InputEvent>> input;
    std::shared_ptr<OutBox> out = std::make_shared<OutBox>();
};

// --- Connection auth + capacity caps -----------------------------------------

class PamAuth;

struct AuthConfig {
    uint32_t max_clients = 0;         // 0 = unlimited
    uint32_t max_clients_per_ip = 0;  // 0 = unlimited
    // Non-empty + a loaded PamAuth = HTTP Basic credentials are verified
    // against this PAM service (static pages and WebSocket upgrades).
    std::string pam_service;
    std::shared_ptr<PamAuth> pam;
};

// --- Shared server state (state.cpp) ----------------------------------------

class State {
   public:
    ClientId add_client(uint32_t peer_addr = 0);
    void remove_client(ClientId id);
    bool has_clients() const;

    void set_auth(const AuthConfig& auth);
    // Within max_clients / max_clients_per_ip. Friction-level: concurrent
    // handshakes can briefly exceed the cap.
    bool connection_allowed(uint32_t peer_addr) const;
    bool pam_auth_enabled() const;
    // Runs the PAM conversation for the configured service.
    bool pam_verify(const std::string& user, const std::string& password);

    void push_input(ClientId id, const InputEvent& ev);
    std::optional<InputEvent> try_poll_input();

    void set_client_capabilities(ClientId id, uint32_t capabilities);
    void set_display_size(ClientId id, float w, float h, float scale = 0.0f);
    // scale <= 0 means "not provided" and keeps the client's last known value.
    void get_display_size(float out[2]) const;
    float get_display_scale() const;  // active client's devicePixelRatio, 1.0 when unknown

    void set_clipboard_text(const std::string& text);
    std::string get_clipboard_text() const;
    void on_clipboard_text(ClientId id, const std::string& text);

    void send_control(const std::vector<uint8_t>& data);
    void send_control_to(ClientId id, const std::vector<uint8_t>& data);
    void send_frame(const std::shared_ptr<const std::vector<uint8_t>>& data);
    void send_texture(uint64_t id, const std::vector<uint8_t>& msg);
    std::vector<std::vector<uint8_t>> snapshot_textures() const;

    // Render-path entry points.
    bool begin_frame(float dpx, float dpy, float dsw, float dsh, float fbsx, float fbsy,
                     uint32_t imgui_flags = 0);
    void end_callstream_frame();

    // Test seam: direct access to the client table under its lock.
    template <typename F>
    void with_clients(F&& f) {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        f(clients_);
    }

   private:
    void send_frame_to(ClientId id, const std::shared_ptr<const std::vector<uint8_t>>& data);
    void send_frame_to_locked(ClientId id, const std::shared_ptr<const std::vector<uint8_t>>& data);

    mutable std::mutex clients_mtx_;
    std::unordered_map<ClientId, ClientState> clients_;
    ClientId next_client_id_ = 1;
    mutable std::mutex active_mtx_;
    std::optional<ClientId> active_client_;

    mutable std::mutex auth_mtx_;
    AuthConfig auth_;

    std::atomic<uint64_t> next_input_seq_{1};

    mutable std::mutex textures_mtx_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> textures_;

    std::mutex header_mtx_;
    FrameHeader header_;
    // Canvas size the most recent frame was laid out at (draw-data DisplaySize).
    // Frame-thread only: written by begin_frame, read by try_poll_input.
    float layout_size_[2] = {1280.0f, 720.0f};
    std::atomic<uint32_t> callstream_frame_id_{1};
};

// --- Embedded frontend assets (generated embedded_assets.cpp) ---------------

struct Asset {
    const char* url_path;
    const char* content_type;
    const unsigned char* data;
    size_t size;
};

extern const Asset kEmbeddedAssets[];
extern const size_t kEmbeddedAssetCount;

}  // namespace imgui_wasm_core
