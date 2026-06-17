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
// Stage B uses a glReadPixels->SHM copy path (the production default). Stage C
// (--dmabuf) backs the output FBO with a pool of gbm/dma-bufs and publishes them
// over PipeWire so the frame never leaves the GPU — producer-side zero-copy is
// proven. A GNOME consumer that imports the dma-buf is a separate, open question
// (docs/40_bridge/40.05); --shm forces the SHM path.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>

#include <gbm.h>
#include <drm_fourcc.h>

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
    bool dmabuf = false;       // Stage C: back the output FBO with a gbm/dma-buf
    bool shm = false;          // force the SHM (glReadPixels) transport even with --dmabuf
    bool verbose = false;      // print the per-second production-rate line
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
        else if (a == "--dmabuf") o.dmabuf = true;
        else if (a == "--shm") o.shm = true;
        else if (a == "--verbose") o.verbose = true;
        else if (a[0] == '-') die ("unknown flag");
        else o.wallpaper = a;
    }
    if (o.wallpaper.empty ())
        die ("usage: wpe-host [--pipewire] [--dmabuf] [--shm] [--verbose] [--assets-dir D] "
             "[--size WxH] [--width W] [--height H] [--frames N] [--fps F] [--out DIR] <wallpaper-folder>");
    return o;
}

// ---------------------------------------------------------------------------
// Shared GPU state: headless EGL context + host-owned output FBO(s).
// ---------------------------------------------------------------------------

// Number of dma-buf buffers to pool for the PipeWire transport. A single shared
// dma-buf deadlocks: the consumer holds the only buffer, so the producer's
// dequeue returns null and it never renders into the buffer being read (the
// consumer sees an empty buffer = black). A small pool lets the producer render
// into a free buffer while the consumer holds another.
constexpr int DMABUF_POOL_N = 4;

// One dma-buf-backed render target: a gbm_bo imported as an EGL image and bound
// to a GL texture + FBO. The GPU renders straight into the dma-buf.
struct DmaBuf {
    gbm_bo* bo = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint tex = 0;
    GLuint fbo = 0;
    int fd = -1; // the gbm_bo's dma-buf fd (we own it; dup'd per PipeWire buffer)
    int stride = 0;
    int offset = 0;
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
};

struct Gfx {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    GLuint texture = 0; // non-dmabuf path
    GLuint fbo = 0;     // current render target (non-dmabuf: the texture's FBO;
                        // dmabuf: pool[i].fbo, switched per frame)
    int width = 0;
    int height = 0;

    // Stage C: a pool of dma-buf render targets (when dmabuf == true).
    bool dmabuf = false;
    int drm_fd = -1; // DRM render node fd
    gbm_device* gbm = nullptr;
    std::vector<DmaBuf> pool;
};

// EGL/GL entry points for the dma-buf path (Stage C), loaded on demand.
PFNEGLCREATEIMAGEKHRPROC pf_eglCreateImageKHR = nullptr;
PFNEGLDESTROYIMAGEKHRPROC pf_eglDestroyImageKHR = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pf_glEGLImageTargetTexture2DOES = nullptr;
PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC pf_eglExportDMABUFImageQueryMESA = nullptr;
PFNEGLEXPORTDMABUFIMAGEMESAPROC pf_eglExportDMABUFImageMESA = nullptr;

void load_dmabuf_procs () {
    pf_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC> (eglGetProcAddress ("eglCreateImageKHR"));
    pf_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC> (eglGetProcAddress ("eglDestroyImageKHR"));
    pf_glEGLImageTargetTexture2DOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC> (eglGetProcAddress ("glEGLImageTargetTexture2DOES"));
    pf_eglExportDMABUFImageQueryMESA =
        reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC> (eglGetProcAddress ("eglExportDMABUFImageQueryMESA"));
    pf_eglExportDMABUFImageMESA =
        reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEMESAPROC> (eglGetProcAddress ("eglExportDMABUFImageMESA"));
    if (!pf_eglCreateImageKHR || !pf_eglDestroyImageKHR || !pf_glEGLImageTargetTexture2DOES)
        die ("dma-buf entry points unavailable (need EGL_EXT_image_dma_buf_import "
             "+ GL_OES_EGL_image)");
}

