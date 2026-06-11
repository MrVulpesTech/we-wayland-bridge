# we-wayland-bridge

Goal: run Wallpaper Engine live wallpapers — animated shader scenes, not
just video — as the desktop background on GNOME Shell under Wayland
(Ubuntu 26+).

Architecture: the `linux-wallpaperengine` renderer (with a new
headless output backend) → PipeWire / dma-buf → a GNOME Shell extension
(a Clutter actor in the background layer). The renderer already exists;
the project is the bridge between it and GNOME on Wayland.

## Read first

1. This file.
2. `docs/MEMORY.md` — documentation index.
3. `docs/_meta/session-handoff.md` — what happened last, gotchas, where
   to pick up.

## Rules

- `upstream/` is a git submodule (`linux-wallpaperengine`). **Do not edit
  it in place.** Renderer changes go on the `pipewire-backend` fork
  branch and are mirrored into `bridge/` as patches; goal is a PR
  upstream. See `docs/_adr/ADR-0001-repository-strategy.md`.
- `steam-workshop` and `steam-assets` are gitignored symlinks to the
  operator's Steam data. Read them; never commit, copy, or redistribute
  their contents. See `docs/10_vision/10.03_non_goals.md`.
- Licensing: renderer-derived code in `bridge/` is GPLv3; the
  `extension/` is a separate process (IPC over PipeWire) and is MIT. Keep
  the boundary real. See `docs/_adr/ADR-0002-licensing.md`.
- Test the extension **only** in a nested shell, never on the live
  desktop: `dbus-run-session -- gnome-shell --nested --wayland`.
- Follow `docs/_meta/writing-style.md` for all prose and comments
  (English only, no AI filler, no unmeasured performance numbers).
- At the end of each session, update `docs/` and the "State" section
  below.

## Target system (reference machine)

- Ubuntu 26.04 LTS, GNOME Shell 50.1, Wayland (Mutter, no
  `wlr-layer-shell`, X11 session removed).
- GPU: Intel CometLake-U UHD Graphics (integrated). The dma-buf zero-copy
  path (the project's main risk) is proven here first and does not
  generalize to other vendors without testing.
- PipeWire present.

## State

- [x] Session 1: upstream built, two scene wallpapers render (see `docs/session-01-build.md`)
- [ ] Session 2: `docs/30_rendering/30.01_output_backends.md` complete
- [ ] Session 3: GNOME extension prototype with a test video stream
- [ ] Session 4: PipeWire bridge, zero-copy validated (Q-1 / ADR-0004)

Documentation scaffold is in place (vision, architecture, ADR-0001..0004,
open questions). Decisions locked: English docs, name `we-wayland-bridge`,
no security/double-blind apparatus, fully open-source / public GitHub the
goal (Q-5). Renderer builds clean on Ubuntu 26.04; scene rendering is the
ground truth for everything downstream. Web (CEF) wallpapers did not run
from the snap-confined VSCode terminal — out of MVP scope, retest later.
