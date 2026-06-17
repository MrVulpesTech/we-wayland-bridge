// SPDX-License-Identifier: MIT
// Copyright (C) 2026 we-wayland-bridge contributors
//
// Two pieces:
//   FrameSource   — ONE PipeWire pipeline + ONE Cogl texture for the whole
//                   extension. Owns all GStreamer state and the only poll timer.
//   LiveWallpaper — a thin Clutter/St actor placed in a monitor's background
//                   actor that PAINTS the FrameSource's shared texture. It owns
//                   no pipeline and no timer.
//
// Why the split (40.04): Background.BackgroundManager._createBackgroundActor is
// called whenever a background actor is (re)built — opening the overview,
// switching workspaces, monitor changes. An earlier design built a full
// GStreamer pipeline per LiveWallpaper, so each overview toggle leaked another
// pipeline + texture + timer; disposed actors' poll timers kept firing
// ("already disposed" criticals), memory ran away, and the shell crashed. With
// a single shared source, actors are cheap and disposable; only the source
// touches GStreamer, exactly once.
//
// Design notes:
//  - GStreamer's streaming thread must NOT touch Clutter/GJS objects. The
//    FrameSource polls the appsink from a GLib timer on the SHELL MAIN THREAD
//    (try_pull_sample); every Clutter call happens on the main loop.
//  - The shared texture is allocated on the first frame and updated in place
//    (set_data) every frame thereafter — no per-frame allocation. Subscribers
//    paint it directly in vfunc_paint_node (Clutter.TextureNode).
//  - Churn guards (all 40.04 Failure 2): the upload is rate-capped
//    (FRAME_INTERVAL_MS) and paused entirely while the overview is open.
//  - Before the first frame the actor is transparent (the normal desktop shows
//    through); the texture is painted over it once frames arrive.

import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Gst from 'gi://Gst';
import GstApp from 'gi://GstApp'; // registers GstAppSink methods (try_pull_sample)
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

// Upload-rate cap (40.04 Failure 2: bound the per-frame GPU work). 20 fps is a
// safe daily-use default, paired with a reduced producer resolution (40.03) so
// the per-frame copy stays small; the SHM upload cost is resolution × rate and
// the resolution drop dominates. 30 fps (33 ms) is fine on capable hardware.
// drop=true on the appsink keeps us on the latest frame, never a backlog.
const FRAME_INTERVAL_MS = 50; // 1000 / 20

// Producer frames are BGRx → videoconvert → RGBA, but the alpha byte can arrive
// as 0 (fully transparent), which paints an invisible texture. Upload in an X
// (ignore-alpha) format so the texture is always opaque; the RGB bytes sit in
// the same positions. Fall back to RGBA if the X format is unavailable.
const UPLOAD_FORMAT =
    (Cogl.PixelFormat && Cogl.PixelFormat.RGBX_8888) || Cogl.PixelFormat.RGBA_8888;

// The producer advertises this PW_KEY_NODE_NAME (see bridge/host/main.cpp).
const PRODUCER_NODE_NAME = 'wpe-host';

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
        `Clutter.TextureNode=${has(Clutter.TextureNode)}`);
}

// A white modulation colour for Clutter.TextureNode. Passing null can make the
// node paint the texture modulated by transparent black (i.e. invisible), so we
// hand it an explicit opaque white. Built once; format varies across Cogl, so
// try both initialisers.
let _whiteColor;
function whiteColor() {
    if (_whiteColor !== undefined)
        return _whiteColor;
    _whiteColor = null;
    try {
        const c = new Cogl.Color();
        if (typeof c.init_from_4f === 'function')
            c.init_from_4f(1.0, 1.0, 1.0, 1.0);
        else if (typeof c.init_from_4ub === 'function')
            c.init_from_4ub(255, 255, 255, 255);
        else
            return _whiteColor;
        _whiteColor = c;
    } catch (e) {
        log(`wwb: white CoglColor construction failed: ${e}`);
    }
    return _whiteColor;
}

// A CoglContext is needed to allocate/update the texture. How to reach it varies
// across Clutter/Mutter versions, so try the known paths and log which worked.
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

