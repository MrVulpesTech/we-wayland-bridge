# Pre-publication code audit

Read-only audit of the runtime code before GitHub publication. Scope:
`bridge/host/main.cpp`, `bridge/build.sh`, `bridge/demo.sh`,
`extension/extension.js`, `extension/liveWallpaper.js`, `extension/metadata.json`.
No source files were modified.

Severity: **MUST FIX** (publication blocker) · **SHOULD FIX** (before or soon
after publish) · **NOTE** (acceptable, recorded).

---

## Section A — Hardcoded values that should be configurable

- [ ] **SHOULD FIX** — `main.cpp:279` — `open("/dev/dri/renderD128", …)` is the
  only render node tried. Recommend enumerating `/dev/dri/renderD*` (or a
  `--drm-node` flag) and falling back. Affects the `--dmabuf` path only (not the
  shipped SHM default), so impact is limited today.
- [ ] **NOTE** — `main.cpp:83` — `Options.assets = "../../steam-assets"` is a
  CWD-relative default. Harmless (overridable via `--assets-dir`, and a clear
  `die()` fires if it is wrong), but fragile. Recommend documenting it or
  requiring `--assets-dir`.
- [ ] **SHOULD FIX** — `demo.sh:17` — `WP="${1:-$root/steam-workshop/2804205787}"`
  hardcodes a specific Workshop scene id as the default. It assumes the user owns
  that exact item. Recommend requiring the argument (error if absent) or using a
  clearly-labelled placeholder.
- [ ] **NOTE** — `build.sh:47` — the example `echo` references
  `steam-workshop/2804205787`. It is printed help text (documentation), so it is
  acceptable; a `<workshop-id>` placeholder would read better.
- [ ] **NOTE** — `main.cpp:84-87` — default `1920×1080 / 60 fps / 120 frames`
  live in a named `Options` struct behind `--size/--width/--height/--fps/--frames`.
  Fine.
- [ ] **NOTE** — `main.cpp:604` / `liveWallpaper.js:57` — the PipeWire node name
  `"wpe-host"` is hardcoded in both the producer (advertise) and consumer
  (discover-by-name). This is the intended cross-process contract key, not a
  value to discover; the node *id* is discovered at runtime. Keep the two copies
  in sync (the consumer comment already points at `main.cpp`).
- [x] **PASS** — A.5: no `/home/<user>`, username, hostname, or developer-machine
  paths anywhere in the runtime code. (`build.sh` derives all paths from
  `$BASH_SOURCE`.)

## Section B — Inappropriate fallbacks and silent failures

- [ ] **SHOULD FIX** — `extension.js:56-62` — `enable()` catches any
  `_enableInner()` throw but only clears `_injectionManager`. If the throw lands
  *after* `monitors-changed` is connected or the deferred-start / stress timers
  are added, `_monitorsId` / `_startId` / `_stressId` / `_source` leak (partial
  init). Recommend the catch run a full cleanup (a shared teardown helper or a
  guarded `disable()`), not just the injection manager. Low probability (those
  calls rarely throw), real gap.
- [ ] **NOTE** — `liveWallpaper.js:307-308` — the consumer logs
  `set_state(PLAYING) => <ret>` but does not act on a `FAILURE` return; it relies
  on the bus ERROR watch to trigger reconnect. Acceptable (the bus watch covers
  it), but an explicit `if (ret === Gst.StateChangeReturn.FAILURE) reconnect`
  would be clearer.
- [ ] **NOTE** — silent `catch {}` cluster, all intentional best-effort and
  individually harmless: `liveWallpaper.js:147` (per-device `node.name` read),
  `:195` (late-subscriber redraw), `:398-400` (self-heal drop of a disposed
  subscriber), `:448` (teardown bus cleanup); `extension.js:45` (kill-switch
  stat), `:60` (injection clear in the error path). None hide a critical error in
  a broken state; the surrounding paths log. Leave as-is.
- [x] **PASS** — B.7: missing resources fail visibly — no producer →
  `findProducerDevice` returns null and the source logs + schedules reconnect
  (`liveWallpaper.js:264-268`); no CoglContext → logged `NO CoglContext found`
  (`:126`) and the upload early-returns; the producer `die()`s with a clear
  message on every fatal init step.

