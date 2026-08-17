#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* lifecycle */

typedef struct {
    const char* host; /* IPv4 dotted quad; NULL = 127.0.0.1 */
    uint16_t port;    /* 0 = 8888 */
    /* Capacity caps: 0 = unlimited. */
    unsigned max_clients;
    unsigned max_clients_per_ip;
    /* PAM-backed HTTP Basic auth: when pam_service is non-NULL and
       non-empty, static pages and WebSocket upgrades require
       "Authorization: Basic" credentials verified against the named PAM
       service (libpam is loaded at runtime). Browsers show their native
       login dialog and forward the credentials to the upgrade. */
    const char* pam_service;
} imgui_wasm_config_t;

int  imgui_wasm_init(const imgui_wasm_config_t* config);
void imgui_wasm_shutdown();
void imgui_wasm_new_frame();
void imgui_wasm_render();

#ifdef __cplusplus
}
#endif
