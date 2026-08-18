// state.cpp — shared server state: clients, input queue, clipboard, textures,
// and the frame path. Frames are built from the capture buffer.

#include "core.hpp"
#include "pam_auth.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace imgui_wasm_core {

namespace {

// Little-endian readers over a (ptr, len) view: client payloads are parsed in
// place, without copying the receive buffer.

bool read_f32(const uint8_t* d, size_t len, size_t off, float& out) {
    if (off + 4 > len) return false;
    uint32_t bits = uint32_t(d[off]) | (uint32_t(d[off + 1]) << 8) | (uint32_t(d[off + 2]) << 16) |
                    (uint32_t(d[off + 3]) << 24);
    memcpy(&out, &bits, 4);
    return true;
}

bool read_u16(const uint8_t* d, size_t len, size_t off, uint16_t& out) {
    if (off + 2 > len) return false;
    out = uint16_t(d[off]) | (uint16_t(d[off + 1]) << 8);
    return true;
}

bool read_u32(const uint8_t* d, size_t len, size_t off, uint32_t& out) {
    if (off + 4 > len) return false;
    out = uint32_t(d[off]) | (uint32_t(d[off + 1]) << 8) | (uint32_t(d[off + 2]) << 16) |
          (uint32_t(d[off + 3]) << 24);
    return true;
}

std::optional<ClientMsg> parse_client_payload(uint8_t type, const uint8_t* d, size_t len) {
    ClientMsg msg;
    switch (type) {
        case 0x10: {  // mouse move
            msg.kind = ClientMsg::Kind::Input;
            msg.input.ev_type = 0;
            if (!read_f32(d, len, 0, msg.input.x) || !read_f32(d, len, 4, msg.input.y))
                return std::nullopt;
            return msg;
        }
        case 0x11:  // mouse down
        case 0x12: {  // mouse up
            msg.kind = ClientMsg::Kind::Input;
            msg.input.ev_type = (type == 0x11) ? 1 : 2;
            if (len < 1) return std::nullopt;
            msg.input.button = int32_t(d[0]);
            return msg;
        }
        case 0x13: {  // mouse wheel
            msg.kind = ClientMsg::Kind::Input;
            msg.input.ev_type = 3;
            if (!read_f32(d, len, 0, msg.input.wheel_x) || !read_f32(d, len, 4, msg.input.wheel_y))
                return std::nullopt;
            return msg;
        }
        case 0x14:  // key down
        case 0x15: {  // key up
            msg.kind = ClientMsg::Kind::Input;
            msg.input.ev_type = (type == 0x14) ? 4 : 5;
            uint16_t key;
            if (!read_u16(d, len, 0, key)) return std::nullopt;
            msg.input.key = int32_t(key);
            return msg;
        }
        case 0x16: {  // text input
            msg.kind = ClientMsg::Kind::Input;
            msg.input.ev_type = 6;
            uint32_t ch;
            if (!read_u32(d, len, 0, ch)) return std::nullopt;
            msg.input.character = ch;
            return msg;
        }
        case 0x17: {  // resize (scale is optional: legacy 13-byte messages omit it)
            msg.kind = ClientMsg::Kind::Resize;
            if (!read_f32(d, len, 0, msg.resize_w) || !read_f32(d, len, 4, msg.resize_h))
                return std::nullopt;
            // Absent or nonsensical scale stays 0 ("not provided").
            if (len >= 12) {
                if (!read_f32(d, len, 8, msg.resize_scale)) return std::nullopt;
                if (!(msg.resize_scale > 0.0f)) msg.resize_scale = 0.0f;
            }
            return msg;
        }
        case 0x18: {  // clipboard text from client
            msg.kind = ClientMsg::Kind::ClipboardText;
            uint32_t text_len;
            if (!read_u32(d, len, 0, text_len)) return std::nullopt;
            if (size_t(4) + text_len > len) return std::nullopt;
            msg.clipboard_text.assign(reinterpret_cast<const char*>(d) + 4, text_len);
            return msg;
        }
        case 0x1a: {  // capability ack
            msg.kind = ClientMsg::Kind::HelloAck;
            uint32_t caps;
            if (!read_u32(d, len, 0, caps)) return std::nullopt;
            msg.capabilities = caps;
            return msg;
        }
        default:
            return std::nullopt;
    }
}

}  // namespace

