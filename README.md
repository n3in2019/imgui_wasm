# ImGuiWasm

[![CI](https://github.com/n3in2019/imgui_wasm/actions/workflows/ci.yml/badge.svg)](https://github.com/n3in2019/imgui_wasm/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

ImGuiWasm is an experimental web backend for [Dear ImGui](https://github.com/ocornut/imgui).
Applications run their ImGui context in native C++ while the core streams
captured API calls over WebSocket to an embedded WebGL frontend that replays
them in a WASM-compiled Dear ImGui twin.

> [!IMPORTANT]
> ImGuiWasm is early-stage software. It does not provide authentication or
> authorization and should not be exposed directly to an untrusted network.

## Features

- Native Dear ImGui rendering streamed to a browser
- Embedded HTTP, WebSocket, and WebGL frontend
- Semantic call-stream transport with the ordinary Dear ImGui `ImGui::` API
- C lifecycle API with a thin C++ wrapper
- Multi-client input and clipboard support

## Architecture

| Component | Responsibility |
| --- | --- |
| `imgui_ws` | Pure C++17 core: HTTP/WebSocket server, call-stream capture and framing, C ABI, Dear ImGui backend, and embedded frontend |

The call-stream transport captures supported API calls and replays them in a
WASM Dear ImGui twin in the browser. C++ application code uses the same
official `ImGui::Begin`, `ImGui::Button`, and other Dear ImGui calls; capture
redirection is internal to the public ImGuiWasm header.

## Requirements

- Git
- CMake 3.10 or newer
- A C++17 compiler
- Emscripten (`em++`) on `PATH`, only when regenerating the committed WASM
  replay twin

Dear ImGui `v1.92.8` is fetched automatically on the first CMake configure and
kept under the ignored `third_party/` directory. Regenerate the
browser WASM replay twin with `python3 tools/build_wasm_twin.py`
after changing `wasm/` or the opcode header; set `IMGUI_WASM_EMCC`
only when the Emscripten compiler is not available as `em++`.

## Quick start

Build the core and example from the repository root:

```bash
cmake -B build
cmake --build build
./build/examples/example_core_cpp_callstream
```

Then open <http://127.0.0.1:8888>.

To bind to a different address, pass it to the example:

```bash
./build/examples/example_core_cpp_callstream 0.0.0.0:8888
```

## Development

Run the primary checks before opening a pull request:

```bash
cmake -B build
cmake --build build
./build/tests/core_cpp_tests
node --check frontend/imgui_wasm.js
node --check frontend/imgui_wasm_callstream.js
python3 -m py_compile tools/generate_bindings.py
git diff --check
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow and binding
generation rules. Release changes are recorded in [CHANGELOG.md](CHANGELOG.md).

## Security

Please do not report vulnerabilities in public issues. Follow the private
reporting guidance in [SECURITY.md](SECURITY.md).

## License

ImGuiWasm is available under the [MIT License](LICENSE). Generated and upstream-
derived files retain the notices documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
