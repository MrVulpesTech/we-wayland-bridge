# we-wayland-bridge

> Wallpaper Engine live wallpapers on GNOME Shell under Wayland.

<video src="https://github.com/user-attachments/assets/14587281-19fa-4ea6-b008-027e57dcc836" autoplay loop muted playsinline>
</video>

**Status:** working prototype. This is a proof-of-concept for developers, not an end-user tool. A scene-type Wallpaper Engine wallpaper
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

Unlike `gnome-ext-hanabi` or `@kv9898`'s fork, which use a window-clone approach (spawning a standard window and hiding it while cloning its texture into the background), this project uses a headless producer that renders offscreen and streams directly to GNOME via PipeWire. This provides a reusable frame source independent of window management hacks.

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

Building the producer requires checking out a specific pull request from the upstream renderer into a git worktree.

```sh
git clone https://github.com/MrVulpesTech/we-wayland-bridge
cd we-wayland-bridge
git submodule update --init

# Create the required worktree from the pinned next-v2 PR
cd upstream
git fetch origin pull/609/head:next-v2-review
git worktree add ../next-v2-review next-v2-review
cd ..

# Build the core library and the PipeWire host
./bridge/build.sh
```

The system runs as two separate processes. **Do not run this blindly on your primary desktop without reading the docs.**

1. **Producer** — build and start the renderer host (`bridge/`), which streams frames over PipeWire. See [`docs/40_bridge/40.01_producer.md`](docs/40_bridge/40.01_producer.md) for the full procedure.
2. **Consumer** — the GNOME extension (`extension/`) paints that stream into the background layer. To watch it on the real desktop, follow the operator runbook in [`docs/40_bridge/40.03_live_session_runbook.md`](docs/40_bridge/40.03_live_session_runbook.md) (start order, recovery, kill-switch).

The renderer reads Wallpaper Engine content you already own in Steam — this
project ships no wallpaper assets.

## Prerequisites

- Tested on Ubuntu 26.04, GNOME Shell 50.1, Intel iGPU. Other configurations may work but are untested.
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

## Troubleshooting

- **Extension enabled but no wallpaper visible**: The extension fails-open and waits for the producer. Start the producer (`bridge/`) and ensure it's streaming.
- **Grey screen on login**: Use the kill-switch. Switch to a TTY (`Ctrl+Alt+F3`), run `touch ~/.config/wwb/disabled`, and relogin.
- **Wrong GNOME Shell version**: Run `gnome-shell --version` to check compatibility (requires 50.1).

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

## Related projects

- [linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine): The upstream renderer.
- [gnome-ext-hanabi](https://github.com/jeffshee/gnome-ext-hanabi): Video-only, window-clone approach.
- [kv9898/linux-wallpaperengine](https://github.com/kv9898/linux-wallpaperengine): A fork of the upstream renderer with a `--gnome` flag that uses the window-clone approach; works today on GNOME 50.1.

## Acknowledgements

The initial prototype and documentation for this project were developed with the assistance of an AI coding agent (Claude).