// One shared PipeWire pipeline + one shared Cogl texture for the whole
// extension. Background actors subscribe via subscribe()/unsubscribe(); the
// source redraws them after each new frame. Not a GObject — plain JS, owned by
// the extension and explicitly start()/stop()ed.
export class FrameSource {
    constructor(pipewirePath) {
        this._path = pipewirePath || '';
        this._pipeline = null;
        this._appsink = null;
        this._busId = 0;
        this._pollId = 0;
        this._reconnectId = 0;
        this._srcDesc = '';
        // The single reused texture (allocated on first frame, updated in place).
        this._texture = null;
        this._texW = 0;
        this._texH = 0;
        this._useSetData = true;
        this._coglContext = null;
        this._paused = false;
        this._ovShowingId = 0;
        this._ovHiddenId = 0;
        this._subscribers = new Set();
        this._sampleCount = 0;
        this._loggedUpload = false;
        this._stopped = false;
    }

    get texture() {
        return this._texture;
    }

    subscribe(actor) {
        this._subscribers.add(actor);
        // A late subscriber (e.g. the overview's background opening mid-stream)
        // should paint the current frame immediately.
        if (this._texture) {
            try { actor.queue_redraw(); } catch (e) {}
        }
    }

    unsubscribe(actor) {
        this._subscribers.delete(actor);
    }

    // Begin discovery + streaming. Safe to call once, after the shell is up
    // (the caller defers it off the enable path — see extension.js).
    start() {
        if (this._stopped)
            return;
        // Pause uploads while the overview is open. The overview blurs, scales,
        // and desaturates every background — a large GL spike on the iGPU that,
        // stacked on our texture upload, hard-locked the GPU (40.04 Failure 2).
        try {
            this._ovShowingId = Main.overview.connect('showing', () => {
                this._paused = true;
                log('wwb: overview showing — uploads paused');
            });
            this._ovHiddenId = Main.overview.connect('hidden', () => {
                this._paused = false;
                log('wwb: overview hidden — uploads resumed');
            });
        } catch (e) {
            logError(e, 'wwb: could not connect overview pause signals');
        }
        this._startPipeline();
    }

    stop() {
        this._stopped = true;
        if (this._ovShowingId) {
            Main.overview.disconnect(this._ovShowingId);
            this._ovShowingId = 0;
        }
        if (this._ovHiddenId) {
            Main.overview.disconnect(this._ovHiddenId);
            this._ovHiddenId = 0;
        }
        if (this._reconnectId) {
            GLib.source_remove(this._reconnectId);
            this._reconnectId = 0;
        }
        this._teardownPipeline();
        this._texture = null;
        this._subscribers.clear();
    }

    _startPipeline() {
        if (this._stopped || !ensureGst())
            return;

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

        // The ONLY poll timer in the extension (FRAME_INTERVAL_MS rate cap).
        this._pollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, FRAME_INTERVAL_MS, () => {
            this._pullFrame();
            return GLib.SOURCE_CONTINUE;
        });

