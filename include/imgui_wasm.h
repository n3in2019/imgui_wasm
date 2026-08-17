#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* lifecycle */

typedef struct {
    const char* host; /* IPv4 dotted quad; NULL = 127.0.0.1 */
    uint16_t port;    /* 0 = 8888 */
} imgui_wasm_config_t;

int  imgui_wasm_init(const imgui_wasm_config_t* config);
void imgui_wasm_shutdown();
void imgui_wasm_new_frame();
void imgui_wasm_render();

#ifdef __cplusplus
}
#endif
