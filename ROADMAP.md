# ImGuiWasm Roadmap

_Last revised: 2026-08-19. Decisions 1–4 were locked 2026-08-17; later
decisions carry their own dates._

## Vision

Your Dear ImGui app, in a browser tab, over a wire — a networked console for
native tools (dev dashboards, robotics/IoT consoles, engine editors, headless
servers) without embedding a web stack. The differentiator to protect: fidelity
at near-zero wire cost. The WASM twin must behave exactly like native ImGui.

## Product identity

ImGuiWasm is a **multi-viewer console**: many simultaneous browser clients
interact with one application. The server holds the single authoritative UI
state; every client renders it at its own native resolution and every
client's input drives the shared state. Secondary clients are not view-only.

## Decision log

| # | Decision | Choice | Consequence |
|---|----------|--------|-------------|
| 1 | Product identity | Multi-viewer now | v0.2 theme is session-ification of the core; breaking API change allowed (0.x) |
| 2 | Security | Auth in core, narrow scope | Timing-safe tokens checked at the WebSocket upgrade, per-IP connection caps, token-bucket rate limiting. No IdP, no cookies, no TLS in-process; reverse proxy remains the documented TLS path |
| 3 | Platforms | Neither in v0.2 | Docker/headless packaging and the winsock port were deferred, then removed from the roadmap entirely (2026-08-19); Linux-only until demand says otherwise |
| 4 | Docking | Full sync (2026-08-18) | `DockSpace`/`DockSpaceOverViewport`/`SetNextWindowDockID` captured + replayed; effective ConfigFlags mirrored via the 0x07 header; INI snapshot carries committed layouts to late joiners. Not streamed: `DockBuilder*` internals, `io.Config*` knobs beyond ConfigFlags |
| 5 | UI-state authority (2026-08-18) | Server-authoritative single context; display size stays a per-client twin concern | Multi-context sessions and the frame_fn API break are cancelled; the remaining stage is cross-size input mapping. Interaction is shared (one mouse/scroll, like tmux) by design |
| 6 | Cross-size input mapping (2026-08-18) | Resolved by the tmux-style deferral; explicit coordinate mapping rejected | Positional input is held one frame while the server re-layouts to the sender's canvas size, so every event lands against geometry matching its sender. Rescaling coordinates across differing sizes would target a layout that is not the sender's — contrary to decision 5's shared-console model |

## v0.2.0 — The networked console

- **Server-authoritative single context**: one ImGui context on the server is
  the single truth for UI state (window arrangement, scroll, widget values).
  `imgui_wasm_new_frame`/`render` stay as they are — the planned per-session
  contexts and `frame_fn` API break are cancelled. Per-client input queues,
  frame suppression, and auth identity (stage 2 + PAM) remain underneath.
- **Per-client twin resolution (done)**: each browser twin renders at its own
  canvas size and DPR; display size is a client concern. Cross-size input is
  handled by the tmux-style deferral: the server re-layouts to the interacting
  client and holds its positional input for one frame, so every event lands
  against geometry matching its sender (decision 6).
- **Auth (done)**: PAM-backed HTTP Basic auth + connection caps, scope as in
  decision 2.
- **Docking (done — full sync)**: `DockSpace`/`DockSpaceOverViewport`/
  `SetNextWindowDockID` are captured and replayed (opcodes 205–207; pointer
  args null-only), the effective `io.ConfigFlags` rides the 0x07 header and
  the twin mirrors the docking bit, live drags work via input mirroring, and
  committed layouts + late joiners resync via the INI snapshot. Verified
  end-to-end in headless Chrome: programmatic initial docking, interactive
  drag-to-dock tab merge, and late-joiner layout restore. Not streamed:
  `DockBuilder*` internals and `io.Config*` knobs beyond ConfigFlags.
- **Upstream-bump automation (done)**: one command bumps the ImGui pin,
  regenerates bindings, rebuilds the twin, runs tests, refreshes the port.
  Promoted from a later milestone into v0.2 — without it the revision-coupled
  project rots in place.

Explicitly cut from v0.2: platform work (Docker/headless packaging and the
winsock port — removed from the roadmap entirely, 2026-08-19), a
protocol-version field (the twin is embedded in the server binary and served by
it; skew is structurally impossible), Rust bindings (→ v1.0-era, only on
demand), per-viewer ImGui contexts (cancelled 2026-08-18: UI state is
server-authoritative).

Interaction is shared by design: simultaneous viewers share one mouse and
scroll — a shared console, like tmux — the direct consequence of
server-authoritative UI state.

## v1.0.0 — Launch

API freeze, docs/examples gallery, public announcement (the Dear ImGui
"Show and tell" thread is the natural venue). Rust binding only if demand
materialized by then.

## Falsifier

90 days after the repo goes public: if there is no external signal — at least
one deployment story from a stranger, or sustained issues/PRs from non-authors —
then the roadmap shrinks to maintenance, and the multi-viewer bet is audited (do
connected sessions ever exceed one client?). A roadmap that can't fail is a
wish list.

## Standing near-term items

1. Make the repo public, then fill the port's source SHA512 in
   `ports/imgui-wasm/portfile.cmake` and refresh the `versions/` git-tree,
   push, and publish the v0.2.0 GitHub release (the tag exists locally).
2. v0.2.0 is assembled (2026-08-18): version bumped, changelog cut, port and
   registry database updated. Every v0.2 engineering item landed (context,
   twin resolution, cross-size deferral, auth, docking, upstream-bump
   automation).
