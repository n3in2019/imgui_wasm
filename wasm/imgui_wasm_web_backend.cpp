// imgui_wasm_web_backend.cpp — the browser-side replay twin's ImGui web backend.
//
// This file is compiled into imgui_wasm_replay.wasm by
// imgui_ws/tools/build_wasm_twin.py via emscripten. It owns the twin ImGui
// context and exposes a minimal C ABI that the frontend JS calls:
//
//   imgui_wasm_replay_init(css_w, css_h, density)
//       Create the ImGui context, configure IO for the browser (no native
//       clipboard/cursor; input arrives via imgui_wasm_replay_set_*). The
//       display size is the browser canvas in CSS pixels; density is the
//       browser devicePixelRatio (fonts rasterize at native device density).
//
//   imgui_wasm_replay_set_display_size(css_w, css_h, density)
//       Update the local display configuration (browser resize / zoom /
//       monitor change). Re-bakes the font atlas when the density changed.
//
//   imgui_wasm_replay_font_tex_version()
//       Counter bumped whenever the font atlas is re-baked; the frontend
//       re-uploads the texture pixels when it changes.
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
// The serialized draw-data layout (vtx stride 20, u32 indices, per-cmd
// clip_rect/tex/offsets/elem_count) matches what the existing
// renderFromParsed (imgui_wasm.js:203) renders, unchanged.

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

// Local (browser-side) display configuration. The twin lays out at the
// browser's real resolution, not the server's: every browser client replays
// the same call stream against its own canvas.
float g_display_density = 1.0f;    // browser devicePixelRatio
unsigned g_font_tex_version = 1;   // bumped on each font atlas re-bake

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

// (Re)build the default font atlas rasterized at the browser's device pixel
// density. RasterizerDensity does not alter font metrics, so layout stays
// identical to the server's density-1.0 layout; only glyph sharpness
// changes. AddFontDefault(&cfg) selects the same font the server's plain
// Build() auto-adds, so glyph metrics match.
void build_font_atlas(float density) {
    g_io->Fonts->ClearFonts();
    ImFontConfig cfg;
    cfg.RasterizerDensity = density;
    g_io->Fonts->AddFontDefault(&cfg);
    g_io->Fonts->Build();
    g_io->Fonts->TexIsBuilt = true;
    // The atlas may produce MULTIPLE texture pages (at higher densities the
    // glyphs no longer fit one page). Every page referenced by draw commands
    // needs a stable non-zero TexID or ImDrawCmd::GetTexID() asserts (which
    // aborts the replay). Page ids start at 2^32, far above the server's
    // sequentially allocated texture ids, so twin atlas pages can never
    // collide with app textures streamed via 0x02.
    for (int i = 0; i < g_io->Fonts->TexList.Size; i++) {
        ImTextureData* tex = g_io->Fonts->TexList[i];
        if (!tex) continue;
        tex->SetTexID((ImTextureID)(uint64_t)(0x100000000ULL + (uint64_t)i));
        tex->Status = ImTextureStatus_OK;
    }
    g_font_tex_version++;
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
int imgui_wasm_replay_init(int css_w, int css_h, float density) {
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
    g_io->DisplaySize = ImVec2((float)css_w, (float)css_h);
    if (density <= 0.0f) density = 1.0f;
    g_io->DisplayFramebufferScale = ImVec2(density, density);
    g_io->DeltaTime = 1.0f / 60.0f;

    // Font atlas: same default font the server's backend auto-adds with its
    // plain io.Fonts->Build(), but rasterized at the browser's device pixel
    // density so glyphs are crisp on HiDPI displays. Density does not alter
    // metrics, so layout matches the server's exactly.
    g_display_density = density;
    build_font_atlas(density);
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void imgui_wasm_replay_set_display_size(int css_w, int css_h, float density) {
    if (!g_io) return;
    g_io->DisplaySize = ImVec2((float)css_w, (float)css_h);
    if (density <= 0.0f) return;
    g_io->DisplayFramebufferScale = ImVec2(density, density);
    // Re-bake only when the density actually changed: the browser fires
    // resize for pure CSS-size changes where the density is unchanged.
    if (density != g_display_density) {
        g_display_density = density;
        build_font_atlas(density);  // bumps g_font_tex_version
    }
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

// Serialize ImDrawData into the flat format the frontend renders
// (vtx stride 20, u32 idx).
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
            // draw-data path uses the u64 value; mirror it. No uintptr_t
            // round-trip: it is 32-bit in wasm32 and would truncate the
            // twin atlas page ids (>= 2^32).
            uint64_t tex = (uint64_t)cmd.GetTexID();
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
    // Display size/scale come from the LOCAL browser canvas (set via
    // imgui_wasm_replay_set_display_size), not from the header: each browser
    // replays the call stream at its own real resolution. The header's
    // dsw/dsh/fbsx/fbsy describe the server-side layout (authoritative for
    // input hit-testing and the legacy draw-data path) and are not used for
    // rendering here. The twin must claim focus every frame, otherwise ImGui
    // skips rendering all windows (the server does the same via
    // io.AddFocusEvent(true)).
    (void)dpx; (void)dpy; (void)dsw; (void)dsh; (void)fbsx; (void)fbsy;
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
// glyph packing may differ). The frontend uploads the pixels once after init
// and re-uploads whenever imgui_wasm_replay_font_tex_version() changes (the
// atlas is re-baked when the browser's devicePixelRatio changes).

EMSCRIPTEN_KEEPALIVE
unsigned imgui_wasm_replay_font_tex_version() {
    return g_font_tex_version;
}

EMSCRIPTEN_KEEPALIVE
unsigned imgui_wasm_replay_font_tex_count() {
    if (!g_ctx) return 0;
    ImGui::SetCurrentContext(g_ctx);
    return (unsigned)g_io->Fonts->TexList.Size;
}

EMSCRIPTEN_KEEPALIVE
unsigned imgui_wasm_replay_font_tex_width(int index) {
    if (!g_ctx) return 0;
    ImGui::SetCurrentContext(g_ctx);
    if (index < 0 || index >= g_io->Fonts->TexList.Size) return 0;
    return (unsigned)g_io->Fonts->TexList[index]->Width;
}

EMSCRIPTEN_KEEPALIVE
unsigned imgui_wasm_replay_font_tex_height(int index) {
    if (!g_ctx) return 0;
    ImGui::SetCurrentContext(g_ctx);
    if (index < 0 || index >= g_io->Fonts->TexList.Size) return 0;
    return (unsigned)g_io->Fonts->TexList[index]->Height;
}

EMSCRIPTEN_KEEPALIVE
const unsigned char* imgui_wasm_replay_font_tex_pixels(int index) {
    if (!g_ctx) return nullptr;
    ImGui::SetCurrentContext(g_ctx);
    if (index < 0 || index >= g_io->Fonts->TexList.Size) return nullptr;
    ImTextureData* tex = g_io->Fonts->TexList[index];
    // Materialize legacy RGBA data on first request when the page has no
    // direct pixel buffer yet.
    if (tex->Pixels == nullptr && index == 0) {
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        g_io->Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        return pixels;
    }
    return tex->Pixels;
}

} // extern "C"
