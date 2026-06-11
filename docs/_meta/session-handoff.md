# Session handoff

> Read this **after** `docs/MEMORY.md` and **before** starting work.
> Captures operational patterns, command recipes, decisions made in chat,
> and gotchas that did not warrant a full ADR but would otherwise force
> the next session to relearn them.
>
> Update the relevant section when a convention solidifies, and the
> "Where we left off" section at the end of every session. Append; do not
> rewrite history.

## 1. Operator and machine

- Git author: `Mr Vulpes <mr.vulpes.tech@gmail.com>`
- Repo path: `~/projects/we-wayland-bridge`
- OS: Ubuntu 26.04 LTS, GNOME Shell 50.1, Wayland (X11 session removed)
- GPU: Intel CometLake-U UHD Graphics (integrated)
- The renderer reads the operator's own Steam wallpapers via two
  gitignored symlinks (`steam-workshop`, `steam-assets`). Read-only.

## 2. The session plan

Four sessions to a minimal viable wallpaper (scene type, one monitor, no
web, no audio-reactivity). Each ends by updating `docs/` and the "State"
section of `CLAUDE.md`.

| Session | Goal | Deliverable |
|---|---|---|
| 1 | Build upstream, run a wallpaper in `--window` | `session-01-build.md` |
| 2 | Survey output backends, scope headless→PipeWire | `30_rendering/30.01_output_backends.md` |
| 3 | GNOME extension prototype: a Clutter actor with any video stream in the background layer | extension prototype + session log |
| 4 | Bridge: renderer → PipeWire → extension; validate zero-copy | session log; confirm ADR-0004 |

## 3. Conventions

- `upstream/` is a submodule. Never edit it in place. After a fresh
  clone: `git submodule update --init`.
- Renderer changes go on the `pipewire-backend` fork branch of
  `linux-wallpaperengine`, mirrored into `bridge/` as patches (ADR-0001).
- Extension testing happens **only** in a nested shell, never on the live
  desktop. GNOME 49+ replaced `--nested` with the Mutter Development Kit
  (`--devkit`); run from a real terminal, not the snap-confined VS Code one:

  ```sh
  dbus-run-session -- gnome-shell --devkit --wayland
  ```

  The assistant must never launch or `pkill` gnome-shell — it shares the
  operator's live session.

- Every performance claim needs a session log behind it before it ships
  in a doc (writing-style §6). The zero-copy claim (Q-1) is unproven.

## 4. Commands that get reused

```sh
# Initialize the submodule on a fresh clone
git submodule update --init

# Pick a wallpaper to test: list subscribed wallpapers and their types
for d in steam-workshop/*/; do
  printf '%s\t' "$(basename "$d")"
  grep -o '"type"[^,]*' "$d/project.json" 2>/dev/null | head -1
done

# Run a wallpaper in a window (filled in with exact flags in Session 1)
# upstream/build/linux-wallpaperengine --window <geometry> \
#   --assets-dir steam-assets <workshop_id>
```

## 5. Decisions made in chat (not in ADRs)

| Decision | Detail | Status |
|---|---|---|
| Docs language | English only, including for an upstream PR | Settled (writing-style §1) |
| Working project name | `we-wayland-bridge` | Provisional (Q-5) |
| Security posture | No threat model / double-blind protocol / CLA — this is a desktop wallpaper bridge, not a privileged server | Settled |
| Doc structure | Johnny.Decimal under `docs/`, adapted from a prior project's doc set | Settled |

## 6. Gotchas

- GNOME extension APIs break across GNOME releases (principle P6). Pin
  assumptions to GNOME 50.1 and keep the extension surface small.
- "Works on integrated Intel" does not prove the dma-buf path on NVIDIA
  or multi-GPU. Record what was actually tested.
- Do not try to set a wallpaper on the live GNOME background during
  Sessions 1–2: there is no path yet, and `--window` is sufficient for
  build verification.

## 7. Where we left off

> Update this at the end of every session.

- **As of 2026-06-11, start of Session 1:** documentation scaffold
  created (vision, architecture, ADR-0001..0004, writing-style, this
  file, open questions). Project decisions locked in: English docs, name
  `we-wayland-bridge`, no security/double-blind apparatus, fully
  open-source / public GitHub as the goal.

- **As of 2026-06-11, end of Session 1:** renderer builds clean on the
  reference machine and renders scene wallpapers correctly (verified
  visually on `2804205787` and `3622495963`). Full write-up in
  `session-01-build.md`. Key gotchas captured there: nested submodules
  need `--init --recursive`; CEF downloads at configure time; web/CEF
  wallpapers hang under the snap-confined VSCode terminal (non-MVP).
  **Next: Session 2** — read `upstream/src/WallpaperEngine/Render/Drivers/Output/`
  and document the backend interface in `30_rendering/30.01_output_backends.md`,
  then check upstream issues/PRs for an existing headless/PipeWire effort
  (Q-2).

