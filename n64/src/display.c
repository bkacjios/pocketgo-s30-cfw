/*
 * Display output for the PocketGo S30: KMS (sun4i-drm, DSI panel) + GBM + EGL
 * with GLES2 on lima.
 *
 * The panel is 320x480 portrait, mounted landscape. The game renders 3D into
 * an offscreen framebuffer the size of the landscape screen (S30_FB_W x
 * S30_FB_H; widescreen, see gfx.c) and 2D rectangles into a native-resolution
 * layer (S30_2D_W x S30_2D_H) that is composited onto it with sharp-bilinear
 * scaling whenever 3D follows 2D and at frame end. The image is drawn
 * rotated onto the panel, then page-flipped. Frames are
 * paced to the game's update rate in panel refreshes (S30Display_SetUpdateRate:
 * 3 = 20 fps in play, 2 = 30 fps in the pause menu, 1 = 60 fps on the logo).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "s30_display.h"
#include "s30_gfx.h"

static void apply_refresh(drmModeModeInfo* m, const drmModeConnector* conn);

/* Debug environment variables: <prefix>_<name> (S30_SetEnvPrefix, "S30" until set) */
static char sEnvPrefix[16] = "S30";

void S30_SetEnvPrefix(const char* prefix) {
    snprintf(sEnvPrefix, sizeof(sEnvPrefix), "%s", prefix != NULL ? prefix : "S30");
}

const char* S30_Getenv(const char* name) {
    char var[64];

    snprintf(var, sizeof(var), "%s_%s", sEnvPrefix, name);
    return getenv(var);
}

/* /tmp/<prefix>_shot.ppm<suffix> */
static const char* shot_path(const char* suffix) {
    static char path[64];
    int i, n = snprintf(path, sizeof(path), "/tmp/%s", sEnvPrefix);

    for (i = 5; i < n; i++) {
        path[i] = (char)tolower((unsigned char)path[i]);
    }
    snprintf(path + n, sizeof(path) - n, "_shot.ppm%s", suffix);
    return path;
}

static int sVblanksPerFrame = 3;

void S30Display_SetUpdateRate(int vblanks) {
    sVblanksPerFrame = vblanks < 1 ? 1 : vblanks > 3 ? 3 : vblanks;
}

static struct {
    int fd;
    drmModeModeInfo mode;
    uint32_t connectorId;
    uint32_t crtcId;
    struct gbm_device* gbm;
    struct gbm_surface* surface;
    struct gbm_bo* curBo;     // on screen
    struct gbm_bo* pendingBo; // flip queued, not yet on screen
    EGLDisplay eglDisplay;
    EGLContext eglContext;
    EGLSurface eglSurface;
    GLuint fbo;
    GLuint fbo2d;
    GLuint tex2d;
    int dirty2d;   // the 2D layer holds draws not yet composited
    int bound;     // 0 none, 1 3D, 2 2D
    GLuint compProgram;
    GLint compPos, compUv;
    GLuint fboTex;
    GLuint fboDepth;
    GLuint blitProgram;
    GLint blitPos;
    GLint blitUv;
    GLuint bgTex; // pause background: a copy of the 3D image
    GLuint bgFbo;
    GLuint blurTex; // motion blur: the previous frame
    GLuint blurFbo;
    GLuint monoSrcTex; // VisMono source: a copy of the 3D image
    GLuint monoSrcFbo;
    GLuint monoProgram;
    GLint monoPos, monoUv, monoPrim, monoEnv;
    GLuint probeProgram;
    GLint probePos, probeZ;
    uint32_t lastFlipSeq;
} sDisp;

static void die(const char* what) {
    fprintf(stderr, "display: %s failed (%s)\n", what, strerror(errno));
    exit(1);
}

/* The scanout device is the DRM card with a connected connector (card0 is the
 * lima render node on this board, card1 sun4i-drm). */
