# Binding generation

ImGuiWasm uses [`dear_bindings`](https://github.com/dearimgui/dear_bindings) to
generate its C API and machine-readable metadata from the pinned Dear ImGui
docking revision.

Regenerate the complete binding surface from the repository root:

```bash
python3 imgui-wasm-core/tools/dear_bindings/generate.py
```

The script uses temporary, pinned checkouts and emits:

- `include/imgui_wasm_imgui.h`
- `src/imgui_wasm_imgui.cpp`
- `tools/dear_bindings/imgui_wasm_imgui.json`
- the Rust dynamic-library export layer
- the Python API
- all semantic call-stream headers, schemas, and replay code

Commit the metadata and every changed generated artifact together. Keep
backend-private transport and frame helpers in `src/imgui_wasm_internal.h`.
