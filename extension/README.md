# extension/

The GNOME Shell extension (GJS). A separate process from the renderer; it
consumes the producer's PipeWire stream and paints it as a Clutter actor in the
background layer, beneath the desktop icons. It links no renderer code — IPC
over PipeWire only. **Licence: MIT** (ADR-0002).

> **Status: working — a scene animates on the live GNOME desktop (Shell 50.1,
> 2026-06-14).** Architecture and the rendering path below are the verified
> ones. Known limit: laggy at native 2560×1440 (the SHM-copy upload cost; Stage
> C / dma-buf is the fix). Full story: `docs/session-04-live-wallpaper.md`,
> `docs/40_bridge/40.02_consumer.md`.

## What it does

- Overrides `Background.BackgroundManager._createBackgroundActor` and adds a
  `LiveWallpaper` actor to each monitor's background actor (the only sanctioned
  background-layer path on GNOME Wayland). No window cloning, so none of the
  overview/Alt-Tab/app-hiding overrides Hanabi and the kv9898 fork need.
- A single shared `FrameSource` owns the **only** GStreamer pipeline
  (`pipewiresrc ! videoconvert ! RGBA ! appsink`), poll timer, and Cogl texture.
  Each `LiveWallpaper` is a thin, pipeline-less subscriber that paints the shared
  texture in `vfunc_paint_node`. (`_createBackgroundActor` fires on every
  overview toggle; a pipeline-per-actor leaked and crashed the shell — see
  `docs/40_bridge/40.04`.)
- Polls the appsink from a GLib timer on the **shell main thread**
  (`try_pull_sample`), so Clutter is never touched from GStreamer's streaming
  thread.
- Reuses one texture (`set_data` per frame, no per-frame allocation), throttled
  to ~15 fps, and **pauses uploads while the overview is open**.

## Implementation notes (GNOME 50.1)

Important technical details for this environment (details in `40.02`):

- **`import GstApp`** or `appsink.try_pull_sample` is unbound and throws.
- Upload as **`Cogl.PixelFormat.RGBX_8888`, not RGBA** — the producer is BGRx and
  `videoconvert` leaves alpha at 0, so an RGBA texture paints fully transparent.
- Override **`vfunc_get_paint_volume`** to claim the allocation, or Clutter culls
  the actor (no CSS background / no content ⇒ empty paint volume) and stops
  painting.
- Give `Clutter.TextureNode` an **explicit opaque white `Cogl.Color`** (null
  paints transparent).
- A bare `pipewiresrc` is **not** auto-linked; the source finds the producer by
  `node.name=wpe-host` via `Gst.DeviceMonitor`.
- Tear pipelines down to NULL and **wait** before releasing — disposing a
  PLAYING pipewiresrc SIGSEGV'd the shell.

## Safety

- **Kill-switch:** `touch ~/.config/wwb/disabled` makes `enable()` a no-op (use
  it from a TTY to break a bad state). `WWB_FORCE=1` bypasses it for testing.
- `enable()` is fully guarded and all GStreamer work is deferred off the startup
  path and fail-open (no producer ⇒ idle, never block/throw) — a blocking
  enable once caused a grey-screen login loop (`40.04`).

## Running it

**Test in a nested session first; for the live desktop follow the runbook in
`docs/40_bridge/40.03_live_session_runbook.md`** (start order, recovery, the
kill-switch). The assistant never launches gnome-shell — the operator does.

1. Build and start the producer (`docs/40_bridge/40.01_producer.md`), at 1080p+
   (low-res scenes wash out — Q-12):

   ```sh
   bridge/build/host/wpe-host --pipewire --assets-dir steam-assets \
     --size 2560x1440 --fps 30 steam-workshop/2804205787
   ```

   The extension finds it by `node.name`; `WWB_PIPEWIRE_PATH=<id>` is a debug
   override.

2. Install the extension (symlink):

   ```sh
   ln -sfn "$PWD/extension" \
     ~/.local/share/gnome-shell/extensions/we-wayland-bridge@we-wayland-bridge.github.io
   ```

3. Nested shell (GNOME 49+ replaced `--nested` with the Mutter Development Kit;
   run from a real terminal, not the snap-confined VS Code one). It is headless
   (no on-screen window), so verify by the journal:

   ```sh
   dbus-run-session -- gnome-shell --devkit --wayland --virtual-monitor 1920x1080
   journalctl --user -f -o cat | grep -i wwb
   ```

   Expected: `wwb: enabled` → `pipeline … set_state(PLAYING)` →
   `first frame uploaded (WxH), setData=true`. `WWB_STRESS=1` toggles the
   overview every 2 s (nested stress testing only; never set in production).

## Follow-ups

Stage C (dma-buf zero-copy, for smooth full-res — the priority); fit/crop modes;
per-scene properties (`--set-property`); pointer forwarding (mouse-interactive
scenes); audio-reactivity; multi-monitor; fullscreen-pause. See `40.02` §5.
