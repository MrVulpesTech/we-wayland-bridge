# Session 4 (live) — the wallpaper on the real desktop

Date: 2026-06-14. Machine: Ubuntu 26.04, GNOME Shell 50.1, Wayland, Intel
CometLake-U UHD. Producer: `bridge/host/wpe-host` (next-v2 core @ `828485a`).
Scene: astronaut `2804205787`.

This session closed the consumer loop: a Wallpaper Engine scene **animating as
the live GNOME background**, after resolving the freeze incident and then a
chain of rendering bugs that only a visible display could expose.

## Part 1 — the incident, fully resolved

Two earlier live tests had frozen the machine. The four distinct failure modes
(full diagnosis in `40_bridge/40.04`) are now fixed and **verified in a 2-minute
nested stress test** (overview toggled every 2 s while streaming, then a clean
disable):

1. **Grey-screen login** — GStreamer init/discovery ran synchronously on the
   shell main thread at login with no producer. Fixed: deferred, fail-open init.
2. **GPU hard-lock** — per-frame texture *allocation* under the overview's
   background blur. Fixed: one reused Cogl texture (`set_data`), 15 fps cap,
   uploads paused while the overview is open.
3. **libgstpipewire SIGSEGV on teardown** — disposing a PLAYING pipewiresrc.
   Fixed: clean teardown (drop bus handler → NULL → wait for NULL).
4. **Per-actor pipeline leak** — `_createBackgroundActor` fires on every
   overview toggle; each spun up a new pipeline and leaked the disposed actor's
   timer (`already disposed` spam, runaway VmRSS → crash). Fixed:
   **rearchitected to one shared `FrameSource`** (single pipeline + texture)
   feeding thin, paint-only `LiveWallpaper` subscribers; cleanup via the
   `destroy` *signal*, not a `destroy()` override.

Stress result: one pipeline across ~35 actor rebuilds, zero `already disposed`,
VmRSS bounded ~508–613 MB (no leak), clean `wwb: disabled` with no GStreamer
criticals, shell alive.

## Part 2 — getting pixels on screen (live only)

Nested runs are headless (the `mutter-devkit` viewer is unpackaged), so
rendering bugs were invisible until the live test. Three, found in order, each
from one diagnostic log line:

1. **The actor was culled.** With no CSS background and no Clutter content, the
   `St.Widget`'s paint volume is empty, so Clutter stops calling
   `vfunc_paint_node` after the first paint. The texture was uploaded
   (`setData=true`) but never drawn. Fix: override `vfunc_get_paint_volume` to
   claim the full allocation.
2. **The `TextureNode` colour was null.** A null modulation colour can paint the
   texture as transparent. Fix: hand it an explicit opaque white `Cogl.Color`.
3. **Alpha was 0 → the texture was fully transparent.** The decisive log:
   `firstpx RGBA=239,243,246,0`. The producer emits BGRx; `videoconvert` to RGBA
   left alpha at 0, so the texture painted see-through (the symptom across every
   prior attempt: you saw whatever was behind it). Fix: upload in an
   **ignore-alpha format** (`Cogl.PixelFormat.RGBX_8888`) so the texture is
   always opaque; the RGB bytes sit in the same positions.

With all three: the scene is **visible and animating** on the live desktop.
Clean disable verified live too (VmRSS dropped 751→266 MB, `wwb: disabled`).

## Part 3 — the cost, and the limit

It works but the desktop is **laggy at native 2560×1440**. The cause is the
SHM-copy path: every frame the shell main thread copies and uploads a
~14 MB texture. VmRSS sits bounded but elevated (~620–751 MB at 1440p). This is
exactly the case **Stage C (dma-buf zero-copy)** removes — the GPU would sample
the producer's buffer with no per-frame copy. The picture is correct; smoothness
at full resolution needs Stage C.

Lowering the producer to 1080p reduces the upload but changes the scene framing
(`linux-wallpaperengine` frames the scene to its render size), so it is not a
clean fit — a per-resolution fit/crop mode is a separate follow-up.

## Where this leaves the project

- **Done:** producer → PipeWire → GNOME background, animating on the real
  desktop; the freeze incident closed; the rendering path understood and
  recorded (`40_bridge/40.02_consumer.md`).
- **Next, in order:** Stage C (dma-buf) for smooth full-res — the priority;
  fit/crop modes; fullscreen-pause; multi-monitor; then the Q-10 / #302
  upstream-vs-standalone decision.
