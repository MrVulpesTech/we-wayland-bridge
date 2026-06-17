# bridge/

The producer: a headless host that embeds `linux-wallpaperengine-core`, renders
a Wallpaper Engine scene into an offscreen framebuffer, and publishes the frames
as a PipeWire `Video/Source`. The GNOME extension (`extension/`) consumes that
stream. This is the renderer side of the pipe.

**Licence: GPLv3-or-later** (derived from the upstream renderer). See
[ADR-0002](../docs/_adr/ADR-0002-licensing.md) and
[ADR-0003](../docs/_adr/ADR-0003-output-backend.md).

> **Status: working (Stage A + B + C producer).** The host renders offscreen via the core
> embedding API (`wp_project_set_output_framebuffer` + `wp_render_frame`) and
> streams BGRx frames over PipeWire (PTS-stamped), consumed live by
> the extension and by `gst-launch`. The producer supports zero-copy via dma-buf (proven in Stage C); 
> the consumer currently uses an SHM copy. End-to-end zero-copy is 
> planned pending a dma-buf import path in the GNOME Shell extension API. Full detail:
> [`docs/40_bridge/40.01_producer.md`](../docs/40_bridge/40.01_producer.md).

## Layout

- `host/main.cpp` — the host (surfaceless EGL GL context, core embedding,
  PipeWire `Video/Source`, `node.name=wpe-host`).
- `build.sh` — one-command build (builds core with webhelper/frontend/dev-viewer
  /tests OFF, then the host). Output in `build/` (gitignored).

## Build and run

```sh
./bridge/build.sh                       # build core + host
bridge/build/host/wpe-host --pipewire --assets-dir steam-assets \
  --size 2560x1440 --fps 30 steam-workshop/<id>
```

Flags: `--size WxH` / `--width` / `--height`, `--fps`, `--frames`, `--pipewire`,
`--dmabuf` (zero-copy dma-buf transport), `--shm` (force the SHM path),
`--verbose` (per-second fps line), `--out <dir>` (PNG dump), `--readback-all`.
Run scenes at **1080p+** (Q-12 washout below that). The host prints its PipeWire
node id, but the extension finds it by `node.name`.

## Build dependency

The host builds against `next-v2-review/` — a git worktree of the upstream
submodule pinned at the PR #609 head (`828485a`), with its `External/` submodules
populated. It is on disk, gitignored, and **must not be removed** while working
on the producer (`CLAUDE.md` State has the rebuild-from-scratch note).
