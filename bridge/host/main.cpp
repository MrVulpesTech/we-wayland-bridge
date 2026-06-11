// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 we-wayland-bridge contributors
//
// Frame producer: a host that embeds linux-wallpaperengine-core (next-v2),
// renders a wallpaper offscreen into a host-owned FBO via the public C
// embedding API, and either
//   - dumps verification PNGs (Stage A, default), or
//   - publishes the frames as a PipeWire video source (Stage B, --pipewire).
//
// This file links the GPLv3 core library, so it is GPLv3 (ADR-0002). The GNOME
// extension that will consume the PipeWire stream is a separate process (MIT).
//
// Stage B uses a glReadPixels->SHM copy path (correctness first). The dma-buf
// zero-copy path is Stage C.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/buffer/meta.h>

extern "C" {
#include <linux-wallpaperengine/configuration.h>
#include <linux-wallpaperengine/context.h>
#include <linux-wallpaperengine/project.h>
#include <linux-wallpaperengine/render.h>
}

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

namespace {

// Virtual clock read by core through the wp_time_counter callback. In dump
// mode it advances deterministically per frame (reproducible); in PipeWire
// mode it tracks real elapsed time (the wallpaper animates in real time).
float g_virtual_time = 0.0f;
long g_time_calls = 0;

float get_time_cb (void* /*user*/) {
    g_time_calls++;
    return g_virtual_time;
}

void* get_proc_address_cb (void* /*user*/, const char* name) {
    return reinterpret_cast<void*> (eglGetProcAddress (name));
}

[[noreturn]] void die (const char* msg) {
    std::fprintf (stderr, "fatal: %s\n", msg);
    std::exit (1);
}

double monotonic_ms () {
    timespec ts {};
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

struct Options {
    std::string wallpaper;
    std::string assets = "../../steam-assets";
    int width = 1920;
    int height = 1080;
    int frames = 120;
    int fps = 60;
    std::string out = "out";
    bool readback_all = false; // dump-mode benchmark: time glReadPixels each frame
    bool pipewire = false;     // Stage B: run as a PipeWire video source
};

Options parse_args (int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&] () -> const char* {
            if (i + 1 >= argc) die ("missing value for flag");
            return argv[++i];
        };
        if (a == "--assets-dir") o.assets = next ();
        else if (a == "--width") o.width = std::atoi (next ());
        else if (a == "--height") o.height = std::atoi (next ());
        else if (a == "--size") { // WxH convenience
            std::string s = next ();
            auto x = s.find ('x');
            if (x == std::string::npos) die ("--size expects WxH");
            o.width = std::atoi (s.substr (0, x).c_str ());
            o.height = std::atoi (s.substr (x + 1).c_str ());
        }
        else if (a == "--frames") o.frames = std::atoi (next ());
        else if (a == "--fps") o.fps = std::atoi (next ());
        else if (a == "--out") o.out = next ();
        else if (a == "--readback-all") o.readback_all = true;
        else if (a == "--pipewire") o.pipewire = true;
        else if (a[0] == '-') die ("unknown flag");
        else o.wallpaper = a;
    }
    if (o.wallpaper.empty ())
        die ("usage: wpe-host [--pipewire] [--assets-dir D] [--size WxH] "
             "[--width W] [--height H] [--frames N] [--fps F] [--out DIR] <wallpaper-folder>");
    return o;
}

// ---------------------------------------------------------------------------
// Shared GPU state: headless EGL context + host-owned output FBO.
// ---------------------------------------------------------------------------
struct Gfx {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    GLuint texture = 0;
    GLuint fbo = 0;
    int width = 0;
    int height = 0;
};

