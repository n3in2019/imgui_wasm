# ImGuiWasm Roadmap

_Last revised: 2026-08-17. The four decisions below were locked on this date._

## Vision

Your Dear ImGui app, in a browser tab, over a wire — a networked console for
native tools (dev dashboards, robotics/IoT consoles, engine editors, headless
servers) without embedding a web stack. The differentiator to protect: fidelity
at near-zero wire cost. The WASM twin must behave exactly like native ImGui.

## Product identity

ImGuiWasm is a **multi-viewer console**: many simultaneous browser clients
interact correctly with one application. v0.2 delivers per-client sessions —
own context, input, layout, and call-stream. Secondary clients are not view-only.

## Decision log

| # | Decision | Choice | Consequence |
|---|----------|--------|-------------|
| 1 | Product identity | Multi-viewer now | v0.2 theme is session-ification of the core; breaking API change allowed (0.x) |
| 2 | Security | Auth in core, narrow scope | Timing-safe tokens checked at the WebSocket upgrade, per-IP connection caps, token-bucket rate limiting. No IdP, no cookies, no TLS in-process; reverse proxy remains the documented TLS path |
| 3 | Platforms | Neither in v0.2 | Docker/headless Linux and the winsock port move to v0.3 |
| 4 | Docking | Time-boxed spike (2–3 days) | Full sync, initial-layout-only, or documented wontfix are all acceptable outcomes |

## v0.2.0 — The networked console

- **Session-ified core**: one session per WebSocket client = auth identity + Dear
  ImGui context + input + call-stream. Removes the active-client-geometry caveat
  (every client's input lands where it aims; each session's layout is computed at
  its own display size).
- **In-core auth**, scope as in decision 2. Browser WebSocket cannot set custom
  headers, so the token rides the query string or a subprotocol prefix and will
  appear in logs — documented honestly, not solved with machinery.
- **Docking spike**, run after session-ification: per-context ini state improves
  its odds. 2–3 days, time-boxed; record whichever verdict is true.
- **Upstream-bump automation**: one command bumps the ImGui pin, regenerates
  bindings, rebuilds the twin, runs tests, refreshes the port. Promoted from
  v0.3 — without it the revision-coupled project rots in place.
- **Drift smoke test**: run the server and the twin over the same golden call
  stream at pinned scale 1 and compare vertex counts / clip rects per frame.
  A smoke test, not a correctness proof. Spike-first: requires the twin runnable
  headless under node in CI.

Explicitly cut from v0.2: platform work (→ v0.3), a protocol-version field (the
twin is embedded in the server binary and served by it; skew is structurally
impossible), Rust bindings (→ v1.0-era, only on demand).

Known ceiling: multi-viewer means N ImGui contexts per frame. Fine at panel
scale (5–20 clients); documented rather than engineered around.

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
2. Session API design: per-session frame iteration vs. callback, capture/state
   rework plan, auth config surface.
