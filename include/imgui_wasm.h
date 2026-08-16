#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* lifecycle */

typedef struct {
    const char* host_port;
    int compression;
    int config_flags;
    int dark_style;
} imgui_wasm_config_t;

int  imgui_wasm_init(const imgui_wasm_config_t* config);
void imgui_wasm_shutdown();
void imgui_wasm_new_frame();
void imgui_wasm_render();

#ifdef __cplusplus
}
#endif
