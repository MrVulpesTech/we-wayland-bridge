# ADR-0002: Licensing boundary

- **Status:** Accepted
- **Date:** 2026-06-11
- **Deciders:** project author

## Context

`linux-wallpaperengine` is GPLv3. Two parts of this project have
different relationships to it:

- The output backend (`bridge/`) is **derived from** the renderer — it
  links the renderer's GL context and buffer code and is compiled into
  the renderer binary. GPLv3 obligations follow the derivative.
- The GNOME extension (`extension/`) **does not link** any renderer code.
  It is a separate process that receives frames over PipeWire. It derives
  nothing from upstream.

The project wants the extension to be reusable and permissively licensed,
while honouring the GPLv3 on everything that genuinely derives from the
renderer. Principle P5 (honour the licence boundary) and P3 (respect the
process boundary) are the same boundary seen from two angles.

## Decision

- **`bridge/` and any renderer-side code: GPLv3-or-later**, matching
  upstream. Renderer changes are upstreamable without a licence change.
- **`extension/` and project tooling that does not link the renderer:
  MIT.** The extension communicates with the renderer only over PipeWire
  IPC; it is a separate program, not a derivative work.
- **Documentation:** no separate licence decision needed for v1; docs
  ship with the repository. (A docs licence such as CC-BY can be added
  later if it matters for reuse.)
- The PipeWire IPC stream is the boundary that makes the MIT/GPLv3 split
  legitimate. Keep it a real process boundary (P3): the extension must
  not gain a build-time or link-time dependency on renderer code.

## Alternatives considered

- **Everything GPLv3.** Simplest, but needlessly restricts reuse of the
  extension, which derives nothing from the renderer.
- **Everything MIT.** Not available — the output backend derives from
  GPLv3 upstream; relicensing it is not ours to do.
- **LGPL for the bridge.** No benefit here: the bridge is compiled into
  the GPLv3 renderer, so it inherits GPLv3 regardless.

## Consequences

- **Positive:** The extension can be adopted and adapted freely; the
  renderer work stays upstream-compatible. The split mirrors the real
  architecture, so it is easy to explain and defend.
- **Negative:** Contributors must know which directory they are in and
  which licence applies. The repository carries two licences.
- **Risks:** Someone links renderer code into the extension "to make it
  simpler", collapsing the boundary and pulling GPLv3 onto the extension.
  Mitigation: the IPC boundary is architectural (P3), enforced in review,
  and documented in `20_architecture/20.03_component_map.md`.

## Revisit triggers

- A future feature genuinely needs the extension to link renderer code:
  re-examine whether that feature belongs in the renderer process
  instead, and if not, accept GPLv3 on that component.
- Upstream relicenses.

## Outstanding

`LICENSE` files are not yet committed. Before the repository goes public:
add the GPLv3 text covering `bridge/`, an `extension/LICENSE` (MIT), and a
top-level note pointing at the boundary. Tracked in
`90_open_questions/90.01_open_questions.md`.