- **As of 2026-06-11, end of Session 2:** mapped the output abstraction
  (VideoDriver + Output + OutputViewport; self-registering factories;
  the GLFW driver already renders to a hidden window and has a
  `glReadnPixels` read-back path). Two backend options documented (copy
  via memfd vs zero-copy via dma-buf). **Major finding:** upstream PR
  #609 (`next-v2`) is refactoring the renderer into an embeddable
  `linux-wallpaperengine-core` library explicitly to support GNOME/KDE —
  no competing PipeWire effort exists, but our integration should target
  `next-v2`, not `main`. This reopens ADR-0001 and ADR-0003 (Q-9). Full
  detail in `30_rendering/30.01_output_backends.md` §6.
  **Next (Session 2b): fetch and read the `next-v2` branch** —
  the `linux-wallpaperengine-core` API and its DE-integration docs —
  then decide the ADR-0001/0003 revisions and comment on #609.

- **As of 2026-06-11, end of Session 2b:** studied `next-v2` (PR #609) and
  issue #302. Findings in `30_rendering/30.02_next-v2-core-api.md` and
  `90_upstream/302-context.md`. Three things changed the project:
  1. **Q-9 answered:** core exposes a C embedding API where the host owns
     the GL context and hands core an FBO
     (`wp_project_set_output_framebuffer` + `wp_render_frame`), verified
     in `dev-viewer` and the frontend. Our producer = a host linking core
     that renders into a dma-buf-backed texture and publishes PipeWire; no
     renderer-internal patch. The `Drivers/Output` plan (30.01, for
     `main`) is superseded by this.
  2. **The `…-core` repo is not public yet** (404); core lives in
     `next-v2/src/core`. Pin a `next-v2` commit (Q-11).
  3. **Field is not empty (Q-10):** @kv9898 already has a working GNOME
     wallpaper integration (window-clone, tested on GNOME 50.1/Ubuntu
     26.04) and the maintainer intends an official extension. Our
     differentiator is the PipeWire/dma-buf path. Whether that justifies a
     separate project vs. contributing upstream is now the pivotal call.
  **Operator actions pending (no code/posting done):** review and post
  `90_upstream/comment-pr609.md` on issue #302; decide Q-10; then revise
  ADR-0001/0003.
  **Next coding session (3):** build the GNOME extension consumer against
  a dummy PipeWire source (`videotestsrc`) — independent of the producer
  decision, validates the hard GNOME-specific unknowns.
  **Scratch on disk:** `next-v2-review/` worktree.

- **As of 2026-06-11, Session 3 Stage A done:** built the frame producer.
  `bridge/host/main.cpp` embeds `linux-wallpaperengine-core` (pinned
  `next-v2` @ `828485a`), creates a headless surfaceless-EGL GL 3.3 context,
  renders a scene into a host-owned FBO via the embedding API, dumps PNGs.
  Build: `bridge/build.sh` (one command; builds core with webhelper/
  frontend/dev-viewer/tests OFF, then the host). Verified: core calls the
  host time callback once/frame; frozen-clock frames are identical (MAD
  0.000) while moving-clock frames differ — animation is genuinely
  clock-driven. Perf (render-only, Intel UHD): astronaut ~20–26 ms/frame,
  Frieren (5160x2160 scene) ~100 ms/frame. Full doc:
  `40_bridge/40.01_producer.md`.
  **The `next-v2-review/` worktree is now a build dependency of `bridge/`
  — do not remove it.** Its `External/` submodules were initialized
  (required for the core build); this does not touch our submodule pin
  (`b016d7d`) or taint any committed tree.
  Stage A reviewed and committed (`3b04c37`).

- **As of 2026-06-11, Session 3 Stage B done:** the host now also runs as a
  PipeWire `Video/Source` (`--pipewire`, BGRx, timer-driven, SHM mapped
  buffers, `glReadPixels` + row-flip copy). Verified end-to-end: a
  GStreamer `pipewiresrc` consumer pulled live animated frames (correct
  orientation, opaque); ~21% of one core CPU and ~40 fps at 1080p
  (GPU-render-bound on the iGPU); clean SIGINT/SIGTERM shutdown. Demo:
  `gst-launch-1.0 pipewiresrc path=<id> ! videoconvert ! autovideosink`
  (the host prints the node id). Gotcha for the alpha fix: stream is BGRx
  so the scene's alpha=0 is harmless. Doc: `40_bridge/40.01_producer.md`
  (Stage B section + consumer requirements for Session 4: per-monitor
  size, fill/fit/crop).
  **Stopped for operator review at the Stage B checkpoint.** Next on
  go-ahead: Stage C (gbm/dma-buf zero-copy, `eglExportDMABUFImageMESA`,
  SHM kept as `--shm` fallback) — this is where Q-1 is actually proven.

- **Stage B review fixes (2026-06-11):** operator reported the stream was
  upside-down and (at the size they didn't use) washed-out. Two real
  findings, both resolved/recorded:
  1. **Orientation:** next-v2 core renders top-down, so the vertical flip
     was wrong — removed (glReadPixels now writes straight into the mapped
     buffer; one fewer copy). Verified at 1080p against the upstream
     binary: astronaut at the bottom, flare rising. Correct.
  2. **Resolution-dependent washout (Q-12):** scene `2804205787` renders
     fully at 1920x1080 but washes to its white fog layer at ≤720p (red
     flare/astronaut vanish). next-v2 **core** behaviour, not transport;
     a clean upstream reproducer. Wallpapers run ≥1080p so impact is
     limited. **Operator note:** always test the producer at 1080p+.
  Operator never commits via the assistant — assistant provides `git add`
  + commit message in chat only.

- **Stage B fix #2 — buffer timestamps (2026-06-11):** operator's
  `autovideosink` showed a frozen frame even at 1080p. Root cause: the
  PipeWire buffers had no PTS, so a *synchronizing* sink displays only the
  first frame (a non-syncing `multifilesink` consumer animated, which
  masked it during my earlier verification). Fix: request a
  `SPA_META_Header` on the stream's buffers and stamp `pts` (elapsed ns) +
  `seq` each frame. Verified: a `fakesink sync=true` now renders buffers
  with advancing PTS (0→5.6s, offsets 0→77). **Lesson: always verify a
  video stream with a *synchronizing* sink, not just multifilesink.**
  Operator must rebuild (`bridge/build.sh` or rebuild the host) and re-run
  `bridge/demo.sh`.

- **As of 2026-06-11, Session 4 (consumer) — first cut, UNVERIFIED:**
  studied `gnome-ext-hanabi` and `@kv9898`'s fork. Big finding: both clone
  a hidden window and need ~10 overrides to hide it; our PipeWire producer
  has **no shell window**, so the consumer needs only the one injection
  point (override `BackgroundManager._createBackgroundActor`, add our actor
  to each background actor) — none of the window-hiding overhead. Wrote
  `extension/` (MIT): `extension.js` + `liveWallpaper.js` —
  `pipewiresrc ! videoconvert ! RGBA ! appsink`, polled on the shell main
  thread, uploaded via `St.ImageContent.set_bytes`, solid-colour fallback,
  reconnect on EOS. Design in `40_bridge/40.02_consumer.md`; run/test in
  `extension/README.md`.
  **Blocker:** the dev sandbox cannot launch gnome-shell — `--nested` is
  removed in 50.1; `--wayland` → EBUSY (can't take the live seat);
  `--headless` → gsettings schema crash. Same snap-confinement class as the
  CEF block. So the extension is **untested**; the operator runs the nested
  test on the live session and reports the `wwb:` journal lines (the API
  probe + whether `first frame uploaded` or upload error), then the
  frame-upload path is fixed from that. The correct GNOME 50.1 nested
  command also needs confirming on the live session.
  **Did not touch the live desktop.** Probe extension was a throwaway in
  `~/.local/.../probe@wwb` (removal flaked on the sandbox; harmless,
  disabled — operator can `rm -rf` it).

- **As of 2026-06-11, Session 4 — pipeline VERIFIED end-to-end (nested):**
  the GNOME extension consumer works. Frames flow renderer → PipeWire →
  `pipewiresrc`/appsink → `St.ImageContent.set_bytes` → Clutter actor in the
  background layer (`wwb: first frame uploaded`, samples streaming) on GNOME
  50.1 via the Mutter Devkit. Q-1 answered (SHM/PipeWire path reaches GNOME).
  Hard-won facts (all in `40_bridge/40.02_consumer.md`):
  1. Nested shell = `dbus-run-session -- gnome-shell --wayland --devkit
     --virtual-monitor 1920x1080` (run from a real terminal; `--nested` is
     gone; Devkit starts with 0 monitors without `--virtual-monitor`).
  2. `appsink` needs `import GstApp` or `try_pull_sample` throws (silently,
     if caught) → samples=0.
  3. `St.ImageContent.set_bytes(coglContext, bytes, format, w, h, stride)` —
     6 args, leading CoglContext.
  4. CoglContext = `global.stage.context.get_backend().get_cogl_context()`.
  5. `pipewiresrc` is NOT auto-linked to a Video/Source; needs `path=<id>`
     (testing via `WWB_PIPEWIRE_PATH` env; production needs node-by-name).
  **Remaining:** watch it on a visible display (`--virtual-monitor` is
  headless) — likely a careful live-session test (extension is contained;
  `gnome-extensions disable` is the escape hatch); producer node discovery;
  fit/crop modes; fullscreen-pause; producer lifecycle.
