#include "imgui_wasm.h"
#include "imgui_wasm_capture.h"
#include "imgui_wasm_internal.h"
#include "imgui_wasm_opcodes.h"

#include <cstdint>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"  // GImGui/SettingsDirtyTimer: INI refresh signal

static const imgui_wasm_core_api_t* GImGuiWasmCore = nullptr;
static bool GCallstream = false;
static unsigned GCallstreamFrameCount = 0;
static std::string GCallstreamIni;

struct ImGuiWasmBackendData {
    imgui_wasm_backend_t* Core;
};

static char GClipboardBuf[4096];

static const char* ImGuiWasm_GetClipboardText(void* user_data) {
    (void)user_data;
    if (GImGuiWasmCore && GImGuiWasmCore->get_clipboard_text) {
        GImGuiWasmCore->get_clipboard_text(GClipboardBuf, sizeof(GClipboardBuf));
    } else {
        GClipboardBuf[0] = '\0';
    }
    return GClipboardBuf;
}

static void ImGuiWasm_SetClipboardText(void* user_data, const char* text) {
    (void)user_data;
    if (GImGuiWasmCore && GImGuiWasmCore->set_clipboard_text && text) {
        GImGuiWasmCore->set_clipboard_text(text);
    }
}

static ImGuiWasmBackendData* ImGuiWasm_GetBackendData() {
    return ImGui::GetCurrentContext() ? (ImGuiWasmBackendData*)ImGui::GetIO().BackendRendererUserData
                                      : nullptr;
}

extern "C" void imgui_wasm_imgui_backend_set_core_api(const imgui_wasm_core_api_t* api) {
    GImGuiWasmCore = api;
}

extern "C" void imgui_wasm_imgui_backend_new_frame(imgui_wasm_backend_t* backend) {
    ImGuiIO& io = ImGui::GetIO();

    // Keep the replay twin on the authoritative server's persisted window
    // geometry. This call is captured before any Begin(), interned when the
    // INI is unchanged, and replayed before the browser creates its windows.
    // ImGui loads its disk INI lazily during the first NewFrame. Capture the
    // resulting authoritative snapshot on the following frame, cache it, and
    // prefix every captured frame with the same interned id. Repeating the
    // tiny reference lets clients that connect later restore geometry too;
    // the browser strips it after applying each distinct snapshot once.
    //
    // The snapshot must REFRESH whenever ImGui's window settings are dirty
    // (a window was moved, resized, or collapsed): re-saving mints a new
    // interned id, so connected clients re-apply the authoritative layout.
    // Without this a client that connects after a drag decodes frames
    // against stale geometry and its clicks miss the server-side windows.
    // SaveIniSettingsToMemory() resets the dirty timer, so each settings
    // change triggers exactly one re-save on the next frame.
    if (GCallstream && GCallstreamFrameCount++ > 0) {
        ImGuiContext& g = *GImGui;
        if (GCallstreamIni.empty() || g.SettingsDirtyTimer > 0.0f) {
            size_t ini_size = 0;
            const char* ini = ImGui::SaveIniSettingsToMemory(&ini_size);
            GCallstreamIni.assign(ini, ini_size);
        }
        imgui_wasm_capture_begin(OP_IG_LOAD_INI_SETTINGS_FROM_MEMORY);
        imgui_wasm_capture_string(GCallstreamIni.c_str());
        imgui_wasm_capture_u32((uint64_t)GCallstreamIni.size());
        imgui_wasm_capture_end();
    }

    imgui_wasm_event_t ev;
    while (GImGuiWasmCore->backend_poll_event(backend, &ev)) {
        switch (ev.type) {
            case IMGUI_WASM_EVENT_MOUSE_MOVE:
                io.AddMousePosEvent(ev.mouse_move.x, ev.mouse_move.y);
                break;
            case IMGUI_WASM_EVENT_MOUSE_DOWN:
                io.AddMouseButtonEvent(ev.mouse_button.button, true);
                break;
            case IMGUI_WASM_EVENT_MOUSE_UP:
                io.AddMouseButtonEvent(ev.mouse_button.button, false);
                break;
            case IMGUI_WASM_EVENT_MOUSE_WHEEL:
                io.AddMouseWheelEvent(ev.mouse_wheel.dx, ev.mouse_wheel.dy);
                break;
            case IMGUI_WASM_EVENT_KEY_DOWN:
                io.AddKeyEvent((ImGuiKey)ev.key.key, true);
                break;
            case IMGUI_WASM_EVENT_KEY_UP:
                io.AddKeyEvent((ImGuiKey)ev.key.key, false);
                break;
            case IMGUI_WASM_EVENT_TEXT_INPUT:
                io.AddInputCharacter(ev.text.ch);
                break;
        }
    }

    imgui_wasm_frame_info_t frame_info;
    GImGuiWasmCore->backend_new_frame(backend, ImGui::GetTime(), &frame_info);
    io.DeltaTime = frame_info.delta_time;
    io.DisplaySize = ImVec2(frame_info.display_w, frame_info.display_h);
    // The active client's devicePixelRatio: reaches the browser in the
    // call-stream header (fbsx/fbsy) and ImDrawData::FramebufferScale on the
    // legacy draw-data path. Layout itself stays in CSS pixels.
    io.DisplayFramebufferScale = ImVec2(frame_info.display_scale, frame_info.display_scale);
    io.AddFocusEvent(true);
    ImGui::NewFrame();
}

