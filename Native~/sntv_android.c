// SNTV receiver for Android (Meta Quest), zero-copy into a Unity texture:
// TCP -> AMediaCodec (hardware, low-latency mode) -> AImageReader (AHardwareBuffer on the GPU)
// -> EGLImage -> external OES texture -> one full-screen draw into Unity's RenderTexture.
//
// Copyright 2026 Alejandro Bermúdez Fajardo (alexoberco)
// SPDX-License-Identifier: Apache-2.0
//
// Why: Unity.WebRTC on Android decodes in hardware but uploads every frame to its texture
// through the CPU on the render thread (~9 ms per frame at 2x1280 px, measured with the Unity
// Profiler on a Quest 3S). Here the frame never leaves the GPU; the render thread only issues
// one draw (0.41 ms at 4320x2176). Same scheme as ALVR: MediaCodec in low-latency mode fed
// straight from a socket.
//
// Protocol: SNTV, see sntv/sntv.h and https://github.com/alexoberco/sntv. Versions 1-4.
// Build: Native~/build.sh (Unity's NDK) -> Runtime/Plugins/Android/arm64-v8a/libsntv.so.

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>   // after gl3.h: uses its types

#include "sntv/sntv.h"

#define TAG "SNTV"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

#define EXPORT __attribute__((visibility("default")))

// ---------------------------------------------------------------------------------- state
// Mirrored by SntvReceiver.NativeStats in C#: keep the layout in sync.
typedef struct {
    int connected;
    int decoder_running;
    // stream header
    int version, width, height, codec, layout, projection, flags;
    float fps, fov_deg, lens_c2;
    // counters
    long long frames_received, frames_decoded, frames_drawn, bytes_received;
    float decode_ms_avg, decode_ms_max;      // from decoder input to output, current window
    float draw_ms_avg;                        // render-thread CPU per draw
} sntv_stats;

// Metadata of the frame most recently drawn into the texture. Mirrored by
// SntvReceiver.NativeFrame in C#.
typedef struct {
    long long timestamp_ns;
    int n_poses;
    float poses[2][7];   // per view: x, y, z, qw, qx, qy, qz (sender's world, X fwd Y left Z up)
} sntv_frame_info;

// The C# mirrors are laid out by the same rules; these catch a field added on one side only.
_Static_assert(sizeof(sntv_stats) == 96 && offsetof(sntv_stats, frames_received) == 48, "SntvReceiver.NativeStats");
_Static_assert(sizeof(sntv_frame_info) == 72 && offsetof(sntv_frame_info, poses) == 12, "SntvReceiver.NativeFrame");

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sntv_stats g_stats;
static sntv_frame_info g_drawn;
static char g_host[256];
static int g_port;
static atomic_int g_run;
static pthread_t g_net_thread, g_out_thread;

static AMediaCodec *g_codec;
static AImageReader *g_reader;
static atomic_int g_codec_ready;

// Per input frame, by pts slot: arrival time (decode latency) and its metadata, so the frame
// that comes out of the decoder can be matched with the pose it was rendered from.
#define PTS_RING 64
typedef struct {
    int64_t arrival_us;
    sntv_frame_info info;
} pts_slot;
static pts_slot g_pts[PTS_RING];

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------------------------- network
static int read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

