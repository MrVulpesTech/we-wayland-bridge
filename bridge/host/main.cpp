// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 we-wayland-bridge contributors
//
// Stage A frame producer: a minimal host that embeds linux-wallpaperengine-core
// (next-v2), renders a wallpaper offscreen into a host-owned FBO via the public
// C embedding API, and dumps verification PNGs. No window, no compositor.
//
// This file links the GPLv3 core library, so it is GPLv3 (ADR-0002). The GNOME
// extension that will consume the PipeWire stream is a separate process (MIT).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>

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

// Virtual clock: advanced deterministically per frame so a fixed frame count
// shows real animation progression regardless of how fast we render. Core reads
// this through the wp_time_counter callback.
float g_virtual_time = 0.0f;
long g_time_calls = 0; // diagnostic: how often core asks for time

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
    std::string wallpaper;                                  // folder containing project.json
    std::string assets = "../../steam-assets";              // relative to repo root by default
    int width = 1920;
    int height = 1080;
    int frames = 120;
    int fps = 60;
    std::string out = "out";
    bool readback_all = false; // benchmark: glReadPixels every frame, timed
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
        else if (a == "--frames") o.frames = std::atoi (next ());
        else if (a == "--fps") o.fps = std::atoi (next ());
        else if (a == "--out") o.out = next ();
        else if (a == "--readback-all") o.readback_all = true;
        else if (a[0] == '-') die ("unknown flag");
        else o.wallpaper = a;
    }
    if (o.wallpaper.empty ()) die ("usage: wpe-host [--assets-dir D] [--width W] [--height H] [--frames N] [--fps F] [--out DIR] <wallpaper-folder>");
    return o;
}

} // namespace

