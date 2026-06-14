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
  desktop. On GNOME 49+ `--nested` is gone; the nested shell is now the
  Mutter Development Kit: `dbus-run-session -- gnome-shell --devkit --wayland`.
  Run it from a **real terminal**, not the snap-confined VS Code one. The
  assistant must **never** launch or `pkill` gnome-shell (it shares the
  live session — a `pkill gnome-shell` once logged the operator out).
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
- [~] Session 3 (frame producer): **Stage A + B done** — `bridge/` host embeds core, renders offscreen, and publishes a PipeWire `Video/Source` (BGRx, SHM copy) consumed live by GStreamer (~21% CPU, ~40 fps, clean shutdown). See `docs/40_bridge/40.01_producer.md`. Stage C (dma-buf zero-copy, Q-1) pending operator go-ahead.
- [x] Session 4 (live, 2026-06-14): **the wallpaper is VISIBLE and animating on the real GNOME desktop.** Nested is headless, so three rendering bugs only surfaced live, each fixed from one diagnostic log line: (1) actor culled (no CSS bg + no content → empty paint volume → `vfunc_paint_node` stops) → override `vfunc_get_paint_volume`; (2) `TextureNode` null colour paints transparent → explicit opaque white `Cogl.Color`; (3) **alpha=0** (`firstpx RGBA=…,…,…,0`; BGRx→RGBA leaves alpha 0) → upload as `Cogl.PixelFormat.RGBX_8888` (ignore-alpha = opaque) — this was the actual "nothing on screen". Clean disable verified live (VmRSS 751→266 MB). **Known limit:** laggy at native 2560×1440 — the SHM-copy path uploads ~14 MB/frame on the shell main thread; **Stage C (dma-buf) is the priority fix**. Lowering producer res changes scene framing (renderer frames to output size), so it is not a clean fit. See `docs/session-04-live-wallpaper.md` and `docs/40_bridge/40.02_consumer.md`.
- [!] Session 4 (GNOME extension consumer): pipeline works end-to-end, but **two live tests froze the machine**, exposing FOUR failure modes — all now fixed and **verified in a full nested stress test (2026-06-14)**: (1) grey-screen login = main thread blocked at login with no producer → deferred fail-open init; (2) GPU hard-lock = per-frame texture allocation under the overview's GL blur → single reused Cogl texture (`set_data`), 15 fps cap, pause uploads while overview open; (3) libgstpipewire SIGSEGV on teardown → clean NULL+wait; (4) per-actor pipeline leak (`_createBackgroundActor` fires on every overview toggle, each spinning a new pipeline + leaking the disposed actor's timer) → **rearchitected to ONE shared `FrameSource` (single pipeline+texture) feeding thin paint-only `LiveWallpaper` subscribers; cleanup via the `destroy` signal**. Plus kill-switch (`~/.config/wwb/disabled`, `WWB_FORCE` bypass) and guarded enable(). Stress test (2 min overview thrash + clean disable): single pipeline, zero "already disposed", VmRSS bounded ~508–613 MB (no leak), `wwb: disabled` with no GStreamer criticals, shell ALIVE. Full writeup `docs/40_bridge/40.04_incident_2026-06-12_freeze.md`. **A careful live retry is now reasonable but is the operator's call** (40.03 runbook; visual needs live since `mutter-devkit` viewer isn't packaged). VmRSS is stable but elevated — Stage C (dma-buf) is the proper fix. Original end-to-end note below:
- [x] Session 4 (GNOME extension consumer): **pipeline VERIFIED end-to-end in a nested session** — `extension/` injects a `LiveWallpaper` into the background layer; frames flow renderer → PipeWire → `pipewiresrc`/appsink → `St.ImageContent` → Clutter actor (`wwb: first frame uploaded`, samples streaming) on GNOME 50.1 via the Mutter Devkit. **Q-1 answered** (frames reach GNOME via the SHM/PipeWire path). Tested headless, so not yet *watched* on a visible display; producer node-discovery and fit modes remain. See `docs/40_bridge/40.02_consumer.md`.

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

On disk, not committed: `next-v2-review/` git worktree of the submodule at
the PR #609 head (`828485a`), with its `External/` submodules populated.
**The `bridge/` host now builds against it — do not remove it** while
working on the producer. `bridge/build/` and `bridge/out/` are gitignored
build/render output. To rebuild from scratch: `bridge/build.sh`.
