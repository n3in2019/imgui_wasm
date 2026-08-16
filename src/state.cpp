// state.cpp — shared server state: clients, input queue, clipboard, textures,
// and the frame path. Frames are built from the capture buffer.

#include "core.hpp"

#include <algorithm>
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
        case 0x17: {  // resize
            msg.kind = ClientMsg::Kind::Resize;
            if (!read_f32(d, len, 0, msg.resize_w) || !read_f32(d, len, 4, msg.resize_h))
                return std::nullopt;
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

ClientId State::add_client() {
    ClientId id;
    std::shared_ptr<OutBox> out;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        id = next_client_id_++;
        auto [it, inserted] = clients_.emplace(id, ClientState{});
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

void State::push_input(ClientId id, const InputEvent& ev) {
    {
        std::lock_guard<std::mutex> lk(input_mtx_);
        input_.emplace_back(id, ev);
    }
    std::lock_guard<std::mutex> lk(active_mtx_);
    active_client_ = id;
}

std::optional<InputEvent> State::try_poll_input() {
    ClientId client_id;
    InputEvent ev;
    {
        std::lock_guard<std::mutex> lk(input_mtx_);
        if (input_.empty()) return std::nullopt;
        client_id = input_.front().first;
        ev = input_.front().second;
        input_.pop_front();
    }
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(client_id);
        if (it != clients_.end()) {
            ev.display_w = it->second.display_size[0];
            ev.display_h = it->second.display_size[1];
        }
    }
    std::lock_guard<std::mutex> lk(active_mtx_);
    active_client_ = client_id;
    return ev;
}

void State::set_client_capabilities(ClientId id, uint32_t capabilities) {
    std::lock_guard<std::mutex> lk(clients_mtx_);
    auto it = clients_.find(id);
    if (it != clients_.end()) it->second.capabilities = capabilities;
}

void State::set_display_size(ClientId id, float w, float h) {
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        it->second.display_size[0] = w;
        it->second.display_size[1] = h;
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

    // Identical-frame skip: suppress re-broadcast when the call sequence +
    // scalars are unchanged and no new strings appeared. The envelope's
    // frame_id changes every frame and must not defeat the suppression, so
    // only the header + call bytes are hashed.
    uint32_t new_hash = callstream_frame_hash(header, call_bytes.data(), call_bytes.size());

    bool force = false;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        for (auto& [id, cs] : clients_) {
            if (cs.force_frames > 0) force = true;
        }
        if (force) {
            for (auto& [id, cs] : clients_) {
                if (cs.force_frames > 0) cs.force_frames--;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(last_hash_mtx_);
        bool unchanged = !force && new_hash == last_callstream_hash_ && last_callstream_hash_ != 0 &&
                         string_payload.size() <= 5;
        if (unchanged) {
            // Idle frame: nothing new to send. Clients keep their last
            // rendered frame.
            return;
        }
        last_callstream_hash_ = new_hash;
    }

    // A string update must precede the frame referencing it; control drains
    // before frames in the sender thread as well.
    if (string_payload.size() > 5) {
        send_control(string_payload);
    }
    send_frame(frame_payload);
}

}  // namespace imgui_wasm_core
