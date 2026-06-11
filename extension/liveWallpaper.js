// SPDX-License-Identifier: MIT
// Copyright (C) 2026 we-wayland-bridge contributors
//
// LiveWallpaper: a Clutter/St actor placed in a monitor's background actor
// that paints frames pulled from a PipeWire video stream (the producer).
//
// Design notes:
//  - GStreamer's streaming thread must NOT touch Clutter/GJS objects. We poll
//    the appsink from a GLib timer on the SHELL MAIN THREAD (try_pull_sample),
//    so every Clutter call happens on the main loop. No cross-thread GJS.
//  - Frame upload uses St.ImageContent.set_bytes. This file logs which
//    shell-side API is actually available (probeShellApi) because it cannot
//    be probed outside gnome-shell; if set_bytes is unavailable on a given
//    release we adapt here.
//  - A solid fallback colour is shown until the first frame, so the
//    background-layer injection is verifiable independently of the video path.

import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Gst from 'gi://Gst';
import GstApp from 'gi://GstApp'; // registers GstAppSink methods (try_pull_sample)
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

let _gstReady = false;
function ensureGst() {
    if (_gstReady)
        return true;
    try {
        Gst.init(null);
        _gstReady = true;
    } catch (e) {
        logError(e, 'wwb: Gst.init failed');
    }
    return _gstReady;
}

// One-time dump of the shell API we depend on. Read this in the journal on the
// first run to confirm the frame-upload path.
export function probeShellApi() {
    const has = x => (x ? 'yes' : 'no');
    log(`wwb: API probe — St.ImageContent=${has(St.ImageContent)} ` +
        `Clutter.Image=${has(Clutter.Image)} ` +
        `Cogl.PixelFormat=${has(Cogl.PixelFormat)} ` +
        `Clutter.ContentGravity=${has(Clutter.ContentGravity)}`);
    if (St.ImageContent) {
        try {
            const ic = new St.ImageContent();
            log(`wwb: St.ImageContent.set_bytes=${typeof ic.set_bytes}`);
        } catch (e) {
            log(`wwb: St.ImageContent construct failed: ${e}`);
        }
    }
}

// St.ImageContent.set_bytes needs a CoglContext. How to reach it varies across
// Clutter/Mutter versions, so try the known paths and log which one works.
function getCoglContext() {
    const candidates = {
        'stage.context.get_backend': () => global.stage.context.get_backend().get_cogl_context(),
        'stage.get_context().get_backend': () => global.stage.get_context().get_backend().get_cogl_context(),
        'backend.get_cogl_context': () => global.backend.get_cogl_context(),
        'Clutter.get_default_backend': () => Clutter.get_default_backend().get_cogl_context(),
    };
    for (const [name, fn] of Object.entries(candidates)) {
        try {
            const ctx = fn();
            if (ctx) {
                log(`wwb: CoglContext via ${name}`);
                return ctx;
            }
        } catch (e) {
            log(`wwb: CoglContext ${name} -> ${e.message}`);
        }
    }
    log('wwb: NO CoglContext found');
    return null;
}

// The producer advertises this PW_KEY_NODE_NAME (see bridge/host/main.cpp).
const PRODUCER_NODE_NAME = 'wpe-host';

// Find the producer's PipeWire node by node.name among Video/Source devices.
// Returns a Gst.Device (build the source from it via create_element) or null.
// A bare pipewiresrc is not auto-linked to a Video/Source, so we must target
// the producer explicitly; matching by name avoids depending on the volatile
// node id.
function findProducerDevice(wantName) {
    const monitor = new Gst.DeviceMonitor();
    monitor.add_filter('Video/Source', null);
    if (!monitor.start()) {
        log('wwb: DeviceMonitor.start failed');
        return null;
    }
    let found = null;
    for (const dev of monitor.get_devices()) {
        let name = null;
        try {
            name = dev.get_properties()?.get_string('node.name');
        } catch (e) {}
        log(`wwb: video source "${dev.get_display_name()}" node.name=${name}`);
        if (name === wantName) {
            found = dev;
            break;
        }
    }
    monitor.stop();
    return found;
}