// Host name, IPv4 or IPv6 literal (link-local with %iface works too).
static int connect_to(const char *host, int port) {
    char service[16];
    snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        int rcv = 8 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        struct timeval tv = {2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

// ---------------------------------------------------------------------------------- decoder
static int start_codec(int w, int h, int codec) {
    // The reader is reused while the size does not change: the render thread may hold images
    // of it, and destroying it underneath would be a use-after-free.
    if (g_reader) {
        int32_t rw = 0, rh = 0;
        AImageReader_getWidth(g_reader, &rw);
        AImageReader_getHeight(g_reader, &rh);
        if (rw != w || rh != h) {
            LOGW("Size change %dx%d -> %dx%d is not supported without restarting the app", rw, rh, w, h);
            return -1;
        }
    } else {
        // PRIVATE + GPU_SAMPLED_IMAGE: the buffer lives on the GPU and can only be sampled.
        // 6 images: the render thread keeps up to 3 in flight (until their fence passes), the
        // decoder writes into another, and acquireLatestImage needs room to take the new one
        // before releasing the old ones. With 4: "Unable to acquire a lockedBuffer ... more
        // than maxImages" on a Quest 3S.
        media_status_t s = AImageReader_newWithUsage(w, h, AIMAGE_FORMAT_PRIVATE,
                                                     AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 6, &g_reader);
        if (s != AMEDIA_OK) {
            LOGW("AImageReader_newWithUsage failed (%d)", s);
            return -1;
        }
    }
    ANativeWindow *win = NULL;
    AImageReader_getWindow(g_reader, &win);

    const char *mime = codec == SNTV_CODEC_HEVC ? "video/hevc" : "video/avc";
    g_codec = AMediaCodec_createDecoderByType(mime);
    if (!g_codec) {
        LOGW("No decoder for %s", mime);
        return -1;
    }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h);
    // Low latency: the decoder releases each frame as soon as it has it, without waiting for
    // more input. The same keys ALVR uses on the Quest (Qualcomm's and Android 11's).
    AMediaFormat_setInt32(f, "low-latency", 1);
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(f, "priority", 0);
    AMediaFormat_setInt32(f, "operating-rate", 0x7fff);
    media_status_t s = AMediaCodec_configure(g_codec, f, win, NULL, 0);
    AMediaFormat_delete(f);
    if (s != AMEDIA_OK || AMediaCodec_start(g_codec) != AMEDIA_OK) {
        LOGW("Could not configure or start the decoder (%d)", s);
        AMediaCodec_delete(g_codec);
        g_codec = NULL;
        return -1;
    }
    atomic_store(&g_codec_ready, 1);
    pthread_mutex_lock(&g_lock);
    g_stats.decoder_running = 1;
    pthread_mutex_unlock(&g_lock);
    LOGI("Decoder %s %dx%d running (low latency)", mime, w, h);
    return 0;
}

static void stop_codec(void) {
    atomic_store(&g_codec_ready, 0);
    if (g_codec) {
        usleep(30000);   // let the output thread let go of the codec
        AMediaCodec_stop(g_codec);
        AMediaCodec_delete(g_codec);
        g_codec = NULL;
    }
    pthread_mutex_lock(&g_lock);
    g_stats.decoder_running = 0;
    pthread_mutex_unlock(&g_lock);
}

// Output thread: blocks on the decoder and releases each frame to the surface as soon as it
// comes out. Draining from the network thread would make a ready frame wait for the next one
// to arrive (22 ms at 45 fps).
static void *out_thread(void *arg) {
    (void)arg;
    double acc = 0, mx = 0;
    int n = 0;
    while (atomic_load(&g_run)) {
        if (!atomic_load(&g_codec_ready) || !g_codec) {
            usleep(5000);
            continue;
        }
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(g_codec, &info, 10000);
        if (idx >= 0) {
            AMediaCodec_releaseOutputBuffer(g_codec, (size_t)idx, info.size > 0);
            int64_t arrival = g_pts[(info.presentationTimeUs / 1000) % PTS_RING].arrival_us;
            double ms = arrival > 0 ? (now_us() - arrival) / 1000.0 : 0;
            acc += ms;
            if (ms > mx) mx = ms;
            n++;
            pthread_mutex_lock(&g_lock);
            g_stats.frames_decoded++;
            g_stats.decode_ms_avg = (float)(acc / n);
            g_stats.decode_ms_max = (float)mx;
            if (n >= 90) { acc = 0; mx = 0; n = 0; }
            pthread_mutex_unlock(&g_lock);
        }
    }
    return NULL;
}

// Network and input thread: connects, reads the header and feeds each access unit to the decoder.
static void *net_thread(void *arg) {
    (void)arg;
    uint8_t *buf = NULL, meta[65536];
    size_t cap = 0;
    int64_t pts = 0;
    while (atomic_load(&g_run)) {
        int fd = connect_to(g_host, g_port);
        if (fd < 0) {
            sleep(1);
            continue;
        }
        uint8_t hdr[4096];
        sntv_header h;
        uint32_t hsize = 0;
        if (read_exact(fd, hdr, SNTV_HEADER_PREFIX) != 0 || !(hsize = sntv_header_size(hdr)) ||
            (hsize > SNTV_HEADER_PREFIX && read_exact(fd, hdr + SNTV_HEADER_PREFIX, hsize - SNTV_HEADER_PREFIX) != 0) ||
            sntv_parse_header(hdr, &h) != 0) {
            LOGW("Invalid SNTV header from %s:%d", g_host, g_port);
            close(fd);
            sleep(1);
            continue;
        }
        LOGI("Connected to %s:%d: SNTV v%u, %ux%u %s, %.0f fps, %s, projection %u %.0f deg",
             g_host, g_port, h.version, h.width, h.height, h.codec == SNTV_CODEC_HEVC ? "HEVC" : "H.264",
             h.fps, h.layout == SNTV_LAYOUT_STEREO_SBS ? "stereo" : "mono", h.projection, h.fov_deg);
        pthread_mutex_lock(&g_lock);
        g_stats.connected = 1;
        g_stats.version = (int)h.version;
        g_stats.width = (int)h.width;
        g_stats.height = (int)h.height;
        g_stats.codec = (int)h.codec;
        g_stats.layout = (int)h.layout;
        g_stats.projection = (int)h.projection;
        g_stats.flags = (int)h.flags;
        g_stats.fps = h.fps;
        g_stats.fov_deg = h.fov_deg;
        g_stats.lens_c2 = h.lens_c2;
        pthread_mutex_unlock(&g_lock);
        if (start_codec((int)h.width, (int)h.height, (int)h.codec) != 0) {
            close(fd);
            sleep(2);
            continue;
        }

        const uint32_t fh_size = sntv_frame_header_size(&h);
        while (atomic_load(&g_run)) {
            uint8_t fhb[SNTV_FRAME_HEADER_SIZE];
            sntv_frame_header fh;
            if (read_exact(fd, fhb, fh_size) != 0 || sntv_parse_frame_header(fhb, &h, &fh) != 0) break;
            if (fh.meta_size && read_exact(fd, meta, fh.meta_size) != 0) break;
            if (fh.au_size > cap) {
                free(buf);
                cap = fh.au_size + fh.au_size / 2;
                buf = malloc(cap);
            }
            if (read_exact(fd, buf, fh.au_size) != 0) break;
            int64_t arrival = now_us();
            pthread_mutex_lock(&g_lock);
            g_stats.frames_received++;
            g_stats.bytes_received += fh.au_size;
            pthread_mutex_unlock(&g_lock);

            // No dropping here: P frames depend on the previous one. Wait for a free buffer.
            ssize_t in = -1;
            while (atomic_load(&g_run) && (in = AMediaCodec_dequeueInputBuffer(g_codec, 20000)) < 0) {}
            if (in < 0) break;
            size_t bsz = 0;
            uint8_t *dst = AMediaCodec_getInputBuffer(g_codec, (size_t)in, &bsz);
            if (!dst || bsz < fh.au_size) {
                LOGW("Input buffer too small (%zu < %u)", bsz, fh.au_size);
                AMediaCodec_queueInputBuffer(g_codec, (size_t)in, 0, 0, pts, 0);
                continue;
            }
            memcpy(dst, buf, fh.au_size);
            // pts in microseconds, in whole milliseconds so it can index the ring.
            pts += 1000;
            pts_slot *slot = &g_pts[(pts / 1000) % PTS_RING];
            slot->arrival_us = arrival;
            slot->info.timestamp_ns = (long long)fh.timestamp_ns;
            sntv_view_pose poses[2];
            slot->info.n_poses = fh.meta_size ? (int)sntv_read_view_poses(meta, fh.meta_size, poses, 2) : 0;
            for (int v = 0; v < slot->info.n_poses; ++v) {
                const float p[7] = {poses[v].px, poses[v].py, poses[v].pz, poses[v].qw, poses[v].qx, poses[v].qy, poses[v].qz};
                memcpy(slot->info.poses[v], p, sizeof p);
            }
            AMediaCodec_queueInputBuffer(g_codec, (size_t)in, 0, fh.au_size, pts, 0);
        }
        LOGW("Video connection closed; retrying");
        close(fd);
        stop_codec();
        pthread_mutex_lock(&g_lock);
        g_stats.connected = 0;
        pthread_mutex_unlock(&g_lock);
    }
    free(buf);
    return NULL;
}

// ---------------------------------------------------------------------------------- render (Unity's render thread)
static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_eglGetNativeClientBuffer;
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImage;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2D;

static int g_gl_ready;
static GLuint g_prog, g_vao, g_fbo, g_oes;
static GLuint g_target_tex;
static int g_target_w, g_target_h;
static GLuint g_bound_target;   // texture attached to the FBO

// Images in flight: the one just drawn and the previous two, with their EGLImage and fence.
typedef struct { AImage *img; EGLImageKHR egl; GLsync fence; } inflight;
static inflight g_inflight[3];

static const char *k_vs =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main(){\n"
    "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "  uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// The Unity texture is sRGB: GLES encodes to sRGB on write, so decode first and the stored
// values are the video's. v flipped: decoder images run top to bottom, Unity textures bottom
// to top.
static const char *k_fs =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES tex;\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "vec3 to_linear(vec3 c){ return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }\n"
    "void main(){\n"
    "  color = vec4(to_linear(texture(tex, vec2(uv.x, 1.0 - uv.y)).rgb), 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        LOGW("Shader: %s", log);
    }
    return s;
}