std::string fourcc_str (uint32_t f) {
    char c[5] = { char (f & 0xff), char ((f >> 8) & 0xff), char ((f >> 16) & 0xff), char ((f >> 24) & 0xff), 0 };
    for (char& ch : c)
        if (ch && (ch < 32 || ch > 126)) ch = '?';
    return std::string (c);
}

// Create one dma-buf render target: gbm_bo (LINEAR, with fallbacks) imported as
// an EGL image, bound to a texture + FBO. The GPU renders straight into the
// dma-buf — no glReadPixels.
DmaBuf create_dmabuf (EGLDisplay dpy, gbm_device* gbm, int w, int h, bool log) {
    DmaBuf d;
    // XBGR8888 = memory bytes R,G,B,X — same order GL writes for
    // GL_RGBA8/UNSIGNED_BYTE (no swizzle) but with NO alpha. The scene renders
    // alpha=0; an alpha format (ABGR8888) makes GL consumers premultiply the RGB
    // to black. X = ignore => opaque, matching the RGBx wire format.
    const uint32_t fmt = GBM_FORMAT_XBGR8888;
    uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    const char* how = "with_modifiers(LINEAR)";
    d.bo = gbm_bo_create_with_modifiers (gbm, w, h, fmt, &linear, 1);
    if (!d.bo) {
        how = "create(USE_RENDERING|USE_LINEAR)";
        d.bo = gbm_bo_create (gbm, w, h, fmt, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    }
    if (!d.bo) {
        how = "create(USE_RENDERING, driver-picked modifier)";
        d.bo = gbm_bo_create (gbm, w, h, fmt, GBM_BO_USE_RENDERING);
    }
    if (!d.bo) die ("gbm_bo_create failed (all strategies)");

    d.fourcc = gbm_bo_get_format (d.bo);
    d.modifier = gbm_bo_get_modifier (d.bo);
    d.stride = static_cast<int> (gbm_bo_get_stride (d.bo));
    d.offset = static_cast<int> (gbm_bo_get_offset (d.bo, 0));
    d.fd = gbm_bo_get_fd (d.bo);
    if (d.fd < 0) die ("gbm_bo_get_fd failed");
    if (log)
        std::fprintf (stderr,
                      "gbm_bo via %s: %dx%d fourcc=%s(0x%08x) modifier=0x%016llx stride=%d offset=%d\n",
                      how, w, h, fourcc_str (d.fourcc).c_str (), d.fourcc,
                      static_cast<unsigned long long> (d.modifier), d.stride, d.offset);

    EGLint attrs[32];
    int n = 0;
    attrs[n++] = EGL_WIDTH;  attrs[n++] = w;
    attrs[n++] = EGL_HEIGHT; attrs[n++] = h;
    attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;      attrs[n++] = static_cast<EGLint> (d.fourcc);
    attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;     attrs[n++] = d.fd;
    attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attrs[n++] = d.offset;
    attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attrs[n++] = d.stride;
    if (d.modifier != DRM_FORMAT_MOD_INVALID) {
        attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attrs[n++] = static_cast<EGLint> (d.modifier & 0xffffffff);
        attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attrs[n++] = static_cast<EGLint> (d.modifier >> 32);
    }
    attrs[n++] = EGL_NONE;

    d.image = pf_eglCreateImageKHR (dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                    static_cast<EGLClientBuffer> (nullptr), attrs);
    if (d.image == EGL_NO_IMAGE_KHR) die ("eglCreateImageKHR(LINUX_DMA_BUF) failed");

    glGenTextures (1, &d.tex);
    glBindTexture (GL_TEXTURE_2D, d.tex);
    pf_glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, static_cast<GLeglImageOES> (d.image));
    if (GLenum e = glGetError (); e != GL_NO_ERROR) {
        std::fprintf (stderr, "glEGLImageTargetTexture2DOES GL error 0x%04x\n", e);
        die ("binding EGL image to texture failed");
    }
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers (1, &d.fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, d.fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.tex, 0);
    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        die ("dma-buf FBO incomplete");
    return d;
}

