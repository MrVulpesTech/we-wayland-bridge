# ADR-0003: Output backend strategy

- **Status:** Proposed
- **Date:** 2026-06-11
- **Deciders:** project author
- **Note:** Provisional. Confirmed or revised after Session 2 (output
  backend survey) and Session 4 (transport prototype).

## Context

`linux-wallpaperengine` renders into an output target chosen at startup:
a window, or a `wlr-layer-shell` surface, or X11. The source tree groups
these under `src/WallpaperEngine/Render/Drivers/Output` (an output-driver
abstraction already exists — confirmed by directory layout; details are
Session 2's job to map precisely).

To reach GNOME, the renderer must produce frames that leave the process
and arrive in `gnome-shell` (principle P3). None of the existing output
targets do that. The renderer needs a new output target that renders
off-screen and exposes the result for IPC.

## Decision (proposed)

Add a **new output backend** to the renderer that:

1. Renders the scene into an **offscreen framebuffer** (FBO / texture),
   not a window or layer-shell surface.
2. Exports that GPU buffer as a **dma-buf** and publishes it as a
   **PipeWire** video stream (transport details in ADR-0004).

It is a **sibling** to the existing window/layer-shell/X11 backends,
selected by a CLI flag (working name `--output pipewire` or similar),
leaving every existing backend untouched (principle P2). It is developed
on the `pipewire-backend` fork branch (ADR-0001) with an upstream PR as
the goal.

The MVP target is a single output at the scene's native resolution.

## Alternatives considered

- **Screen-scrape the existing window backend** (read pixels back from a
  hidden window with `glReadPixels`). Rejected: forces a GPU→CPU readback
  every frame — the exact copy cost principle P4 forbids — and depends on
  a real window existing.
- **Reuse the `wlr-layer-shell` backend and make GNOME accept it.**
  Rejected: Mutter does not implement `wlr-layer-shell` and will not
  (this is the premise of the whole project).
- **A second renderer process that mirrors output via a compositor
  protocol.** Rejected: more moving parts than rendering offscreen and
  publishing one PipeWire stream.

## Consequences

- **Positive:** One clean new backend, no change to existing ones, useful
  to any PipeWire consumer (not only GNOME), and a plausible upstream PR.
- **Negative:** Touches the renderer's GL setup (offscreen context, FBO
  management, buffer export) — the most involved renderer change in the
  project.
- **Risks:** The renderer's GL context assumes an on-screen surface in
  ways that complicate fully headless rendering; dma-buf export from the
  renderer's context may not be straightforward on every driver. Both are
  probed in Session 2 and proven in Session 4.

## Revisit triggers

- Session 2 finds the output-driver abstraction does not cleanly admit a
  headless sibling — revise the integration approach.
- Session 4 shows zero-copy export is not achievable on the target GPU —
  escalates to ADR-0004's fallback discussion.
