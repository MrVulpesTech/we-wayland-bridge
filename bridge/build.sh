#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 we-wayland-bridge contributors
#
# One-command setup for the Stage A frame producer. Builds
# linux-wallpaperengine-core from the pinned next-v2 worktree, then the host.
# Reproducible: safe to run after wiping bridge/build/. Assumes the worktree
# at next-v2-review/ already exists (see docs/40_bridge/40.01_producer.md for
# how to create it).
set -euo pipefail

# Pinned upstream commit. The producer is built against exactly this; do not
# chase upstream mid-development.
PIN="828485aa0804e6889cff895e139e293bf6a3fb28"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # bridge/
root="$(cd "$here/.." && pwd)"                          # repo root
worktree="$root/next-v2-review"

[ -d "$worktree" ] || { echo "error: worktree missing at $worktree"; exit 1; }
have="$(git -C "$worktree" rev-parse HEAD)"
if [ "$have" != "$PIN" ]; then
  echo "warning: worktree at $have, expected pin $PIN" >&2
fi

echo "==> Populating worktree submodules (idempotent)"
git -C "$worktree" submodule update --init --recursive

echo "==> Configuring core (webhelper/frontend/dev-viewer/tests OFF)"
cmake -S "$worktree" -B "$here/build/core" \
  -DCMAKE_BUILD_TYPE=Release \
  -DWPBUILD_CORE=ON -DWPBUILD_WEBHELPER=OFF -DWPBUILD_FRONTEND=OFF \
  -DWPBUILD_DEVVIEWER=OFF -DWPBUILD_TESTS=OFF

echo "==> Building core"
cmake --build "$here/build/core" --target linux-wallpaperengine-core -j"$(nproc)"

echo "==> Configuring + building host"
cmake -S "$here" -B "$here/build/host" -DCMAKE_BUILD_TYPE=Release
cmake --build "$here/build/host" -j"$(nproc)"

echo
echo "Done. Host binary: $here/build/host/wpe-host"
echo "Example:"
echo "  $here/build/host/wpe-host \\"
echo "    --assets-dir $root/steam-assets --out $here/out \\"
echo "    $root/steam-workshop/2804205787"
