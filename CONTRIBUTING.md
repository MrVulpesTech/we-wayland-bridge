# Contributing

This project is a proof-of-concept bridge between `linux-wallpaperengine` and GNOME Shell under Wayland.

## Build Environment Setup

The producer (`bridge/`) requires a specific pull request from the upstream renderer checked out into a git worktree. To set up the build environment:

```sh
# Clone the repository
git clone https://github.com/MrVulpesTech/we-wayland-bridge
cd we-wayland-bridge
git submodule update --init

# Create the required worktree from the pinned next-v2 PR
cd upstream
git fetch origin pull/609/head:next-v2-review
git worktree add ../next-v2-review next-v2-review
cd ..

# Build the core library and the PipeWire host
./bridge/build.sh
```

## Testing Changes

Never test GNOME Shell extension changes directly on your primary session without safeguards, as a crash or a blocking main thread can cause a login loop. Always test in a nested session first:

```sh
dbus-run-session -- gnome-shell --devkit --wayland --virtual-monitor 1920x1080 &
journalctl --user -f -o cat | grep -i wwb
```

## Documentation Structure

The `docs/` folder uses the Johnny.Decimal layout to organize project knowledge:
- **ADRs (`docs/_adr/`)** are the architectural decision logs.
- **Session logs** are the development history, maintained specifically for AI agent continuity and context.

## Reporting Issues

When reporting an issue, please include the following diagnostic information:
- Your exact GNOME Shell version (`gnome-shell --version`).
- Your GPU model and driver stack.
- The output of the extension's journal logs: `journalctl --user -f -o cat | grep -i wwb`
