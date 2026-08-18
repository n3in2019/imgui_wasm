// callstream_protocol.cpp — wire framing for the call-stream transport.
//
// Wire messages:
//   0x07  call-stream frame: header (6 x f32) + frame_id (u32) + call_count
//         (u32, hint; 0 = unknown) + imgui_flags (u32, effective
//         ImGuiIO::ConfigFlags; the twin mirrors the docking bit) + call bytes
//   0x09  string-table update: count (u32) + { id, len, utf-8 } entries
//   0x02  texture (replayed on connect)
// LZ4/0x08 compression is intentionally not ported: it is dead code upstream.

#include "core.hpp"

#include <cstring>

namespace imgui_wasm_core {

namespace {

void push_f32(std::vector<uint8_t>& out, float v) {
    static_assert(sizeof(float) == 4, "expecting ieee754 32-bit float");
    uint32_t bits;
    memcpy(&bits, &v, 4);
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(bits >> (8 * i)));
}

void push_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (8 * i)));
}

void push_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; i++) out.push_back(uint8_t(v >> (8 * i)));
}

}  // namespace

std::vector<uint8_t> serialize_callstream_frame(FrameHeader header, uint32_t frame_id,
                                                const uint8_t* call_bytes, size_t call_len,
                                                uint32_t call_count) {
    std::vector<uint8_t> out;
    out.reserve(1 + 24 + 4 + 4 + 4 + call_len);
    out.push_back(0x07);
    push_f32(out, header.dpx);
    push_f32(out, header.dpy);
    push_f32(out, header.dsw);
    push_f32(out, header.dsh);
    push_f32(out, header.fbsx);
    push_f32(out, header.fbsy);
    push_u32(out, frame_id);
    push_u32(out, call_count);
    push_u32(out, header.imgui_flags);
    out.insert(out.end(), call_bytes, call_bytes + call_len);
    return out;
}

std::vector<uint8_t> serialize_string_update(
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& strings) {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + strings.size() * 8);
    out.push_back(0x09);
    push_u32(out, uint32_t(strings.size()));
    for (const auto& [id, bytes] : strings) {
        push_u32(out, id);
        push_u32(out, uint32_t(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

uint32_t callstream_frame_hash(FrameHeader header, const uint8_t* call_bytes, size_t call_len) {
    // Fixed-seed FNV-1a over header + call bytes (deterministic across runs —
    // required for identical-frame suppression).
    uint32_t h = 0x811c9dc5;
    auto mix = [&h](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; i++) {
            h ^= uint32_t(p[i]);
            h *= 0x01000193;
        }
    };
    float header_f[6] = {header.dpx, header.dpy, header.dsw, header.dsh, header.fbsx, header.fbsy};
    for (float v : header_f) {
        uint32_t bits;
        memcpy(&bits, &v, 4);
        uint8_t bytes[4] = {uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16),
                            uint8_t(bits >> 24)};
        mix(bytes, 4);
    }
    // Flags are part of the frame identity: toggling docking (or any future
    // mirrored bit) must break identical-frame suppression.
    uint8_t flags_b[4] = {uint8_t(header.imgui_flags), uint8_t(header.imgui_flags >> 8),
                          uint8_t(header.imgui_flags >> 16), uint8_t(header.imgui_flags >> 24)};
    mix(flags_b, 4);
    mix(call_bytes, call_len);
    return h;
}

std::vector<uint8_t> make_texture_msg(uint64_t id, uint32_t width, uint32_t height,
                                      const uint8_t* pixels, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(1 + 8 + 4 + 4 + 4 + len);
    out.push_back(0x02);
    push_u64(out, id);
    push_u32(out, width);
    push_u32(out, height);
    push_u32(out, uint32_t(len));
    out.insert(out.end(), pixels, pixels + len);
    return out;
}

}  // namespace imgui_wasm_core
