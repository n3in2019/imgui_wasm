// protocol_tests.cpp — parse/handshake/coalescing tests for the wire input
// path, plus a coalescing test for the OutBox.

#include <cassert>
#include <cstdio>
#include <cstring>

#include "core.hpp"

using namespace imgui_wasm_core;

namespace {

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
}

void serialize_roundtrip_layouts() {
    FrameHeader h{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const uint8_t calls[] = {0xAA, 0xBB};
    auto frame = serialize_callstream_frame(h, 77, calls, sizeof(calls), 0);
    assert(frame.size() == 1 + 24 + 4 + 4 + 2);
    assert(frame[0] == 0x07);
    assert(frame[25] == 77);        // frame_id byte 0
    assert(frame[33] == 0xAA);      // first call byte
    assert(frame[34] == 0xBB);

    auto strings = serialize_string_update({{7, {'h', 'i'}}});
    assert(strings.size() == 1 + 4 + 4 + 4 + 2);
    assert(strings[0] == 0x09);
    assert(strings[5] == 7);  // id byte 0
}

}  // namespace

int main() {
    parses_batched_pointer_click_in_order();
    rejects_truncated_or_trailing_batch_data();
    parses_capability_ack();
    leading_hello_ack_accepts_envelope_and_bare_batches();
    leading_hello_ack_rejects_non_leading_or_foreign_acks();
    resize_selects_the_active_client();
    clipboard_write_is_sent_only_to_active_client();
    coalesced_frame_forces_a_resync();
    frame_hash_is_deterministic_and_sensitive();
    serialize_roundtrip_layouts();
    printf("all core_cpp_tests passed\n");
    return 0;
}