## Section C — Unsafe patterns for a public release

- [x] **PASS** — C.10: no real `TODO`/`FIXME`/`HACK`/`XXX` markers. The only grep
  hit is `demo.sh:20` `mktemp /tmp/wpe-demo.XXXXXX.log` (a mktemp template, not a
  marker).
- [ ] **SHOULD FIX** — `extension.js:124-137` — the `WWB_STRESS` hook (toggles the
  overview every 2 s) is debug-only test scaffolding. It is inert unless the env
  var is set, but it should be removed (or moved behind a clearly non-production
  build) before a public release.
- [ ] **SHOULD FIX** — `main.cpp:449-460` — the producer prints
  `producing %.1f fps …` to stderr **once per second, forever**. Useful in a
  terminal, but log spam when run as a systemd service. Gate behind `--verbose`
  or drop it.
- [ ] **SHOULD FIX** — `main.cpp:653` — `std::system("mkdir -p '" + opt.out + "'")`
  builds a shell command from `--out`; a single quote in the path breaks out
  (command-injection hygiene). Dump-mode + local CLI only, so low risk. Replace
  with `mkdir()`/`std::filesystem::create_directories`.
- [ ] **NOTE** — `liveWallpaper.js` one-shot diagnostic logs (`firstpx RGBA=…`
  `:405-407`, `paint_node called` `:518-525`, `4s status` `:321`, API probe,
  CoglContext path). All one-shot (flag-guarded), not per-frame — helpful for
  first-run bug reports, but verbose. Consider trimming the pixel-level debug
  lines (`firstpx`, `paint_node called`) for v1.
- [x] **PASS** — C.11 (other): `--dmabuf` defaults **off** (`main.cpp:91`, pool
  only when explicitly requested — `:710`); frame dumps (`run_dump`) run only in
  the non-`--pipewire` verification mode. No debug path is on by default.
- [x] **PASS** — C.12: no commented-out code blocks. All `//` content is
  explanatory prose (e.g. `liveWallpaper.js:479-481` explains the *removed* CSS
  background — see the stale-comment item below).

### Stale comments (SHOULD FIX — accuracy for a public repo)

- [ ] **SHOULD FIX** — comments that no longer match the code:
  - `liveWallpaper.js:42-47` and `:310` say "20 fps" / "~15 fps" but
    `FRAME_INTERVAL_MS = 33` (30 fps). Pick the intended value and align all
    three comments.
  - `liveWallpaper.js:29-30` (header) describes "a solid fallback colour … until
    the first frame" — the CSS background was removed (`:479-481`). Delete the
    stale design note.
  - `main.cpp:13-16` (header) says Stage C is "C-1 (current) … no PipeWire
    changes yet" — C-2 (dma-buf PipeWire transport) is done. Update.
  - `main.cpp:498-500` — `build_format_dmabuf` comment says "the dma-buf is
    ABGR8888"; it is `XBGR8888` (`:207`). Fix.

## Section D — Extension metadata (`extension/metadata.json`)

- [ ] **SHOULD FIX** — `:7` — `"url": "https://github.com/we-wayland-bridge/we-wayland-bridge"`
  is the placeholder set during development; the `we-wayland-bridge` org is a
  guess. Point it at the real published repo before release.
- [ ] **NOTE** — `:2` — `"uuid": "we-wayland-bridge@we-wayland-bridge.github.io"`
  works as a namespace, but **finalise it before the first publish** — changing a
  UUID later orphans every existing install and EGO entry.
- [ ] **NOTE** — `:5` — `"shell-version": ["50"]` only. Correct for the Mutter-18
  / GNOME 50.1 target (the APIs are version-pinned), but it will refuse to load
  on 49 or 51; widen deliberately as you test other releases.
- [x] **PASS** — `name`, `description`, `version` are real, descriptive, and
  release-appropriate. No template/dev leftovers besides the URL.

## Section E — License headers

