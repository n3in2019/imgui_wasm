#!/usr/bin/env python3
"""Build the browser WASM replay twin with Emscripten.

Produces wasm/imgui_wasm_replay.{js,wasm}, which are committed and
embedded into the server binary at CMake build time. Run this after changing
wasm/*, the opcode header, or the generated replay switch; then
rebuild with CMake to re-embed the artifacts.

Requires an em++-style C++ driver on PATH (the C driver cannot link the
twin's libc++ usage); override with the IMGUI_WASM_EMCC environment variable.
Dear ImGui must already be fetched under third_party/imgui (any CMake
configure does this).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
IMGUI_DIR = ROOT / "third_party" / "imgui"
WASM_OUT = ROOT / "wasm"

EXPORTED_FUNCTIONS = ",".join(
    "_" + name
    for name in [
        "malloc",
        "free",
        "imgui_wasm_replay_init",
        "imgui_wasm_replay_set_display_size",
        "imgui_wasm_replay_set_string",
        "imgui_wasm_replay_frame",
        "imgui_wasm_replay_draw_data_len",
        "imgui_wasm_replay_list_count",
        "imgui_wasm_replay_font_tex_count",
        "imgui_wasm_replay_font_tex_width",
        "imgui_wasm_replay_font_tex_height",
        "imgui_wasm_replay_font_tex_pixels",
        "imgui_wasm_replay_font_tex_version",
        "imgui_wasm_replay_input_mouse_pos",
        "imgui_wasm_replay_input_mouse_down",
        "imgui_wasm_replay_input_mouse_up",
        "imgui_wasm_replay_input_wheel",
        "imgui_wasm_replay_input_key",
        "imgui_wasm_replay_input_char",
    ]
)


def main() -> int:
    imgui_sources = [IMGUI_DIR / name for name in (
        "imgui.cpp",
        "imgui_draw.cpp",
        "imgui_tables.cpp",
        "imgui_widgets.cpp",
        "imgui_demo.cpp",
    )]
    if not (IMGUI_DIR / "imgui.h").exists():
        print(
            f"Dear ImGui not found at {IMGUI_DIR}. Run "
            "'cmake -B build' once to fetch it.",
            file=sys.stderr,
        )
        return 1

    emcc = os.environ.get("IMGUI_WASM_EMCC") or shutil.which("em++")
    if emcc is None:
        print(
            "Emscripten C++ compiler (em++) not found on PATH. Install "
            "Emscripten or set IMGUI_WASM_EMCC to an em++-style driver.",
            file=sys.stderr,
        )
        return 1

    # Emscripten glue: MODULARIZE with EXPORT_NAME emits a factory assigned to
    # a global (var imgui_wasm_replay = ...). The frontend loads it via a
    # <script> tag and awaits the factory() Promise. We deliberately do NOT
    # use EXPORT_ES6: that mode drops the global assignment in favor of
    # `export default`, but the emscripten runtime still isn't a real ES
    # module. -Os for size; wasm32 needs no special link libs.
    command = [
        emcc,
        "-Os",
        "-std=c++17",
        "-I", str(ROOT / "include"),
        "-I", str(IMGUI_DIR),
        "-I", str(WASM_OUT),  # for generated/replay_switch.cpp's includes
        "-o", str(WASM_OUT / "imgui_wasm_replay.js"),
        str(WASM_OUT / "imgui_wasm_web_backend.cpp"),
        str(WASM_OUT / "generated" / "replay_switch.cpp"),
        *[str(source) for source in imgui_sources],
        "-Os",
        "-s", "MODULARIZE=1",
        "-s", "EXPORT_NAME=imgui_wasm_replay",
        "-s", "ALLOW_MEMORY_GROWTH=1",
        "-s", "EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAP32']",
        "-s", f"EXPORTED_FUNCTIONS=[{EXPORTED_FUNCTIONS}]",
        "-s", "ENVIRONMENT=web",
    ]

    print(f"Building ImGuiWasm WASM replay twin ({emcc})...")
    result = subprocess.run(command, cwd=ROOT)
    if result.returncode != 0:
        print("Emscripten failed building the WASM replay twin", file=sys.stderr)
        return result.returncode or 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