static void ImGuiWasm_HandleTexture(imgui_wasm_backend_t* backend, ImTextureData* tex) {
    if (backend == nullptr || tex == nullptr) return;

    if (tex->Status == ImTextureStatus_WantCreate) {
        uint64_t id = GImGuiWasmCore->backend_alloc_texture_id(backend);
        tex->SetTexID((ImTextureID)(uintptr_t)id);
        tex->SetStatus(ImTextureStatus_OK);

        if (tex->GetPixels()) {
            uint32_t byte_count = (uint32_t)tex->GetSizeInBytes();
            GImGuiWasmCore->send_texture(id, (const uint8_t*)tex->GetPixels(), byte_count,
                                     (uint32_t)tex->Width, (uint32_t)tex->Height);
        }
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        if (tex->GetPixels()) {
            uint32_t byte_count = (uint32_t)tex->GetSizeInBytes();
            GImGuiWasmCore->send_texture((uint64_t)(uintptr_t)tex->GetTexID(),
                                     (const uint8_t*)tex->GetPixels(), byte_count,
                                     (uint32_t)tex->Width, (uint32_t)tex->Height);
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

extern "C" void imgui_wasm_imgui_backend_render_draw_data(imgui_wasm_backend_t* backend,
                                                    const void* draw_data_ptr) {
    const ImDrawData* draw_data = (const ImDrawData*)draw_data_ptr;
    if (draw_data == nullptr) return;
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) return;

    if (draw_data->Textures != nullptr) {
        for (ImTextureData* tex : *draw_data->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                ImGuiWasm_HandleTexture(backend, tex);
            }
        }
    }

    if (!GImGuiWasmCore->begin_frame(draw_data->DisplayPos.x, draw_data->DisplayPos.y,
                                 draw_data->DisplaySize.x, draw_data->DisplaySize.y,
                                 draw_data->FramebufferScale.x, draw_data->FramebufferScale.y)) {
        return;
    }

    // The vertex/index/command buffers are not consumed: the browser's WASM
    // twin regenerates them from the captured calls. Only the frame header
    // (above) and the texture lifecycle (before it) are used from ImDrawData.
    GImGuiWasmCore->end_frame();
}

extern "C" bool imgui_wasm_imgui_backend_init(int config_flags, int dark_style, int callstream) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= config_flags;
    GCallstream = callstream != 0;
    GCallstreamFrameCount = 0;
    GCallstreamIni.clear();

    if (dark_style) {
        ImGui::StyleColorsDark();
    }

    ImGuiWasmBackendData* bd = IM_NEW(ImGuiWasmBackendData)();
    bd->Core = GImGuiWasmCore->backend_create();
    if (!bd->Core) {
        IM_DELETE(bd);
        ImGui::DestroyContext();
        return false;
    }

    io.BackendRendererUserData = (void*)bd;
    io.BackendPlatformUserData = (void*)bd;
    io.BackendPlatformName = "imgui_impl_imgui_wasm";
    io.BackendRendererName = "imgui_impl_imgui_wasm";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    io.GetClipboardTextFn = ImGuiWasm_GetClipboardText;
    io.SetClipboardTextFn = ImGuiWasm_SetClipboardText;

    io.Fonts->Build();

    return true;
}

extern "C" void imgui_wasm_imgui_backend_shutdown() {
    GCallstream = false;
    GCallstreamFrameCount = 0;
    GCallstreamIni.clear();
    ImGuiWasmBackendData* bd = ImGuiWasm_GetBackendData();
    if (bd) {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererUserData = nullptr;
        io.BackendPlatformUserData = nullptr;
        io.BackendPlatformName = nullptr;
        io.BackendRendererName = nullptr;
        GImGuiWasmCore->backend_destroy(bd->Core);
        IM_DELETE(bd);
    }
    if (ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
    }
}

extern "C" void imgui_wasm_imgui_backend_begin_frame() {
    ImGuiWasmBackendData* bd = ImGuiWasm_GetBackendData();
    IM_ASSERT(bd != nullptr && "Did you call imgui_wasm_init()?");
    imgui_wasm_imgui_backend_new_frame(bd->Core);
}

extern "C" void imgui_wasm_imgui_backend_render() {
    ImGuiWasmBackendData* bd = ImGuiWasm_GetBackendData();
    if (!bd) return;
    ImGui::Render();
    imgui_wasm_imgui_backend_render_draw_data(bd->Core, ImGui::GetDrawData());
}
