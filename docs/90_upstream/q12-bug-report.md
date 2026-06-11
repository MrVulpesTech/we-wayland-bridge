# Draft bug report for upstream (Q-12)

> **Where to post:** the PR #609 (`next-v2`) thread, or a new issue the
> maintainer prefers. Draft for the operator to review and post manually.
> Do not post automatically.
>
> **Screenshots:** not committed (they are rendered Wallpaper Engine
> content; writing-style §5). Attach them from
> `bridge/out/q12-evidence/q12-720p.png` and `…/q12-1080p.png` when
> posting. Regenerate with the commands below.

---

## Title

next-v2 core: scene wallpaper renders only its base/fog layer at low
output resolution (≤ ~1366×768), full scene at ≥ ~1500×844

## Environment

- Branch / commit: `next-v2` at `828485aa0804e6889cff895e139e293bf6a3fb28` (PR #609 head)
- Built the `linux-wallpaperengine-core` target standalone:
  `cmake -DWPBUILD_WEBHELPER=OFF -DWPBUILD_FRONTEND=OFF -DWPBUILD_DEVVIEWER=OFF -DWPBUILD_TESTS=OFF`
- OS: Ubuntu 26.04, GNOME 50.1 Wayland
- GPU: Intel CometLake-U UHD Graphics, Mesa
- Driven through the public C embedding API (`include/linux-wallpaperengine/`)

## Summary

For a scene wallpaper, when the output framebuffer is small the renderer
produces **only the white snow/fog (base) layer** — the foreground layers
(the figure, the red flare particles, etc.) are missing. At a larger
output size the full scene renders correctly. The transition is around
1366×768 → 1500×844; the effect is consistent across a multi-second run
(it is not a load-in fade).

| Output size | Result |
|---|---|
| 1280×720 | base/fog only (washed white) |
| 1366×768 | base/fog only (washed white) |
| 1500×844 | full scene |
| 1600×900 | full scene |
| 1920×1080 | full scene |

Scene aspect (this wallpaper is authored at 2560×1440, 16:9) is held
constant across the sizes above, so this is resolution magnitude, not
aspect.

## Steps to reproduce

The host owns the GL context and drives core via the embedding API:

1. Create a headless GL 3.3 context (surfaceless EGL).
2. Create a host FBO of size `W×H` (RGBA8 texture + color attachment).
3. `wp_config_create` → `wp_config_set_assets_dir(<WE assets>)` →
   `wp_config_enable_audio(false)`.
4. `wp_context_create` → `wp_context_set_gl_proc_address(eglGetProcAddress)`
   → `wp_context_set_time_counter(<host clock>)`.
5. `wp_project_load_folder(<workshop 2804205787>)` →
   `wp_project_hint_size(W, H)` → `wp_project_set_output_framebuffer(fbo)`.
6. Per frame: `wp_render_update_time` + `wp_render_frame`; read back the
   FBO.

Run at `W×H = 1280×720` → base/fog only. Run at `1920×1080` → full scene.
Nothing else changes between the two runs.

A ~250-line reference host that does exactly this (public API only) can be
shared if useful; the dev-viewer in this branch exercises the same path
(`SimpleBackgroundViewer` → `wp_project_set_output_framebuffer` →
`wp_render_frame`).

> Note: this uses a wallpaper the reporter owns via Steam (Workshop id
> 2804205787, a `scene` type). No assets are redistributed here.

## Expected vs actual

- **Expected:** the same scene at any output resolution, differing only in
  detail/sharpness.
- **Actual:** at ≤ ~1366×768 only the base/fog layer renders; the
  foreground layers are absent.

## Evidence

- `q12-720p.png` — 1280×720, washed to white fog (attach).
- `q12-1080p.png` — 1920×1080, full scene: figure at the bottom, red flare
  rising (attach).

Both captured at the same virtual time (≈2 s) from the same wallpaper.

## Localization hint (cheap, not a diagnosis)

The scene composites through scaled render-target FBOs — `CFBO` carries a
`scale` factor (`src/core/WallpaperEngine/Render/CFBO.cpp`), and the scene
references `_rt_*` targets (`_rt_shadowAtlas`, `_rt_lightCookie`,
`_rt_MipMappedFrameBuffer` aliased to `_rt_FullFrameBuffer` in
`CWallpaper.cpp`). A plausible direction: at small output sizes a scaled
intermediate FBO degenerates (rounds to 0 / 1 px or trips a completeness
check), so the passes that build the foreground layers produce nothing and
only the base survives. Offered as a starting point, not a root cause.

## Impact

For desktop-wallpaper use the output is the monitor size (usually
≥ 1080p), so the common case renders correctly. But 1366×768 is a very
common laptop panel, and any consumer that renders at a reduced size
(thumbnails, previews, low-DPI outputs) hits the washed-out result.
