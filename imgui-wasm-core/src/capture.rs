//! Call-stream capture buffer and string interning.
//!
//! This module owns:
//!   - a thread-local `CaptureBuffer` holding the per-frame opcoded call bytes
//!     written by the generated host wrappers (include/imgui_wasm_capture.hpp).
//!   - a process-wide `StringTable` that interns every `const char*` argument
//!     to a stable u32 id, so labels are transmitted once per session.
//!   - a global atomic `enabled` flag; when false every capture_* call is a
//!     cheap no-op (the wrappers act as pure forwards to ImGui::).
//!
//! The wire format is defined alongside the generator (see
//! imgui-wasm-python/tools/generate_bindings.py) and decoded by the browser WASM
//! replay twin (wasm/generated/replay_switch.cpp) and imgui_wasm.js.

// The `imgui_wasm_capture_*` functions form a C ABI consumed by the generated C++
// wrappers (include/imgui_wasm_capture.hpp). They are never called from Rust, so
// dead-code warnings are expected for the ABI surface and silenced here. The
// Rust-side helpers (frame_drain, drain_new_strings, ...) are wired in Phase 4.
#![allow(dead_code)]

use std::collections::HashMap;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::Mutex;

/// Sentinel id meaning "NULL string". Real ids start at 1.
pub const NULL_STRING_ID: u32 = 0;

static CAPTURE_ENABLED: AtomicBool = AtomicBool::new(false);
static NEXT_STRING_ID: AtomicU32 = AtomicU32::new(1);

/// Process-wide string internment: bytes -> stable id, plus the set of ids
/// not yet transmitted to any client (sent in the next string-table update).
/// Held under one mutex so capture (host thread) and serialization (render
/// thread) both see a consistent table.
struct StringTable {
    by_bytes: HashMap<Vec<u8>, u32>,
    /// Pending ids added since the last `drain_new`. Drained after a frame is
    /// serialized, so each new string is announced exactly once.
    pending_new: Vec<(u32, Vec<u8>)>,
}

static STRING_TABLE: Mutex<Option<StringTable>> = Mutex::new(None);

fn with_string_table<R>(f: impl FnOnce(&mut StringTable) -> R) -> R {
    let mut guard = STRING_TABLE.lock().unwrap();
    if guard.is_none() {
        *guard = Some(StringTable {
            by_bytes: HashMap::new(),
            pending_new: Vec::new(),
        });
    }
    f(guard.as_mut().unwrap())
}

/// Intern a NUL-terminated C string. Returns its stable id (0 for NULL).
/// Safe for arbitrary `const char*` from the host: we never read past NUL.
unsafe fn intern_cstr(s: *const u8) -> u32 {
    if s.is_null() {
        return NULL_STRING_ID;
    }
    // Read up to the NUL.
    let mut len = 0usize;
    while *unsafe { s.add(len) } != 0 {
        len += 1;
        // Defensive cap; ImGui labels are short. Avoids unbounded scans of a
        // corrupted pointer.
        if len > 1 << 20 {
            return NULL_STRING_ID;
        }
    }
    let bytes = unsafe { std::slice::from_raw_parts(s, len) };
    with_string_table(|t| {
        if let Some(&id) = t.by_bytes.get(bytes) {
            id
        } else {
            let id = NEXT_STRING_ID.fetch_add(1, Ordering::Relaxed);
            t.by_bytes.insert(bytes.to_vec(), id);
            t.pending_new.push((id, bytes.to_vec()));
            id
        }
    })
}

// --- Thread-local capture buffer -------------------------------------------

use std::cell::RefCell;

thread_local! {
    static BUFFER: RefCell<CaptureBuffer> = RefCell::new(CaptureBuffer::new());
}

struct CaptureBuffer {
    buf: Vec<u8>,
    enabled_snapshot: bool,
}

impl CaptureBuffer {
    const fn new() -> Self {
        Self { buf: Vec::new(), enabled_snapshot: false }
    }
}

#[inline]
fn enabled() -> bool {
    CAPTURE_ENABLED.load(Ordering::Relaxed)
}

