# we-wayland-bridge

> Wallpaper Engine live wallpapers on GNOME Shell under Wayland.

<video src="https://github.com/user-attachments/assets/14587281-19fa-4ea6-b008-027e57dcc836" autoplay loop muted playsinline>
</video>

**Status:** working prototype. A scene-type Wallpaper Engine wallpaper
**renders and animates on the GNOME Wayland desktop background** on the reference
machine (Ubuntu 26.04, GNOME Shell 50.1, Intel iGPU). The producer supports zero-copy via dma-buf (proven in Stage C); 
the consumer currently uses an SHM copy. End-to-end zero-copy is 
planned pending a dma-buf import path in the GNOME Shell extension API.

The SHM path is tuned for
daily use (reduced producer resolution + 20 fps; [40.03](docs/40_bridge/40.03_live_session_runbook.md)).
See also [the session-4 log](docs/session-04-live-wallpaper.md).

Wallpaper Engine is a Windows application; its live wallpapers are
bought through the Steam Workshop. On Linux,
[`linux-wallpaperengine`](https://github.com/Almamu/linux-wallpaperengine)
already reimplements the runtime and renders those wallpapers with OpenGL.
It works on Wayland — but only on compositors that implement the
`wlr-layer-shell` protocol (Hyprland, Sway, other wlroots compositors).

**GNOME's compositor (Mutter) does not implement `wlr-layer-shell`.** So
on Ubuntu with GNOME on Wayland — the default desktop on the most common
Linux distribution — no external program can draw behind the desktop
icons. The only sanctioned background-layer path on GNOME Wayland is a
**GNOME Shell extension**, which runs inside `gnome-shell` itself.

This project is the bridge between the working renderer and GNOME Shell:
it gets frames out of `linux-wallpaperengine` and into a Clutter actor in
the GNOME background layer, ultimately without an expensive per-frame copy (currently uses an SHM fallback).

## Architecture

```text
Wallpaper Engine files (Steam) → linux-wallpaperengine renderer
  → [new headless output backend] → PipeWire / dma-buf (zero-copy)
  → GNOME Shell extension (Clutter actor in the background layer)
  → live wallpaper on GNOME Wayland
```

The renderer and `gnome-shell` are separate processes. Frames cross
between them via PipeWire, which also serves as the licensing boundary: the
renderer-side code is GPLv3, the extension is MIT.

See [`docs/MEMORY.md`](docs/MEMORY.md) for the full design, starting with
[the project vision](docs/10_vision/10.01_project_vision.md) and the
[component map](docs/20_architecture/20.03_component_map.md).

## Repository layout

| Path | What it is |
|---|---|
| `upstream/` | git submodule: `linux-wallpaperengine` (read-only) |
| `bridge/` | renderer-side output backend (headless → PipeWire); GPLv3 |
| `extension/` | GNOME Shell extension (GJS); MIT |
| `docs/` | design, architecture decisions, session logs |

`docs/` follows [Johnny.Decimal](https://johnnydecimal.com) numbering.
The renderer is pulled in as a submodule, not forked — see
[ADR-0001](docs/_adr/ADR-0001-repository-strategy.md).

## Getting started

```sh
git clone https://github.com/MrVulpesTech/we-wayland-bridge
cd we-wayland-bridge
git submodule update --init
```

Build the producer: `./bridge/build.sh`

The two halves run as separate processes:

1. **Producer** — build and start the renderer host (`bridge/`), which streams
   frames over PipeWire. Build/run in
   [`docs/40_bridge/40.01_producer.md`](docs/40_bridge/40.01_producer.md).
2. **Consumer** — the GNOME extension (`extension/`) paints that stream into the
   background layer. To watch it on the real desktop, follow the operator
   runbook in
   [`docs/40_bridge/40.03_live_session_runbook.md`](docs/40_bridge/40.03_live_session_runbook.md)
   (start order, recovery, kill-switch).

The renderer reads Wallpaper Engine content you already own in Steam — this
project ships no wallpaper assets.

## Prerequisites

- Ubuntu 26.04 or later (GNOME Shell 50, Wayland session)
- Wallpaper Engine installed via Steam (App ID 431960, scene-type wallpapers subscribed)
- PipeWire (default on Ubuntu 26.04)
- GStreamer 1.x with PipeWire plugin (gstreamer1.0-pipewire)
- Build tools: cmake, pkg-config, libgbm-dev, libegl-dev, libpipewire-0.3-dev (full list in docs/40_bridge/40.01_producer.md)

## Known limitations

- Scene-type wallpapers only (video and web types not supported)
- Single monitor
- No mouse interactivity (parallax/hover effects)
- No audio reactivity  
- No autostart (manual launch required for now)
- ~30fps at 1080p on Intel iGPU (hardware renderer limit on Linux)
- Consumer uses SHM copy; end-to-end zero-copy is planned

## Scope

The first milestone is deliberately narrow: scene-type wallpapers, a
single monitor, no web wallpapers, no audio-reactivity. See
[non-goals](docs/10_vision/10.03_non_goals.md).

## Licensing

Renderer-derived code (`bridge/`) is GPLv3, matching upstream. The GNOME
extension (`extension/`) is a separate process and is MIT. See
[ADR-0002](docs/_adr/ADR-0002-licensing.md) and the root [`LICENSE`](LICENSE),
which points to [`bridge/LICENSE`](bridge/LICENSE) (GPL-3.0) and
[`extension/LICENSE`](extension/LICENSE) (MIT).

## Legal

This project renders Wallpaper Engine content that the user has legally
purchased and installed through Steam — the same footing on which
`linux-wallpaperengine` has operated for years. It redistributes no
Wallpaper Engine code or assets. You must own Wallpaper Engine on Steam
and have subscribed to the live wallpapers you use.

## Acknowledgements

The initial prototype and documentation for this project were developed with the assistance of an AI coding agent (Claude).
