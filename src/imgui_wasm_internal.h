#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct imgui_wasm_backend_t imgui_wasm_backend_t;

typedef struct {
    float delta_time;
    float display_w;
    float display_h;
    float display_scale;  // client devicePixelRatio; drives io.DisplayFramebufferScale
} imgui_wasm_frame_info_t;

typedef struct {
    float clip_rect[4];
    uint64_t texture_id;
    uint32_t vtx_offset;
    uint32_t idx_offset;
    uint32_t elem_count;
} imgui_wasm_draw_cmd_t;

#define IMGUI_WASM_EVENT_MOUSE_MOVE  0
#define IMGUI_WASM_EVENT_MOUSE_DOWN  1
#define IMGUI_WASM_EVENT_MOUSE_UP    2
#define IMGUI_WASM_EVENT_MOUSE_WHEEL 3
#define IMGUI_WASM_EVENT_KEY_DOWN    4
#define IMGUI_WASM_EVENT_KEY_UP      5
#define IMGUI_WASM_EVENT_TEXT_INPUT  6

typedef struct {
    int type;
    float display_w, display_h;
    union {
        struct { float x, y; }       mouse_move;
        struct { int button; }       mouse_button;
        struct { float dx, dy; }     mouse_wheel;
        struct { int key; }          key;
        struct { unsigned int ch; }  text;
    };
} imgui_wasm_event_t;

typedef struct {
    imgui_wasm_backend_t* (*backend_create)();
    void (*backend_destroy)(imgui_wasm_backend_t* backend);
    void (*backend_new_frame)(imgui_wasm_backend_t* backend, double current_time,
                              imgui_wasm_frame_info_t* out_info);
    uint64_t (*backend_alloc_texture_id)(imgui_wasm_backend_t* backend);
    int (*backend_poll_event)(imgui_wasm_backend_t* backend, imgui_wasm_event_t* event);
    int (*begin_frame)(float dpx, float dpy, float dsw, float dsh, float fbsx, float fbsy);
    void (*add_draw_list)(const void* vtx_buffer, uint32_t vtx_count,
                          const void* idx_buffer, uint32_t idx_count, int idx_size,
                          const imgui_wasm_draw_cmd_t* cmd_buffer, uint32_t cmd_count);
    void (*end_frame)();
    void (*send_texture)(uint64_t id, const uint8_t* pixels,
                         uint32_t len, uint32_t width, uint32_t height);
    int (*get_clipboard_text)(char* buf, int buf_size);
    void (*set_clipboard_text)(const char* text);
} imgui_wasm_core_api_t;

void imgui_wasm_imgui_backend_set_core_api(const imgui_wasm_core_api_t* api);

#ifdef __cplusplus
}
#endif
