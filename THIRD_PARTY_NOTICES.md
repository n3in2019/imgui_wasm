# Third-Party Notices

ImGuiWasm includes generated source and metadata derived from these projects:

## Dear ImGui

- Project: https://github.com/ocornut/imgui
- License: MIT
- Copyright: Copyright (c) 2014-2024 Omar Cornut

The Rust core downloads and builds pinned Dear ImGui docking revision
`b61e56346a92cfcaf1f43a545ca37b0b32239654` (version 1.92.8) when
`imgui-wasm-core/third_party/imgui` is not already present. This matches the
version used for the committed generated bindings. If redistributed with Dear
ImGui sources or binaries, keep the Dear ImGui MIT license notice.

## dear_bindings

- Project: https://github.com/dearimgui/dear_bindings
- License: MIT

`imgui-wasm-core/include/imgui_wasm_imgui.h`, `imgui-wasm-core/src/imgui_wasm_imgui.cpp`, and the
JSON metadata under `imgui-wasm-core/tools/dear_bindings/` are generated with
`dear_bindings` revision `c9ff64913915df41c0f4beef485b98a1c685eda5`.
If regenerated or redistributed, keep applicable dear_bindings and Dear ImGui
notices.
