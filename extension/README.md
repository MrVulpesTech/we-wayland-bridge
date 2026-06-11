# extension/

The GNOME Shell extension (GJS). A separate process from the renderer; it
consumes the producer's PipeWire stream and paints it as a Clutter actor
in the background layer, beneath the desktop icons. It links no renderer
code — IPC over PipeWire only. **Licence: MIT** (ADR-0002).

> **Status: first cut, UNVERIFIED.** Written from a study of
> gnome-ext-hanabi and @kv9898's fork. It has **not** been run in a shell
> yet — the development sandbox cannot launch gnome-shell (see Caveats).
> The first nested run on a real session is the verification step.

## What it does

- Overrides `Background.BackgroundManager._createBackgroundActor` and adds
  a `LiveWallpaper` actor to each monitor's background actor (the only
  sanctioned background-layer path on GNOME Wayland).
- `LiveWallpaper` runs a GStreamer pipeline
  (`pipewiresrc ! videoconvert ! video/x-raw,format=RGBA ! appsink`) and
  paints frames onto the actor via `St.ImageContent`.
- Polls the appsink from a GLib timer on the **shell main thread**
  (`try_pull_sample`), so Clutter is never touched from GStreamer's
  streaming thread (avoids GJS cross-thread crashes).
- Shows a solid colour until the first frame, so the background-layer
  injection is verifiable even if the video upload needs iterating.
- Reconnects (2 s) if the producer stops (bus ERROR/EOS).

Because the producer is a separate process with **no shell window**, this
extension does **not** clone a window and needs none of the
overview/Alt-Tab/app-hiding overrides that Hanabi and the kv9898 fork
require. That is the structural payoff of the PipeWire approach.

## Run it (test in a nested session — never the live desktop first)

1. Build and start the producer (see `docs/40_bridge/40.01_producer.md`),
   at 1080p+ (low-res scenes wash out — Q-12):

   ```sh
   bridge/build/host/wpe-host --pipewire --assets-dir steam-assets \
     --size 1920x1080 --fps 60 steam-workshop/2804205787
   ```

   Note the node id it prints. If more than one PipeWire video source is
   running, set `PIPEWIRE_PATH` in `extension.js` to that id.

2. Install the extension:

   ```sh
   ln -s "$PWD/extension" \
     ~/.local/share/gnome-shell/extensions/we-wayland-bridge@we-wayland-bridge.github.io
   ```

3. Launch a **nested** shell and enable it there (do not enable on your
   live session yet). On GNOME 49+ the nested shell is the Mutter
   Development Kit (`--devkit`, not the removed `--nested`); run from a
   real terminal, not the snap-confined VS Code one:

   ```sh
   dbus-run-session -- bash -c '
     gnome-shell --devkit --wayland 2>&1 | grep --line-buffered -iE "wwb|error" &
     sleep 10
     gnome-extensions enable we-wayland-bridge@we-wayland-bridge.github.io
     wait
   '
   ```

4. Capture the journal and look for these lines:

   ```sh
   journalctl --user -f -o cat | grep -i wwb
   ```

   - `wwb: API probe — St.ImageContent=… Clutter.Image=… Cogl.PixelFormat=…`
     — tells us which frame-upload API is available.
   - `wwb: LiveWallpaper on monitor 0 (1920x1080)` — injection ran.
   - `wwb: first frame uploaded (1920x1080)` — video path works.
   - `wwb: frame upload failed …` — share this; the upload call needs
     adjusting for your shell's exact API.

If you see the solid colour (`#0a0f1e`) in the background but no video, the
injection works and only the upload needs fixing — capture the probe line.

## Caveats / open items

- **Nested-shell command on GNOME 50.1.** The brief's
  `gnome-shell --nested --wayland` no longer works — `--nested` was
  removed in GNOME 49. The replacement is the Mutter Development Kit:
  `dbus-run-session -- gnome-shell --devkit --wayland` (see GNOME 49/50
  developer release notes). Plain `--wayland` tries to become a full
  display server (`EBUSY`) and must not be used. Run from a real terminal,
  not the snap-confined VS Code one.
- **Frame-upload API unverified.** `St.ImageContent.set_bytes` is the
  assumed path; the probe line will confirm or send us to a Cogl-texture
  fallback.
- **Fit modes.** MVP stretches to the monitor (`RESIZE_FILL`). Fit/crop
  per the Session-4 requirements in `40.01` are a follow-up.
- **Per-monitor / reconnect** are implemented but untested.
