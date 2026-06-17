#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 we-wayland-bridge contributors
#
# Stage B live demo: start the PipeWire producer and open a viewer window
# fed only by its stream. Handles the dynamic node id for you (it changes
# every run, so a hard-coded `path=` from a previous run will not work).
#
# Usage: bridge/demo.sh <wallpaper-folder> [WxH] [fps]
#   e.g. bridge/demo.sh steam-workshop/<workshop-id> 1920x1080 30
#   defaults: 1920x1080, 30 fps. The wallpaper folder is required.
# Render the producer at 1080p+ — some scenes wash out at low resolution
# (Q-12 / 40.01). Ctrl-C stops both the viewer and the producer.
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: bridge/demo.sh <wallpaper-folder> [WxH] [fps]" >&2
    echo "  e.g. bridge/demo.sh steam-workshop/<workshop-id> 1920x1080 30" >&2
    exit 1
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
WP="$1"
SIZE="${2:-1920x1080}"
FPS="${3:-30}"
LOG="$(mktemp /tmp/wpe-demo.XXXXXX.log)"

"$here/build/host/wpe-host" --pipewire --assets-dir "$root/steam-assets" \
    --size "$SIZE" --fps "$FPS" "$WP" >"$LOG" 2>&1 &
HPID=$!
trap 'kill -INT "$HPID" 2>/dev/null; wait "$HPID" 2>/dev/null; rm -f "$LOG"' EXIT INT TERM

for _ in $(seq 1 40); do
    sleep 0.25
    grep -q "node id" "$LOG" 2>/dev/null && break
    kill -0 "$HPID" 2>/dev/null || { echo "producer exited early:"; cat "$LOG"; exit 1; }
done

NODE="$(grep -oE 'node id: [0-9]+' "$LOG" | grep -oE '[0-9]+' | head -1)"
[ -n "$NODE" ] || { echo "could not get node id; log:"; cat "$LOG"; exit 1; }

echo "producer running: node=$NODE, ${SIZE}@${FPS}fps. Ctrl-C to stop."
gst-launch-1.0 pipewiresrc path="$NODE" ! videoconvert ! autovideosink