// Open the render node, create a gbm device, and fill the dma-buf pool.
void gfx_init_dmabuf_pool (Gfx& g, int count) {
    const char* exts = eglQueryString (g.dpy, EGL_EXTENSIONS);
    const bool has_import = exts && std::strstr (exts, "EGL_EXT_image_dma_buf_import");
    std::fprintf (stderr, "EGL dma-buf import=%s\n", has_import ? "yes" : "no");
    if (!has_import) die ("EGL display lacks EGL_EXT_image_dma_buf_import");
    load_dmabuf_procs ();

    // The render node is renderD128 on most machines, but that is not
    // guaranteed (multi-GPU, different enumeration). Try the conventional range
    // and use the first node that opens.
    for (int i = 128; i <= 135 && g.drm_fd < 0; i++) {
        char path[64];
        std::snprintf (path, sizeof (path), "/dev/dri/renderD%d", i);
        g.drm_fd = open (path, O_RDWR | O_CLOEXEC);
        if (g.drm_fd >= 0)
            std::fprintf (stderr, "using DRM render node %s\n", path);
    }
    if (g.drm_fd < 0) die ("no usable DRM render node (/dev/dri/renderD128..135)");
    g.gbm = gbm_create_device (g.drm_fd);
    if (!g.gbm) die ("gbm_create_device failed");

    g.pool.reserve (count);
    for (int i = 0; i < count; i++)
        g.pool.push_back (create_dmabuf (g.dpy, g.gbm, g.width, g.height, i == 0));
    g.fbo = g.pool[0].fbo; // initial render target
    g.texture = g.pool[0].tex;
    std::fprintf (stderr, "dma-buf pool: %d buffer(s)\n", count);
}