static int gl_init(void) {
    p_eglGetNativeClientBuffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    p_eglCreateImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTargetTexture2D = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!p_eglGetNativeClientBuffer || !p_eglCreateImage || !p_eglDestroyImage || !p_glEGLImageTargetTexture2D) {
        LOGW("Missing EGL/GLES extensions to import AHardwareBuffer (Unity must run on OpenGL ES 3)");
        return -1;
    }
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, k_vs));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, k_fs));
    glLinkProgram(g_prog);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOGW("The copy program does not link");
        return -1;
    }
    glGenVertexArrays(1, &g_vao);
    glGenFramebuffers(1, &g_fbo);
    glGenTextures(1, &g_oes);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_oes);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    LOGI("GL ready to import decoder frames");
    return 0;
}

static void release_inflight(inflight *f) {
    if (!f->img) return;
    if (f->fence) {
        glClientWaitSync(f->fence, 0, 50000000);   // 50 ms at most: it should be long done
        glDeleteSync(f->fence);
    }
    if (f->egl != EGL_NO_IMAGE_KHR) p_eglDestroyImage(eglGetCurrentDisplay(), f->egl);
    AImage_delete(f->img);
    memset(f, 0, sizeof *f);
}

static void draw_latest(void) {
    if (!g_reader || !g_target_tex) return;
    AImage *img = NULL;
    if (AImageReader_acquireLatestImage(g_reader, &img) != AMEDIA_OK || !img) return;   // no new frame
    int64_t t0 = now_us();

    AHardwareBuffer *ahb = NULL;
    AImage_getHardwareBuffer(img, &ahb);
    EGLClientBuffer cb = p_eglGetNativeClientBuffer(ahb);
    EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR egl = p_eglCreateImage(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
    if (egl == EGL_NO_IMAGE_KHR) {
        AImage_delete(img);
        LOGW("eglCreateImageKHR failed (0x%x)", eglGetError());
        return;
    }

    // Save the GL state this draw touches: Unity does not restore it.
    GLint fb, prog, vao, active, tex0, vp[4], sampler0;
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
              scissor = glIsEnabled(GL_SCISSOR_TEST), stencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean mask[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &tex0);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
    if (g_bound_target != g_target_tex) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_target_tex, 0);
        g_bound_target = g_target_tex;
        GLenum st = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) LOGW("Incomplete FBO (0x%x) with texture %u", st, g_target_tex);
    }
    glViewport(0, 0, g_target_w, g_target_h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_oes);
    p_glEGLImageTargetTexture2D(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)egl);
    glUniform1i(glGetUniformLocation(g_prog, "tex"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // Restore.
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, (GLuint)tex0);
    glBindSampler(0, (GLuint)sampler0);
    glActiveTexture((GLenum)active);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)fb);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);

    // The image is released two frames later, after its fence: the GPU may still be reading it.
    release_inflight(&g_inflight[2]);
    g_inflight[2] = g_inflight[1];
    g_inflight[1] = g_inflight[0];
    g_inflight[0] = (inflight){img, egl, fence};

    // The image's timestamp is its pts in ns: it finds the metadata it arrived with.
    int64_t ts_ns = 0;
    AImage_getTimestamp(img, &ts_ns);
    const pts_slot *slot = &g_pts[(ts_ns / 1000000) % PTS_RING];

    float ms = (now_us() - t0) / 1000.0f;
    pthread_mutex_lock(&g_lock);
    g_drawn = slot->info;
    g_stats.frames_drawn++;
    g_stats.draw_ms_avg = g_stats.draw_ms_avg * 0.95f + ms * 0.05f;
    pthread_mutex_unlock(&g_lock);
}

