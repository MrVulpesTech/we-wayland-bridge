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

import * as Background from 'resource:///org/gnome/shell/ui/background.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';

import {LiveWallpaper, probeShellApi} from './liveWallpaper.js';

// PipeWire node to consume. Empty => let pipewiresrc auto-connect to a
// compatible Video/Source (fine when the producer is the only one running).
// Set to the producer's node id (it prints one) to be explicit.
const PIPEWIRE_PATH = '';

export default class WeWaylandBridgeExtension extends Extension {
    enable() {
        // One-time log of the shell-side API we have to work with, so the
        // first run on a real session tells us which frame-upload path is
        // available (we cannot probe this outside gnome-shell).
        probeShellApi();

        this._injectionManager = new InjectionManager();
        this._wallpapers = new Set();

        // Each monitor's BackgroundManager builds a background actor; we hang
        // a LiveWallpaper off each one.
        this._injectionManager.overrideMethod(
            Background.BackgroundManager.prototype,
            '_createBackgroundActor',
            originalMethod => {
                const ext = this;
                return function () {
                    const backgroundActor = originalMethod.call(this);
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

        this._reloadBackgrounds();
        log('wwb: enabled');
    }

    disable() {
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