std::optional<std::vector<std::pair<ClientId, ClientMsg>>> parse_client_msgs(
    const uint8_t* data, size_t len) {
    std::vector<std::pair<ClientId, ClientMsg>> messages;

    if (len == 0) return std::nullopt;

    if (data[0] != 0x19) {
        // Legacy single message.
        if (len < 5) return std::nullopt;
        uint32_t id;
        if (!read_u32(data, len, 1, id)) return std::nullopt;
        auto payload = parse_client_payload(data[0], data + 5, len - 5);
        if (!payload) return std::nullopt;
        messages.emplace_back(id, std::move(*payload));
        return messages;
    }

    // 0x19 batch envelope: records share the outer client id and contain
    // schema-sized payloads without the repeated four-byte id.
    uint32_t id;
    uint16_t count;
    if (!read_u32(data, len, 1, id) || !read_u16(data, len, 5, count)) return std::nullopt;
    size_t off = 7;
    messages.reserve(count);
    for (size_t i = 0; i < count; i++) {
        if (off + 3 > len) return std::nullopt;
        uint8_t msg_type = data[off];
        uint16_t payload_len = uint16_t(data[off + 1]) | (uint16_t(data[off + 2]) << 8);
        off += 3;
        if (off + payload_len > len) return std::nullopt;
        auto payload = parse_client_payload(msg_type, data + off, payload_len);
        if (!payload) return std::nullopt;
        off += payload_len;
        messages.emplace_back(id, std::move(*payload));
    }
    if (off != len) return std::nullopt;
    return messages;
}

std::optional<uint32_t> leading_hello_ack(
    const std::vector<std::pair<ClientId, ClientMsg>>& msgs, ClientId expected) {
    // Handshake acceptance: the capability ack must lead its WebSocket
    // message; anything else leaves the handshake window waiting.
    if (msgs.empty()) return std::nullopt;
    const auto& [cid, msg] = msgs.front();
    if (msg.kind != ClientMsg::Kind::HelloAck) return std::nullopt;
    if (cid != expected) return std::nullopt;
    return msg.capabilities;
}

std::vector<uint8_t> make_clipboard_write_msg(const std::string& text) {
    std::vector<uint8_t> msg;
    msg.reserve(1 + 4 + text.size());
    msg.push_back(0x18);
    uint32_t len = uint32_t(text.size());
    for (int i = 0; i < 4; i++) msg.push_back(uint8_t(len >> (8 * i)));
    msg.insert(msg.end(), text.begin(), text.end());
    return msg;
}

// --- State -------------------------------------------------------------------

ClientId State::add_client(uint32_t peer_addr) {
    ClientId id;
    std::shared_ptr<OutBox> out;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        id = next_client_id_++;
        auto [it, inserted] = clients_.emplace(id, ClientState{});
        it->second.peer_addr = peer_addr;
        out = it->second.out;
    }
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        if (!active_client_.has_value()) active_client_ = id;
    }
    return id;
}

void State::remove_client(ClientId id) {
    std::shared_ptr<OutBox> out;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(id);
        if (it != clients_.end()) {
            out = it->second.out;
            clients_.erase(it);
        }
    }
    if (out) {
        out->closed.store(true, std::memory_order_release);
        out->cv.notify_all();
    }
    std::lock_guard<std::mutex> lk(active_mtx_);
    if (active_client_ == id) {
        std::lock_guard<std::mutex> clk(clients_mtx_);
        active_client_ = clients_.empty() ? std::nullopt
                                          : std::optional<ClientId>(clients_.begin()->first);
    }
}

bool State::has_clients() const {
    std::lock_guard<std::mutex> lk(clients_mtx_);
    return !clients_.empty();
}

void State::set_auth(const AuthConfig& auth) {
    std::lock_guard<std::mutex> lk(auth_mtx_);
    auth_ = auth;
}

bool State::connection_allowed(uint32_t peer_addr) const {
    uint32_t max_total;
    uint32_t max_ip;
    {
        std::lock_guard<std::mutex> lk(auth_mtx_);
        max_total = auth_.max_clients;
        max_ip = auth_.max_clients_per_ip;
    }
    if (max_total == 0 && max_ip == 0) return true;
    std::lock_guard<std::mutex> lk(clients_mtx_);
    if (max_total != 0 && clients_.size() >= max_total) return false;
    if (max_ip != 0) {
        size_t from_peer = 0;
        for (const auto& [id, cs] : clients_) {
            if (cs.peer_addr == peer_addr) from_peer++;
        }
        if (from_peer >= max_ip) return false;
    }
    return true;
}

bool State::pam_auth_enabled() const {
    std::lock_guard<std::mutex> lk(auth_mtx_);
    return !auth_.pam_service.empty() && auth_.pam != nullptr;
}

bool State::pam_verify(const std::string& user, const std::string& password) {
    std::string service;
    std::shared_ptr<PamAuth> pam;
    {
        std::lock_guard<std::mutex> lk(auth_mtx_);
        service = auth_.pam_service;
        pam = auth_.pam;
    }
    if (!pam) return false;
    std::string err;
    bool ok = pam->verify(service, user, password, &err);
    if (!ok) {
        fprintf(stderr, "[imgui_wasm] PAM rejected user '%s': %s\n", user.c_str(), err.c_str());
    }
    return ok;
}