void gfx_init (Gfx& g, int width, int height, int pool_n) {
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

    if (g.dmabuf) {
        // Stage C: a pool of dma-buf-backed FBOs (zero-copy render targets).
        gfx_init_dmabuf_pool (g, pool_n);
    } else {
        // Stage A/B: a plain GL-allocated RGBA8 texture + FBO (glReadPixels path).
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
}

void gfx_destroy (Gfx& g) {
    if (g.dmabuf) {
        for (DmaBuf& d : g.pool) {
            if (d.fbo) glDeleteFramebuffers (1, &d.fbo);
            if (d.tex) glDeleteTextures (1, &d.tex);
            if (d.image != EGL_NO_IMAGE_KHR && pf_eglDestroyImageKHR)
                pf_eglDestroyImageKHR (g.dpy, d.image);
        }
    } else {
        glDeleteFramebuffers (1, &g.fbo);
        glDeleteTextures (1, &g.texture);
    }
    eglMakeCurrent (g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (g.dpy, g.ctx);
    eglTerminate (g.dpy);
    // dma-buf resources (Stage C) — independent of EGL.
    for (DmaBuf& d : g.pool)
        if (d.fd >= 0) close (d.fd);
    if (g.gbm) gbm_device_destroy (g.gbm);
    if (g.drm_fd >= 0) close (g.drm_fd);
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
    bool dmabuf = false; // transport: dma-buf (zero-copy) vs SHM (glReadPixels)
    bool verbose = false; // print the per-second production-rate line
    int next_buf = 0;    // next pool index to hand out in pw_on_add_buffer
};

void pw_on_process (void* userdata) {
    auto* s = static_cast<PwState*> (userdata);
    pw_buffer* b = pw_stream_dequeue_buffer (s->stream);
    if (!b) return; // consumer has no free buffer yet; skip this tick
    spa_buffer* buf = b->buffer;

    const int w = s->gfx->width, h = s->gfx->height;

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

    if (s->dmabuf) {
        // Zero-copy: render straight into THIS buffer's dma-buf (the consumer
        // imported its fd in pw_on_add_buffer). Rotate the core's output FBO to
        // the dequeued pool entry. No glReadPixels.
        const int idx = static_cast<int> (reinterpret_cast<intptr_t> (b->user_data));
        DmaBuf& pb = s->gfx->pool[idx];
        s->gfx->fbo = pb.fbo;
        wp_project_set_output_framebuffer (s->project, pb.fbo);
        render_frame (*s->gfx, s->ctx, s->project);
        glFinish (); // GPU writes to the dma-buf must land before the consumer reads
        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = pb.stride;
        buf->datas[0].chunk->size = static_cast<uint32_t> (pb.stride) * h;
    } else {
        // SHM: render, then copy GPU->CPU into the mapped buffer (Stage B path).
        auto* dst = static_cast<unsigned char*> (buf->datas[0].data);
        if (!dst) { pw_stream_queue_buffer (s->stream, b); return; }
        const int stride = w * 4;
        render_frame (*s->gfx, s->ctx, s->project);
        // BGRx: the x byte is ignored downstream, so the scene's alpha=0 is fine.
        // next-v2 core renders top-down into the FBO, so no vertical flip.
        glReadPixels (0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, dst);
        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = stride;
        buf->datas[0].chunk->size = static_cast<uint32_t> (stride) * h;
    }
    pw_stream_queue_buffer (s->stream, b);

    // Report actual production rate once a second (--verbose only; otherwise
    // this is a line of stderr per second forever, which spams a service log).
    if (s->verbose) {
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
}

void pw_on_timeout (void* userdata, uint64_t /*expirations*/) {
    pw_on_process (userdata);
}

// dma-buf transport: PipeWire created a buffer (PW_STREAM_FLAG_ALLOC_BUFFERS);
// bind one pool entry's dma-buf fd to it and remember which entry, so
// pw_on_process renders into the matching FBO.
void pw_on_add_buffer (void* userdata, pw_buffer* b) {
    auto* s = static_cast<PwState*> (userdata);
    if (!s->dmabuf) return;
    const int n = static_cast<int> (s->gfx->pool.size ());
    const int idx = s->next_buf++ % n; // PipeWire may request fewer than the pool size
    DmaBuf& pb = s->gfx->pool[idx];
    spa_data* d = &b->buffer->datas[0];
    d->type = SPA_DATA_DmaBuf;
    d->flags = SPA_DATA_FLAG_READABLE;
    d->fd = dup (pb.fd); // dup: the consumer/PipeWire closes its copy
    d->mapoffset = 0;
    d->maxsize = static_cast<uint32_t> (pb.stride * s->gfx->height);
    d->data = nullptr;
    d->chunk->offset = 0;
    d->chunk->stride = pb.stride;
    d->chunk->size = d->maxsize;
    b->user_data = reinterpret_cast<void*> (static_cast<intptr_t> (idx));
    std::fprintf (stderr, "dma-buf buffer[%d] attached: fd=%d (dup of %d) stride=%d\n",
                  idx, static_cast<int> (d->fd), pb.fd, pb.stride);
}

void pw_on_remove_buffer (void* userdata, pw_buffer* b) {
    auto* s = static_cast<PwState*> (userdata);
    if (!s->dmabuf) return;
    spa_data* d = &b->buffer->datas[0];
    if (d->fd >= 0) { close (static_cast<int> (d->fd)); d->fd = -1; }
}

// Build a dma-buf EnumFormat pod: RGBx (opaque; the dma-buf is XBGR8888 — the x
// ignores the scene's alpha=0) with a mandatory LINEAR modifier — the modifier
// property is what signals "this is dma-buf" to the consumer.
const spa_pod* build_format_dmabuf (spa_pod_builder* bld, int w, int h, int fps) {
    spa_rectangle size = SPA_RECTANGLE (static_cast<uint32_t> (w), static_cast<uint32_t> (h));
    spa_fraction rate = SPA_FRACTION (static_cast<uint32_t> (fps), 1);
    spa_pod_frame f;
    spa_pod_builder_push_object (bld, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add (bld,
        SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id (SPA_VIDEO_FORMAT_RGBx), 0);
    spa_pod_builder_prop (bld, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
    spa_pod_builder_long (bld, DRM_FORMAT_MOD_LINEAR);
    spa_pod_builder_add (bld,
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle (&size),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction (&rate), 0);
    return static_cast<const spa_pod*> (spa_pod_builder_pop (bld, &f));
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

    uint8_t buffer[1024];
    spa_pod_builder bld = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
    const spa_pod* params[2];
    if (s->dmabuf) {
        // A pool of dma-buf buffers (blocks=1 each). At least 2 so the producer
        // and consumer never fight over a single buffer (the deadlock that left
        // the consumer reading an empty buffer). The consumer imports the fd set
        // in pw_on_add_buffer.
        const int n = static_cast<int> (s->gfx->pool.size ());
        const int stride = s->gfx->pool[0].stride;
        const int size = stride * s->gfx->height;
        params[0] = static_cast<const spa_pod*> (spa_pod_builder_add_object (
            &bld, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int (n, 2, n),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int (1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int (size),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int (stride),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int (1 << SPA_DATA_DmaBuf)));
    } else {
        const int stride = s->gfx->width * 4;
        const int size = stride * s->gfx->height;
        params[0] = static_cast<const spa_pod*> (spa_pod_builder_add_object (
            &bld, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int (8, 2, 16),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int (1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int (size),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int (stride),
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
    }
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
    s.dmabuf = opt.dmabuf && !opt.shm; // dma-buf FBO + not forced back to SHM
    s.verbose = opt.verbose;
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
    events.add_buffer = pw_on_add_buffer;       // dma-buf: provide the fd
    events.remove_buffer = pw_on_remove_buffer;  // dma-buf: close our dup'd fd

    s.stream = pw_stream_new_simple (l, "wpe-host", props, &events, &s);
    if (!s.stream) die ("pw_stream_new_simple failed");

    uint8_t buffer[512];
    spa_pod_builder bld = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
    const spa_pod* params[1];
    pw_stream_flags flags;
    if (s.dmabuf) {
        params[0] = build_format_dmabuf (&bld, opt.width, opt.height, opt.fps);
        // ALLOC_BUFFERS: we hand PipeWire the dma-buf fd (pw_on_add_buffer). No
        // MAP_BUFFERS — a dma-buf is not mmap'd by the producer.
        flags = static_cast<pw_stream_flags> (PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS);
    } else {
        spa_video_info_raw info {};
        info.format = SPA_VIDEO_FORMAT_BGRx;
        info.size = SPA_RECTANGLE (static_cast<uint32_t> (opt.width), static_cast<uint32_t> (opt.height));
        info.framerate = SPA_FRACTION (static_cast<uint32_t> (opt.fps), 1);
        params[0] = spa_format_video_raw_build (&bld, SPA_PARAM_EnumFormat, &info);
        flags = static_cast<pw_stream_flags> (PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS);
    }

    if (pw_stream_connect (s.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0)
        die ("pw_stream_connect failed");

    std::fprintf (stderr, "PipeWire source running at %dx%d@%dfps (%s). Ctrl-C to stop.\n",
                  opt.width, opt.height, opt.fps, s.dmabuf ? "dma-buf RGBx/LINEAR" : "SHM BGRx");
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
    std::error_code ec;
    std::filesystem::create_directories (opt.out, ec);
    if (ec) die ("could not create out dir");

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

        // Frame 5 is the Stage C-1 content check (a short --frames 10 run).
        const bool dump = (frame == 1 || frame == 60 || frame == 120) || (gfx.dmabuf && frame == 5);
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
    gfx.dmabuf = opt.dmabuf;
    // A pool is only needed for the dma-buf PipeWire transport; the dump and
    // SHM paths use a single buffer.
    const int pool_n = (opt.dmabuf && opt.pipewire && !opt.shm) ? DMABUF_POOL_N : 1;
    gfx_init (gfx, opt.width, opt.height, pool_n);

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
