# ImGuiWasm

[![CI](https://github.com/n3in2019/imgui_wasm/actions/workflows/ci.yml/badge.svg)](https://github.com/n3in2019/imgui_wasm/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

ImGuiWasm is an experimental web backend for [Dear ImGui](https://github.com/ocornut/imgui).
Applications run their ImGui context in native C++ while the core streams
captured API calls over WebSocket to an embedded WebGL frontend that replays
them in a WASM-compiled Dear ImGui twin.

> [!IMPORTANT]
> ImGuiWasm is early-stage software. Authentication is opt-in (PAM-backed
> HTTP Basic, see [Auth](#auth)); without it the server is open, so it should
> not be exposed directly to an untrusted network.

## Features

- Native Dear ImGui rendering streamed to a browser
- Embedded HTTP, WebSocket, and WebGL frontend
- Semantic call-stream transport with the ordinary Dear ImGui `ImGui::` API
- Dear ImGui docking, synced to every client (dockspaces, tab merges, and
  layouts reach late joiners through the authoritative INI snapshot)
- C lifecycle API with a thin C++ wrapper
- Multi-client input and clipboard support, each client rendering at its own
  native resolution

## Architecture

| Component | Responsibility |
| --- | --- |
| `imgui_wasm` | Pure C++17 core: HTTP/WebSocket server, call-stream capture and framing, C ABI, Dear ImGui backend, and embedded frontend |

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

## Install via vcpkg

ImGuiWasm ships a vcpkg port (`ports/imgui-wasm`), and this repository also
works as a vcpkg git registry (`versions/`). The port name is `imgui-wasm`
(vcpkg does not allow underscores); the CMake package is `imgui_wasm` with the
`imgui_wasm::core` target.

Use the registry from another project by placing a `vcpkg-configuration.json`
next to your `vcpkg.json` (pin `baseline` to a commit of this repository):

```json
{
  "default-registry": {
    "kind": "builtin",
    "baseline": "<any recent microsoft/vcpkg commit>"
  },
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/n3in2019/imgui_wasm",
      "baseline": "<commit hash>",
      "packages": ["imgui-wasm"]
    }
  ]
}
```

Then depend on it and build with the vcpkg toolchain:

```json
{ "name": "my-app", "version": "0.0.0", "dependencies": ["imgui-wasm"] }
```

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
```

and consume it as:

```cmake
find_package(imgui_wasm CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE imgui_wasm::core)
```

For a quick trial without a manifest, install the port directly:

```bash
vcpkg install imgui-wasm --overlay-ports=/path/to/imgui_wasm/ports
```

Before cutting a release tag you can validate the port against a working
tree: set `IMGUI_WASM_LOCAL_SOURCE_DIR` to the repository root and the port
builds from that tree instead of fetching the tag from GitHub. This
validation branch is stripped when submitting the port to microsoft/vcpkg.

Notes and limitations:

- The core uses POSIX sockets, so the port declares `!windows` until a
  winsock backend exists; only static linkage is provided (the library
  embeds its own Dear ImGui copy).
- Dear ImGui `v1.92.8` (docking, pinned commit) is vendored inside the
  library: the call-stream protocol, generated bindings, and the browser
  WASM twin are revision-coupled and cannot track the standalone `imgui`
  port.
- After cutting a release tag, update the source SHA512 in
  `ports/imgui-wasm/portfile.cmake` (vcpkg prints the expected value on
  the first failed run; the vendored Dear ImGui fetch needs no hash on
  current vcpkg), refresh the port's `git-tree` in
  `versions/i-/imgui-wasm.json` (`git rev-parse HEAD:ports/imgui-wasm`),
  and bump `versions/baseline.json`.

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

### Access control

Auth uses Linux accounts via PAM, configured through `imgui_wasm::Config`
(or `imgui_wasm_config_t`):

```cpp
config.pam_service = "imgui_wasm";  // empty = auth disabled (default)
config.max_clients = 8;             // connection caps; 0 = unlimited
config.max_clients_per_ip = 2;
```

With a `pam_service` set, static pages and WebSocket upgrades require HTTP
Basic credentials verified against that PAM service: the browser shows its
native login dialog on the page request and forwards the credentials to
the `/ws` upgrade automatically, so passwords never appear in URLs.
Rejections carry the `WWW-Authenticate` challenge before the upgrade
completes, over-cap connections get `503`, and verified usernames are
logged per connection.

Deploy a service file first, e.g. `/etc/pam.d/imgui_wasm`:

```
auth     required  pam_unix.so
account  required  pam_unix.so
```

Privilege reality: an unprivileged server can verify passwords against
SSSD/LDAP-backed domains and **its own user's** account (the
`unix_chkpwd` rule). Verifying *other* local `/etc/shadow` accounts
requires root, the shadow group, or a small setuid helper — the same trade
`sshd` makes. If none of those fit your deployment, keep the server on a
trusted network (LAN, VPN) or behind an authenticating reverse proxy.

libpam is loaded at runtime (`dlopen`), so builds stay dependency-free;
PAM auth itself is Linux-only.

Please do not report vulnerabilities in public issues. Follow the private
reporting guidance in [SECURITY.md](SECURITY.md).

## License

ImGuiWasm is available under the [MIT License](LICENSE). Generated and upstream-
derived files retain the notices documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