// --- C ABI entry points (called by include/imgui_wasm_capture.hpp) --------------

/// # Safety
/// C strings must be NUL-terminated or NULL.
#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_begin(opcode: u16) {
    if !enabled() {
        return;
    }
    BUFFER.with(|b| {
        let mut b = b.borrow_mut();
        b.buf.extend_from_slice(&opcode.to_le_bytes());
    });
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_u8(value: u64) {
    if !enabled() {
        return;
    }
    push_u(value, 1);
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_u16(value: u64) {
    if !enabled() {
        return;
    }
    push_u(value, 2);
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_u32(value: u64) {
    if !enabled() {
        return;
    }
    push_u(value, 4);
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_u64(value: u64) {
    if !enabled() {
        return;
    }
    push_u(value, 8);
}

#[inline]
fn push_u(value: u64, width: usize) {
    let bytes = value.to_le_bytes();
    BUFFER.with(|b| {
        let mut b = b.borrow_mut();
        b.buf.extend_from_slice(&bytes[..width]);
    });
}

/// # Safety
/// `s` is NULL or a NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_string(s: *const i8) {
    if !enabled() {
        return;
    }
    let id = unsafe { intern_cstr(s as *const u8) };
    push_u(id as u64, 4);
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_vec2(x: f32, y: f32) {
    if !enabled() {
        return;
    }
    BUFFER.with(|b| {
        let mut b = b.borrow_mut();
        b.buf.extend_from_slice(&x.to_le_bytes());
        b.buf.extend_from_slice(&y.to_le_bytes());
    });
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_vec4(x: f32, y: f32, z: f32, w: f32) {
    if !enabled() {
        return;
    }
    BUFFER.with(|b| {
        let mut b = b.borrow_mut();
        b.buf.extend_from_slice(&x.to_le_bytes());
        b.buf.extend_from_slice(&y.to_le_bytes());
        b.buf.extend_from_slice(&z.to_le_bytes());
        b.buf.extend_from_slice(&w.to_le_bytes());
    });
}

/// # Safety
/// `p` points to `count` f32 values or is NULL.
#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_floats(p: *const f32, count: u32) {
    if !enabled() || p.is_null() {
        return;
    }
    let slice = unsafe { std::slice::from_raw_parts(p, count as usize) };
    BUFFER.with(|b| {
        let mut b = b.borrow_mut();
        for f in slice {
            b.buf.extend_from_slice(&f.to_le_bytes());
        }
    });
}

/// # Safety
/// `p` points to `count` f32 values or is NULL.
#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_floats_n(p: *const f32, count: u32) {
    unsafe { imgui_wasm_capture_floats(p, count) }
}

/// # Safety
/// `p` points to `count` NUL-terminated C strings or is NULL.
#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_strings_n(p: *const *const i8, count: u32) {
    if !enabled() || p.is_null() {
        return;
    }
    for i in 0..count as usize {
        let s = unsafe { *p.add(i) };
        unsafe { imgui_wasm_capture_string(s) };
    }
}

/// Output/inout scalar pointer (server-authoritative echo). Reads `width`
/// raw bytes from `p` into the buffer, or emits the null sentinel if `p` is
/// NULL. Taking `const void*` avoids type-punning at the call site (float
/// bit patterns are preserved byte-for-byte).
///
/// # Safety
/// `p` is NULL or points to at least `width` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_ptr(p: *const u8, width: u32) {
    if !enabled() {
        return;
    }
    if p.is_null() {
        // Null-pointer sentinel: emit `width` 0xFF bytes so the replay side
        // (which reads exactly `width` bytes) stays byte-aligned and can
        // detect the null. The replay's scratch value is left as all-0xFF,
        // which ImGui treats as "true"/max — but since the server is
        // authoritative and echoes the real value next frame, the scratch
        // value is discarded anyway.
        let w = width as usize;
        BUFFER.with(|b| {
            let mut b = b.borrow_mut();
            let new_len = b.buf.len() + w;
            b.buf.resize(new_len, 0xFF);
        });
        return;
    }
    let bytes = unsafe { std::slice::from_raw_parts(p, width as usize) };
    BUFFER.with(|b| {
        let mut b = b.borrow_mut();
        b.buf.extend_from_slice(bytes);
    });
}