- [ ] **MUST FIX** — **No `LICENSE` files exist** in the repo (root, `bridge/`, or
  `extension/`). The README states they are "added before the repository goes
  public" — this is that moment. SPDX headers reference licenses whose text is
  absent. Add: `bridge/LICENSE` (GPL-3.0), `extension/LICENSE` (MIT), and a root
  `LICENSE`/`COPYING` note explaining the split (ADR-0002).
- [x] **PASS** — SPDX headers are present and correct on every code file:
  `main.cpp` `GPL-3.0-or-later`, `build.sh` `GPL-3.0-or-later`, `demo.sh`
  `GPL-3.0-or-later` (all under `bridge/`, GPLv3 ✓); `extension.js` `MIT`,
  `liveWallpaper.js` `MIT` (under `extension/`, MIT ✓). The boundary matches
  ADR-0002.
- [ ] **NOTE** — `metadata.json` has no SPDX header; JSON has no comment syntax,
  so this is expected/acceptable. (Optional: an SPDX entry could go in a sibling
  doc.)

---

## Summary by severity

| Severity | Count | Items |
|---|---|---|
| **MUST FIX** | **1** | Missing `LICENSE` files (E) |
| **SHOULD FIX** | **8** | render-node hardcode (A); demo.sh default scene id (A); `enable()` partial-init cleanup (B); `WWB_STRESS` test hook (C); per-second fps stderr (C); `std::system` mkdir (C); stale comments ×4 (C); metadata `url` placeholder (D) |
| **NOTE** | **11** | assets default path; node-name contract dup; consumer `set_state` return; silent-catch cluster; verbose diagnostic logs; metadata `uuid` finalise; `shell-version` breadth; metadata no-SPDX |
| **PASS** | **6** | no machine/user paths; failures are visible; no TODO markers; debug paths off by default; no commented-out code; SPDX headers correct |

**Publication blocker:** the single **MUST FIX** is the absent `LICENSE` files.
Everything else is SHOULD-FIX polish (stale comments, the test hook, the
placeholder URL, the partial-init cleanup) or recorded NOTEs. Nothing leaks the
developer's machine, username, or home directory, and no debug path is active in
the default (SHM, no `--dmabuf`) configuration.

---

## Resolution (applied 2026-06-17)

**MUST FIX — done.** `LICENSE` files added: `bridge/LICENSE` (GPL-3.0, canonical
text), `extension/LICENSE` (MIT), root `LICENSE` (explains the per-directory
split, ADR-0002).

**SHOULD FIX — all done:**
- Render node: `main.cpp` now enumerates `/dev/dri/renderD128..135` and uses the
  first that opens (verified: "using DRM render node …").
- `demo.sh`: the wallpaper folder is now required (errors with usage if absent);
  no scene id default. `build.sh`'s example echo uses `<workshop-id>`.
- `enable()`: the catch now runs `this.disable()` (guarded against undefined
  fields) for a full teardown on a mid-init throw.
- `WWB_STRESS` test hook removed from `extension.js`.
- Per-second fps line gated behind `--verbose`.
- `std::system("mkdir…")` → `std::filesystem::create_directories` (verified).
- Stale comments fixed: `main.cpp` header (Stage C status), `build_format_dmabuf`
  (XBGR8888 not ABGR8888); `liveWallpaper.js` fps comments (×3) and the removed
  fallback-colour note. Verbose `firstpx`/`paint_node` debug logs trimmed.
- `metadata.json` `url` → real repo (`MrVulpesTech/we-wayland-bridge`).

Producer rebuilt clean; both JS files `node --check`; both shell scripts
`bash -n`; dump + `--dmabuf` smoke runs pass.

**Decision applied:** the consumer's shipped rate cap is **20 fps**
(`FRAME_INTERVAL_MS = 50`), matching the 40.03 daily-use default and safe on
unknown hardware. 30 fps (33 ms) is a one-line change for capable setups.

**Left as deliberate NOTEs (confirm before first publish):**
- `metadata.json` `uuid` `we-wayland-bridge@we-wayland-bridge.github.io` — valid,
  project-namespaced, and stable; recommend keeping it. Changing it after publish
  orphans installs, so lock it now.
- `shell-version` `["50"]` only — correct for the Mutter-18 target; widen as
  other releases are tested.
