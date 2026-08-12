mod callstream_protocol;
mod capture;
mod generated_imgui_api;
mod protocol;
mod server;
mod state;

use std::ffi::{c_void, CStr};
use std::os::raw::c_char;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};

use state::ImGuiWasmState;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ImGuiWasmMouseEvent {
    pub button: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ImGuiWasmMouseMoveEvent {
    pub x: f32,
    pub y: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ImGuiWasmMouseWheelEvent {
    pub dx: f32,
    pub dy: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ImGuiWasmKeyEvent {
    pub key: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ImGuiWasmTextEvent {
    pub ch: u32,
}

#[repr(C)]
pub union ImGuiWasmEventData {
    pub mouse_move: ImGuiWasmMouseMoveEvent,
    pub mouse_button: ImGuiWasmMouseEvent,
    pub mouse_wheel: ImGuiWasmMouseWheelEvent,
    pub key: ImGuiWasmKeyEvent,
    pub text: ImGuiWasmTextEvent,
}

#[repr(C)]
pub struct ImGuiWasmEvent {
    pub ev_type: i32,
    pub display_w: f32,
    pub display_h: f32,
    pub data: ImGuiWasmEventData,
}

#[repr(C)]
pub struct ImGuiWasmConfig {
    pub host_port: *const c_char,
    pub compression: i32,
    pub config_flags: i32,
    pub dark_style: i32,
    /// 0 = draw-data (default), 1 = call-stream. See IMGUI_WASM_TRANSPORT_*.
    pub transport: i32,
}

#[repr(C)]
pub struct ImGuiWasmFrameInfo {
    pub delta_time: f32,
    pub display_w: f32,
    pub display_h: f32,
}

#[repr(C)]
pub struct ImGuiWasmBackend {
    time: f64,
    next_texture_id: u64,
    display_w: f32,
    display_h: f32,
}

extern "C" {
    fn imgui_wasm_imgui_backend_set_core_api(api: *const ImGuiWasmCoreApi);
    fn imgui_wasm_imgui_backend_init(config_flags: i32, dark_style: i32, callstream: i32) -> bool;
    fn imgui_wasm_imgui_backend_shutdown();
    fn imgui_wasm_imgui_backend_begin_frame();
    fn imgui_wasm_imgui_backend_render();
}

#[repr(C)]
struct ImGuiWasmCoreApi {
    backend_create: extern "C" fn() -> *mut ImGuiWasmBackend,
    backend_destroy: unsafe extern "C" fn(*mut ImGuiWasmBackend),
    backend_new_frame: unsafe extern "C" fn(*mut ImGuiWasmBackend, f64, *mut ImGuiWasmFrameInfo),
    backend_alloc_texture_id: unsafe extern "C" fn(*mut ImGuiWasmBackend) -> u64,
    backend_poll_event: unsafe extern "C" fn(*mut ImGuiWasmBackend, *mut ImGuiWasmEvent) -> i32,
    begin_frame: unsafe extern "C" fn(f32, f32, f32, f32, f32, f32) -> i32,
    add_draw_list: unsafe extern "C" fn(
        *const c_void,
        u32,
        *const c_void,
        u32,
        i32,
        *const state::DrawCmd,
        u32,
    ),
    end_frame: extern "C" fn(),
    send_texture: unsafe extern "C" fn(u64, *const u8, u32, u32, u32),
    get_clipboard_text: unsafe extern "C" fn(*mut c_char, i32) -> i32,
    set_clipboard_text: unsafe extern "C" fn(*const c_char),
}

static CORE_API: ImGuiWasmCoreApi = ImGuiWasmCoreApi {
    backend_create: imgui_wasm_backend_create,
    backend_destroy: imgui_wasm_backend_destroy,
    backend_new_frame: imgui_wasm_backend_new_frame,
    backend_alloc_texture_id: imgui_wasm_backend_alloc_texture_id,
    backend_poll_event: imgui_wasm_backend_poll_event,
    begin_frame: imgui_wasm_begin_frame,
    add_draw_list: imgui_wasm_add_draw_list,
    end_frame: imgui_wasm_end_frame,
    send_texture: imgui_wasm_send_texture,
    get_clipboard_text: imgui_wasm_get_clipboard_text,
    set_clipboard_text: imgui_wasm_set_clipboard_text,
};

static INITIALIZED: AtomicBool = AtomicBool::new(false);
static BACKEND_INITIALIZED: AtomicBool = AtomicBool::new(false);
static GLOBAL: OnceLock<Mutex<Option<GlobalCtx>>> = OnceLock::new();
static COMPRESSION: AtomicBool = AtomicBool::new(false);
/// 0 = draw-data, 1 = call-stream. Read by the render path to decide whether
/// to serialize ImDrawData (state.rs) or drain the capture buffer
/// (callstream_protocol.rs).
static TRANSPORT: std::sync::atomic::AtomicI32 = std::sync::atomic::AtomicI32::new(0);

struct GlobalCtx {
    state: std::sync::Arc<ImGuiWasmState>,
    _runtime: tokio::runtime::Runtime,
}

fn global() -> &'static Mutex<Option<GlobalCtx>> {
    GLOBAL.get_or_init(|| Mutex::new(None))
}

pub fn is_compression_enabled() -> bool {
    COMPRESSION.load(Ordering::SeqCst)
}

/// True when call-stream transport is active (server.rs uses this to decide
/// whether to replay the string table to new clients on connect).
pub fn is_callstream_enabled() -> bool {
    TRANSPORT.load(Ordering::SeqCst) != 0
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_init(config: *const ImGuiWasmConfig) -> i32 {
    if INITIALIZED.load(Ordering::SeqCst) {
        eprintln!("[imgui_wasm] Already initialized");
        return 0;
    }

    let addr_str = if config.is_null() {
        "127.0.0.1:8888".to_string()
    } else {
        let cfg = &*config;
        if cfg.host_port.is_null() {
            "127.0.0.1:8888".to_string()
        } else {
            CStr::from_ptr(cfg.host_port)
                .to_str()
                .unwrap_or("127.0.0.1:8888")
                .to_string()
        }
    };

    let compression = if config.is_null() {
        false
    } else {
        (*config).compression != 0
    };
    COMPRESSION.store(compression, Ordering::SeqCst);
    let transport = if config.is_null() {
        0
    } else {
        (*config).transport
    };
    TRANSPORT.store(transport, Ordering::SeqCst);
    // Call-stream transport enables the capture buffer so host wrappers
    // (imgui_wasm_capture.hpp) record their calls. Draw-data mode leaves capture
    // off; the wrappers then run as zero-cost forwards.
    capture::set_enabled(transport != 0);
    if transport != 0 {
        eprintln!("[imgui_wasm] Transport: call-stream (WASM replay twin required in browser)");
    }
    let config_flags = if config.is_null() {
        0
    } else {
        (*config).config_flags
    };
    let dark_style = if config.is_null() {
        1
    } else {
        (*config).dark_style
    };

    let addr: std::net::SocketAddr = match addr_str.parse() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("[imgui_wasm] Invalid address '{}': {}", addr_str, e);
            return -1;
        }
    };

    let runtime = match tokio::runtime::Runtime::new() {
        Ok(r) => r,
        Err(e) => {
            eprintln!("[imgui_wasm] Failed to create tokio runtime: {}", e);
            return -2;
        }
    };

    let (state, _frame_rx) = ImGuiWasmState::new();
    let state = std::sync::Arc::new(state);

    let state_clone = state.clone();
    runtime.spawn(async move {
        server::run_server(state_clone, addr).await;
    });

    {
        let mut g = global().lock().unwrap();
        *g = Some(GlobalCtx {
            state,
            _runtime: runtime,
        });
    }

    imgui_wasm_imgui_backend_set_core_api(&CORE_API);
    if !imgui_wasm_imgui_backend_init(config_flags, dark_style, transport) {
        let mut g = global().lock().unwrap();
        *g = None;
        return -3;
    }
    BACKEND_INITIALIZED.store(true, Ordering::SeqCst);
    INITIALIZED.store(true, Ordering::SeqCst);

    eprintln!("[imgui_wasm] Initialized, open http://{} in your browser", addr);
    0
}

#[no_mangle]
pub extern "C" fn imgui_wasm_shutdown() {
    if !INITIALIZED.load(Ordering::SeqCst) {
        return;
    }
    if BACKEND_INITIALIZED.swap(false, Ordering::SeqCst) {
        unsafe {
            imgui_wasm_imgui_backend_shutdown();
        }
    }
    {
        let mut g = global().lock().unwrap();
        *g = None;
    }
    INITIALIZED.store(false, Ordering::SeqCst);
    eprintln!("[imgui_wasm] Shutdown complete");
}

extern "C" fn imgui_wasm_backend_create() -> *mut ImGuiWasmBackend {
    let size = {
        let g = global().lock().unwrap();
        g.as_ref()
            .map(|ctx| ctx.state.get_display_size())
            .unwrap_or([1280.0, 720.0])
    };

    Box::into_raw(Box::new(ImGuiWasmBackend {
        time: 0.0,
        next_texture_id: 1,
        display_w: size[0],
        display_h: size[1],
    }))
}

unsafe extern "C" fn imgui_wasm_backend_destroy(backend: *mut ImGuiWasmBackend) {
    if !backend.is_null() {
        drop(Box::from_raw(backend));
    }
}

unsafe extern "C" fn imgui_wasm_backend_new_frame(
    backend: *mut ImGuiWasmBackend,
    current_time: f64,
    out_info: *mut ImGuiWasmFrameInfo,
) {
    if backend.is_null() || out_info.is_null() {
        return;
    }

    let backend = &mut *backend;
    let mut delta_time = if backend.time > 0.0 {
        (current_time - backend.time) as f32
    } else {
        1.0 / 60.0
    };
    backend.time = current_time;
    if delta_time <= 0.0 {
        delta_time = 1.0 / 10000.0;
    }

    let size = {
        let g = global().lock().unwrap();
        g.as_ref()
            .map(|ctx| ctx.state.get_display_size())
            .unwrap_or([backend.display_w, backend.display_h])
    };
    if size[0] > 0.0 && size[1] > 0.0 {
        backend.display_w = size[0];
        backend.display_h = size[1];
    }

    *out_info = ImGuiWasmFrameInfo {
        delta_time,
        display_w: backend.display_w,
        display_h: backend.display_h,
    };
}

unsafe extern "C" fn imgui_wasm_backend_alloc_texture_id(backend: *mut ImGuiWasmBackend) -> u64 {
    if backend.is_null() {
        return 0;
    }

    let backend = &mut *backend;
    let id = backend.next_texture_id;
    backend.next_texture_id = backend.next_texture_id.saturating_add(1);
    id
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_new_frame() {
    // Call-stream mode: reset the capture buffer BEFORE ImGui::NewFrame() and
    // the host render callback run, so the host's imgui_wasm:: widget calls are
    // recorded into a fresh buffer. The buffer is drained at end_frame (after
    // ImGui::Render()). frame_begin is a no-op in draw-data mode.
    if TRANSPORT.load(Ordering::SeqCst) != 0 {
        capture::frame_begin();
    }
    imgui_wasm_imgui_backend_begin_frame();
}

#[no_mangle]
pub unsafe extern "C" fn imgui_wasm_render() {
    imgui_wasm_imgui_backend_render();
}

unsafe extern "C" fn imgui_wasm_begin_frame(
    dpx: f32,
    dpy: f32,
    dsw: f32,
    dsh: f32,
    fbsx: f32,
    fbsy: f32,
) -> i32 {
    let g = global().lock().unwrap();
    let Some(ctx) = g.as_ref() else { return 0 };
    // Call-stream mode: the capture buffer was already reset in imgui_wasm_new_frame
    // (before the host render callback). Here we just stash the frame header
    // (DisplayPos/Size/Scale) for end_frame's 0x07 envelope, and gate on
    // connected clients. add_draw_list is a no-op in this mode.
    if TRANSPORT.load(Ordering::SeqCst) != 0 {
        if !ctx.state.has_clients() {
            return 0;
        }
        ctx.state
            .set_callstream_header(dpx, dpy, dsw, dsh, fbsx, fbsy);
        return 1;
    }
    if ctx.state.begin_frame(dpx, dpy, dsw, dsh, fbsx, fbsy) {
        1
    } else {
        0
    }
}

unsafe extern "C" fn imgui_wasm_backend_poll_event(
    backend: *mut ImGuiWasmBackend,
    out: *mut ImGuiWasmEvent,
) -> i32 {
    let result = imgui_wasm_poll_event(out);
    if result != 0 && !backend.is_null() && !out.is_null() {
        let backend = &mut *backend;
        let event = &*out;
        if event.display_w > 0.0 && event.display_h > 0.0 {
            backend.display_w = event.display_w;
            backend.display_h = event.display_h;
        }
    }
    result
}

unsafe extern "C" fn imgui_wasm_add_draw_list(
    vtx_buffer: *const c_void,
    vtx_count: u32,
    idx_buffer: *const c_void,
    idx_count: u32,
    idx_size: i32,
    cmd_buffer: *const state::DrawCmd,
    cmd_count: u32,
) {
    // Call-stream mode ignores draw data: the browser regenerates it from the
    // captured call sequence. Short-circuit to avoid the vertex copies.
    if TRANSPORT.load(Ordering::SeqCst) != 0 {
        return;
    }
    if vtx_buffer.is_null() || idx_buffer.is_null() || cmd_buffer.is_null() {
        return;
    }
    let vtx_slice = std::slice::from_raw_parts(vtx_buffer as *const u8, (vtx_count as usize) * 20);
    let idx_slice = std::slice::from_raw_parts(
        idx_buffer as *const u8,
        (idx_count as usize) * (idx_size as usize),
    );
    let cmd_slice = std::slice::from_raw_parts(cmd_buffer, cmd_count as usize);

    let g = global().lock().unwrap();
    if let Some(ctx) = g.as_ref() {
        ctx.state
            .add_draw_list(vtx_slice, idx_slice, idx_size, cmd_slice.to_vec());
    }
}

extern "C" fn imgui_wasm_end_frame() {
    let g = global().lock().unwrap();
    let Some(ctx) = g.as_ref() else { return };
    // Call-stream mode: drain the captured calls and broadcast a 0x07 frame
    // (plus a 0x09 string update if new labels were interned this frame).
    if TRANSPORT.load(Ordering::SeqCst) != 0 {
        ctx.state.end_callstream_frame();
        return;
    }
    ctx.state.end_frame();
}

unsafe extern "C" fn imgui_wasm_send_texture(
    id: u64,
    data: *const u8,
    len: u32,
    width: u32,
    height: u32,
) {
    if data.is_null() || len == 0 {
        return;
    }
    let slice = std::slice::from_raw_parts(data, len as usize);
    let msg = protocol::make_texture_msg(id, width, height, slice);
    let g = global().lock().unwrap();
    if let Some(ctx) = g.as_ref() {
        ctx.state.send_texture(id, msg);
    }
}

unsafe extern "C" fn imgui_wasm_poll_event(out: *mut ImGuiWasmEvent) -> i32 {
    if out.is_null() {
        return 0;
    }
    let g = global().lock().unwrap();
    if let Some(ctx) = g.as_ref() {
        if let Some(ev) = ctx.state.try_poll_input() {
            let size = ctx.state.get_display_size();
            *out = ImGuiWasmEvent {
                ev_type: ev.ev_type,
                display_w: size[0],
                display_h: size[1],
                data: match ev.ev_type {
                    0 => ImGuiWasmEventData {
                        mouse_move: ImGuiWasmMouseMoveEvent { x: ev.x, y: ev.y },
                    },
                    1 | 2 => ImGuiWasmEventData {
                        mouse_button: ImGuiWasmMouseEvent { button: ev.button },
                    },
                    3 => ImGuiWasmEventData {
                        mouse_wheel: ImGuiWasmMouseWheelEvent {
                            dx: ev.wheel_x,
                            dy: ev.wheel_y,
                        },
                    },
                    4 | 5 => ImGuiWasmEventData {
                        key: ImGuiWasmKeyEvent { key: ev.key },
                    },
                    6 => ImGuiWasmEventData {
                        text: ImGuiWasmTextEvent { ch: ev.character },
                    },
                    _ => ImGuiWasmEventData {
                        mouse_move: ImGuiWasmMouseMoveEvent { x: 0.0, y: 0.0 },
                    },
                },
            };
            return 1;
        }
    }
    0
}

unsafe extern "C" fn imgui_wasm_get_clipboard_text(buf: *mut c_char, buf_size: i32) -> i32 {
    let g = global().lock().unwrap();
    if let Some(ctx) = g.as_ref() {
        let text = ctx.state.get_clipboard_text();
        let bytes = text.as_bytes();
        let copy_len = std::cmp::min(bytes.len(), (buf_size - 1) as usize).max(0);
        if copy_len > 0 && !buf.is_null() {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, copy_len);
        }
        if !buf.is_null() && buf_size > 0 {
            *buf.add(copy_len) = 0;
        }
        return copy_len as i32;
    }
    if !buf.is_null() && buf_size > 0 {
        *buf = 0;
    }
    0
}

unsafe extern "C" fn imgui_wasm_set_clipboard_text(text: *const c_char) {
    if text.is_null() {
        return;
    }
    let cstr = CStr::from_ptr(text);
    let text_str = cstr.to_str().unwrap_or("");
    let g = global().lock().unwrap();
    if let Some(ctx) = g.as_ref() {
        ctx.state.set_clipboard_text(text_str);
    }
}
