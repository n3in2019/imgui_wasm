#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* lifecycle */

/* Transport selects how draw data reaches the browser.
   0 = draw-data (default): the server serializes ImGui's rendered vertex/
       index/command buffers each frame (binary protocol messages 0x01-0x05).
   1 = call-stream: the server captures the sequence of ImGui API calls and
       streams them (messages 0x07-0x09); the browser replays them against a
       WASM-compiled Dear ImGui twin to regenerate draw data locally. Drastic
       bandwidth reduction; requires the WASM twin to be built and served. */
#define IMGUI_WASM_TRANSPORT_DRAWDATA   0
#define IMGUI_WASM_TRANSPORT_CALLSTREAM 1

typedef struct {
    const char* host_port;
    int compression;
    int config_flags;
    int dark_style;
    /* 0 = draw-data (default), 1 = call-stream. See IMGUI_WASM_TRANSPORT_*. */
    int transport;
} imgui_wasm_config_t;

int  imgui_wasm_init(const imgui_wasm_config_t* config);
void imgui_wasm_shutdown();
void imgui_wasm_new_frame();
void imgui_wasm_render();

#ifdef __cplusplus
}
#endif
