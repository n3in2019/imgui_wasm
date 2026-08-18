// protocol_tests.cpp — parse/handshake/coalescing tests for the wire input
// path, plus a coalescing test for the OutBox.

#include <arpa/inet.h>

#include <cassert>
#include <cstdio>
#include <cstring>

#include "core.hpp"
#include "net.hpp"

using namespace imgui_wasm_core;

namespace {

// ImGuiConfigFlags_DockingEnable (1 << 6); spelled out so the protocol tests
// stay independent of imgui.h.
constexpr uint32_t kDockingEnable = 1u << 6;

void push_u32b(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back(uint8_t(x >> (8 * i)));
}
void push_u16b(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
void push_f32b(std::vector<uint8_t>& v, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    push_u32b(v, bits);
}

void parses_batched_pointer_click_in_order() {
    const ClientId id = 42;
    std::vector<uint8_t> batch = {0x19};
    push_u32b(batch, id);
    push_u16b(batch, 2);

    std::vector<uint8_t> position;
    push_f32b(position, 12.5f);
    push_f32b(position, 34.0f);
    batch.push_back(0x10);
    push_u16b(batch, uint16_t(position.size()));
    batch.insert(batch.end(), position.begin(), position.end());

    batch.push_back(0x11);
    push_u16b(batch, 1);
    batch.push_back(0);

    auto messages = parse_client_msgs(batch.data(), batch.size());
    assert(messages.has_value());
    assert(messages->size() == 2);
    assert((*messages)[0].first == id);
    assert((*messages)[0].second.kind == ClientMsg::Kind::Input);
    assert((*messages)[0].second.input.ev_type == 0);
    assert((*messages)[0].second.input.x == 12.5f);
    assert((*messages)[0].second.input.y == 34.0f);
    assert((*messages)[1].second.kind == ClientMsg::Kind::Input);
    assert((*messages)[1].second.input.ev_type == 1);
    assert((*messages)[1].second.input.button == 0);
}

void rejects_truncated_or_trailing_batch_data() {
    const uint8_t batch[] = {0x19, 1, 0, 0, 0, 1, 0, 0x11, 1, 0};
    assert(!parse_client_msgs(batch, sizeof(batch)).has_value());

    std::vector<uint8_t> valid(batch, batch + sizeof(batch));
    valid.push_back(0);
    valid.push_back(0xff);
    assert(!parse_client_msgs(valid.data(), valid.size()).has_value());
}

void parses_capability_ack() {
    std::vector<uint8_t> ack = {0x1a};
    push_u32b(ack, 7);
    push_u32b(ack, 0b1011);
    auto messages = parse_client_msgs(ack.data(), ack.size());
    assert(messages.has_value());
    assert((*messages).size() == 1);
    assert((*messages)[0].first == 7);
    assert((*messages)[0].second.kind == ClientMsg::Kind::HelloAck);
    assert((*messages)[0].second.capabilities == 0b1011);
}

void leading_hello_ack_accepts_envelope_and_bare_batches() {
    // 0x19 envelope: ack leads, trailing input is dropped.
    std::vector<uint8_t> env = {0x19};
    push_u32b(env, 7);
    push_u16b(env, 2);
    env.push_back(0x1a);
    push_u16b(env, 4);
    push_u32b(env, 0b1011);
    env.push_back(0x10);
    push_u16b(env, 8);
    push_f32b(env, 1.0f);
    push_f32b(env, 2.0f);
    auto msgs = parse_client_msgs(env.data(), env.size());
    assert(msgs.has_value());
    assert(msgs->size() == 2);
    assert(leading_hello_ack(*msgs, 7) == std::optional<uint32_t>(0b1011));

    // Bare ack: stray trailing bytes never become records, so the ack still
    // leads a one-record list.
    std::vector<uint8_t> bare = {0x1a};
    push_u32b(bare, 7);
    push_u32b(bare, 0b1011);
    bare.push_back(0x10);
    msgs = parse_client_msgs(bare.data(), bare.size());
    assert(msgs.has_value());
    assert(msgs->size() == 1);
    assert(leading_hello_ack(*msgs, 7) == std::optional<uint32_t>(0b1011));
}

void leading_hello_ack_rejects_non_leading_or_foreign_acks() {
    // Envelope with input before the ack: not a leading ack.
    std::vector<uint8_t> env = {0x19};
    push_u32b(env, 7);
    push_u16b(env, 2);
    env.push_back(0x10);
    push_u16b(env, 8);
    push_f32b(env, 1.0f);
    push_f32b(env, 2.0f);
    env.push_back(0x1a);
    push_u16b(env, 4);
    push_u32b(env, 1);
    auto msgs = parse_client_msgs(env.data(), env.size());
    assert(msgs.has_value());
    assert(!leading_hello_ack(*msgs, 7).has_value());

    // Foreign client id.
    std::vector<uint8_t> foreign = {0x1a};
    push_u32b(foreign, 8);
    push_u32b(foreign, 1);
    msgs = parse_client_msgs(foreign.data(), foreign.size());
    assert(msgs.has_value());
    assert(!leading_hello_ack(*msgs, 7).has_value());

    // Empty message list.
    std::vector<std::pair<ClientId, ClientMsg>> empty;
    assert(!leading_hello_ack(empty, 7).has_value());
}

void resize_selects_the_active_client() {
    State state;
    ClientId first = state.add_client();
    ClientId second = state.add_client();
    state.set_display_size(second, 800.0f, 437.0f);
    float size[2];
    state.get_display_size(size);
    assert(size[0] == 800.0f && size[1] == 437.0f);
    state.set_display_size(first, 1024.0f, 768.0f);
    state.get_display_size(size);
    assert(size[0] == 1024.0f && size[1] == 768.0f);
}

void parses_resize_with_and_without_scale() {
    const ClientId id = 42;
    // Modern resize: w, h, scale (devicePixelRatio).
    std::vector<uint8_t> modern = {0x17};
    push_u32b(modern, id);
    push_f32b(modern, 800.0f);
    push_f32b(modern, 437.0f);
    push_f32b(modern, 2.0f);
    auto messages = parse_client_msgs(modern.data(), modern.size());
    assert(messages.has_value());
    assert(messages->size() == 1);
    assert((*messages)[0].first == id);
    assert((*messages)[0].second.kind == ClientMsg::Kind::Resize);
    assert((*messages)[0].second.resize_w == 800.0f);
    assert((*messages)[0].second.resize_h == 437.0f);
    assert((*messages)[0].second.resize_scale == 2.0f);

    // Legacy 13-byte resize: no scale field ("not provided").
    std::vector<uint8_t> legacy = {0x17};
    push_u32b(legacy, id);
    push_f32b(legacy, 800.0f);
    push_f32b(legacy, 437.0f);
    messages = parse_client_msgs(legacy.data(), legacy.size());
    assert(messages.has_value());
    assert(messages->size() == 1);
    assert((*messages)[0].second.resize_scale == 0.0f);
}

void resize_scale_follows_active_client() {
    State state;
    ClientId first = state.add_client();
    ClientId second = state.add_client();
    assert(state.get_display_scale() == 1.0f);  // no active client yet
    state.set_display_size(second, 800.0f, 437.0f, 2.0f);
    assert(state.get_display_scale() == 2.0f);
    // A later resize without a scale keeps the client's last known scale.
    state.set_display_size(second, 801.0f, 438.0f);
    assert(state.get_display_scale() == 2.0f);
    // The most recently active client's scale wins.
    state.set_display_size(first, 1024.0f, 768.0f, 1.5f);
    assert(state.get_display_scale() == 1.5f);
}

void clipboard_write_is_sent_only_to_active_client() {
    State state;
    ClientId first = state.add_client();
    ClientId second = state.add_client();
    state.set_display_size(second, 800.0f, 600.0f);

    std::shared_ptr<OutBox> first_out;
    std::shared_ptr<OutBox> second_out;
    state.with_clients([&](std::unordered_map<ClientId, ClientState>& clients) {
        first_out = clients[first].out;
        second_out = clients[second].out;
    });

    state.set_clipboard_text("private");

    assert(first_out->control.empty());
    assert(!second_out->control.empty());
    assert(second_out->control.front() == make_clipboard_write_msg("private"));
}

void coalesced_frame_forces_a_resync() {
    State state;
    ClientId client = state.add_client();
    std::shared_ptr<OutBox> out;
    state.with_clients([&](std::unordered_map<ClientId, ClientState>& clients) {
        // Neutralize add_client's initial force frames so only the coalescing
        // detection under test can raise it again.
        clients[client].force_frames = 0;
        out = clients[client].out;
    });

    auto frame1 = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x07, 1});
    auto frame2 = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x07, 2});
    state.send_frame(frame1);
    state.send_frame(frame2);  // replaces an unobserved frame
    size_t forced_after = 0;
    state.with_clients([&](std::unordered_map<ClientId, ClientState>& clients) {
        forced_after = clients[client].force_frames;
    });
    assert(forced_after >= 1);

    // An observed queue slot must not trip the detection: the frame is
    // merely in flight, not dropped.
    out->unobserved.store(false, std::memory_order_release);
    auto frame3 = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x07, 3});
    state.send_frame(frame3);
    state.with_clients([&](std::unordered_map<ClientId, ClientState>& clients) {
        assert(clients[client].force_frames == 1);
    });
}

