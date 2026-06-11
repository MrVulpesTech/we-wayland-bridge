# we-wayland-bridge — Documentation Index

Entry point for all design, architecture, and session docs. Read
top-to-bottom on first visit; afterwards, jump by area.

Numbering follows **Johnny.Decimal**: areas are 10-wide ranges, items
are two-digit suffixes within an area. Folders prefixed with `_` are
context/meta directories — they describe the project rather than being
part of the shipped product.

## Read order on a new session

1. Root `CLAUDE.md` — rules, target system, current state.
2. This file.
3. `_meta/session-handoff.md` — what happened last, gotchas, where to
   pick up.

## Areas

### `_adr/` — Architecture Decision Records
Dated, durable decisions. New facts amend via *new* ADRs (Superseded-by),
never by rewriting an accepted one.

- [ADR-0001: Repository strategy](_adr/ADR-0001-repository-strategy.md) — *Accepted: separate repo + submodule, fork for renderer changes*
- [ADR-0002: Licensing boundary](_adr/ADR-0002-licensing.md) — *Accepted: GPLv3 for renderer-derived code, MIT for extension*
- [ADR-0003: Output backend strategy](_adr/ADR-0003-output-backend.md) — *Proposed: headless render target feeding PipeWire*
- [ADR-0004: Frame transport](_adr/ADR-0004-frame-transport.md) — *Proposed: PipeWire with dma-buf zero-copy*
- [ADR template](_adr/ADR-template.md)

### `10_vision/` — What and why
- [10.01 Project vision](10_vision/10.01_project_vision.md)
- [10.02 Principles](10_vision/10.02_principles.md)
- [10.03 Non-goals](10_vision/10.03_non_goals.md)

### `20_architecture/` — How it is built
- [20.01 Tech stack](20_architecture/20.01_tech_stack.md)
- [20.02 Directory structure](20_architecture/20.02_directory_structure.md)
- [20.03 Component map](20_architecture/20.03_component_map.md) — process boundaries and the frame pipeline

### `30_rendering/` — Renderer and frame transport
- [30.01 Output backends](30_rendering/30.01_output_backends.md) — the renderer's output abstraction on current `main`, and two ways to add a PipeWire backend
- [30.02 next-v2 core API](30_rendering/30.02_next-v2-core-api.md) — the embeddable C library in PR #609 (`wp_project_set_output_framebuffer`), the offscreen entry point, maturity, and the ADR recommendation matrix

### `90_upstream/` — Coordinating with the upstream project
- [Issue #302 context](90_upstream/302-context.md) — the canonical GNOME thread: maintainer's reasoning, GNOME tracker issues, and the existing @kv9898 window-clone solution
- [Draft comment for #302](90_upstream/comment-pr609.md) — review-and-post-manually coordination comment

### `40_bridge/` — The frame producer
- [40.01 PipeWire frame producer](40_bridge/40.01_producer.md) — the `bridge/` host that embeds core and renders offscreen; build/run/verify, perf, staged plan (A done, B/C pending)

### `50_build_and_run/` — Build, run, requirements
- [50.01 System requirements](50_build_and_run/50.01_system_requirements.md) — target machine, dependencies

### `90_open_questions/` — Things not yet decided
- [90.01 Open questions log](90_open_questions/90.01_open_questions.md)

### `_meta/` — Project-internal conventions
- [Writing style](_meta/writing-style.md) — language, tone, file hygiene
- [Session handoff](_meta/session-handoff.md) — **read on every new session**

## Session logs

Numbered, append-only records of each working session. The plan in the
brief runs four sessions to a minimal viable wallpaper.

- [Session 01 — build & baseline](session-01-build.md)
