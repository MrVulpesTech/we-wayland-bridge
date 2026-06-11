# Draft comment for upstream

> **Where to post:** issue
> [#302](https://github.com/Almamu/linux-wallpaperengine/issues/302), not
> PR #609. (The filename is kept for continuity; the target is #302, the
> canonical GNOME thread.)
>
> **Status:** draft for the operator to review and post manually. Do not
> post automatically. Verify the API names below still match the branch at
> post time (checked against `next-v2` @ `828485a`, 2026-06-11).

---

Following up on the C++↔JS-extension idea from your 2025 reply — that is
the experiment we are building: a GNOME Shell extension that shows
Wallpaper Engine **scenes** (not just video) on Wayland by consuming
frames over **PipeWire/dma-buf**, rather than cloning a hidden window like
Hanabi and @kv9898's fork. In `next-v2`, the core library looks like the
right foundation: the host owns the GL context and calls
`wp_project_set_output_framebuffer` + `wp_render_frame` (as `dev-viewer`
and the frontend do), so a host could render core into a dma-buf-backed
texture and publish it. Three questions:

1. Is that the intended embedding path, or is a render-to-dma-buf helper
   planned inside core itself?
2. How settled is the `include/linux-wallpaperengine` C API — safe to
   build a consumer against a pinned `next-v2` commit, or still in flux
   pre-merge?
3. Would a PipeWire output belong in core, or in a separate host on top of
   it — i.e. would you want a PipeWire producer upstreamed?

Happy to be an early tester of the core library. And if the official GNOME
extension is going the window-clone route, knowing that helps us avoid
duplicating it.

---

**Word count:** 184 words (body, excluding the meta header).
