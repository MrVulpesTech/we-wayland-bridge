# ADR-0001: Repository strategy

- **Status:** Accepted
- **Date:** 2026-06-11
- **Deciders:** project author

## Context

The project has three kinds of code with different relationships to the
upstream `linux-wallpaperengine` renderer:

1. The renderer itself — we depend on it, do not own it, and want to
   track its `main`.
2. Renderer-side changes (a new output backend) — derived from upstream,
   ideally merged back upstream.
3. Original work that only *consumes* the renderer over IPC (the GNOME
   extension) — not derived from upstream at all.

Putting all of this in a fork of `linux-wallpaperengine` would entangle
the extension and docs with upstream's history and licence, make rebasing
painful, and blur the question of what is ours versus theirs. Principles
P1 (do not rebuild the renderer) and P2 (upstream-friendly) point at
keeping the renderer at arm's length.

## Decision

Use a **separate public repository** (`we-wayland-bridge`) that is neither
a fork nor a branch of upstream. Inside it:

- `upstream/` is a **git submodule** pinned to a specific
  `linux-wallpaperengine` commit. Read-only; never edited in place.
- Renderer changes live in `bridge/` as patches and, when they grow
  beyond a patch, on a **dedicated fork branch** of
  `linux-wallpaperengine` (working name `pipewire-backend`), developed
  with a pull request into upstream as the goal.
- The GNOME extension and all docs are first-class citizens of this
  repository, independent of upstream's history.

## Alternatives considered

- **Fork `linux-wallpaperengine` and add everything to it.** Rejected:
  couples the MIT extension and project docs to a GPLv3 history, makes
  the licence boundary (ADR-0002) harder to see, and forces every
  upstream sync through a merge of unrelated files. The extension does
  not belong in the renderer's tree.
- **Vendor a copy of the renderer source (no submodule).** Rejected:
  loses the clean pin to an upstream commit, invites local edits that
  drift from upstream, and makes a future PR harder to extract.
- **Keep renderer changes only as out-of-tree patches forever.**
  Rejected as the primary plan: fine for a small backend, but a real
  output backend is enough code that a tracked fork branch is easier to
  develop and review than a growing patch stack. Patches remain the
  exchange format into `bridge/`; the fork branch is where the work
  happens.

## Consequences

- **Positive:** Clear ownership — the renderer is a dependency, the
  bridge and extension are the product. Submodule pin makes the exact
  renderer version reproducible. The fork branch keeps renderer changes
  rebaseable and PR-ready.
- **Negative:** Contributors must `git submodule update --init` and
  understand two repositories. Renderer changes are developed in the fork
  and mirrored into `bridge/`, which is an extra step.
- **Risks:** Submodule pin drifts behind upstream and the fork branch
  becomes hard to rebase if neglected. Mitigation: bump the submodule and
  rebase the fork on a schedule, not in a big-bang.

## Revisit triggers

- Upstream merges the output backend: `bridge/` then becomes a thin
  config/packaging layer and the fork branch is retired.
- Upstream declines the PR permanently: the fork branch becomes a
  long-lived maintained fork, and this ADR is superseded with the new
  maintenance policy.