        // One-shot diagnostic: 4 s in, report whether any sample arrived.
        GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 4, () => {
            if (this._stopped || !this._pipeline)
                return GLib.SOURCE_REMOVE;
            const [, st] = this._pipeline.get_state(0);
            log(`wwb: 4s status — samples=${this._sampleCount}, ` +
                `pipelineState=${st}, subscribers=${this._subscribers.size}, src=${this._srcDesc}`);
            return GLib.SOURCE_REMOVE;
        });
    }

    _pullFrame() {
        const sink = this._appsink;
        if (!sink || this._paused)
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

        this._sampleCount += 1;
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
        const ctx = this._coglContext;
        if (!ctx)
            return;
        const fmt = UPLOAD_FORMAT;

        // Allocate ONCE (or on a resolution change); update in place otherwise.
        if (!this._texture || this._texW !== width || this._texH !== height) {
            this._texture = Cogl.Texture2D.new_from_data(ctx, width, height, fmt, stride, bytes);
            this._texW = width;
            this._texH = height;
            this._useSetData = true;
        } else if (this._useSetData) {
            try {
                this._texture.set_data(fmt, stride, bytes, 0);
            } catch (e) {
                this._useSetData = false;
                logError(e, 'wwb: Cogl texture set_data unavailable — recreating per frame');
            }
        }
        if (!this._useSetData)
            this._texture = Cogl.Texture2D.new_from_data(ctx, width, height, fmt, stride, bytes);

        // Redraw every subscriber with the updated texture. Self-heal if a
        // disposed actor lingers (its queue_redraw throws → drop it).
        for (const actor of this._subscribers) {
            try {
                actor.queue_redraw();
            } catch (e) {
                this._subscribers.delete(actor);
            }
        }

        if (!this._loggedUpload) {
            this._loggedUpload = true;
            log(`wwb: first frame uploaded (${width}x${height}), setData=${this._useSetData}`);
        }
    }

    _scheduleReconnect() {
        this._teardownPipeline();
        if (this._stopped || this._reconnectId)
            return;
        this._reconnectId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 2, () => {
            this._reconnectId = 0;
            if (!this._stopped) {
                log('wwb: reconnecting to producer');
                this._startPipeline();
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _teardownPipeline() {
        if (this._pollId) {
            GLib.source_remove(this._pollId);
            this._pollId = 0;
        }
        const pipeline = this._pipeline;
        this._pipeline = null;
        this._appsink = null;
        if (!pipeline)
            return;
        // Tear down cleanly: drop the bus handler, drive to NULL, and WAIT for
        // NULL before releasing the reference. Finalizing a pipewiresrc while
        // still PLAYING crashed gnome-shell (SIGSEGV in libgstpipewire) — see
        // docs/40_bridge/40.04. get_state() blocks until the transition lands.
        try {
            const bus = pipeline.get_bus();
            if (bus) {
                if (this._busId) {
                    bus.disconnect(this._busId);
                    this._busId = 0;
                }
                bus.remove_signal_watch();
            }
        } catch (e) {}
        try {
            pipeline.set_state(Gst.State.NULL);
            pipeline.get_state(2000000000); // wait up to 2s (ns) for NULL
        } catch (e) {
            logError(e, 'wwb: pipeline teardown');
        }
    }
}

// A thin background-layer actor: paints the FrameSource's shared texture. Owns
// no pipeline and no timer, so it is cheap to create/destroy as the shell
// rebuilds background actors (overview, workspace and monitor changes).
export const LiveWallpaper = GObject.registerClass({
    GTypeName: 'WwbLiveWallpaper',
}, class LiveWallpaper extends St.Widget {
    _init(backgroundActor, source) {
        super._init({
            reactive: false,
            x_expand: true,
            y_expand: true,
        });

        this._source = source;
        this._monitorIndex = backgroundActor.monitor;

        const monitor = Main.layoutManager.monitors[this._monitorIndex];
        const w = monitor?.width ?? backgroundActor.width;
        const h = monitor?.height ?? backgroundActor.height;
        this.set_size(w, h);

        // No CSS background: an St.Widget background-color was occluding the
        // painted texture (only the placeholder showed). Before the first frame
        // the actor is simply transparent and the normal desktop shows through.

        // Meta.BackgroundActor has no layout manager, so a fixed-size child can
        // end up unallocated/invisible. BinLayout + x/y_expand fills it.
        backgroundActor.layout_manager = new Clutter.BinLayout();
        backgroundActor.add_child(this);

        // Clean up via the 'destroy' SIGNAL, not a destroy() override: when the
        // shell disposes this actor as a child of a rebuilt background actor it
        // emits 'destroy' but does not call a JS destroy() override — relying on
        // the override leaked subscriptions and produced "already disposed"
        // criticals (40.04).
        this.connect('destroy', () => {
            this._source?.unsubscribe(this);
            this._source = null;
        });

        source.subscribe(this);
        log(`wwb: LiveWallpaper on monitor ${this._monitorIndex} (${w}x${h}), ` +
            `bgActor ${backgroundActor.width}x${backgroundActor.height}`);
    }

    // Without a CSS background or Clutter content, an St.Widget's paint volume
    // is empty and Clutter CULLS the actor — vfunc_paint_node then never runs
    // after the first paint, so the texture is never drawn. Claim the full
    // allocation as our paint volume so we keep painting every frame.
    vfunc_get_paint_volume(volume) {
        volume.set_width(this.get_width());
        volume.set_height(this.get_height());
        return true;
    }

    vfunc_paint_node(node, paintContext) {
        super.vfunc_paint_node(node, paintContext);
        const texture = this._source?.texture;
        const w = this.get_width();
        const h = this.get_height();
        if (texture && !this._loggedPaint) {
            this._loggedPaint = true;
            log(`wwb: painting texture (${w}x${h})`);
        }
        if (!texture || w <= 0 || h <= 0)
            return;
        // Stretch to fill (MVP); 'fit'/'crop' modes adjust this rectangle later.
        const tnode = new Clutter.TextureNode(
            texture, whiteColor(),
            Clutter.ScalingFilter.LINEAR, Clutter.ScalingFilter.LINEAR);
        const box = new Clutter.ActorBox();
        box.set_origin(0, 0);
        box.set_size(w, h);
        tnode.add_rectangle(box);
        node.add_child(tnode);
    }
});
