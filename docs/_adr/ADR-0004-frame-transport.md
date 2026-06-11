# ADR-0004: Frame transport

- **Status:** Proposed
- **Date:** 2026-06-11
- **Deciders:** project author
- **Note:** Provisional. This is the project's central technical risk;
  confirmed or revised after the Session 4 prototype.

## Context

Frames must cross from the renderer process to `gnome-shell` (the process
boundary of P3). A scene wallpaper is a continuous stream at the display's
resolution and refresh rate. Copying every frame through system memory
(GPU→CPU→GPU) costs bandwidth and a CPU core that scales with resolution;
at high resolutions it defeats the purpose (principle P4: zero-copy or it
does not ship).

The consumer side, `gnome-shell`, renders with Clutter/Cogl and can take
a GStreamer source. The producer side is the new renderer output backend
(ADR-0003).

## Decision (proposed)

Transport frames over **PipeWire** using **dma-buf** buffers, so the
GPU buffer is shared by handle and never copied through the CPU:

```text
renderer GL FBO ──► dma-buf export ──► PipeWire stream ──► dma-buf import ──► Cogl/Clutter texture
```

- **Producer:** the renderer's new output backend publishes a PipeWire
  video stream, negotiating a dma-buf format.
- **Consumer:** the extension consumes the stream — via GStreamer
  `pipewiresrc` into a Clutter sink, or a direct PipeWire consumer if that
  proves cleaner — importing the dma-buf as a Cogl texture for the
  background actor.

PipeWire is chosen because it already runs on the target desktop, brokers
dma-buf sharing with format negotiation, and has a GStreamer consumer
element, so neither end invents a shared-memory protocol.

## Alternatives considered

- **PipeWire with shared-memory (`memfd`) buffers instead of dma-buf.**
  Acceptable *fallback* if dma-buf import fails on a given driver, but it
  reintroduces a CPU copy. Kept as the documented degraded mode, not the
  target.
- **Raw dma-buf fd passed over a private Unix socket, no PipeWire.**
  Rejected: reinvents PipeWire's negotiation and lifecycle handling for no
  gain, and is less reusable.
- **Software copy via a pipe / shared file.** Rejected outright — this is
  exactly the per-frame copy P4 forbids.

## Consequences

- **Positive:** If it holds, the whole pipeline is zero-copy GPU-to-GPU,
  and the producer is reusable by any PipeWire consumer.
- **Negative:** dma-buf sharing is sensitive to GPU vendor, driver,
  format modifiers, and the GL/Cogl import path; getting it right is
  fiddly and hardware-dependent.
- **Risks:** The target machine is integrated Intel. "Works on Intel"
  does not prove NVIDIA (proprietary driver) or cross-vendor cases.
  Multi-GPU and format-modifier mismatches are known failure modes.
  Mitigation: prove Intel first (Session 4), keep the memfd fallback,
  document what is tested versus assumed.

## Revisit triggers

- Session 4 prototype cannot achieve zero-copy import into Cogl on the
  target GPU: fall back to memfd, and re-scope whether high-resolution
  scenes are viable on this hardware.
- A different GPU vendor needs support and the dma-buf path fails there:
  per-vendor handling or a documented hardware support matrix.

## Open question

The end-to-end zero-copy claim is **unproven** until the Session 4
prototype runs. It is tracked as the top open question in
`90_open_questions/90.01_open_questions.md` and must not be stated as fact
in any doc until a session log backs it (writing-style §6).
