// imgui_wasm_capture.h — C ABI for the call-stream capture buffer.
//
// The generated host wrappers (include/imgui_wasm_capture.hpp, produced by
// imgui-wasm-python/tools/generate_bindings.py) call these functions to record
// each ImGui call's opcode + args into a thread-local buffer. The Rust core
// (src/capture.rs) implements them.
//
// When call-stream transport is disabled (the default), every function here
// checks an atomic enabled flag and returns immediately, so the wrappers are
// zero-cost in draw-data mode.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opcodes are generated in imgui_wasm_opcodes.h (included by imgui_wasm_capture.hpp).
// Begin recording a call. Writes the 2-byte opcode and reserves space.
void imgui_wasm_capture_begin(uint16_t opcode);

// Scalars (LE). Each appends width bytes.
void imgui_wasm_capture_u8(uint64_t value);
void imgui_wasm_capture_u16(uint64_t value);
void imgui_wasm_capture_u32(uint64_t value);
void imgui_wasm_capture_u64(uint64_t value);

// Enums serialize as u32.
#define imgui_wasm_capture_enum(v) imgui_wasm_capture_u32((uint64_t)(v))

// Interned string. The server assigns / looks up `s` in the per-session string
// table and appends the stable u32 id. NULL -> id 0 (the empty string slot).
void imgui_wasm_capture_string(const char* s);

// ImVec2 / ImVec4 by value.
void imgui_wasm_capture_vec2(float x, float y);
void imgui_wasm_capture_vec4(float x, float y, float z, float w);

// Fixed-size float array (float[3]/float[4]). Appends `count` * 4 bytes.
void imgui_wasm_capture_floats(const float* p, unsigned count);

// Counted input arrays. These do NOT emit the count prefix; the generated
// wrapper emits `imgui_wasm_capture_u32(count)` immediately before calling these,
// so the wire layout is [count u32][count elements].
void imgui_wasm_capture_floats_n(const float* p, unsigned count);
void imgui_wasm_capture_strings_n(const char* const* p, unsigned count);

// Output / inout scalar pointer (server-authoritative echo). `p` points at the
// scalar whose CURRENT value is serialized as `width` raw LE bytes. If `p` is
// NULL a sentinel (0xFFFFFFFF) is emitted so the replay side leaves its scratch
// storage untouched. `p` is const void* + width so callers don't need type
// punning (the bytes are read directly, preserving float bit patterns).
void imgui_wasm_capture_ptr(const void* p, unsigned width);

// char* buffer (InputText etc.). Appends [len u32][bytes]; NUL-terminated for
// the replay side. `s` is never NULL (the wrapper guarantees it).
void imgui_wasm_capture_buf(const char* s);

// End of one call's arg stream. Currently a no-op (the next begin_call frames
// the next call), but reserved for per-call checksumming / length prefixing.
void imgui_wasm_capture_end(void);

// --- Frame lifecycle (called by the Rust core, not host code) -------------

// Reset the thread-local capture buffer at frame start. Returns 0 on success.
void imgui_wasm_capture_frame_begin(void);

// Take ownership of the captured call stream for this frame. The returned
// pointer + len cover every call since imgui_wasm_capture_frame_begin. After this
// call the thread-local buffer is empty.
const uint8_t* imgui_wasm_capture_frame_take(unsigned* out_len);

// Enable / disable capture globally (atomic). Checked at the top of every
// imgui_wasm_capture_* call. Disabled means the buffer stays empty and wrappers
// run as pure forwards.
void imgui_wasm_capture_set_enabled(int enabled);
int  imgui_wasm_capture_get_enabled(void);

#ifdef __cplusplus
} // extern "C"
#endif
