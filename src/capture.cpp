// capture.cpp — call-stream capture buffer and string interning.
//
// Implements the C ABI declared in
// imgui_ws/include/imgui_wasm_capture.h, which the generated host
// wrappers (imgui_wasm_capture.hpp) call to record each ImGui call's opcode +
// args into a thread-local buffer, plus the process-wide string table that
// interns every const char* argument to a stable u32 id.

#include "core.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

// The header declares the exact C
// ABI the generated wrappers were built against.
#include "imgui_wasm_capture.h"

namespace imgui_wasm_core {
namespace capture {

namespace {

constexpr uint32_t kNullStringId = 0;

std::atomic<bool> g_enabled{false};
std::atomic<uint32_t> g_next_string_id{1};

// Process-wide string internment: bytes -> stable id, plus ids not yet
// transmitted to any client. One mutex keeps capture (host thread) and
// serialization (render thread) consistent.
struct StringTable {
    std::unordered_map<std::string, uint32_t> by_bytes;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pending_new;
};

StringTable& table() {
    static StringTable* t = new StringTable();  // leaky singleton: no teardown-order issues
    return *t;
}

std::mutex& table_mutex() {
    static std::mutex m;
    return m;
}

// Thread-local capture buffer, cleared at frame start.
std::vector<uint8_t>& buffer() {
    thread_local std::vector<uint8_t> buf;
    return buf;
}

bool on() { return g_enabled.load(std::memory_order_relaxed); }

void push_u64(uint64_t value, size_t width) {
    uint8_t bytes[8];
    for (size_t i = 0; i < 8; i++) bytes[i] = uint8_t(value >> (8 * i));
    auto& buf = buffer();
    buf.insert(buf.end(), bytes, bytes + width);
}

void push_f32(float v) {
    static_assert(sizeof(float) == 4, "expecting ieee754 32-bit float");
    uint32_t bits;
    memcpy(&bits, &v, 4);
    push_u64(bits, 4);
}

uint32_t intern_cstr(const char* s) {
    if (s == nullptr) return kNullStringId;
    // Defensive cap; ImGui labels are short.
    size_t len = strnlen(s, 1u << 20);
    if (len == (1u << 20)) return kNullStringId;
    std::string key(s, len);
    std::lock_guard<std::mutex> lk(table_mutex());
    auto& t = table();
    auto it = t.by_bytes.find(key);
    if (it != t.by_bytes.end()) return it->second;
    uint32_t id = g_next_string_id.fetch_add(1, std::memory_order_relaxed);
    // One copy: the map takes ownership of the key, the pending 0x09 entry
    // is built from it before the move.
    t.pending_new.emplace_back(id, std::vector<uint8_t>(key.begin(), key.end()));
    t.by_bytes.emplace(std::move(key), id);
    return id;
}

}  // namespace

void set_enabled(bool enabled) { g_enabled.store(enabled, std::memory_order_relaxed); }
bool is_enabled() { return on(); }

void frame_begin() { buffer().clear(); }

const uint8_t* frame_peek(unsigned* out_len) {
    const auto& buf = buffer();
    if (out_len) *out_len = unsigned(buf.size());
    return buf.data();
}

std::vector<uint8_t> frame_drain() {
    auto& buf = buffer();
    std::vector<uint8_t> out;
    if (!on()) {
        // Still clear any stale bytes defensively.
        buf.clear();
        return out;
    }
    // Swap (not move): the thread-local keeps its capacity so the next frame
    // records calls without regrowing the buffer from zero.
    buf.swap(out);
    return out;
}

std::vector<std::pair<uint32_t, std::vector<uint8_t>>> drain_new_strings() {
    std::lock_guard<std::mutex> lk(table_mutex());
    auto drained = std::move(table().pending_new);
    table().pending_new.clear();
    return drained;
}

std::vector<std::pair<uint32_t, std::vector<uint8_t>>> snapshot_all_strings() {
    std::lock_guard<std::mutex> lk(table_mutex());
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> out;
    out.reserve(table().by_bytes.size());
    for (const auto& [bytes, id] : table().by_bytes) {
        out.emplace_back(id, std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }
    return out;
}

void begin_call(uint16_t opcode) {
    if (!on()) return;
    auto& buf = buffer();
    buf.push_back(uint8_t(opcode));
    buf.push_back(uint8_t(opcode >> 8));
}

void scalar(uint64_t value, size_t width) {
    if (!on()) return;
    push_u64(value, width);
}

void interned_string(const char* s) {
    if (!on()) return;
    push_u64(intern_cstr(s), 4);
}

void vec2(float x, float y) {
    if (!on()) return;
    push_f32(x);
    push_f32(y);
}

void vec4(float x, float y, float z, float w) {
    if (!on()) return;
    push_f32(x);
    push_f32(y);
    push_f32(z);
    push_f32(w);
}

void floats(const float* p, unsigned count) {
    if (!on() || p == nullptr) return;
    for (unsigned i = 0; i < count; i++) push_f32(p[i]);
}

void scalar_ptr(const void* p, unsigned width) {
    if (!on()) return;
    if (p == nullptr) {
        // Null-pointer sentinel: `width` 0xFF bytes keep the replay side
        // byte-aligned.
        auto& buf = buffer();
        buf.insert(buf.end(), width, 0xFF);
        return;
    }
    auto& buf = buffer();
    const uint8_t* bytes = static_cast<const uint8_t*>(p);
    buf.insert(buf.end(), bytes, bytes + width);
}

void char_buf(const char* s) {
    if (!on()) return;
    size_t len = (s != nullptr) ? strlen(s) : 0;
    push_u64(len, 4);
    auto& buf = buffer();
    if (len > 0) buf.insert(buf.end(), s, s + len);
}

}  // namespace capture
}  // namespace imgui_wasm_core

// --- C ABI (called by the generated imgui_wasm_capture.hpp wrappers) -----------

extern "C" {

void imgui_wasm_capture_begin(uint16_t opcode) { imgui_wasm_core::capture::begin_call(opcode); }

void imgui_wasm_capture_u8(uint64_t value) { imgui_wasm_core::capture::scalar(value, 1); }
void imgui_wasm_capture_u16(uint64_t value) { imgui_wasm_core::capture::scalar(value, 2); }
void imgui_wasm_capture_u32(uint64_t value) { imgui_wasm_core::capture::scalar(value, 4); }
void imgui_wasm_capture_u64(uint64_t value) { imgui_wasm_core::capture::scalar(value, 8); }

void imgui_wasm_capture_string(const char* s) { imgui_wasm_core::capture::interned_string(s); }

void imgui_wasm_capture_vec2(float x, float y) { imgui_wasm_core::capture::vec2(x, y); }

void imgui_wasm_capture_vec4(float x, float y, float z, float w) {
    imgui_wasm_core::capture::vec4(x, y, z, w);
}

void imgui_wasm_capture_floats(const float* p, unsigned count) {
    imgui_wasm_core::capture::floats(p, count);
}

void imgui_wasm_capture_floats_n(const float* p, unsigned count) {
    imgui_wasm_core::capture::floats(p, count);
}

void imgui_wasm_capture_strings_n(const char* const* p, unsigned count) {
    if (p == nullptr) return;
    for (unsigned i = 0; i < count; i++) {
        imgui_wasm_core::capture::interned_string(p[i]);
    }
}

void imgui_wasm_capture_ptr(const void* p, unsigned width) {
    imgui_wasm_core::capture::scalar_ptr(p, width);
}

void imgui_wasm_capture_buf(const char* s) { imgui_wasm_core::capture::char_buf(s); }

void imgui_wasm_capture_end(void) {
    // No-op; reserved for per-call framing.
}

void imgui_wasm_capture_frame_begin() { imgui_wasm_core::capture::frame_begin(); }

const uint8_t* imgui_wasm_capture_frame_take(unsigned* out_len) {
    // Not used by the C++ core (frame_drain moves the buffer instead); kept
    // for C ABI completeness. Peeks without consuming.
    return imgui_wasm_core::capture::frame_peek(out_len);
}

void imgui_wasm_capture_set_enabled(int value) {
    imgui_wasm_core::capture::set_enabled(value != 0);
}

int imgui_wasm_capture_get_enabled() { return imgui_wasm_core::capture::is_enabled() ? 1 : 0; }

}  // extern "C"
