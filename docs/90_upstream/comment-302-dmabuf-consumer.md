# Draft comment for issue #302 — dma-buf consumer import

> Review and post manually (operator). Same coordination etiquette as
> `comment-pr609.md`. Body is the text below the line; ~140 words.

---

Following up on the PipeWire route for GNOME.

I built a producer on the `next-v2` core: a headless surfaceless-EGL context
renders into a gbm dma-buf (LINEAR modifier) via
`wp_project_set_output_framebuffer`, published as a PipeWire `Video/Source` with
`SPA_DATA_DmaBuf`. Verified on Intel CometLake/Mesa — GStreamer negotiates a
direct dma-buf import (XRGB, LINEAR) and the contents are correct. Producer-side
zero-copy works.

The consumer side hit a wall: a GJS GNOME Shell extension cannot import an
external dma-buf fd into a Cogl texture. GJS has no EGL;
`cogl_egl_texture_2d_new_from_image` isn't introspectable; `CoglDmaBufHandle` is
export-only; `St.ImageContent` only does CPU uploads — so a pure-GJS consumer
falls back to an SHM copy.

Does the `next-v2` / planned official-extension roadmap expose a GJS-reachable
dma-buf import (a Cogl/Mutter API or a shipped helper), or is window-cloning the
intended zero-copy path on GNOME?
