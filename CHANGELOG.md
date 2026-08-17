# Changelog

All notable changes to imgui-wasm are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and releases use
[Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-08-16

- Initial imgui-wasm release: a pure C++17 call-stream core. The server
  captures ImGui API calls and streams them (messages `0x07`/`0x09`); the
  browser replays them against a WASM-compiled Dear ImGui twin to regenerate
  draw data locally. Per-frame bandwidth drops from KB of vertices to tens of
  bytes of calls, and idle frames cost ~nothing.
- Native C++ API: `imgui_wasm::Server` with the thin C lifecycle ABI
  (`imgui_wasm_init`/`shutdown`/`new_frame`/`render`). Supported `ImGui::`
  calls are captured transparently through the public header.
- Hand-rolled HTTP/WebSocket server (no dependencies): capability handshake
  (`0x0a` hello / `0x1a` ack), per-client outbound mailboxes with
  newest-frame coalescing and corrective resync, string interning, texture
  streaming, clipboard sync, and deterministic identical-frame suppression.
- Browser frontend: WebGL renderer, capability negotiation, batched
  microtask input flushing, and the WASM replay twin built with Emscripten.
- Window geometry: the authoritative INI settings stream with each frame as
  an interned `LoadIniSettingsFromMemory` record; late-joining clients
  restore layout and drags never snap back.
- Performance: WebSocket frames written via stack header + `writev` (zero
  payload copies), broadcast frames shared across clients by refcount, and
  in-place client-message parsing over the receive buffer.
- CMake build with a pinned Dear ImGui fetch, an example app, protocol unit
  tests, and reproducible generated bindings from pinned `dear_bindings`
  metadata.

## [0.1.1] - 2026-08-17

- CMake install and export support: `find_package(imgui_wasm CONFIG REQUIRED)`
  with the `imgui_wasm::core` target, installing the public headers plus the
  pinned Dear ImGui headers and a versioned package config under the
  GNUInstallDirs layout (project version is now 0.1.1).
- vcpkg distribution: a `ports/imgui-wasm` port (static linkage, `!windows`)
  and a `versions/` git-registry database, so this repository can be used as
  a vcpkg registry or submitted to microsoft/vcpkg. Dear ImGui stays vendored
  at the pinned revision because the call-stream protocol is revision-coupled.
- `IMGUI_DIR` is now a cache variable so package managers can pre-seed
  `third_party/imgui` with the pinned revision; the asset embed step finds
  Python via `find_program` (`python3`, falling back to `python`) instead of
  hardcoding `python3`.
- CMake naming aligned with the project: the project, library target
  (`imgui_wasm_core_cpp`), exported namespace (`imgui_wasm::core`), and build
  options (`IMGUI_WASM_BUILD_EXAMPLES`/`IMGUI_WASM_BUILD_TESTS`) use the
  `imgui_wasm` spelling throughout; the previous `imgui_ws` spellings are gone.

[0.1.1]: https://github.com/n3in2019/imgui_wasm/releases/tag/v0.1.1
[0.1.0]: https://github.com/n3in2019/imgui_wasm/releases/tag/v0.1.0