/// # Safety
/// `s` is a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_buf(s: *const i8) {
    if !enabled() {
        return;
    }
    let cstr = if s.is_null() {
        &b""[..]
    } else {
        // Reuse std's CStr for the length.
        match unsafe { std::ffi::CStr::from_ptr(s) }.to_str() {
            Ok(_) => unsafe { std::ffi::CStr::from_ptr(s) }.to_bytes(),
            Err(_) => &b""[..],
        }
    };
    push_u(cstr.len() as u64, 4);
    BUFFER.with(|b| {
        let mut b = b.borrow_mut();
        b.buf.extend_from_slice(cstr);
    });
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_end() {
    // No-op; reserved for per-call framing.
}

// --- Frame lifecycle (called by the Rust core) -----------------------------

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_frame_begin() {
    BUFFER.with(|b| {
        let mut b = b.borrow_mut();
        b.buf.clear();
        b.enabled_snapshot = enabled();
    });
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_frame_take(out_len: *mut u32) -> *const u8 {
    BUFFER.with(|b| {
        let b = b.borrow_mut();
        unsafe { *out_len = b.buf.len() as u32 };
        b.buf.as_ptr()
    })
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_set_enabled(value: i32) {
    CAPTURE_ENABLED.store(value != 0, Ordering::Relaxed);
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_capture_get_enabled() -> i32 {
    if enabled() {
        1
    } else {
        0
    }
}

// --- Rust-side helpers (used by state.rs / callstream_protocol.rs) ---------

/// Reset the thread-local buffer at frame start. Called from the render path
/// before the host emits ImGui calls.
pub fn frame_begin() {
    // SAFETY: clears a thread-local Vec; no UB.
    unsafe { imgui_wasm_capture_frame_begin() };
}

/// Drain this frame's captured call bytes into an owned Vec. The thread-local
/// buffer is cleared, ready for the next frame. Returns an empty Vec when
/// capture is disabled.
pub fn frame_drain() -> Vec<u8> {
    if !enabled() {
        // Still clear any stale bytes defensively.
        BUFFER.with(|b| b.borrow_mut().buf.clear());
        return Vec::new();
    }
    let mut out = Vec::new();
    BUFFER.with(|b| {
        let mut b = b.borrow_mut();
        std::mem::swap(&mut out, &mut b.buf);
    });
    out
}

/// Drain the string table's "newly added" entries: pairs of (id, bytes) that
/// must be announced to clients before the next frame can be decoded. Each new
/// string is announced exactly once (across all clients, since the table is
/// shared).
pub fn drain_new_strings() -> Vec<(u32, Vec<u8>)> {
    with_string_table(|t| std::mem::take(&mut t.pending_new))
}

/// Look up a string by id (used only for parity tests / debugging). Returns
/// None if the id is unknown.
pub fn lookup_string(id: u32) -> Option<Vec<u8>> {
    if id == NULL_STRING_ID {
        return Some(Vec::new());
    }
    with_string_table(|t| {
        t.by_bytes
            .iter()
            .find_map(|(k, &v)| if v == id { Some(k.clone()) } else { None })
    })
}

/// Snapshot of the entire current string table (id, bytes). Used to replay the
/// full table to a newly-connected client so it can decode frames immediately,
/// mirroring the texture replay in server.rs::handle_ws.
pub fn snapshot_all_strings() -> Vec<(u32, Vec<u8>)> {
    with_string_table(|t| t.by_bytes.iter().map(|(k, &v)| (v, k.clone())).collect())
}

pub fn set_enabled(on: bool) {
    CAPTURE_ENABLED.store(on, Ordering::Relaxed);
}

pub fn is_enabled() -> bool {
    enabled()
}

// Suppress unused warning for ptr::read_volatile; kept as a future-proofing
// marker that this module does not mutate ImGui state.
#[allow(dead_code)]
fn _no_op_marker() {
    let _ = ptr::null::<u8>();
}
