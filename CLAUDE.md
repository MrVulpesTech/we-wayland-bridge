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
- [x] Session 2: output abstraction mapped; `docs/30_rendering/30.01_output_backends.md` complete
- [x] Session 2b: studied `next-v2` core API + issue #302; Q-9 answered (build on core, not `main`). See `30_rendering/30.02_next-v2-core-api.md`, `90_upstream/302-context.md`
- [ ] Pending operator review: post `90_upstream/comment-pr609.md` on issue #302; decide Q-10 (separate project vs upstream contribution); revise ADR-0001/0003
- [ ] Session 3: GNOME extension prototype consuming a dummy PipeWire stream (no renderer coupling)
- [ ] Session 4: PipeWire bridge from a core-embedding host, zero-copy validated (Q-1 / ADR-0004)

Documentation scaffold is in place (vision, architecture, ADR-0001..0004,
open questions). Decisions locked: English docs, name `we-wayland-bridge`,
no security/double-blind apparatus, fully open-source / public GitHub the
goal (Q-5). Renderer builds clean on Ubuntu 26.04; scene rendering verified.

Key shift from Session 2b: upstream `next-v2` (PR #609) ships an
embeddable `linux-wallpaperengine-core` C library with an explicit
offscreen render entry point (`wp_project_set_output_framebuffer`), so our
frame producer should be a host that links core, not a renderer patch. But
a working GNOME solution already exists (@kv9898 window-clone) and the
maintainer plans an official extension — so Q-10 (differentiate via
PipeWire vs contribute upstream) is now the pivotal decision, pending the
#302 conversation.

Temporary on disk (not committed; remove when done): `next-v2-review/` git
worktree of the submodule at the PR #609 head. Clean up with
`cd upstream && git worktree remove ../next-v2-review` then
`git branch -D next-v2-review`.
