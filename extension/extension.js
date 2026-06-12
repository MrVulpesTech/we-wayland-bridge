// SPDX-License-Identifier: MIT
// Copyright (C) 2026 we-wayland-bridge contributors
//
// Consumer side of we-wayland-bridge. This runs inside gnome-shell and is a
// SEPARATE process from the GPLv3 renderer/producer; it links no renderer
// code and only consumes a PipeWire video stream (ADR-0002). MIT.
//
// Background-layer injection follows the pattern proven by gnome-ext-hanabi
// and @kv9898's fork: override BackgroundManager._createBackgroundActor and
// add our own actor as a child of each per-monitor background actor. Unlike
// those projects we do NOT clone a renderer window — the producer is a
// separate process exposing a PipeWire stream, so there is no shell window to
// hide. That removes the ~10 overview/Alt-Tab/app overrides they need.

import GLib from 'gi://GLib';

import * as Background from 'resource:///org/gnome/shell/ui/background.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';

import {LiveWallpaper, probeShellApi} from './liveWallpaper.js';

// PipeWire node id to consume. A bare pipewiresrc with no target is NOT
// auto-linked to a Video/Source by WirePlumber (camera-like policy), so we
// must name the producer's node. For testing it is read from the
// WWB_PIPEWIRE_PATH env var (the producer prints its node id); a future
// version discovers the producer by node.name. Empty => auto (rarely works).
const PIPEWIRE_PATH = GLib.getenv('WWB_PIPEWIRE_PATH') || '';

// Kill-switch. If this file exists, the extension no-ops at enable. Lets the
// operator stop a misbehaving extension from a TTY (`touch ~/.config/wwb/disabled`)
// without removing it or reaching a (possibly grey) GNOME session.
function killSwitchEngaged() {
    try {
        // WWB_FORCE=1 bypasses the kill-switch (for nested verification while
        // the live session keeps the kill-switch file as a safety net).
        if (GLib.getenv('WWB_FORCE'))
            return false;
        const path = GLib.build_filenamev([GLib.get_user_config_dir(), 'wwb', 'disabled']);
        return GLib.file_test(path, GLib.FileTest.EXISTS);
    } catch (e) {
        return false;
    }
}

export default class WeWaylandBridgeExtension extends Extension {
    enable() {
        // Hard guard: nothing in enable() may throw or block. A grey-screen
        // login (docs/40_bridge/40.04) was caused by enable-path work blocking
        // the shell main thread; this wrapper, plus the LiveWallpaper deferring
        // all GStreamer off the startup path, keep the shell safe.
        try {
            this._enableInner();
        } catch (e) {
            logError(e, 'wwb: enable() failed — extension is a no-op this session');
            try { this._injectionManager?.clear(); } catch (_e) {}
            this._injectionManager = null;
        }
    }

    _enableInner() {
        if (killSwitchEngaged()) {
            log('wwb: kill-switch (~/.config/wwb/disabled) present — not enabling');
            return;
        }

        probeShellApi();

        this._injectionManager = new InjectionManager();
        this._wallpapers = new Set();

        // Each monitor's BackgroundManager builds a background actor; we hang a
        // LiveWallpaper off each one.
        this._injectionManager.overrideMethod(
            Background.BackgroundManager.prototype,
            '_createBackgroundActor',
            originalMethod => {
                const ext = this;
                return function () {
                    const backgroundActor = originalMethod.call(this);
                    log(`wwb: _createBackgroundActor fired (monitor ${backgroundActor.monitor})`);
                    try {
                        const lw = new LiveWallpaper(backgroundActor, PIPEWIRE_PATH);
                        ext._wallpapers.add(lw);
                        lw.connect('destroy', () => ext._wallpapers.delete(lw));
                    } catch (e) {
                        logError(e, 'wwb: failed to create LiveWallpaper');
                    }
                    return backgroundActor;
                };
            }
        );

        // Re-inject when the set of monitors changes (hotplug, or a monitor
        // appearing after enable — e.g. a Mutter Devkit session that starts
        // with none, or a display connected at runtime).
        this._monitorsId = Main.layoutManager.connect('monitors-changed', () => {
            log(`wwb: monitors-changed (${Main.layoutManager.monitors.length} monitors)`);
            this._reloadBackgrounds();
        });

        this._reloadBackgrounds();
        log(`wwb: enabled (${Main.layoutManager.monitors.length} monitors, ` +
            `${this._wallpapers.size} wallpaper actors attached)`);
    }

    disable() {
        if (this._monitorsId) {
            Main.layoutManager.disconnect(this._monitorsId);
            this._monitorsId = 0;
        }

        this._injectionManager?.clear();
        this._injectionManager = null;

        this._wallpapers?.forEach(lw => lw.destroy());
        this._wallpapers = null;

        this._reloadBackgrounds();
        log('wwb: disabled');
    }

    // Rebuild background actors so our override runs for the currently
    // visible backgrounds (on enable) and they revert cleanly (on disable).
    _reloadBackgrounds() {
        try {
            Main.layoutManager._updateBackgrounds();
        } catch (e) {
            logError(e, 'wwb: _updateBackgrounds failed');
        }
    }
}