export const LiveWallpaper = GObject.registerClass({
    GTypeName: 'WwbLiveWallpaper',
}, class LiveWallpaper extends St.Widget {
    _init(backgroundActor, pipewirePath) {
        super._init({
            reactive: false,
            x_expand: true,
            y_expand: true,
        });

        this._backgroundActor = backgroundActor;
        this._path = pipewirePath || '';
        this._monitorIndex = backgroundActor.monitor;
        this._pipeline = null;
        this._appsink = null;
        this._pollId = 0;
        this._reconnectId = 0;
        this._content = null;
        this._loggedUpload = false;

        const monitor = Main.layoutManager.monitors[this._monitorIndex];
        this._w = monitor?.width ?? backgroundActor.width;
        this._h = monitor?.height ?? backgroundActor.height;
        this.set_size(this._w, this._h);

        // Visible until the first frame: proves the injection independently of
        // the video path.
        this.set_style('background-color: #0a0f1e;');

        // Stretch the content to the monitor (MVP). 'fit'/'crop' modes are a
        // follow-up; Clutter.ContentGravity.RESIZE_ASPECT gives letterbox-fit.
        if (Clutter.ContentGravity)
            this.set_content_gravity(Clutter.ContentGravity.RESIZE_FILL);

        backgroundActor.add_child(this);
        log(`wwb: LiveWallpaper on monitor ${this._monitorIndex} (${this._w}x${this._h})`);

        this._start();
    }

    _start() {
        if (!ensureGst())
            return;

        // Override (debugging): explicit node id via WWB_PIPEWIRE_PATH.
        // Otherwise discover the producer by node.name and build from its
        // device — so the extension finds it without a hardcoded/volatile id.
        if (this._path) {
            const desc =
                `pipewiresrc path=${this._path} ! videoconvert ! ` +
                `video/x-raw,format=RGBA ! ` +
                `appsink name=sink emit-signals=false max-buffers=1 drop=true sync=false`;
            try {
                this._pipeline = Gst.parse_launch(desc);
            } catch (e) {
                logError(e, 'wwb: pipeline parse failed');
                this._scheduleReconnect();
                return;
            }
            this._appsink = this._pipeline.get_by_name('sink');
            this._srcDesc = `path=${this._path}`;
        } else {
            const device = findProducerDevice(PRODUCER_NODE_NAME);
            if (!device) {
                log(`wwb: producer "${PRODUCER_NODE_NAME}" not found yet; will retry`);
                this._scheduleReconnect();
                return;
            }
            const src = device.create_element(null); // pipewiresrc targeted at the node
            const conv = Gst.ElementFactory.make('videoconvert', null);
            const capsf = Gst.ElementFactory.make('capsfilter', null);
            capsf.set_property('caps', Gst.Caps.from_string('video/x-raw,format=RGBA'));
            const sink = Gst.ElementFactory.make('appsink', 'sink');
            sink.set_property('emit-signals', false);
            sink.set_property('max-buffers', 1);
            sink.set_property('drop', true);
            sink.set_property('sync', false);

            this._pipeline = Gst.Pipeline.new('wwb-pipeline');
            for (const el of [src, conv, capsf, sink])
                this._pipeline.add(el);
            if (!src.link(conv) || !conv.link(capsf) || !capsf.link(sink)) {
                log('wwb: failed to link pipeline elements');
                this._scheduleReconnect();
                return;
            }
            this._appsink = sink;
            this._srcDesc = `node.name=${PRODUCER_NODE_NAME}`;
        }

        // Watch for errors / end-of-stream (producer stopped) and reconnect.
        const bus = this._pipeline.get_bus();
        bus.add_signal_watch();
        this._busId = bus.connect('message', (_bus, msg) => {
            const t = msg.type;
            if (t === Gst.MessageType.ERROR) {
                const [err, dbg] = msg.parse_error();
                log(`wwb: pipeline error: ${err.message} (${dbg})`);
                this._scheduleReconnect();
            } else if (t === Gst.MessageType.EOS) {
                log('wwb: producer ended (EOS), will reconnect');
                this._scheduleReconnect();
            }
        });

        const ret = this._pipeline.set_state(Gst.State.PLAYING);
        log(`wwb: pipeline (${this._srcDesc}) set_state(PLAYING) => ${ret}`);

        // Poll the sink on the main thread at ~30 Hz. Clutter is only touched
        // here, never on GStreamer's streaming thread.
        this._pollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 33, () => {
            this._pullFrame();
            return GLib.SOURCE_CONTINUE;
        });

        // One-shot diagnostic: 4 s in, report whether any sample arrived and
        // the pipeline's actual state, so a stalled video path is visible.
        GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 4, () => {
            if (this._destroyed || !this._pipeline)
                return GLib.SOURCE_REMOVE;
            const [, st] = this._pipeline.get_state(0);
            log(`wwb: 4s status — samples=${this._sampleCount || 0}, ` +
                `pipelineState=${st}, src=${this._srcDesc}`);
            return GLib.SOURCE_REMOVE;
        });
    }

    _pullFrame() {
        const sink = this._appsink;
        if (!sink)
            return;
        let sample;
        try {
            sample = sink.try_pull_sample(0); // non-blocking
        } catch (e) {
            if (!this._loggedPullErr) {
                this._loggedPullErr = true;
                logError(e, 'wwb: try_pull_sample threw (appsink method missing?)');
            }
            return;
        }
        if (!sample)
            return;

        this._sampleCount = (this._sampleCount || 0) + 1;
        if (this._sampleCount === 1)
            log('wwb: first sample pulled from appsink');

        const caps = sample.get_caps();
        const s = caps.get_structure(0);
        const [, width] = s.get_int('width');
        const [, height] = s.get_int('height');
        const buffer = sample.get_buffer();
        const [ok, info] = buffer.map(Gst.MapFlags.READ);
        if (!ok)
            return;
        try {
            this._uploadFrame(info.data, width, height, width * 4);
        } catch (e) {
            if (!this._loggedUpload) {
                this._loggedUpload = true;
                logError(e, 'wwb: frame upload failed (first occurrence)');
            }
        } finally {
            buffer.unmap(info);
        }
    }

    _uploadFrame(bytes, width, height, stride) {
        if (!this._coglContext)
            this._coglContext = getCoglContext();
        if (!this._content) {
            this._content = new St.ImageContent();
            this.set_content(this._content);
        }
        // set_bytes(cogl_context, data, pixel_format, width, height, row_stride).
        // RGBA from videoconvert; opaque (producer forces it).
        this._content.set_bytes(
            this._coglContext,
            new GLib.Bytes(bytes),
            Cogl.PixelFormat.RGBA_8888,
            width, height, stride);
        if (!this._loggedUpload) {
            this._loggedUpload = true;
            log(`wwb: first frame uploaded (${width}x${height})`);
        }
    }

    _scheduleReconnect() {
        this._teardownPipeline();
        if (this._reconnectId)
            return;
        this._reconnectId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 2, () => {
            this._reconnectId = 0;
            if (!this._destroyed) {
                log(`wwb: reconnecting on monitor ${this._monitorIndex}`);
                this._start();
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _teardownPipeline() {
        if (this._pollId) {
            GLib.source_remove(this._pollId);
            this._pollId = 0;
        }
        if (this._pipeline) {
            try {
                this._pipeline.get_bus()?.remove_signal_watch();
            } catch (e) {}
            this._pipeline.set_state(Gst.State.NULL);
            this._pipeline = null;
            this._appsink = null;
        }
    }

    destroy() {
        this._destroyed = true;
        if (this._reconnectId) {
            GLib.source_remove(this._reconnectId);
            this._reconnectId = 0;
        }
        this._teardownPipeline();
        this._content = null;
        super.destroy();
    }
});