static void open_kms(void) {
    char path[32];
    int i;

    for (i = 0; i < 4; i++) {
        drmModeRes* res;
        int c;

        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        sDisp.fd = open(path, O_RDWR | O_CLOEXEC);
        if (sDisp.fd < 0) {
            continue;
        }
        res = drmModeGetResources(sDisp.fd);
        if (res == NULL) {
            close(sDisp.fd);
            continue;
        }
        for (c = 0; c < res->count_connectors; c++) {
            drmModeConnector* conn = drmModeGetConnector(sDisp.fd, res->connectors[c]);
            if (conn != NULL && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
                drmModeEncoder* enc = drmModeGetEncoder(sDisp.fd, conn->encoder_id ? conn->encoder_id
                                                                                    : conn->encoders[0]);
                sDisp.mode = conn->modes[0];
                apply_refresh(&sDisp.mode, conn);
                sDisp.connectorId = conn->connector_id;
                sDisp.crtcId = (enc != NULL && enc->crtc_id) ? enc->crtc_id : res->crtcs[0];
                drmModeFreeEncoder(enc);
                drmModeFreeConnector(conn);
                drmModeFreeResources(res);
                printf("display: %s, %ux%u@%u\n", path, sDisp.mode.hdisplay, sDisp.mode.vdisplay,
                       sDisp.mode.vrefresh);
                return;
            }
            drmModeFreeConnector(conn);
        }
        drmModeFreeResources(res);
        close(sDisp.fd);
    }
    fprintf(stderr, "display: no connected DRM output\n");
    exit(1);
}

static void init_egl(void) {
    static const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE,
    };
    static const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLConfig configs[64];
    EGLint numConfigs;
    EGLConfig config = NULL;
    int i;

    sDisp.gbm = gbm_create_device(sDisp.fd);
    if (sDisp.gbm == NULL) {
        die("gbm_create_device");
    }
    sDisp.surface = gbm_surface_create(sDisp.gbm, sDisp.mode.hdisplay, sDisp.mode.vdisplay,
                                       GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (sDisp.surface == NULL) {
        die("gbm_surface_create");
    }
    sDisp.eglDisplay = eglGetDisplay((EGLNativeDisplayType)sDisp.gbm);
    if (!eglInitialize(sDisp.eglDisplay, NULL, NULL)) {
        die("eglInitialize");
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    /* The window surface needs the config whose visual matches the GBM format */
    eglChooseConfig(sDisp.eglDisplay, configAttribs, configs, 64, &numConfigs);
    for (i = 0; i < numConfigs; i++) {
        EGLint id;
        eglGetConfigAttrib(sDisp.eglDisplay, configs[i], EGL_NATIVE_VISUAL_ID, &id);
        if (id == GBM_FORMAT_XRGB8888) {
            config = configs[i];
            break;
        }
    }
    if (config == NULL) {
        fprintf(stderr, "display: no EGL config for XRGB8888\n");
        exit(1);
    }
    sDisp.eglContext = eglCreateContext(sDisp.eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    sDisp.eglSurface = eglCreateWindowSurface(sDisp.eglDisplay, config, (EGLNativeWindowType)sDisp.surface, NULL);
    if (sDisp.eglContext == EGL_NO_CONTEXT || sDisp.eglSurface == EGL_NO_SURFACE) {
        die("eglCreate*");
    }
    eglMakeCurrent(sDisp.eglDisplay, sDisp.eglSurface, sDisp.eglSurface, sDisp.eglContext);
    printf("display: GL %s, %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
}

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    GLint ok;

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "display: shader: %s\n", log);
        exit(1);
    }
    return s;
}

GLuint S30Display_LinkProgram(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    GLint ok;

    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "display: link: %s\n", log);
        exit(1);
    }
    return p;
}

