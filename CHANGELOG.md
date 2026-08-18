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

## [0.2.0] - 2026-08-18 — The networked console

- Breaking: removed the `compression` field from `imgui_wasm_config_t` and
  `imgui_wasm::Config`. It had been a compatibility no-op since the call-stream
  transport replaced draw-data streaming — frames are tens of bytes, so there is
  nothing worth compressing. Capability bit 2 is now documented as reserved.
- Breaking: removed the `dark_style` field from both config structs and the
  backend init parameter. It could never change the theme: Dear ImGui has
  defaulted to dark since 1.53, so both settings left the context dark.
  Applications pick themes the idiomatic way (`ImGui::StyleColorsLight()` etc.
  after init).
- Breaking: split the `host_port` string into separate `host` (`const char*`,
  IPv4 dotted quad, NULL = 127.0.0.1) and `port` (`uint16_t`, 0 = 8888) fields
  in both config structs. The `host:port` string is no longer parsed at
  runtime; the example still accepts a `[host:]port` CLI argument.
- Breaking: removed the `config_flags` field from both config structs — the
  config is now transport-only (`host`, `port`). Applications set
  `ImGui::GetIO().ConfigFlags` after init, as with any Dear ImGui backend.
  This also drops the wrapper default that enabled `DockingEnable`, which
  call-stream mode does not support. The replay twin still never sees
  ConfigFlags; streaming them to it is planned for v0.2.
- Removed the dead `callstream` mode flag from the internal backend init: the
  legacy draw-data transport is gone, so the parameter (and its `GCallstream`
  guard) could never be false.
- Auth: Linux account login via PAM plus connection caps. Setting
  `pam_service` in either config struct makes static pages and WebSocket
  upgrades require HTTP Basic credentials verified against the named PAM
  service — the browser's native login dialog covers the whole protection
  space, and credentials ride the upgrade, never URLs.
  `max_clients`/`max_clients_per_ip` cap connections (over-cap gets 503).
  libpam is loaded at runtime via dlopen (the build gains no dependency);
  unprivileged verification covers SSSD/LDAP domains and the server's own
  user, other local accounts need elevated privileges (documented in the
  README). Verified usernames are logged per connection. Linux only.
- Cross-size input correctness: positional input (mouse move/buttons/wheel)
  from a client is deferred for one frame when the authoritative layout was
  computed at a different canvas size. The server re-layouts to the
  interacting client each frame, so every positional event is now applied
  against geometry matching its sender's window — previously such events
  could misclick. Keyboard and text input never wait on geometry.
- Upstream-bump automation: `python3 tools/bump_upstream.py <imgui-tag-or-sha>`
  resolves and fetches the revision, re-pins it in CMakeLists.txt, the
  bindings generator, and the vcpkg port, regenerates all bindings and
  call-stream artifacts, rebuilds the WASM replay twin, and rebuilds and
  tests the core. Verified end-to-end by re-running against the current
  pin: every regenerated artifact, including the Emscripten twin, came out
  byte-identical to the committed files.
- Docking (full sync): Dear ImGui docking now works over the call stream.
  `DockSpace`, `DockSpaceOverViewport`, and `SetNextWindowDockID` are captured
  and replayed (opcodes 205–207; their pointer args are null-only — non-null
  `window_class`/`viewport` calls run server-side uncaptured). The server's
  effective `ImGuiIO::ConfigFlags` rides every `0x07` frame header as a new
  trailing `u32`, and the replay twin mirrors the docking bit so `DockSpace*`
  semantics match the server exactly; the header change also feeds the frame
  hash, so a flag toggle always breaks identical-frame suppression. Live dock
  drags work through the twin's input mirroring, and committed layouts reach
  every client (including late joiners) via the authoritative INI snapshot's
  `[Docking][Data]`. The bindings generator also learned the `ImGuiCond`
  argument type (it had silently excluded `SetNextWindowDockID`). Not
  streamed: `DockBuilder*` internals and `io.Config*` behavior knobs beyond
  ConfigFlags. Verified end-to-end in headless Chrome (programmatic initial
  docking, interactive drag-to-dock tab merge, late-joiner layout restore);
  the example app now enables docking and submits a dockspace.

[0.2.0]: https://github.com/n3in2019/imgui_wasm/releases/tag/v0.2.0
[0.1.1]: https://github.com/n3in2019/imgui_wasm/releases/tag/v0.1.1
[0.1.0]: https://github.com/n3in2019/imgui_wasm/releases/tag/v0.1.0