void frame_hash_is_deterministic_and_sensitive() {
    FrameHeader h{0.0f, 0.0f, 1280.0f, 720.0f, 1.0f, 1.0f};
    const uint8_t calls_a[] = {0x01, 0x02, 0x03};
    const uint8_t calls_b[] = {0x01, 0x02, 0x04};
    assert(callstream_frame_hash(h, calls_a, sizeof(calls_a)) ==
           callstream_frame_hash(h, calls_a, sizeof(calls_a)));
    assert(callstream_frame_hash(h, calls_a, sizeof(calls_a)) !=
           callstream_frame_hash(h, calls_b, sizeof(calls_b)));
    // Flags are part of the frame identity: toggling docking must break
    // identical-frame suppression.
    FrameHeader h_dock = h;
    h_dock.imgui_flags = kDockingEnable;
    assert(callstream_frame_hash(h, calls_a, sizeof(calls_a)) !=
           callstream_frame_hash(h_dock, calls_a, sizeof(calls_a)));
}

void serialize_roundtrip_layouts() {
    FrameHeader h{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    h.imgui_flags = kDockingEnable;
    const uint8_t calls[] = {0xAA, 0xBB};
    auto frame = serialize_callstream_frame(h, 77, calls, sizeof(calls), 0);
    assert(frame.size() == 1 + 24 + 4 + 4 + 4 + 2);
    assert(frame[0] == 0x07);
    assert(frame[25] == 77);        // frame_id byte 0
    assert(frame[37] == 0xAA);      // first call byte
    assert(frame[38] == 0xBB);
    // imgui_flags: byte 33 = low byte of the u32 after frame_id + call_count.
    assert(frame[33] == (kDockingEnable & 0xFF));
    assert(frame[34] == 0 && frame[35] == 0 && frame[36] == 0);

    auto strings = serialize_string_update({{7, {'h', 'i'}}});
    assert(strings.size() == 1 + 4 + 4 + 4 + 2);
    assert(strings[0] == 0x09);
    assert(strings[5] == 7);  // id byte 0
}

void input_preserves_cross_client_arrival_order() {
    State state;
    ClientId a = state.add_client();
    ClientId b = state.add_client();
    InputEvent ev;
    ev.ev_type = 0;
    ev.x = 1.0f;
    state.push_input(a, ev);
    ev.x = 2.0f;
    state.push_input(b, ev);
    ev.x = 3.0f;
    state.push_input(a, ev);
    auto e1 = state.try_poll_input();
    auto e2 = state.try_poll_input();
    auto e3 = state.try_poll_input();
    assert(e1.has_value() && e2.has_value() && e3.has_value());
    assert(e1->x == 1.0f && e2->x == 2.0f && e3->x == 3.0f);
    assert(!state.try_poll_input().has_value());
}

void suppression_is_per_client_and_force_resends() {
    State state;
    ClientId a = state.add_client();
    ClientId b = state.add_client();

    std::shared_ptr<OutBox> a_out;
    std::shared_ptr<OutBox> b_out;
    auto grab = [&](ClientId id, std::shared_ptr<OutBox>& out) {
        state.with_clients([&](std::unordered_map<ClientId, ClientState>& clients) {
            out = clients[id].out;
        });
    };
    grab(a, a_out);
    grab(b, b_out);
    state.with_clients([&](std::unordered_map<ClientId, ClientState>& clients) {
        // Neutralize add_client's initial force frames so only the
        // suppression logic under test decides what is sent.
        clients[a].force_frames = 0;
        clients[b].force_frames = 0;
    });

    // First frame: both receive it (no last hash yet).
    state.begin_frame(0.0f, 0.0f, 800.0f, 600.0f, 1.0f, 1.0f);
    state.end_callstream_frame();
    assert(a_out->frame_seq == 1 && b_out->frame_seq == 1);

    // Identical frame: suppressed for both.
    state.begin_frame(0.0f, 0.0f, 800.0f, 600.0f, 1.0f, 1.0f);
    state.end_callstream_frame();
    assert(a_out->frame_seq == 1 && b_out->frame_seq == 1);

    // A late joiner is forced to resync while established clients stay idle.
    ClientId c = state.add_client();
    std::shared_ptr<OutBox> c_out;
    grab(c, c_out);
    state.begin_frame(0.0f, 0.0f, 800.0f, 600.0f, 1.0f, 1.0f);
    state.end_callstream_frame();
    assert(a_out->frame_seq == 1 && b_out->frame_seq == 1);
    assert(c_out->frame_seq == 1);
}

void positional_input_waits_for_matching_layout() {
    State state;
    ClientId big = state.add_client();
    ClientId small = state.add_client();
    state.set_display_size(big, 1920.0f, 1080.0f);
    state.set_display_size(small, 1280.0f, 720.0f);  // small is now active

    // Frame still laid out at big's size.
    state.begin_frame(0.0f, 0.0f, 1920.0f, 1080.0f, 1.0f, 1.0f);
    InputEvent key;
    key.ev_type = 4;
    key.key = 65;
    state.push_input(small, key);
    InputEvent click;
    click.ev_type = 1;
    click.button = 0;
    state.push_input(small, click);

    // Keys carry no geometry and flow immediately; the click behind them
    // waits for a layout computed at the sender's size.
    auto k = state.try_poll_input();
    assert(k.has_value() && k->key == 65);
    assert(!state.try_poll_input().has_value());  // click held

    // The next frame follows the active (small) client's size; the click flows.
    state.begin_frame(0.0f, 0.0f, 1280.0f, 720.0f, 1.0f, 1.0f);
    auto c = state.try_poll_input();
    assert(c.has_value() && c->ev_type == 1 && c->button == 0);

    // Same-size clients interchange: big's move flows against small's layout.
    state.set_display_size(big, 1280.0f, 720.0f);
    InputEvent move;
    move.ev_type = 0;
    move.x = 10.0f;
    move.y = 20.0f;
    state.push_input(big, move);
    auto m = state.try_poll_input();
    assert(m.has_value() && m->x == 10.0f && m->y == 20.0f);
}

void caps_limit_total_and_per_peer() {
    State state;
    AuthConfig auth;
    auth.max_clients = 2;
    auth.max_clients_per_ip = 1;
    state.set_auth(auth);
    const uint32_t peer_a = htonl(0x7f000001);
    const uint32_t peer_b = htonl(0x7f000002);
    assert(state.connection_allowed(peer_a));
    ClientId a = state.add_client(peer_a);
    assert(!state.connection_allowed(peer_a));  // per-IP cap
    assert(state.connection_allowed(peer_b));
    ClientId b = state.add_client(peer_b);
    assert(!state.connection_allowed(peer_b));  // total cap reached
    state.remove_client(a);
    assert(state.connection_allowed(peer_a));
    state.remove_client(b);
}

void base64_decode_round_trips_and_rejects_garbage() {
    std::vector<uint8_t> out;
    assert(net::base64_decode("dXNlcjpwYXNz", out));
    assert(out.size() == 9 && memcmp(out.data(), "user:pass", 9) == 0);
    assert(net::base64_decode("YQ==", out));
    assert(out.size() == 1 && out[0] == 'a');
    assert(!net::base64_decode("%%%%", out));
}

}  // namespace

int main() {
    parses_batched_pointer_click_in_order();
    rejects_truncated_or_trailing_batch_data();
    parses_capability_ack();
    leading_hello_ack_accepts_envelope_and_bare_batches();
    leading_hello_ack_rejects_non_leading_or_foreign_acks();
    resize_selects_the_active_client();
    input_preserves_cross_client_arrival_order();
    positional_input_waits_for_matching_layout();
    suppression_is_per_client_and_force_resends();
    parses_resize_with_and_without_scale();
    resize_scale_follows_active_client();
    clipboard_write_is_sent_only_to_active_client();
    coalesced_frame_forces_a_resync();
    frame_hash_is_deterministic_and_sensitive();
    serialize_roundtrip_layouts();
    base64_decode_round_trips_and_rejects_garbage();
    caps_limit_total_and_per_peer();
    printf("all core_cpp_tests passed\n");
    return 0;
}