static void init_fbo(void) {
    glGenTextures(1, &sDisp.fboTex);
    glBindTexture(GL_TEXTURE_2D, sDisp.fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, S30_FB_W, S30_FB_H, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &sDisp.fboDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, sDisp.fboDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, S30_FB_W, S30_FB_H);

    glGenFramebuffers(1, &sDisp.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, sDisp.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sDisp.fboTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sDisp.fboDepth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "display: offscreen framebuffer incomplete\n");
        exit(1);
    }

    sDisp.blitProgram = S30Display_LinkProgram(
        "attribute vec2 aPos; attribute vec2 aUv; varying vec2 vUv;\n"
        "void main() { vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }\n",
        "precision mediump float; varying vec2 vUv; uniform sampler2D uTex;\n"
        "void main() { gl_FragColor = texture2D(uTex, vUv); }\n");
    sDisp.blitPos = glGetAttribLocation(sDisp.blitProgram, "aPos");
    sDisp.blitUv = glGetAttribLocation(sDisp.blitProgram, "aUv");

    // 2D layer: RGBA, no depth
    glGenTextures(1, &sDisp.tex2d);
    glBindTexture(GL_TEXTURE_2D, sDisp.tex2d);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, S30_2D_W, S30_2D_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &sDisp.fbo2d);
    glBindFramebuffer(GL_FRAMEBUFFER, sDisp.fbo2d);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sDisp.tex2d, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "display: 2D layer framebuffer incomplete\n");
        exit(1);
    }

    /* Composite: sharp bilinear (texels stay crisp, only the pixel straddling
       a texel boundary is blended), premultiplied alpha */
    sDisp.compProgram = S30Display_LinkProgram(
        "attribute vec2 aPos; attribute vec2 aUv; varying vec2 vUv;\n"
        "void main() { vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }\n",
        "precision mediump float; varying vec2 vUv; uniform sampler2D uTex;\n"
        "const vec2 size = vec2(360.0, 240.0);\n" // S30_2D_W x S30_2D_H
        "const vec2 scale = vec2(480.0 / 360.0, 320.0 / 240.0);\n"
        "void main() {\n"
        "  vec2 texel = vUv * size;\n"
        "  vec2 d = fract(texel) - 0.5;\n"
        "  vec2 r = 0.5 - 0.5 / scale;\n"
        "  vec2 f = (d - clamp(d, -r, r)) * scale + 0.5;\n"
        "  gl_FragColor = texture2D(uTex, (floor(texel) + f) / size);\n"
        "}\n");
    sDisp.compPos = glGetAttribLocation(sDisp.compProgram, "aPos");
    sDisp.compUv = glGetAttribLocation(sDisp.compProgram, "aUv");
}

/* Refresh rate (S30Display_SetRefresh, <prefix>_REFRESH): the panel's listed
   mode with that rate (the panel driver lists 60, 61.8, 59.77 and 50 Hz), else
   the preferred mode with the vertical front porch changed to get it. The
   pixel clock is fixed (PLL-MIPI) and so is the line, so the rate is set by
   the number of lines. 0: the preferred mode. */
static float sWantedRefresh;

void S30Display_SetRefresh(float hz) {
    sWantedRefresh = hz;
}

static double mode_hz(const drmModeModeInfo* m) {
    return m->htotal && m->vtotal ? m->clock * 1000.0 / ((double)m->htotal * m->vtotal) : 0.0;
}

static void apply_refresh(drmModeModeInfo* m, const drmModeConnector* conn) {
    const char* env = S30_Getenv("REFRESH");
    float hz = env != NULL ? (float)atof(env) : sWantedRefresh;
    long vtotal;
    int delta, i;

    if (hz <= 0.0f || m->htotal == 0) {
        return;
    }
    for (i = 0; i < conn->count_modes; i++) {
        const drmModeModeInfo* c = &conn->modes[i];

        if (c->hdisplay == m->hdisplay && c->vdisplay == m->vdisplay && fabs(mode_hz(c) - hz) < 0.1) {
            *m = *c;
            printf("display: mode %ux%u@%.2f (listed)\n", m->hdisplay, m->vdisplay, mode_hz(m));
            return;
        }
    }
    vtotal = lroundf(m->clock * 1000.0f / (m->htotal * hz));
    if (vtotal < m->vsync_end - m->vsync_start + (m->vtotal - m->vsync_end) + m->vdisplay + 2 || vtotal > 4095) {
        printf("display: %.2f Hz is out of the panel's range, %u Hz kept\n", hz, m->vrefresh);
        return;
    }
    delta = (int)(vtotal - m->vtotal);
    m->vsync_start += delta;
    m->vsync_end += delta;
    m->vtotal = vtotal;
    m->vrefresh = lroundf(m->clock * 1000.0f / (m->htotal * m->vtotal));
    snprintf(m->name, sizeof(m->name), "%ux%u@%.2f", m->hdisplay, m->vdisplay,
             m->clock * 1000.0 / (m->htotal * m->vtotal));
    printf("display: mode %s (vtotal %u)\n", m->name, m->vtotal);
}