void State::push_input(ClientId id, const InputEvent& ev) {
    uint64_t seq = next_input_seq_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(id);
        // Events for a vanished client are dropped rather than parked in a
        // global queue someone else would have to drain.
        if (it != clients_.end()) it->second.input.emplace_back(seq, ev);
    }
    std::lock_guard<std::mutex> lk(active_mtx_);
    active_client_ = id;
}

std::optional<InputEvent> State::try_poll_input() {
    std::optional<InputEvent> out;
    ClientId polled = 0;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        // Lowest arrival sequence across clients: same order the old global
        // FIFO produced, now without cross-client queue sharing.
        //
        // Positional events (move/buttons/wheel, types 0-3) are held until
        // the authoritative layout was computed at the sender's canvas size:
        // ImGui hit-tests against the previous frame's geometry, so applying
        // them earlier — right after a differently-sized client became
        // active — would misclick. The server re-layouts to the interacting
        // client each frame, so the wait is exactly one frame. Keyboard and
        // text events carry no geometry; they are only ever delayed by
        // in-order queueing behind a held positional event.
        auto best = clients_.end();
        for (auto it = clients_.begin(); it != clients_.end(); ++it) {
            auto& q = it->second.input;
            if (q.empty()) continue;
            if (q.front().second.ev_type <= 3) {
                const float* sz = it->second.display_size;
                if (sz[0] != layout_size_[0] || sz[1] != layout_size_[1]) continue;
            }
            if (best == clients_.end() || q.front().first < best->second.input.front().first) {
                best = it;
            }
        }
        if (best != clients_.end()) {
            ClientState& cs = best->second;
            out = cs.input.front().second;
            out->display_w = cs.display_size[0];
            out->display_h = cs.display_size[1];
            polled = best->first;
            cs.input.pop_front();
        }
    }
    if (out.has_value()) {
        std::lock_guard<std::mutex> lk(active_mtx_);
        active_client_ = polled;
    }
    return out;
}

void State::set_client_capabilities(ClientId id, uint32_t capabilities) {
    std::lock_guard<std::mutex> lk(clients_mtx_);
    auto it = clients_.find(id);
    if (it != clients_.end()) it->second.capabilities = capabilities;
}

void State::set_display_size(ClientId id, float w, float h, float scale) {
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        it->second.display_size[0] = w;
        it->second.display_size[1] = h;
        if (scale > 0.0f) it->second.display_scale = scale;
    }
    // Select the resized client before its first pointer event. This
    // prevents a click from changing the shared viewport mid-frame.
    std::lock_guard<std::mutex> lk(active_mtx_);
    active_client_ = id;
}

void State::get_display_size(float out[2]) const {
    std::optional<ClientId> id;
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        id = active_client_;
    }
    if (!id.has_value()) {
        out[0] = 1280.0f;
        out[1] = 720.0f;
        return;
    }
    std::lock_guard<std::mutex> lk(clients_mtx_);
    auto it = clients_.find(*id);
    if (it != clients_.end()) {
        out[0] = it->second.display_size[0];
        out[1] = it->second.display_size[1];
    } else {
        out[0] = 1280.0f;
        out[1] = 720.0f;
    }
}

float State::get_display_scale() const {
    std::optional<ClientId> id;
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        id = active_client_;
    }
    if (!id.has_value()) return 1.0f;
    std::lock_guard<std::mutex> lk(clients_mtx_);
    auto it = clients_.find(*id);
    return it != clients_.end() ? it->second.display_scale : 1.0f;
}

void State::set_clipboard_text(const std::string& text) {
    std::optional<ClientId> id;
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        id = active_client_;
    }
    if (!id.has_value()) return;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(*id);
        if (it != clients_.end()) it->second.clipboard_text = text;
    }
    send_control_to(*id, make_clipboard_write_msg(text));
}

std::string State::get_clipboard_text() const {
    std::optional<ClientId> id;
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        id = active_client_;
    }
    if (!id.has_value()) return std::string();
    std::lock_guard<std::mutex> lk(clients_mtx_);
    auto it = clients_.find(*id);
    return it != clients_.end() ? it->second.clipboard_text : std::string();
}

void State::on_clipboard_text(ClientId id, const std::string& text) {
    std::lock_guard<std::mutex> lk(clients_mtx_);
    auto it = clients_.find(id);
    if (it != clients_.end()) it->second.clipboard_text = text;
}

void State::send_control(const std::vector<uint8_t>& data) {
    std::vector<std::shared_ptr<OutBox>> outs;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        outs.reserve(clients_.size());
        for (auto& [id, cs] : clients_) outs.push_back(cs.out);
    }
    for (auto& out : outs) {
        {
            std::lock_guard<std::mutex> lk(out->mtx);
            out->control.push_back(data);
        }
        out->cv.notify_one();
    }
}

