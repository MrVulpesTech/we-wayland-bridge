# Session handoff

> Read this **after** `docs/MEMORY.md` and **before** starting work.
> Captures operational patterns, command recipes, decisions made in chat,
> and gotchas that did not warrant a full ADR but would otherwise force
> the next session to relearn them.
>
> Update the relevant section when a convention solidifies, and the
> "Where we left off" section at the end of every session. Append; do not
> rewrite history.

## 1. Operator and machine

- Git author: `Mr Vulpes <mr.vulpes.tech@gmail.com>`
- Repo path: `~/projects/we-wayland-bridge`
- OS: Ubuntu 26.04 LTS, GNOME Shell 50.1, Wayland (X11 session removed)
- GPU: Intel CometLake-U UHD Graphics (integrated)
- The renderer reads the operator's own Steam wallpapers via two
  gitignored symlinks (`steam-workshop`, `steam-assets`). Read-only.

## 2. The session plan

Four sessions to a minimal viable wallpaper (scene type, one monitor, no
web, no audio-reactivity). Each ends by updating `docs/` and the "State"
section of `CLAUDE.md`.

| Session | Goal | Deliverable |
|---|---|---|
| 1 | Build upstream, run a wallpaper in `--window` | `session-01-build.md` |
| 2 | Survey output backends, scope headless→PipeWire | `30_rendering/30.01_output_backends.md` |
| 3 | GNOME extension prototype: a Clutter actor with any video stream in the background layer | extension prototype + session log |
| 4 | Bridge: renderer → PipeWire → extension; validate zero-copy | session log; confirm ADR-0004 |

## 3. Conventions

- `upstream/` is a submodule. Never edit it in place. After a fresh
  clone: `git submodule update --init`.
- Renderer changes go on the `pipewire-backend` fork branch of
  `linux-wallpaperengine`, mirrored into `bridge/` as patches (ADR-0001).
- Extension testing happens **only** in a nested shell, never on the live
  desktop:

  ```sh
  dbus-run-session -- gnome-shell --nested --wayland
  ```

- Every performance claim needs a session log behind it before it ships
  in a doc (writing-style §6). The zero-copy claim (Q-1) is unproven.

## 4. Commands that get reused

```sh
# Initialize the submodule on a fresh clone
git submodule update --init

# Pick a wallpaper to test: list subscribed wallpapers and their types
for d in steam-workshop/*/; do
  printf '%s\t' "$(basename "$d")"
  grep -o '"type"[^,]*' "$d/project.json" 2>/dev/null | head -1
done

# Run a wallpaper in a window (filled in with exact flags in Session 1)
# upstream/build/linux-wallpaperengine --window <geometry> \
#   --assets-dir steam-assets <workshop_id>
```

## 5. Decisions made in chat (not in ADRs)

| Decision | Detail | Status |
|---|---|---|
| Docs language | English only, including for an upstream PR | Settled (writing-style §1) |
| Working project name | `we-wayland-bridge` | Provisional (Q-5) |
| Security posture | No threat model / double-blind protocol / CLA — this is a desktop wallpaper bridge, not a privileged server | Settled |
| Doc structure | Johnny.Decimal under `docs/`, adapted from a prior project's doc set | Settled |

## 6. Gotchas

- GNOME extension APIs break across GNOME releases (principle P6). Pin
  assumptions to GNOME 50.1 and keep the extension surface small.
- "Works on integrated Intel" does not prove the dma-buf path on NVIDIA
  or multi-GPU. Record what was actually tested.
- Do not try to set a wallpaper on the live GNOME background during
  Sessions 1–2: there is no path yet, and `--window` is sufficient for
  build verification.

## 7. Where we left off

> Update this at the end of every session.

- **As of 2026-06-11, start of Session 1:** documentation scaffold
  created (vision, architecture, ADR-0001..0004, writing-style, this
  file, open questions). Project decisions locked in: English docs, name
  `we-wayland-bridge`, no security/double-blind apparatus, fully
  open-source / public GitHub as the goal.

- **As of 2026-06-11, end of Session 1:** renderer builds clean on the
  reference machine and renders scene wallpapers correctly (verified
  visually on `2804205787` and `3622495963`). Full write-up in
  `session-01-build.md`. Key gotchas captured there: nested submodules
  need `--init --recursive`; CEF downloads at configure time; web/CEF
  wallpapers hang under the snap-confined VSCode terminal (non-MVP).
  **Next: Session 2** — read `upstream/src/WallpaperEngine/Render/Drivers/Output/`
  and document the backend interface in `30_rendering/30.01_output_backends.md`,
  then check upstream issues/PRs for an existing headless/PipeWire effort
  (Q-2).