void gfx_init (Gfx& g, int width, int height) {
    g.width = width;
    g.height = height;

    // Headless surfaceless EGL (Mesa). No window, no compositor surface; we
    // render only into our own FBO. gbm/dma-buf comes in Stage C.
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC> (
        eglGetProcAddress ("eglGetPlatformDisplayEXT"));
    if (!getPlatformDisplay) die ("eglGetPlatformDisplayEXT unavailable");

    g.dpy = getPlatformDisplay (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (g.dpy == EGL_NO_DISPLAY) die ("eglGetPlatformDisplay(SURFACELESS) failed");

    EGLint major = 0, minor = 0;
    if (!eglInitialize (g.dpy, &major, &minor)) die ("eglInitialize failed");
    std::fprintf (stderr, "EGL %d.%d, surfaceless\n", major, minor);
    if (!eglBindAPI (EGL_OPENGL_API)) die ("eglBindAPI(OpenGL) failed");

    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
    };
    EGLConfig config {};
    EGLint num = 0;
    if (!eglChooseConfig (g.dpy, config_attrs, &config, 1, &num) || num == 0)
        die ("eglChooseConfig found no config");

    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE,
    };
    g.ctx = eglCreateContext (g.dpy, config, EGL_NO_CONTEXT, ctx_attrs);
    if (g.ctx == EGL_NO_CONTEXT) die ("eglCreateContext failed (GL 3.3 core)");
    if (!eglMakeCurrent (g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g.ctx))
        die ("eglMakeCurrent(surfaceless) failed");

    glewExperimental = GL_TRUE;
    if (GLenum e = glewInit (); e != GLEW_OK)
        std::fprintf (stderr, "glewInit: %s (continuing)\n", glewGetErrorString (e));
    glGetError ();
    std::fprintf (stderr, "GL renderer: %s\n", reinterpret_cast<const char*> (glGetString (GL_RENDERER)));

    glGenTextures (1, &g.texture);
    glBindTexture (GL_TEXTURE_2D, g.texture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers (1, &g.fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, g.fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.texture, 0);
    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        die ("output FBO incomplete");
}

void gfx_destroy (Gfx& g) {
    glDeleteFramebuffers (1, &g.fbo);
    glDeleteTextures (1, &g.texture);
    eglMakeCurrent (g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (g.dpy, g.ctx);
    eglTerminate (g.dpy);
}

void render_frame (const Gfx& g, wp_context* ctx, wp_project* project) {
    wp_render_update_time (ctx);
    glBindFramebuffer (GL_FRAMEBUFFER, g.fbo);
    glViewport (0, 0, g.width, g.height);
    wp_render_frame (project);
}

// ---------------------------------------------------------------------------
// Stage B: PipeWire video source. SHM (mapped) buffers, BGRx, timer-driven.
// ---------------------------------------------------------------------------
struct PwState {
    pw_main_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    spa_source* timer = nullptr;
    Gfx* gfx = nullptr;
    wp_context* ctx = nullptr;
    wp_project* project = nullptr;
    int fps = 60;
    double start_ms = 0.0;
};

void pw_on_process (void* userdata) {
    auto* s = static_cast<PwState*> (userdata);
    pw_buffer* b = pw_stream_dequeue_buffer (s->stream);
    if (!b) return; // consumer has no free buffer yet; skip this tick
    spa_buffer* buf = b->buffer;
    auto* dst = static_cast<unsigned char*> (buf->datas[0].data);
    if (!dst) { pw_stream_queue_buffer (s->stream, b); return; }

    const int w = s->gfx->width, h = s->gfx->height;
    const int stride = w * 4;

    if (s->start_ms == 0.0) s->start_ms = monotonic_ms ();
    const double elapsed_ms = monotonic_ms () - s->start_ms;
    g_virtual_time = static_cast<float> (elapsed_ms / 1000.0);

    // Timestamp the buffer so synchronizing sinks (autovideosink) schedule
    // frames over time instead of showing the first one forever.
    if (auto* hdr = static_cast<spa_meta_header*> (
            spa_buffer_find_meta_data (buf, SPA_META_Header, sizeof (spa_meta_header)))) {
        static uint64_t seq = 0;
        hdr->pts = static_cast<int64_t> (elapsed_ms * 1.0e6); // ns
        hdr->dts_offset = 0;
        hdr->seq = seq++;
        hdr->flags = 0;
        hdr->offset = 0;
    }

    render_frame (*s->gfx, s->ctx, s->project);

    // GL origin is bottom-left; flip rows into the mapped buffer (top-left).
    // BGRx: the x byte is ignored downstream, so the scene's alpha=0 is fine.
    // next-v2 core already renders top-down into the FBO, so glReadPixels
    // lands in the right orientation with no flip (verified against upstream).
    glReadPixels (0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, dst);

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size = static_cast<uint32_t> (stride) * h;
    pw_stream_queue_buffer (s->stream, b);

    // Report actual production rate once a second.
    static long produced = 0;
    static double window_start = 0.0;
    double now = monotonic_ms ();
    if (window_start == 0.0) window_start = now;
    produced++;
    if (now - window_start >= 1000.0) {
        std::fprintf (stderr, "producing %.1f fps, virtual_time=%.2fs\n",
                      produced * 1000.0 / (now - window_start), g_virtual_time);
        produced = 0;
        window_start = now;
    }
}

void pw_on_timeout (void* userdata, uint64_t /*expirations*/) {
    pw_on_process (userdata);
}

void pw_on_state_changed (void* userdata, pw_stream_state /*old*/, pw_stream_state state, const char* error) {
    auto* s = static_cast<PwState*> (userdata);
    std::fprintf (stderr, "stream state: %s%s%s\n", pw_stream_state_as_string (state),
                  error ? " - " : "", error ? error : "");
    // The node id is valid once connected (paused). Print it once so the demo
    // command is copy-pasteable; a Video/Source stays paused until a consumer
    // links and starts driving it.
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
        uint32_t id = pw_stream_get_node_id (s->stream);
        std::fprintf (stderr,
                      "PipeWire node id: %u\n  demo: gst-launch-1.0 pipewiresrc path=%u ! videoconvert ! autovideosink\n",
                      id, id);
    }
}