void State::send_control_to(ClientId id, const std::vector<uint8_t>& data) {
    std::shared_ptr<OutBox> out;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        out = it->second.out;
    }
    {
        std::lock_guard<std::mutex> lk(out->mtx);
        out->control.push_back(data);
    }
    out->cv.notify_one();
}

void State::send_frame_to(ClientId id, const std::shared_ptr<const std::vector<uint8_t>>& data) {
    std::lock_guard<std::mutex> lk(clients_mtx_);
    send_frame_to_locked(id, data);
}

void State::send_frame_to_locked(ClientId id,
                                 const std::shared_ptr<const std::vector<uint8_t>>& data) {
    // Caller holds clients_mtx_.
    auto it = clients_.find(id);
    if (it == clients_.end()) return;
    ClientState& cs = it->second;
    // mark_frame_queued: if the previous frame never reached the socket task,
    // latest-wins coalescing silently dropped a stateful frame — force a
    // resync on the next end_callstream_frame.
    if (cs.out->unobserved.exchange(true, std::memory_order_acq_rel)) {
        cs.force_frames = std::max<size_t>(cs.force_frames, 1);
    }
    {
        std::lock_guard<std::mutex> lk(cs.out->mtx);
        cs.out->frame = data;  // refcount bump: the bytes are shared
        cs.out->frame_seq++;
    }
    cs.out->cv.notify_one();
}

void State::send_frame(const std::shared_ptr<const std::vector<uint8_t>>& data) {
    std::lock_guard<std::mutex> lk(clients_mtx_);
    for (auto& [id, cs] : clients_) {
        send_frame_to_locked(id, data);
    }
}

void State::send_texture(uint64_t id, const std::vector<uint8_t>& msg) {
    {
        std::lock_guard<std::mutex> lk(textures_mtx_);
        textures_[id] = msg;
    }
    send_control(msg);
}

std::vector<std::vector<uint8_t>> State::snapshot_textures() const {
    std::lock_guard<std::mutex> lk(textures_mtx_);
    std::vector<std::vector<uint8_t>> out;
    out.reserve(textures_.size());
    for (const auto& [id, data] : textures_) out.push_back(data);
    return out;
}

bool State::begin_frame(float dpx, float dpy, float dsw, float dsh, float fbsx, float fbsy) {
    // Call-stream gating: only stash the header when clients are connected;
    // add_draw_list is a no-op and end_frame builds the 0x07 envelope.
    if (!has_clients()) return false;
    layout_size_[0] = dsw;
    layout_size_[1] = dsh;
    std::lock_guard<std::mutex> lk(header_mtx_);
    header_ = FrameHeader{dpx, dpy, dsw, dsh, fbsx, fbsy};
    return true;
}

void State::end_callstream_frame() {
    FrameHeader header;
    {
        std::lock_guard<std::mutex> lk(header_mtx_);
        header = header_;
    }
    uint32_t frame_id = callstream_frame_id_.fetch_add(1, std::memory_order_relaxed);

    // Drain the capture buffer + any newly-interned strings. These are
    // process-global (the string table is shared across clients); one frame
    // is computed and broadcast.
    std::vector<uint8_t> call_bytes = capture::frame_drain();
    auto new_strings = capture::drain_new_strings();

    // call_count is a pre-allocate hint only; the wire is self-framing.
    auto frame_payload = std::make_shared<const std::vector<uint8_t>>(
        serialize_callstream_frame(header, frame_id, call_bytes.data(), call_bytes.size(), 0));
    std::vector<uint8_t> string_payload = serialize_string_update(new_strings);

    // Identical-frame skip: suppress per receiver when the call sequence +
    // scalars are unchanged and no new strings appeared. The envelope's
    // frame_id changes every frame and must not defeat the suppression, so
    // only the header + call bytes are hashed.
    uint32_t new_hash = callstream_frame_hash(header, call_bytes.data(), call_bytes.size());

    std::vector<ClientId> targets;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        for (auto& [id, cs] : clients_) {
            bool unchanged = cs.last_call_hash == new_hash && cs.last_call_hash != 0 &&
                             cs.force_frames == 0 && string_payload.size() <= 5;
            if (unchanged) continue;  // idle frame for this receiver
            cs.last_call_hash = new_hash;
            if (cs.force_frames > 0) cs.force_frames--;
            targets.push_back(id);
        }
    }
    if (targets.empty()) return;

    // A string update must precede the frame referencing it; control drains
    // before frames in the sender thread as well. Suppressed receivers can
    // safely re-apply it (interned-table upsert).
    if (string_payload.size() > 5) {
        send_control(string_payload);
    }
    for (ClientId id : targets) {
        send_frame_to(id, frame_payload);
    }
}

}  // namespace imgui_wasm_core