/* <prefix>_REFRESHTEST: the panel's measured rate after the mode is set */
static void measure_refresh(void) {
    drmVBlank vbl;
    struct timespec t0, t1;
    int i;

    memset(&vbl, 0, sizeof(vbl));
    vbl.request.type = DRM_VBLANK_RELATIVE;
    vbl.request.sequence = 1;
    drmWaitVBlank(sDisp.fd, &vbl);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < 120; i++) {
        memset(&vbl, 0, sizeof(vbl));
        vbl.request.type = DRM_VBLANK_RELATIVE;
        vbl.request.sequence = 1;
        drmWaitVBlank(sDisp.fd, &vbl);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("display: measured %.3f Hz\n",
           120.0 / ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9));
}

void S30Display_Init(void) {
    open_kms();
    init_egl();
    init_fbo();
}

static void composite_2d(void) {
    static const GLfloat quad[] = { -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1 };

    glBindFramebuffer(GL_FRAMEBUFFER, sDisp.fbo);
    glViewport(0, 0, S30_FB_W, S30_FB_H);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(sDisp.compProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sDisp.tex2d);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(sDisp.compPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(sDisp.compUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glEnableVertexAttribArray(sDisp.compPos);
    glEnableVertexAttribArray(sDisp.compUv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Clear the layer for the 2D drawn after the next 3D
    glBindFramebuffer(GL_FRAMEBUFFER, sDisp.fbo2d);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    sDisp.dirty2d = 0;
    sDisp.bound = 0;
}

/* Draws tex over the whole bound framebuffer (S30_FB_W x S30_FB_H), opaque */
static void blit(GLuint tex) {
    static const GLfloat quad[] = { -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1 };

    glViewport(0, 0, S30_FB_W, S30_FB_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_BLEND);
    glUseProgram(sDisp.blitProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(sDisp.blitPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(sDisp.blitUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glEnableVertexAttribArray(sDisp.blitPos);
    glEnableVertexAttribArray(sDisp.blitUv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(sDisp.blitPos);
    glDisableVertexAttribArray(sDisp.blitUv);
}

/* Copies the 3D image (pending 2D composited in) into *tex, creating it and
   its framebuffer *fbo on first use */
static void copy_frame(GLuint* tex, GLuint* fbo) {
    if (sDisp.dirty2d) {
        composite_2d();
    }
    if (*fbo == 0) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, S30_FB_W, S30_FB_H, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    sDisp.bound = 0;
    blit(sDisp.fboTex);
}

/* A rectangle of the 3D image (x0, y0)-(x1, y1), framebuffer pixels from the
   top left, drawn into *tex (w x h, created with its *fbo on first use) with
   its top row first, as texture data is laid out */
void S30Display_CopyRect(GLuint* tex, GLuint* fbo, int w, int h, float x0, float y0, float x1, float y1) {
    float u0 = x0 / S30_FB_W, u1 = x1 / S30_FB_W;
    float vTop = 1.0f - y0 / S30_FB_H, vBottom = 1.0f - y1 / S30_FB_H;
    // Target row 0 (NDC y -1) is the source's top
    GLfloat quad[] = { -1, -1, u0, vTop, 1, -1, u1, vTop, -1, 1, u0, vBottom, 1, 1, u1, vBottom };

    if (*fbo == 0) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    sDisp.bound = 0;
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_BLEND);
    glUseProgram(sDisp.blitProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sDisp.fboTex);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(sDisp.blitPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(sDisp.blitUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glEnableVertexAttribArray(sDisp.blitPos);
    glEnableVertexAttribArray(sDisp.blitUv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(sDisp.blitPos);
    glDisableVertexAttribArray(sDisp.blitUv);
}

void S30Display_SaveBackground(void) {
    copy_frame(&sDisp.bgTex, &sDisp.bgFbo);
}

void S30Display_RestoreBackground(void) {
    S30Display_Bind3D();
    if (sDisp.bgFbo != 0) {
        blit(sDisp.bgTex);
    }
}

/* Motion blur (z_play.c Play_DrawMotionBlur): the previous frame, saved by
   S30Display_MotionBlurSave, blended over this one at alpha; the result is
   saved next, so the blur trails off over frames as on the N64 */
void S30Display_MotionBlurBlend(float alpha) {
    if (sDisp.blurFbo == 0) {
        return;
    }
    S30Display_Bind3D();
    if (sDisp.dirty2d) {
        composite_2d();
        S30Display_Bind3D();
    }
    // blit() with blending: the texture at constant alpha over the frame
    {
        static const GLfloat quad[] = { -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1 };

        glViewport(0, 0, S30_FB_W, S30_FB_H);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glEnable(GL_BLEND);
        glBlendColor(0.0f, 0.0f, 0.0f, alpha);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
        glUseProgram(sDisp.blitProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sDisp.blurTex);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(sDisp.blitPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
        glVertexAttribPointer(sDisp.blitUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
        glEnableVertexAttribArray(sDisp.blitPos);
        glEnableVertexAttribArray(sDisp.blitUv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(sDisp.blitPos);
        glDisableVertexAttribArray(sDisp.blitUv);
        glDisable(GL_BLEND);
    }
}

void S30Display_MotionBlurSave(void) {
    copy_frame(&sDisp.blurTex, &sDisp.blurFbo);
    S30Display_Bind3D();
}

/* VisMono (z_vismono.c): the frame's intensity (R:G:B weighted 2:4:1) picks
   a color between env and prim, blended over the frame by prim alpha */
void S30Display_VisMono(const float prim[4], const float env[4]) {
    static const GLfloat quad[] = { -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1 };

    copy_frame(&sDisp.monoSrcTex, &sDisp.monoSrcFbo);
    if (sDisp.monoProgram == 0) {
        sDisp.monoProgram = S30Display_LinkProgram(
            "attribute vec2 aPos; attribute vec2 aUv; varying vec2 vUv;\n"
            "void main() { vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }\n",
            "precision mediump float; varying vec2 vUv; uniform sampler2D uTex;\n"
            "uniform vec4 uPrim; uniform vec3 uEnv;\n"
            "void main() {\n"
            "  vec3 c = texture2D(uTex, vUv).rgb;\n"
            "  float i = dot(c, vec3(2.0, 4.0, 1.0) / 7.0);\n"
            "  gl_FragColor = vec4(mix(c, mix(uEnv, uPrim.rgb, i), uPrim.a), 1.0);\n"
            "}\n");
        sDisp.monoPos = glGetAttribLocation(sDisp.monoProgram, "aPos");
        sDisp.monoUv = glGetAttribLocation(sDisp.monoProgram, "aUv");
        sDisp.monoPrim = glGetUniformLocation(sDisp.monoProgram, "uPrim");
        sDisp.monoEnv = glGetUniformLocation(sDisp.monoProgram, "uEnv");
    }
    S30Display_Bind3D();
    glViewport(0, 0, S30_FB_W, S30_FB_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_BLEND);
    glUseProgram(sDisp.monoProgram);
    glUniform4fv(sDisp.monoPrim, 1, prim);
    glUniform3fv(sDisp.monoEnv, 1, env);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sDisp.monoSrcTex);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(sDisp.monoPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(sDisp.monoUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glEnableVertexAttribArray(sDisp.monoPos);
    glEnableVertexAttribArray(sDisp.monoUv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(sDisp.monoPos);
    glDisableVertexAttribArray(sDisp.monoUv);
}

/* Depth probes (the graph callback's sun and light glow tests): queued
   during a frame, drawn at its end against its depth buffer (each pixel
   cleared, then a full-screen quad at the probe's depth through a 1-pixel
   scissor), and gathered into one row of a small texture, read at the next
   frame's start when that frame is complete: no GPU stall. */
#define MAX_PROBES 32

static struct {
    int x, y;
    float z;
    int slot;
} sProbes[MAX_PROBES];
static int sNumProbes;
/* Results are gathered into one of two 1-row targets, alternating by frame,
   and read back from the other one: the frame before last, which the GPU
   has finished (EndFrame waited for its flip), so glReadPixels doesn't stall
   on the frame still rendering. Results arrive two frames late. */
static int sProbeSlots[2]; // result slots drawn into each target
static int sProbeCur;      // target the next draw_probes gathers into
static GLuint sProbeTex[2], sProbeFbo[2], sProbeProgram, sGatherProgram;
static GLint sProbePos, sProbeZ, sGatherPos, sGatherUv;

void S30Display_QueueProbe(int slot, int x, int y, float z) {
    if (sNumProbes < MAX_PROBES && slot < MAX_PROBES) {
        sProbes[sNumProbes].x = x;
        sProbes[sNumProbes].y = y;
        sProbes[sNumProbes].z = z;
        sProbes[sNumProbes].slot = slot;
        sNumProbes++;
    }
}

static void draw_probes(void) {
    static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLfloat pts[MAX_PROBES * 4];
    int i;

    sProbeCur ^= 1;
    sProbeSlots[sProbeCur] = 0;
    if (sNumProbes == 0) {
        return;
    }
    if (sProbeProgram == 0) {
        sProbeProgram = S30Display_LinkProgram("attribute vec2 aPos; uniform float uZ;\n"
                                               "void main() { gl_Position = vec4(aPos, uZ, 1.0); }\n",
                                               "precision mediump float;\n"
                                               "void main() { gl_FragColor = vec4(1.0); }\n");
        sProbePos = glGetAttribLocation(sProbeProgram, "aPos");
        sProbeZ = glGetUniformLocation(sProbeProgram, "uZ");
        sGatherProgram = S30Display_LinkProgram(
            "attribute vec2 aPos; attribute vec2 aUv; varying vec2 vUv;\n"
            "void main() { vUv = aUv; gl_PointSize = 1.0; gl_Position = vec4(aPos, 0.0, 1.0); }\n",
            "precision mediump float; varying vec2 vUv; uniform sampler2D uTex;\n"
            "void main() { gl_FragColor = texture2D(uTex, vUv); }\n");
        sGatherPos = glGetAttribLocation(sGatherProgram, "aPos");
        sGatherUv = glGetAttribLocation(sGatherProgram, "aUv");
        glGenTextures(2, sProbeTex);
        glGenFramebuffers(2, sProbeFbo);
        for (i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, sProbeTex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MAX_PROBES, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, sProbeFbo[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sProbeTex[i], 0);
        }
    }

    // Depth tests in the (already presented) 3D image
    glBindFramebuffer(GL_FRAMEBUFFER, sDisp.fbo);
    glViewport(0, 0, S30_FB_W, S30_FB_H);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glClearColor(0, 0, 0, 1);
    glUseProgram(sProbeProgram);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(sProbePos, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), quad);
    glEnableVertexAttribArray(sProbePos);
    for (i = 0; i < sNumProbes; i++) {
        glScissor(sProbes[i].x, sProbes[i].y, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1f(sProbeZ, sProbes[i].z);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisableVertexAttribArray(sProbePos);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);

    // Gather: result of probe i to texel (slot, 0); slots not probed read 0
    glBindFramebuffer(GL_FRAMEBUFFER, sProbeFbo[sProbeCur]);
    glViewport(0, 0, MAX_PROBES, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < sNumProbes; i++) {
        pts[i * 4 + 0] = (sProbes[i].slot + 0.5f) * 2.0f / MAX_PROBES - 1.0f;
        pts[i * 4 + 1] = 0.0f;
        pts[i * 4 + 2] = (sProbes[i].x + 0.5f) / S30_FB_W;
        pts[i * 4 + 3] = (sProbes[i].y + 0.5f) / S30_FB_H;
        if (sProbes[i].slot + 1 > sProbeSlots[sProbeCur]) {
            sProbeSlots[sProbeCur] = sProbes[i].slot + 1;
        }
    }
    glUseProgram(sGatherProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sDisp.fboTex);
    glVertexAttribPointer(sGatherPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), pts);
    glVertexAttribPointer(sGatherUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), pts + 2);
    glEnableVertexAttribArray(sGatherPos);
    glEnableVertexAttribArray(sGatherUv);
    glDrawArrays(GL_POINTS, 0, sNumProbes);
    glDisableVertexAttribArray(sGatherPos);
    glDisableVertexAttribArray(sGatherUv);
    sNumProbes = 0;
}

int S30Display_ReadProbes(unsigned char* out, int max) {
    unsigned char px[MAX_PROBES * 4];
    int prev = sProbeCur ^ 1; // gathered the frame before last
    int i, n = sProbeSlots[prev] < max ? sProbeSlots[prev] : max;

    if (n == 0) {
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, sProbeFbo[prev]);
    sDisp.bound = 0;
    glReadPixels(0, 0, n, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (i = 0; i < n; i++) {
        out[i] = px[i * 4] > 127;
    }
    return n;
}

void S30Display_Unbind(void) {
    sDisp.bound = 0;
}

void S30Display_BeginFrame(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, sDisp.fbo2d);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    sDisp.dirty2d = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, sDisp.fbo);
    glViewport(0, 0, S30_FB_W, S30_FB_H);
    sDisp.bound = 1;
}

int S30Display_Bind3D(void) {
    int composited = sDisp.dirty2d;

    if (sDisp.dirty2d) {
        composite_2d();
    }
    if (sDisp.bound != 1) {
        glBindFramebuffer(GL_FRAMEBUFFER, sDisp.fbo);
        sDisp.bound = 1;
    }
    return composited;
}

void S30Display_Bind2D(void) {
    if (sDisp.bound != 2) {
        glBindFramebuffer(GL_FRAMEBUFFER, sDisp.fbo2d);
        sDisp.bound = 2;
    }
    sDisp.dirty2d = 1;
}

/* Frames shown later than the update rate asks, and the refreshes lost (for
   the gfx stats) */
static int sLateFrames, sLostVblanks;

void S30Display_TakeLateFrames(int* frames, int* vblanks) {
    *frames = sLateFrames;
    *vblanks = sLostVblanks;
    sLateFrames = sLostVblanks = 0;
}

static void page_flip_handler(int fd, unsigned int seq, unsigned int sec, unsigned int usec, void* data) {
    if (sDisp.lastFlipSeq != 0 && (int)(seq - sDisp.lastFlipSeq) > sVblanksPerFrame) {
        sLateFrames++;
        sLostVblanks += (int)(seq - sDisp.lastFlipSeq) - sVblanksPerFrame;
    }
    sDisp.lastFlipSeq = seq;
    (void)data;
    if (sDisp.pendingBo != NULL) {
        gbm_surface_release_buffer(sDisp.surface, sDisp.curBo);
        sDisp.curBo = sDisp.pendingBo;
        sDisp.pendingBo = NULL;
    }
}

static void drm_fb_destroy(struct gbm_bo* bo, void* data) {
    uint32_t fbId = (uint32_t)(uintptr_t)data;

    drmModeRmFB(sDisp.fd, fbId);
}

static uint32_t bo_fb(struct gbm_bo* bo) {
    uint32_t fbId = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);

    if (fbId == 0) {
        uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 };
        uint32_t strides[4] = { gbm_bo_get_stride(bo) };
        uint32_t offsets[4] = { 0 };

        if (drmModeAddFB2(sDisp.fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), GBM_FORMAT_XRGB8888,
                          handles, strides, offsets, &fbId, 0) != 0) {
            die("drmModeAddFB2");
        }
        gbm_bo_set_user_data(bo, (void*)(uintptr_t)fbId, drm_fb_destroy);
    }
    return fbId;
}

/* Debug: <PREFIX>_SHOT=N saves the offscreen image every N frames to
   /tmp/<prefix>_shot.ppm (bottom-up rows flipped to top-down); SIGUSR1 saves the
   next frame */
static volatile sig_atomic_t sShotRequested;

static void on_sigusr1(int sig) {
    (void)sig;
    sShotRequested = 1;
}

static void maybe_screenshot(void) {
    static int every = -1, frame;
    static unsigned char px[S30_FB_W * S30_FB_H * 4];
    FILE* f;
    int y;

    if (every < 0) {
        every = S30_Getenv("SHOT") ? atoi(S30_Getenv("SHOT")) : 0;
        signal(SIGUSR1, on_sigusr1);
    }
    frame++;
    if (!sShotRequested && (every <= 0 || frame % every != 0)) {
        return;
    }
    sShotRequested = 0;
    glReadPixels(0, 0, S30_FB_W, S30_FB_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    f = fopen(shot_path(".tmp"), "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", S30_FB_W, S30_FB_H);
    for (y = S30_FB_H - 1; y >= 0; y--) {
        int x;
        for (x = 0; x < S30_FB_W; x++) {
            fwrite(&px[(y * S30_FB_W + x) * 4], 1, 3, f);
        }
    }
    fclose(f);
    {
        char tmp[64];

        snprintf(tmp, sizeof(tmp), "%s", shot_path(".tmp")); // shot_path's buffer is reused
        rename(tmp, shot_path(""));
    }
}

/* Wait for the queued flip, if any; the buffer it replaced goes back to the
   surface */
static void wait_flip(void) {
    drmEventContext ev = { .version = 2, .page_flip_handler = page_flip_handler };

    while (sDisp.pendingBo != NULL) {
        drmHandleEvent(sDisp.fd, &ev);
    }
}

void S30Display_EndFrame(void) {
    /* Landscape 480x320 view of the portrait panel, filled by the image. In
     * panel (portrait) NDC the landscape x axis runs along the panel's y
     * axis. */
    const float w = 1.0f;
    /* aPos (panel NDC), aUv: rotated 90 degrees (game top-left at panel
     * x = -1, y = -w) */
    const GLfloat quad[] = {
        -1.0f, -w, 0.0f, 1.0f,
        1.0f,  -w, 0.0f, 0.0f,
        -1.0f, w,  1.0f, 1.0f,
        1.0f,  w,  1.0f, 0.0f,
    };
    struct gbm_bo* bo;
    drmVBlank vbl;

    if (sDisp.dirty2d) {
        composite_2d();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, sDisp.fbo);
    maybe_screenshot();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    sDisp.bound = 0;
    glViewport(0, 0, sDisp.mode.hdisplay, sDisp.mode.vdisplay);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(sDisp.blitProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sDisp.fboTex);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(sDisp.blitPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(sDisp.blitUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glEnableVertexAttribArray(sDisp.blitPos);
    glEnableVertexAttribArray(sDisp.blitUv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(sDisp.blitPos);
    glDisableVertexAttribArray(sDisp.blitUv);
    glDisableVertexAttribArray(sDisp.compPos);
    glDisableVertexAttribArray(sDisp.compUv);
    draw_probes();

    eglSwapBuffers(sDisp.eglDisplay, sDisp.eglSurface);
    bo = gbm_surface_lock_front_buffer(sDisp.surface);

    if (sDisp.curBo == NULL) {
        if (drmModeSetCrtc(sDisp.fd, sDisp.crtcId, bo_fb(bo), 0, 0, &sDisp.connectorId, 1, &sDisp.mode) != 0) {
            die("drmModeSetCrtc");
        }
        if (S30_Getenv("REFRESHTEST") != NULL) {
            measure_refresh();
        }
        sDisp.curBo = bo;
    } else {
        /* The flip is queued and not waited for: the next frame's game logic
           and display list walk run while the GPU renders this one and the
           kernel flips to it (it waits for the rendering itself). The
           previous flip has to be done first: one flip at a time. */
        static int fast = -1;
        if (fast < 0) {
            fast = S30_Getenv("FAST") != NULL;
        }
        wait_flip();
        /* Hold the frame until sVblanksPerFrame refreshes after the last
           flip (<PREFIX>_FAST: run at panel rate, for tests) */
        if (!fast) {
            memset(&vbl, 0, sizeof(vbl));
            vbl.request.type = DRM_VBLANK_ABSOLUTE;
            vbl.request.sequence = sDisp.lastFlipSeq + sVblanksPerFrame - 1;
            drmWaitVBlank(sDisp.fd, &vbl);
        }
        if (drmModePageFlip(sDisp.fd, sDisp.crtcId, bo_fb(bo), DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0) {
            die("drmModePageFlip");
        }
        sDisp.pendingBo = bo;
    }
}
