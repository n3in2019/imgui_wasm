// imgui_wasm_web_backend.cpp — the browser-side replay twin's ImGui web backend.
//
// This file is compiled (by build.rs, Phase 3) into imgui_wasm_replay.wasm via
// emscripten. It owns the twin ImGui context and exposes a minimal C ABI that
// the frontend JS calls:
//
//   imgui_wasm_replay_init(canvas_w, canvas_h)
//       Create the ImGui context, configure IO for the browser (no native
//       clipboard/cursor; input arrives via imgui_wasm_replay_set_*). Set the
//       display size + framebuffer scale.
//
//   imgui_wasm_replay_frame(call_bytes, call_len, call_count,
//                       dpx,dpy,dsw,dsh,fbsx,fbsy)
//       Run one replay frame: ImGui::NewFrame(), dispatch the captured calls
//       via the generated replay switch (replay_switch.cpp), ImGui::Render(),
//       then return a pointer to the serialized ImDrawData in the same flat
//       vtx/idx/cmd byte format the existing frontend already renders.
//
//   imgui_wasm_replay_set_string(id, bytes, len)
//       Install/replace an interned string in the string table (from 0x09).
//
//   imgui_wasm_replay_input_mouse_move(x, y) / _mouse_down(button) / ...
//       Mirror the browser's input into the twin's io so its state (hover,
//       active, scroll) matches the server's. The server is authoritative for
//       geometry; this keeps widget interaction visuals in sync.
//
//   imgui_wasm_replay_draw_data(out_len)
//       Return the serialized ImDrawData from the last replay_frame. The
//       frontend reads vtx/idx/cmds from this pointer (linear memory) and
//       uploads to WebGL exactly as it does for draw-data frames today.
//
// The serialized draw-data layout matches imgui-wasm-core/src/protocol.rs exactly
// (vtx stride 20, u32 indices, per-cmd clip_rect/tex/offsets/elem_count), so
// the existing renderFromParsed (imgui_wasm.js:203) renders it unchanged.

#include "imgui.h"
#include "imgui_wasm_opcodes.h"
#ifdef IMGUI_WASM_REPLAY_DEBUG
#include "imgui_internal.h"
#endif
#include <emscripten.h>
#include <stdint.h>
#include <string.h>
#include <unordered_map>
#include <vector>

// Generated replay dispatch (one case per opcode). Declared by replay_switch.cpp.
extern "C" void imweb_replay_calls(const unsigned char* body, unsigned body_len, unsigned call_count);

namespace {

ImGuiContext* g_ctx = nullptr;
ImGuiIO* g_io = nullptr;

// Session string table: id -> std::string. id 0 is reserved (NULL).
std::unordered_map<unsigned, std::string> g_strings;
// Owning storage for strings so the returned const char* stays valid. We never
// evict (labels are stable for a session); see the texture-eviction note in
// AGENTS.md for the parallel concern on the draw-data path.

// Output buffer holding the serialized ImDrawData of the last frame. Returned
// to JS as a raw pointer into WASM linear memory.
std::vector<unsigned char> g_draw_out;

void set_string(unsigned id, const char* bytes, unsigned len) {
    if (id == 0) return; // reserved
    g_strings[id].assign(bytes, len);
}

} // namespace

// The generated replay_switch.cpp calls ImGuiWasm::replay::lookup_string(id).
// Define it here (in that namespace) backed by the session string table.
namespace ImGuiWasm { namespace replay {
const char* lookup_string(unsigned id) {
    if (id == 0) return nullptr;
    auto it = g_strings.find(id);
    const char* r = it == g_strings.end() ? "" : it->second.c_str();
#ifdef IMGUI_WASM_REPLAY_DEBUG
    static unsigned last = 0;
    if (id != last) { fprintf(stderr, "[imgui_wasm_replay] lookup(%u) -> \"%s\"\n", id, r ? r : "(null)"); last = id; }
#endif
    return r;
}
}}