static void on_render_event(int event_id) {
    if (event_id != 1) return;
    if (!g_gl_ready) {
        if (gl_init() != 0) { g_gl_ready = -1; return; }
        g_gl_ready = 1;
    }
    if (g_gl_ready == 1) draw_latest();
}

// ---------------------------------------------------------------------------------- C API (used from C#)
EXPORT void sntv_start(const char *host, int port) {
    if (atomic_load(&g_run)) return;
    strncpy(g_host, host, sizeof g_host - 1);
    g_port = port;
    atomic_store(&g_run, 1);
    pthread_create(&g_net_thread, NULL, net_thread, NULL);
    pthread_create(&g_out_thread, NULL, out_thread, NULL);
    LOGI("Receiver started, sender %s:%d", g_host, g_port);
}

EXPORT void sntv_stop(void) {
    if (!atomic_load(&g_run)) return;
    atomic_store(&g_run, 0);
    pthread_join(g_net_thread, NULL);
    pthread_join(g_out_thread, NULL);
}

// Unity texture (GetNativeTexturePtr, a GL texture name) every frame is drawn into.
EXPORT void sntv_set_target(int gl_texture, int width, int height) {
    g_target_tex = (GLuint)gl_texture;
    g_target_w = width;
    g_target_h = height;
}

EXPORT void sntv_get_stats(sntv_stats *out) {
    pthread_mutex_lock(&g_lock);
    *out = g_stats;
    pthread_mutex_unlock(&g_lock);
}

EXPORT void sntv_get_drawn_frame(sntv_frame_info *out) {
    pthread_mutex_lock(&g_lock);
    *out = g_drawn;
    pthread_mutex_unlock(&g_lock);
}

typedef void (*render_event_fn)(int);
EXPORT render_event_fn sntv_get_render_event_func(void) { return on_render_event; }