int main (int argc, char** argv) {
    Options opt = parse_args (argc, argv);

    // ---- Headless EGL (surfaceless, Mesa) -----------------------------------
    // Surfaceless avoids any window/compositor surface; we render only into an
    // FBO we own. Chosen over gbm because it needs no DRM render-node handling
    // and works on the reference Intel/Mesa stack. gbm becomes relevant in
    // Stage C, where the FBO texture must be dma-buf backed for zero-copy.
    auto eglGetPlatformDisplayEXT = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC> (
        eglGetProcAddress ("eglGetPlatformDisplayEXT"));
    if (!eglGetPlatformDisplayEXT) die ("eglGetPlatformDisplayEXT unavailable");

    EGLDisplay dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY) die ("eglGetPlatformDisplay(SURFACELESS) failed");

    EGLint major = 0, minor = 0;
    if (!eglInitialize (dpy, &major, &minor)) die ("eglInitialize failed");
    std::fprintf (stderr, "EGL %d.%d, surfaceless\n", major, minor);

    if (!eglBindAPI (EGL_OPENGL_API)) die ("eglBindAPI(OpenGL) failed");

    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config {};
    EGLint num_config = 0;
    if (!eglChooseConfig (dpy, config_attrs, &config, 1, &num_config) || num_config == 0)
        die ("eglChooseConfig found no config");

    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_NONE,
    };
    EGLContext ctx = eglCreateContext (dpy, config, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) die ("eglCreateContext failed (GL 3.3 core)");

    // Surfaceless current: requires EGL_KHR_surfaceless_context (Mesa has it).
    if (!eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        die ("eglMakeCurrent(surfaceless) failed");

    glewExperimental = GL_TRUE;
    if (GLenum e = glewInit (); e != GLEW_OK) {
        // GLEW often reports GL_INVALID_ENUM on core profiles; only bail if no GL.
        std::fprintf (stderr, "glewInit: %s (continuing)\n", glewGetErrorString (e));
    }
    glGetError (); // swallow GLEW's spurious error
    std::fprintf (stderr, "GL renderer: %s\n", reinterpret_cast<const char*> (glGetString (GL_RENDERER)));

    // ---- Host-owned output FBO ---------------------------------------------
    GLuint texture = 0, fbo = 0;
    glGenTextures (1, &texture);
    glBindTexture (GL_TEXTURE_2D, texture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, opt.width, opt.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers (1, &fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        die ("output FBO incomplete");

    // ---- Core embedding ----------------------------------------------------
    wp_configuration* config_obj = wp_config_create ();
    if (!wp_config_set_assets_dir (config_obj, opt.assets.c_str ()))
        die ("wp_config_set_assets_dir failed (check --assets-dir)");
    wp_config_enable_audio (config_obj, false); // no audio device in this host

    wp_context* context = wp_context_create (config_obj);
    if (!context) die ("wp_context_create failed");

    wp_gl_proc_address gl_proc { nullptr, get_proc_address_cb };
    wp_context_set_gl_proc_address (context, &gl_proc);
    wp_time_counter time_counter { nullptr, get_time_cb };
    wp_context_set_time_counter (context, &time_counter);

    wp_project* project = wp_project_load_folder (context, nullptr, opt.wallpaper.c_str ());
    if (!project) {
        std::string pj = opt.wallpaper + "/project.json";
        project = wp_project_load_folder (context, nullptr, pj.c_str ());
    }
    if (!project) die ("wp_project_load_folder failed (check wallpaper path)");

    wp_project_hint_size (project, opt.width, opt.height);
    wp_project_set_output_framebuffer (project, fbo);
    std::fprintf (stderr, "loaded %s, output FBO %dx%d\n", opt.wallpaper.c_str (), opt.width, opt.height);
    // Note: wp_project_get_width/height return 1 until the first wp_render_frame,
    // because core creates the renderable wallpaper lazily on first render.

    // ---- Render loop -------------------------------------------------------
    std::vector<unsigned char> pixels (static_cast<size_t> (opt.width) * opt.height * 4);
    std::string mkout = "mkdir -p '" + opt.out + "'";
    if (std::system (mkout.c_str ()) != 0) die ("could not create out dir");

    double total_ms = 0.0;
    double total_readback_ms = 0.0;
    const char* fixed = std::getenv ("WPE_FIXED_TIME"); // diagnostic: freeze the clock
    for (int frame = 1; frame <= opt.frames; frame++) {
        g_virtual_time = fixed ? std::atof (fixed)
                               : static_cast<float> (frame) / static_cast<float> (opt.fps);

        double t0 = monotonic_ms ();
        wp_render_update_time (context);
        glBindFramebuffer (GL_FRAMEBUFFER, fbo);
        glViewport (0, 0, opt.width, opt.height);
        wp_render_frame (project);
        glFinish ();
        total_ms += monotonic_ms () - t0;

        if (frame == 1)
            std::fprintf (stderr, "scene intrinsic size after first render: %dx%d\n",
                          wp_project_get_width (project), wp_project_get_height (project));

        const bool dump = (frame == 1 || frame == 60 || frame == 120);

        // Benchmark the GPU->CPU readback cost every frame (the copy that
        // Stage B pays and Stage C eliminates). glReadPixels is synchronous,
        // so timing it directly is accurate.
        if (opt.readback_all) {
            glBindFramebuffer (GL_FRAMEBUFFER, fbo);
            double r0 = monotonic_ms ();
            glReadPixels (0, 0, opt.width, opt.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data ());
            total_readback_ms += monotonic_ms () - r0;
        }

        if (dump) {
            glBindFramebuffer (GL_FRAMEBUFFER, fbo);
            glReadPixels (0, 0, opt.width, opt.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data ());
            // GL origin is bottom-left; flip to top-left for the PNG.
            std::vector<unsigned char> flipped (pixels.size ());
            const int stride = opt.width * 4;
            for (int y = 0; y < opt.height; y++)
                std::memcpy (&flipped[y * stride], &pixels[(opt.height - 1 - y) * stride], stride);
            // Scenes render opaque RGB but leave alpha at the cleared 0, which
            // makes normal viewers show the frame as fully transparent. A
            // wallpaper is opaque: force alpha. Stage B's PipeWire stream uses
            // an alpha-less format (BGRx) for the same reason.
            for (size_t i = 3; i < flipped.size (); i += 4)
                flipped[i] = 255;
            char path[512];
            std::snprintf (path, sizeof (path), "%s/frame_%03d.png", opt.out.c_str (), frame);
            if (!stbi_write_png (path, opt.width, opt.height, 4, flipped.data (), stride))
                die ("stbi_write_png failed");
            std::fprintf (stderr, "wrote %s\n", path);
        }
    }

    const double render_avg = total_ms / opt.frames;
    std::fprintf (stderr, "rendered %d frames, render avg %.2f ms/frame (%.1f fps), time_cb_calls=%ld%s\n",
                  opt.frames, render_avg, 1000.0 / render_avg, g_time_calls, fixed ? " [FROZEN]" : "");
    if (opt.readback_all) {
        const double rb_avg = total_readback_ms / opt.frames;
        std::fprintf (stderr, "  + readback avg %.2f ms/frame -> render+readback %.2f ms/frame (%.1f fps)\n",
                      rb_avg, render_avg + rb_avg, 1000.0 / (render_avg + rb_avg));
    }

    // ---- Cleanup -----------------------------------------------------------
    wp_project_destroy (project);
    wp_context_destroy (context);
    wp_config_destroy (config_obj);
    glDeleteFramebuffers (1, &fbo);
    glDeleteTextures (1, &texture);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return 0;
}