extern "C" {

EMSCRIPTEN_KEEPALIVE
int imgui_wasm_replay_init(int canvas_w, int canvas_h) {
    if (g_ctx) return 1; // already initialized
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    g_io = &ImGui::GetIO();
    g_io->IniFilename = nullptr;

    // The twin does not need a full platform backend: input is injected via
    // imgui_wasm_replay_input_*, and we render to draw data (not to a GL context).
    g_io->BackendPlatformName = "imgui_wasm_replay";
    g_io->BackendRendererName = "imgui_wasm_replay";
    g_io->DisplaySize = ImVec2((float)canvas_w, (float)canvas_h);
    g_io->DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    g_io->DeltaTime = 1.0f / 60.0f;

    // Font atlas: mirror the server's setup EXACTLY. The server's backend
    // (imgui_backend.cpp) calls io.Fonts->Build() with no explicit font added,
    // so ImGui auto-adds its default during Build(). We replicate that here —
    // same call, no explicit AddFontDefault (which could pick a different
    // font than the auto-add path, diverging layout).
    g_io->Fonts->Build();
    g_io->Fonts->TexIsBuilt = true;
    // Assign a non-zero TexID to the baked font texture so ImDrawCmd::GetTexID()
    // does not assert. The browser already holds the real font texture (sent
    // via 0x02 by the server); the twin only needs a stable, non-null id here.
    // TexID 1 matches the id the server's texture allocator assigns first.
    if (!g_io->Fonts->TexList.empty()) {
        ImTextureData* tex = g_io->Fonts->TexList[0];
        if (tex && tex->TexID == 0) {
            tex->SetTexID((ImTextureID)(intptr_t)1);
            tex->Status = ImTextureStatus_OK;
        }
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void imgui_wasm_replay_set_display_size(int w, int h) {
    if (!g_io) return;
    g_io->DisplaySize = ImVec2((float)w, (float)h);
}

EMSCRIPTEN_KEEPALIVE
void imgui_wasm_replay_set_string(unsigned id, const char* bytes, unsigned len) {
    set_string(id, bytes, len);
}

// --- Input mirroring (keeps twin state in sync with server io) ------------

EMSCRIPTEN_KEEPALIVE
void imgui_wasm_replay_input_mouse_pos(float x, float y) {
    if (g_io) g_io->AddMousePosEvent(x, y);
}
EMSCRIPTEN_KEEPALIVE
void imgui_wasm_replay_input_mouse_down(int button) {
    if (g_io) g_io->AddMouseButtonEvent(button, true);
}
EMSCRIPTEN_KEEPALIVE
void imgui_wasm_replay_input_mouse_up(int button) {
    if (g_io) g_io->AddMouseButtonEvent(button, false);
}
EMSCRIPTEN_KEEPALIVE
void imgui_wasm_replay_input_wheel(float dx, float dy) {
    if (g_io) g_io->AddMouseWheelEvent(dx, dy);
}
EMSCRIPTEN_KEEPALIVE
void imgui_wasm_replay_input_key(int imgui_key, int down) {
    if (g_io) {
        g_io->AddKeyEvent((ImGuiKey)imgui_key, down != 0);
    }
}
EMSCRIPTEN_KEEPALIVE
void imgui_wasm_replay_input_char(unsigned int ch) {
    if (g_io) g_io->AddInputCharacter(ch);
}

// --- The replay frame ------------------------------------------------------

// Serialize ImDrawData into the flat format the frontend renders. Layout
// matches protocol.rs::write_draw_list exactly (vtx stride 20, u32 idx).
static void serialize_draw_data(ImDrawData* dd, std::vector<unsigned char>& out) {
    out.clear();
    // Per-list: vtx_count, idx_count, cmd_count (u32 each), then raw vtx,
    // raw idx (widened to u32), then per-cmd fields.
    for (int n = 0; n < dd->CmdLists.Size; n++) {
        ImDrawList* list = dd->CmdLists[n];
        const ImVector<ImDrawVert>& vtx = list->VtxBuffer;
        const ImVector<ImDrawIdx>& idx = list->IdxBuffer;
        const ImVector<ImDrawCmd>& cmds = list->CmdBuffer;

        unsigned vtx_count = (unsigned)vtx.Size;
        unsigned idx_count = (unsigned)idx.Size;
        unsigned cmd_count = (unsigned)cmds.Size;

        auto push_u32 = [&](unsigned v) {
            out.push_back(v & 0xFF);
            out.push_back((v >> 8) & 0xFF);
            out.push_back((v >> 16) & 0xFF);
            out.push_back((v >> 24) & 0xFF);
        };
        push_u32(vtx_count);
        push_u32(idx_count);
        push_u32(cmd_count);

        // vtx: 20 bytes each, already in the right layout (pos/uv/col).
        const unsigned char* vtx_bytes = (const unsigned char*)vtx.Data;
        out.insert(out.end(), vtx_bytes, vtx_bytes + vtx_count * sizeof(ImDrawVert));

        // idx: widen ImDrawIdx (u16) -> u32, little-endian.
        for (unsigned i = 0; i < idx_count; i++) {
            push_u32((unsigned)idx.Data[i]);
        }

        // cmds: clip_rect(4xf32), tex id (u64 via the TexRef), idx/vtx offset,
        // elem count. Match imgui_wasm_draw_cmd_t in imgui_wasm_internal.h.
        for (unsigned c = 0; c < cmd_count; c++) {
            const ImDrawCmd& cmd = cmds[c];
            // clip_rect
            for (int f = 0; f < 4; f++) {
                float v = ((const float*)&cmd.ClipRect)[f];
                unsigned bits;
                memcpy(&bits, &v, 4);
                push_u32(bits);
            }
            // texture id: ImTextureRef -> ImTextureID (u64). On the wire the
            // draw-data path uses (uint64_t)(uintptr_t)GetTexID(); mirror it.
            uint64_t tex = (uint64_t)(uintptr_t)cmd.GetTexID();
            out.push_back(tex & 0xFF);
            out.push_back((tex >> 8) & 0xFF);
            out.push_back((tex >> 16) & 0xFF);
            out.push_back((tex >> 24) & 0xFF);
            out.push_back((tex >> 32) & 0xFF);
            out.push_back((tex >> 40) & 0xFF);
            out.push_back((tex >> 48) & 0xFF);
            out.push_back((tex >> 56) & 0xFF);
            // idx_offset, vtx_offset, elem_count
            push_u32(cmd.IdxOffset);
            push_u32(cmd.VtxOffset);
            push_u32(cmd.ElemCount);
        }
    }
}

EMSCRIPTEN_KEEPALIVE
const unsigned char* imgui_wasm_replay_frame(const unsigned char* call_bytes,
                                         unsigned call_len,
                                         unsigned call_count,
                                         float dpx, float dpy,
                                         float dsw, float dsh,
                                         float fbsx, float fbsy) {
    if (!g_ctx) return nullptr;
    ImGui::SetCurrentContext(g_ctx);
    // Frame scale/pos mirror the server so layout matches.
    g_io->DisplaySize = ImVec2(dsw, dsh);
    g_io->DisplayFramebufferScale = ImVec2(fbsx, fbsy);
    // The twin must claim focus every frame, otherwise ImGui skips rendering
    // all windows (the server does the same via io.AddFocusEvent(true)).
    g_io->AddFocusEvent(true);

    ImGui::NewFrame();
    // Dispatch the captured calls. The switch reads opcode u16 + args per the
    // schema, calling ImGui::<Widget> with server-echoed values for output
    // pointers. Output writes are discarded (scratch storage in the switch).
    imweb_replay_calls(call_bytes, call_len, call_count);
    ImGui::EndFrame();
    ImGui::Render();

    ImDrawData* dd = ImGui::GetDrawData();
#ifdef IMGUI_WASM_REPLAY_DEBUG
    {
        ImGuiContext& g = *GImGui;
        int win_count = g.Windows.Size;
        int win_active = 0;
        int win_hidden = 0;
        for (ImGuiWindow* w : g.Windows) {
            if (w->Active) win_active++;
            if (w->Hidden) win_hidden++;
        }
        if (dd) {
            fprintf(stderr, "[imgui_wasm_replay] CmdLists=%d Vtx=%d Win=%d Active=%d Hidden=%d Viewports=%d DpSz=(%g,%g) Built=%d RenderWindows=%d\n",
                    dd->CmdLists.Size, dd->TotalVtxCount, win_count, win_active, win_hidden,
                    g.Viewports.Size,
                    dd->DisplaySize.x, dd->DisplaySize.y,
                    g_io->Fonts ? (int)g_io->Fonts->TexIsBuilt : -1,
                    g_io->MetricsRenderWindows);
        }
    }
#endif
    serialize_draw_data(dd, g_draw_out);
    return g_draw_out.empty() ? nullptr : g_draw_out.data();
}

EMSCRIPTEN_KEEPALIVE
unsigned imgui_wasm_replay_draw_data_len() {
    return (unsigned)g_draw_out.size();
}

// The frontend needs to know how many draw lists were produced (each list's
// header is the first 12 bytes: vtx_count, idx_count, cmd_count). We expose a
// helper that returns the list count by walking the serialized buffer.
EMSCRIPTEN_KEEPALIVE
unsigned imgui_wasm_replay_list_count() {
    if (!g_ctx) return 0;
    ImGui::SetCurrentContext(g_ctx);
    ImDrawData* dd = ImGui::GetDrawData();
    return dd ? (unsigned)dd->CmdLists.Size : 0;
}

// --- Font atlas exposure --------------------------------------------------
// The twin's draw commands reference glyph UVs from THIS atlas. The frontend
// must upload the twin's atlas as the font texture (NOT the server's, whose
// glyph packing may differ). These getters let the frontend fetch the pixels
// once after init and upload them to WebGL texture id 1.

EMSCRIPTEN_KEEPALIVE
unsigned imgui_wasm_replay_font_tex_width() {
    if (!g_ctx) return 0;
    ImGui::SetCurrentContext(g_ctx);
    if (g_io->Fonts->TexList.empty()) return 0;
    return (unsigned)g_io->Fonts->TexList[0]->Width;
}

EMSCRIPTEN_KEEPALIVE
unsigned imgui_wasm_replay_font_tex_height() {
    if (!g_ctx) return 0;
    ImGui::SetCurrentContext(g_ctx);
    if (g_io->Fonts->TexList.empty()) return 0;
    return (unsigned)g_io->Fonts->TexList[0]->Height;
}

EMSCRIPTEN_KEEPALIVE
const unsigned char* imgui_wasm_replay_font_tex_pixels() {
    if (!g_ctx) return nullptr;
    ImGui::SetCurrentContext(g_ctx);
    if (g_io->Fonts->TexList.empty()) return nullptr;
    // The atlas pixels are stored in the texture's Pixels buffer. If not yet
    // materialized, force a GetTexDataAsRGBA32 call.
    ImTextureData* tex = g_io->Fonts->TexList[0];
    if (tex->Pixels == nullptr) {
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        g_io->Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        return pixels;
    }
    return tex->Pixels;
}

} // extern "C"
