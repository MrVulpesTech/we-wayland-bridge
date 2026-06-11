# we-wayland-bridge

> Wallpaper Engine live wallpapers on GNOME Shell under Wayland.

**Status:** pre-alpha. Architecture and documentation phase.

Wallpaper Engine is a Windows application; its animated wallpapers are
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
the GNOME background layer, without an expensive per-frame copy.

## Architecture

```text
Wallpaper Engine files (Steam) → linux-wallpaperengine renderer
  → [new headless output backend] → PipeWire / dma-buf (zero-copy)
  → GNOME Shell extension (Clutter actor in the background layer)
  → live wallpaper on GNOME Wayland
```

The renderer and `gnome-shell` are separate processes. Frames cross
between them over PipeWire — which is also the licence boundary: the
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
git clone <repo-url> we-wayland-bridge
cd we-wayland-bridge
git submodule update --init
```

There is nothing to run yet beyond the upstream renderer itself; the
build and a first wallpaper in a window are
[Session 1](docs/session-01-build.md). The renderer reads Wallpaper
Engine content you already own in Steam — this project ships no wallpaper
assets.

## Scope

The first milestone is deliberately narrow: scene-type wallpapers, a
single monitor, no web wallpapers, no audio-reactivity. See
[non-goals](docs/10_vision/10.03_non_goals.md).

## Licensing

Renderer-derived code (`bridge/`) is GPLv3, matching upstream. The GNOME
extension (`extension/`) is a separate process and is MIT. See
[ADR-0002](docs/_adr/ADR-0002-licensing.md). `LICENSE` files are added
before the repository goes public.

## Legal

This project renders Wallpaper Engine content that the user has legally
purchased and installed through Steam — the same footing on which
`linux-wallpaperengine` has operated for years. It redistributes no
Wallpaper Engine code or assets. You must own Wallpaper Engine on Steam
and have subscribed to the wallpapers you use.
