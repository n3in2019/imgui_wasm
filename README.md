# ImGuiWasm

[![CI](https://github.com/n3in2019/imgui_wasm/actions/workflows/ci.yml/badge.svg)](https://github.com/n3in2019/imgui_wasm/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Rust](https://img.shields.io/badge/rust-stable-orange.svg)](https://www.rust-lang.org/)

ImGuiWasm is an experimental web backend for [Dear ImGui](https://github.com/ocornut/imgui).
Applications run their ImGui context in native code while a Rust transport sends
frames over WebSocket to an embedded WebGL frontend.

> [!IMPORTANT]
> ImGuiWasm is early-stage software. It does not provide authentication or
> authorization and should not be exposed directly to an untrusted network.

## Features

- Native Dear ImGui rendering streamed to a browser
- Embedded HTTP, WebSocket, and WebGL frontend
- Draw-data and semantic call-stream transports
- C lifecycle API with thin C++ and Python bindings
- Multi-client input, clipboard, texture, compression, and delta-frame support
- The ordinary Dear ImGui `ImGui::` API in both transport modes

## Architecture

| Component | Responsibility |
| --- | --- |
| `imgui-wasm-core` | Rust transport, protocol, C ABI, native backend, and embedded frontend |
| `imgui-wasm-cpp` | C++ convenience wrapper and examples |
| `imgui-wasm-python` | Python lifecycle wrapper and generated ImGui bindings |

The default draw-data transport serializes native ImGui output. The optional
call-stream transport captures supported API calls and replays them in a WASM
Dear ImGui twin in the browser. C++ application code uses the same official
`ImGui::Begin`, `ImGui::Button`, and other Dear ImGui calls in either mode;
capture redirection is internal to the public ImGuiWasm header.

## Requirements

- Rust stable
- Git
- CMake 3.10 or newer
- A C++17 compiler
- Emscripten (`emcc`) on `PATH`
- Python 3.8 or newer for Python bindings

Dear ImGui `v1.92.8` is fetched automatically on the first Rust build and kept
under the ignored `imgui-wasm-core/third_party/` directory. Every Cargo build
also rebuilds the browser WASM replay twin; set `IMGUI_WASM_EMCC` only when the
Emscripten compiler is not available as `emcc`.

## Quick start

Build the Rust core and C++ example from the repository root:

```bash
cargo build --workspace
cmake -S imgui-wasm-cpp -B build -DIMGUI_WASM_BUILD_EXAMPLES=ON
cmake --build build
./build/example_cpp
```

Then open <http://127.0.0.1:8888>.

To bind to a different address, pass it to the example:

```bash
./build/example_cpp 0.0.0.0:8888
```

## Python example

Build the Rust core first, then run:

```bash
PYTHONPATH=imgui-wasm-python python3 imgui-wasm-python/examples/example.py
```

The Python package does not depend on `pyimgui`. Its lifecycle layer is
handwritten; its ImGui API is generated from committed `dear_bindings` metadata.

## Development

Run the primary checks before opening a pull request:

```bash
cargo test --workspace
cargo build --workspace
cmake -S imgui-wasm-cpp -B build -DIMGUI_WASM_BUILD_EXAMPLES=ON
cmake --build build
node --check imgui-wasm-core/frontend/imgui_wasm.js
node --check imgui-wasm-core/frontend/imgui_wasm_callstream.js
python3 -m py_compile imgui-wasm-python/tools/generate_bindings.py
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
