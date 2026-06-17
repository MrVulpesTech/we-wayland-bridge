# Session 01 — Build & Baseline

- **Date:** 2026-06-11
- **Goal:** Build the upstream renderer and confirm it renders a Wallpaper
  Engine scene on the target machine ("ground truth").
- **Outcome:** Pass. The renderer builds clean and renders scene
  wallpapers correctly. Verified visually via screenshot capture.

## Machine

| Thing | Value |
|---|---|
| OS | Ubuntu 26.04 LTS |
| GNOME Shell | 50.1, Wayland (Mutter) |
| GPU | Intel CometLake-U UHD Graphics (integrated) |
| Compiler | system `g++` (build-essential) |
| CMake | system `cmake` |
| PipeWire | present |
| Upstream commit (submodule pin) | `b016d7d` |

## Submodules

The upstream submodule vendors nine `External/` dependencies as **nested**
submodules (argparse, Catch2, glslang-WallpaperEngine, nlohmann/json,
kissfft, MimeTypes, quickjs, SPIRV-Cross-WallpaperEngine, stb). They are
empty after a plain `git submodule update --init` and the build fails
without them. Initialize recursively:

```sh
git submodule update --init --recursive
```

`quickjs` registers a further `test262` submodule; it is not needed and
git skips it automatically.

## Dependencies installed

`build-essential`, `cmake`, and `libfftw3-dev` were already present. The
remaining 20 packages from upstream's Ubuntu list installed cleanly on
26.04 with no renames:

```sh
sudo apt-get install -y \
  libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxxf86vm-dev \
  libgl-dev libglew-dev freeglut3-dev libglm-dev libglfw3-dev libsdl2-dev \
  liblz4-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  libmpv-dev mpv libpulse-dev libfreetype-dev
```

Note: `mpv` pulled in `libpipewire-0.3-dev` and `libspa-0.2-dev` as
transitive dependencies, so the PipeWire development headers needed for
later sessions (ADR-0003/0004) are already on the machine.

## Build

```sh
cd upstream
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j"$(nproc)"
```

- Configure took ~85 s, most of it spent **downloading CEF**
  (Chromium Embedded Framework, version 135.x) — CEF is enabled by
  default and is what web-type wallpapers use.
- Build completed to 100 % with no errors on the pinned commit, despite
  the machine carrying a newer FFmpeg (8.0.1) than upstream's reference.
- Binary: `upstream/build/output/linux-wallpaperengine` (a small
  executable; the bulk of the code is in
  `output/liblinux-wallpaperengine-lib.so` beside it).

## Output drivers seen during the build

Filenames observed while compiling, useful context for Session 2
(`30_rendering/30.01_output_backends.md`):

```text
Render/Drivers/WaylandOpenGLDriver.cpp
Render/Drivers/Output/WaylandOutput.cpp          (wlr-layer-shell path)
Render/Drivers/Output/WaylandOutputViewport.cpp
Render/Drivers/Output/X11Output.cpp
Render/Drivers/Detectors/WaylandFullScreenDetector.cpp
Render/Drivers/Detectors/X11FullScreenDetector.cpp
wayland/wlr-layer-shell-unstable-v1-protocol.c
wayland/xdg-output-unstable-v1-protocol.c
```

This confirms the output-driver split the project depends on: there is a
`Drivers/Output/` family, and the Wayland path is built on
`wlr-layer-shell` — exactly the protocol Mutter does not implement, which
is why a new sibling output target is needed (ADR-0003).

## Running a wallpaper

The positional form previews a wallpaper in a window; the renderer
auto-detected the Steam asset path, and `--assets-dir` overrides it
explicitly. To verify rendering without needing to watch a live window,
`--screenshot` captures a frame to PNG.

Exact working command (scene, verified):

```sh
cd upstream/build/output
./linux-wallpaperengine \
  --assets-dir ~/projects/we-wayland-bridge/steam-assets \
  --silent --fps 30 \
  --screenshot /tmp/shot.png --screenshot-delay 30 \
  2804205787
```

To watch it live in a window instead, drop the `--screenshot*` flags:

```sh
./linux-wallpaperengine --assets-dir <steam-assets> --silent 2804205787
```

## Wallpapers tested

Limited to three the operator selected.

| ID | Title | Type | Result |
|---|---|---|---|
| 2804205787 | Astronaut with flare | scene | Pass — animated shader scene (drifting red flare smoke over snow) rendered correctly |
| 3622495963 | Frieren (MPGB202525) | scene | Pass — 3D puppet-mesh scene with lighting shaders; log showed `Loaded puppet mesh models/5_puppet.mdl` and `Resolving require module: LightingV1` |
| 1186010785 | Arknights | web | Fail in this environment — see below |

Renders were inspected visually during the session. The screenshots are
derived from the operator's purchased Wallpaper Engine content and are
**not committed** to the repository (writing-style §5, non-goals P8).

### Web wallpaper (CEF) did not run

`1186010785` is a web-type wallpaper (`index.html`, CEF). It did not
produce a frame within 75 s and was killed. The log stalls immediately
after an AppArmor-blocked DBus call:

```text
DBus error: An AppArmor policy prevents this sender from sending this
message ... label="snap.code.code (complain)" ...
destination="org.mpris.MediaPlayer2.brave..." (AccessDenied)
```

The session ran from a terminal inside the **VSCode snap**
(`snap.code.code`), which is AppArmor-confined. CEF spawns its own
sandboxed helper process (`chrome-sandbox`), and that nested sandbox does
not initialize under snap confinement. The DBus error itself is only a
non-fatal MPRIS audio query; the hang is the CEF subprocess.

This is **not a blocker**: web wallpapers are an explicit non-goal for the
MVP (`10.03_non_goals.md`, Q-3). Whether CEF runs at all on this machine
should be retested from a plain (non-snap) terminal before drawing any
conclusion about web support. Logged as a data point, not a defect.

## Notes for next sessions

- `--fps` defaults to 30 and is the battery/perf knob; `--no-audio-processing`
  and `--silent` are useful when testing without sound.
- Screenshot mode renders the frame but the process keeps running (it does
  not exit on its own); wrap test invocations in `timeout`.
- The renderer found Steam assets automatically; `--assets-dir` is still
  worth passing explicitly in scripts so behaviour does not depend on
  autodetection.
- Session 2 starts from the `Drivers/Output/` family listed above:
  document the backend interface and where the GL context is created, to
  scope the headless→PipeWire sibling (ADR-0003).

## State updated

- `CLAUDE.md` — Session 1 checkbox.
- `_meta/session-handoff.md` — "Where we left off".
