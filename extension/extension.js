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
// hide.
//
// One FrameSource owns the single PipeWire pipeline + texture; every background
// actor we create is a thin LiveWallpaper that just paints it. _createBackground-
// Actor fires often (overview, workspace, monitor changes), so per-actor
// pipelines must never exist — see docs/40_bridge/40.04.

import GLib from 'gi://GLib';

import * as Background from 'resource:///org/gnome/shell/ui/background.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';

import {FrameSource, LiveWallpaper, probeShellApi} from './liveWallpaper.js';

// PipeWire node id to consume. A bare pipewiresrc with no target is NOT
// auto-linked to a Video/Source by WirePlumber (camera-like policy), so the
// FrameSource discovers the producer by node.name. WWB_PIPEWIRE_PATH overrides
// with an explicit id for debugging. Empty => discover by name.
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
        // the shell main thread; this wrapper, plus deferring all GStreamer off
        // the startup path, keep the shell safe.
        try {
            this._enableInner();
        } catch (e) {
            logError(e, 'wwb: enable() failed — tearing down to a clean no-op');
            // _enableInner may have already connected signals, added timers, or
            // created the FrameSource before throwing. disable() is guarded
            // against undefined fields, so it cleans up whatever exists.
            try { this.disable(); } catch (_e) {}
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
        // The single shared pipeline + texture for the whole extension.
        this._source = new FrameSource(PIPEWIRE_PATH);

        // Each monitor's BackgroundManager builds a background actor; we hang a
        // (cheap, pipeline-less) LiveWallpaper off each one, all sharing _source.
        this._injectionManager.overrideMethod(
            Background.BackgroundManager.prototype,
            '_createBackgroundActor',
            originalMethod => {
                const ext = this;
                return function () {
                    const backgroundActor = originalMethod.call(this);
                    log(`wwb: _createBackgroundActor fired (monitor ${backgroundActor.monitor})`);
                    try {
                        const lw = new LiveWallpaper(backgroundActor, ext._source);
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
        // appearing after enable).
        this._monitorsId = Main.layoutManager.connect('monitors-changed', () => {
            log(`wwb: monitors-changed (${Main.layoutManager.monitors.length} monitors)`);
            this._reloadBackgrounds();
        });

        this._reloadBackgrounds();

        // Defer the FrameSource start (Gst.init, device discovery, pipeline) off
        // the enable path. Running it synchronously here blocked the shell main
        // thread at login and caused a grey screen (40.04). Low priority + a
        // small delay let the shell finish drawing first; if start is slow or a
        // producer is absent, the shell is already up and the source just idles.
        this._startId = GLib.timeout_add(GLib.PRIORITY_LOW, 1500, () => {
            this._startId = 0;
            try {
                this._source?.start();
            } catch (e) {
                logError(e, 'wwb: FrameSource.start threw — idling');
            }
            return GLib.SOURCE_REMOVE;
        });

        log(`wwb: enabled (${Main.layoutManager.monitors.length} monitors, ` +
            `${this._wallpapers.size} wallpaper actors attached)`);
    }

    disable() {
        if (this._startId) {
            GLib.source_remove(this._startId);
            this._startId = 0;
        }
        if (this._monitorsId) {
            Main.layoutManager.disconnect(this._monitorsId);
            this._monitorsId = 0;
        }

        this._injectionManager?.clear();
        this._injectionManager = null;

        this._wallpapers?.forEach(lw => lw.destroy());
        this._wallpapers = null;

        // Stop the shared pipeline AFTER the actors are gone (clean pipewiresrc
        // teardown — 40.04).
        this._source?.stop();
        this._source = null;

        this._reloadBackgrounds();
        log('wwb: disabled');
    }

    // Rebuild background actors so our override runs for the currently visible
    // backgrounds (on enable) and they revert cleanly (on disable).
    _reloadBackgrounds() {
        try {
            Main.layoutManager._updateBackgrounds();
        } catch (e) {
            logError(e, 'wwb: _updateBackgrounds failed');
        }
    }
}