void pw_on_param_changed (void* userdata, uint32_t id, const spa_pod* param) {
    auto* s = static_cast<PwState*> (userdata);
    if (param == nullptr || id != SPA_PARAM_Format) return;

    const int stride = s->gfx->width * 4;
    const int size = stride * s->gfx->height;

    uint8_t buffer[1024];
    spa_pod_builder bld = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
    const spa_pod* params[2];
    params[0] = static_cast<const spa_pod*> (spa_pod_builder_add_object (
        &bld, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int (8, 2, 16),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int (1),
        SPA_PARAM_BUFFERS_size, SPA_POD_Int (size),
        SPA_PARAM_BUFFERS_stride, SPA_POD_Int (stride),
        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
    // Ask PipeWire to allocate a header meta on each buffer (for PTS).
    params[1] = static_cast<const spa_pod*> (spa_pod_builder_add_object (
        &bld, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id (SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int (sizeof (spa_meta_header))));
    pw_stream_update_params (s->stream, params, 2);

    // Start producing at the target fps once the format is negotiated.
    timespec value { 0, 1 };
    timespec interval { 0, 1000000000L / s->fps };
    pw_loop_update_timer (pw_main_loop_get_loop (s->loop), s->timer, &value, &interval, false);
}

void pw_on_quit (void* userdata, int /*sig*/) {
    auto* s = static_cast<PwState*> (userdata);
    pw_main_loop_quit (s->loop);
}

int run_pipewire (const Options& opt, Gfx& gfx, wp_context* ctx, wp_project* project) {
    pw_init (nullptr, nullptr);

    PwState s;
    s.gfx = &gfx;
    s.ctx = ctx;
    s.project = project;
    s.fps = opt.fps;
    s.loop = pw_main_loop_new (nullptr);
    if (!s.loop) die ("pw_main_loop_new failed");

    pw_loop* l = pw_main_loop_get_loop (s.loop);
    pw_loop_add_signal (l, SIGINT, pw_on_quit, &s);
    pw_loop_add_signal (l, SIGTERM, pw_on_quit, &s);
    s.timer = pw_loop_add_timer (l, pw_on_timeout, &s);

    pw_properties* props = pw_properties_new (
        PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_NODE_NAME, "wpe-host", PW_KEY_NODE_DESCRIPTION, "Wallpaper Engine bridge", nullptr);

    static pw_stream_events events = {};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = pw_on_state_changed;
    events.param_changed = pw_on_param_changed;
    events.process = pw_on_process;

    s.stream = pw_stream_new_simple (l, "wpe-host", props, &events, &s);
    if (!s.stream) die ("pw_stream_new_simple failed");

    uint8_t buffer[512];
    spa_pod_builder bld = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
    spa_video_info_raw info {};
    info.format = SPA_VIDEO_FORMAT_BGRx;
    info.size = SPA_RECTANGLE (static_cast<uint32_t> (opt.width), static_cast<uint32_t> (opt.height));
    info.framerate = SPA_FRACTION (static_cast<uint32_t> (opt.fps), 1);
    const spa_pod* params[1];
    params[0] = spa_format_video_raw_build (&bld, SPA_PARAM_EnumFormat, &info);

    if (pw_stream_connect (s.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                           static_cast<pw_stream_flags> (PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS),
                           params, 1) < 0)
        die ("pw_stream_connect failed");

    std::fprintf (stderr, "PipeWire source running at %dx%d@%dfps (BGRx). Ctrl-C to stop.\n",
                  opt.width, opt.height, opt.fps);
    pw_main_loop_run (s.loop);

    pw_stream_destroy (s.stream);
    pw_main_loop_destroy (s.loop);
    pw_deinit ();
    return 0;
}

// ---------------------------------------------------------------------------
// Stage A: render a fixed number of frames and dump PNGs.
// ---------------------------------------------------------------------------
int run_dump (const Options& opt, Gfx& gfx, wp_context* ctx, wp_project* project) {
    std::vector<unsigned char> pixels (static_cast<size_t> (opt.width) * opt.height * 4);
    std::string mkout = "mkdir -p '" + opt.out + "'";
    if (std::system (mkout.c_str ()) != 0) die ("could not create out dir");

    double total_ms = 0.0, total_readback_ms = 0.0;
    const char* fixed = std::getenv ("WPE_FIXED_TIME");
    for (int frame = 1; frame <= opt.frames; frame++) {
        g_virtual_time = fixed ? std::atof (fixed)
                               : static_cast<float> (frame) / static_cast<float> (opt.fps);
        double t0 = monotonic_ms ();
        render_frame (gfx, ctx, project);
        glFinish ();
        total_ms += monotonic_ms () - t0;

        if (frame == 1)
            std::fprintf (stderr, "scene intrinsic size after first render: %dx%d\n",
                          wp_project_get_width (project), wp_project_get_height (project));

        const bool dump = (frame == 1 || frame == 60 || frame == 120);
        if (opt.readback_all) {
            glBindFramebuffer (GL_FRAMEBUFFER, gfx.fbo);
            double r0 = monotonic_ms ();
            glReadPixels (0, 0, opt.width, opt.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data ());
            total_readback_ms += monotonic_ms () - r0;
        }
        if (dump) {
            glBindFramebuffer (GL_FRAMEBUFFER, gfx.fbo);
            glReadPixels (0, 0, opt.width, opt.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data ());
            const int stride = opt.width * 4;
            for (size_t i = 3; i < pixels.size (); i += 4) pixels[i] = 255; // opaque
            char path[512];
            std::snprintf (path, sizeof (path), "%s/frame_%03d.png", opt.out.c_str (), frame);
            if (!stbi_write_png (path, opt.width, opt.height, 4, pixels.data (), stride))
                die ("stbi_write_png failed");
            std::fprintf (stderr, "wrote %s\n", path);
        }
    }
    const double render_avg = total_ms / opt.frames;
    std::fprintf (stderr, "rendered %d frames, render avg %.2f ms/frame (%.1f fps), time_cb_calls=%ld%s\n",
                  opt.frames, render_avg, 1000.0 / render_avg, g_time_calls, fixed ? " [FROZEN]" : "");
    if (opt.readback_all) {
        const double rb = total_readback_ms / opt.frames;
        std::fprintf (stderr, "  + readback avg %.2f ms/frame -> render+readback %.2f ms/frame (%.1f fps)\n",
                      rb, render_avg + rb, 1000.0 / (render_avg + rb));
    }
    return 0;
}

} // namespace

int main (int argc, char** argv) {
    Options opt = parse_args (argc, argv);

    Gfx gfx;
    gfx_init (gfx, opt.width, opt.height);

    wp_configuration* config = wp_config_create ();
    if (!wp_config_set_assets_dir (config, opt.assets.c_str ()))
        die ("wp_config_set_assets_dir failed (check --assets-dir)");
    wp_config_enable_audio (config, false);

    wp_context* ctx = wp_context_create (config);
    if (!ctx) die ("wp_context_create failed");
    wp_gl_proc_address gl_proc { nullptr, get_proc_address_cb };
    wp_context_set_gl_proc_address (ctx, &gl_proc);
    wp_time_counter time_counter { nullptr, get_time_cb };
    wp_context_set_time_counter (ctx, &time_counter);

    wp_project* project = wp_project_load_folder (ctx, nullptr, opt.wallpaper.c_str ());
    if (!project) die ("wp_project_load_folder failed (check wallpaper path)");
    wp_project_hint_size (project, opt.width, opt.height);
    wp_project_set_output_framebuffer (project, gfx.fbo);
    std::fprintf (stderr, "loaded %s, output FBO %dx%d\n", opt.wallpaper.c_str (), opt.width, opt.height);

    int rc = opt.pipewire ? run_pipewire (opt, gfx, ctx, project) : run_dump (opt, gfx, ctx, project);

    wp_project_destroy (project);
    wp_context_destroy (ctx);
    wp_config_destroy (config);
    gfx_destroy (gfx);
    return rc;
}
