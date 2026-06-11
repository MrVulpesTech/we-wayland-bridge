# Upstream Issue #302 — context

> Source: `Almamu/linux-wallpaperengine` issue #302, read 2026-06-11.
> This is the canonical upstream thread on GNOME support. Our design must
> answer the maintainer's open questions here, and must account for work
> that already exists in this thread.

## The thread in brief

Issue #302 ("Only the wallpaper window opens as an app") was opened by a
GNOME-on-Wayland user whose wallpaper showed up as a window instead of a
background. It became the de-facto GNOME-support discussion.

## The maintainer's position (Almamu, 2025-05-15)

GNOME is unsupported because **Mutter does not implement
`wlr-layer-shell`**, which is the renderer's Wayland background path. He
linked the relevant GNOME tracker issues and one extension as prior art,
and framed a possible approach:

> "There's also an extension that seems to do something close to what we
> do but extensions are written in JS, maybe it's possible to bridge C++
> with the JS extension in some way. It'll need some experimenting."

This bridge-C++-to-a-JS-extension idea is exactly this project's thesis.

### GNOME/Mutter tracker issues he cited (all still Open)

| Link | Title | What it is |
|---|---|---|
| [mutter#973](https://gitlab.gnome.org/GNOME/mutter/-/issues/973) | Implement layer_shell protocol | Request to implement `wlr-layer-shell` in Mutter so external shells/background clients work. Open. |
| [mutter#1053](https://gitlab.gnome.org/GNOME/mutter/-/issues/1053) | Make it possible for libmutter users to implement their own Wayland protocols | Asks for extensibility instead of adopting layer-shell directly. Open. |
| [gnome-shell#1141](https://gitlab.gnome.org/GNOME/gnome-shell/-/issues/1141) | layer-shell protocol support | Asks whether GNOME Shell will support layer-shell. Open. |

These have been open for years with no resolution. The practical
takeaway stands: **do not expect layer-shell on GNOME.** The only path is
code running inside `gnome-shell` (an extension), which is the premise of
our architecture (`10_vision/10.01_project_vision.md`).

### Prior art he pointed at

- [gnome-ext-hanabi](https://github.com/jeffshee/gnome-ext-hanabi) — a
  GNOME extension that plays video in the background. The technique
  (inject a Clutter actor into the background) is sound; it does video
  only, not Wallpaper Engine scenes.

## What already exists in the thread — a working GNOME solution

This is the part that changes our positioning. **@kv9898** posted
(2026-05-29) a working GNOME integration, in a fork
([kv9898/linux-wallpaperengine](https://github.com/kv9898/linux-wallpaperengine),
`gnome` branch), confirmed by a second user, **tested on GNOME Shell 50.1
/ Ubuntu 26.04 — the same configuration as our reference machine.**

Its approach (Hanabi-style window cloning, not PipeWire):

- **C++ side:** a `--gnome` flag makes the Wayland driver create standard
  **xdg-shell windows** (which Mutter supports) instead of layer-shell
  surfaces. The window title encodes monitor metadata.
- **Extension side:** discovers those renderer windows, makes
  `Clutter.Clone` actors of them, injects the clones into GNOME Shell's
  background actors (behind desktop icons), and hides the originals from
  dock/overview/Alt+Tab. Added blur-my-shell support.

The maintainer responded (2026-06-09):

> "I did want to start working on an extension to integrate
> linux-wallpaperengine into GNOME soon, so this will make my life
> easier. Thanks! I'll make sure to credit you."

So as of June 2026: a working window-clone solution exists, and the
**maintainer intends to build official GNOME support**, likely informed
by it, on top of the `next-v2` core library.

## What this means for us

1. **We are not the only path, and may partly duplicate.** A working
   GNOME wallpaper integration already exists and an official one is
   planned. Proceeding as if the field is empty would be wrong.
2. **Our differentiator is the transport, not the goal.** kv9898's and
   Hanabi's approach is `Clutter.Clone` of a hidden real window. Ours is
   the core embedding API (30.02) rendering into a dma-buf, published over
   PipeWire — no hidden toplevel, no window-clone, true offscreen render.
   Whether that is enough better to justify a separate project is an open
   question (see `90_open_questions` Q-10).
3. **Coordinate, do not surprise.** The maintainer is receptive and
   active. The right move is to engage in #302 (draft in
   `comment-pr609.md`), describe the PipeWire/embedding path, and ask
   whether it belongs upstream — rather than quietly shipping a competing
   project. Contributing the PipeWire producer to upstream may serve the
   goal better than maintaining a fork.

## Comparison of GNOME integration techniques

| Technique | Used by | Pros | Cons |
|---|---|---|---|
| Window clone (xdg-shell window + `Clutter.Clone`) | Hanabi, kv9898 fork | Works today on GNOME 50.1; no renderer-internal rework | Hidden real toplevel per monitor; clone/compositing overhead; title-encoded metadata; fragile across shell restarts |
| PipeWire + dma-buf from embedded core | this project (proposed) | No hidden window; potential zero-copy; producer reusable by any consumer; clean process/licence boundary | Unproven zero-copy on the target GPU (Q-1); depends on the unmerged `next-v2` core API; more work |
