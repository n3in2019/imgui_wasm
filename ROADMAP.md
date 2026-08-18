# ImGuiWasm Roadmap

_Last revised: 2026-08-17. The four decisions below were locked on this date._

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
| 3 | Platforms | Neither in v0.2 | Docker/headless Linux and the winsock port move to v0.3 |
| 4 | Docking | Time-boxed spike (2–3 days) | Full sync, initial-layout-only, or documented wontfix are all acceptable outcomes |
| 5 | UI-state authority (2026-08-18) | Server-authoritative single context; display size stays a per-client twin concern | Multi-context sessions and the frame_fn API break are cancelled; the remaining stage is cross-size input mapping. Interaction is shared (one mouse/scroll, like tmux) by design |

## v0.2.0 — The networked console

- **Server-authoritative single context**: one ImGui context on the server is
  the single truth for UI state (window arrangement, scroll, widget values).
  `imgui_wasm_new_frame`/`render` stay as they are — the planned per-session
  contexts and `frame_fn` API break are cancelled. Per-client input queues,
  frame suppression, and auth identity (stage 2 + PAM) remain underneath.
- **Per-client twin resolution**: each browser twin keeps rendering at its
  own canvas size and DPR; display size is a client concern. Cross-size
  input mapping — landing a non-active client's clicks against the server's
  authoritative geometry (`dsw`/`dsh` ride every `0x07` header) — is the
  remaining time-boxed work: linear canvas scale vs. window-relative mapping
  through the twin's local geometry.
- **Auth (done)**: PAM-backed HTTP Basic auth + connection caps, scope as in
  decision 2.
- **Docking spike**: 2–3 days, time-boxed; full sync, initial-layout-only, or
  documented wontfix are all acceptable outcomes.
- **Upstream-bump automation**: one command bumps the ImGui pin, regenerates
  bindings, rebuilds the twin, runs tests, refreshes the port. Promoted from
  v0.3 — without it the revision-coupled project rots in place.
- **Drift smoke test**: run the server and the twin over the same golden call
  stream at pinned scale 1 and compare vertex counts / clip rects per frame.
  A smoke test, not a correctness proof. Spike-first: requires the twin runnable
  headless under node in CI.

Explicitly cut from v0.2: platform work (→ v0.3), a protocol-version field (the
twin is embedded in the server binary and served by it; skew is structurally
impossible), Rust bindings (→ v1.0-era, only on demand), per-viewer ImGui
contexts (cancelled 2026-08-18: UI state is server-authoritative).

Interaction is shared by design: simultaneous viewers share one mouse and
scroll — a shared console, like tmux — the direct consequence of
server-authoritative UI state.

## v0.3.0 — Deployable & broader

Docker/headless Linux first (example Dockerfile, SIGTERM graceful shutdown,
healthcheck endpoint, non-root image), the winsock port of `src/net.cpp` after,
touch input for mobile browsers, plus any v0.2 spike outcomes that earned
promotion.

## v1.0.0 — Launch

API freeze, docs/examples gallery, public announcement (the Dear ImGui
"Show and tell" thread is the natural venue). Rust binding only if demand
materialized by then.

## Falsifier

90 days after the repo goes public: if there is no external signal — at least
one deployment story from a stranger, or sustained issues/PRs from non-authors —
then v0.3+ shrinks to maintenance, and the multi-viewer bet is audited (do
connected sessions ever exceed one client?). A roadmap that can't fail is a
wish list.

## Standing near-term items

1. Make the repo public, then fill the port's source SHA512 in
   `ports/imgui-wasm/portfile.cmake` and refresh the `versions/` git-tree.
2. Cross-size input mapping spike: land a non-active client's clicks against
   the server's authoritative geometry (`dsw`/`dsh` from the `0x07` header) —
   linear canvas scale vs. window-relative mapping via the twin's local
   geometry.
