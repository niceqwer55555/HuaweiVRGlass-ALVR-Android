/*
 * xr_display.c - 输出到 HUAWEI VR Glass（普通 HDMI 副屏）的合成器
 *
 * 渲染模型：
 *   ALVR 用 OpenXR swapchain 提交左右眼画面（GL 纹理）
 *   我们用自己的 EGL 上下文（与 ALVR 的上下文共享 share group）
 *   把两张纹理并排画到眼镜窗口的左右半屏。
 */
#include "xr_shim.h"

#include <stdlib.h>
#include <sys/stat.h>

static void cfg_load(void);
static bool init_gl_locked(void);

/* 我们的 EGLSurface 是“绑死在某个 ANativeWindow 上”的。Android 在下面这些
 * 时机都会把旧 Surface 销毁、换一个新的给我们：应用切后台再回来、VR 显示器
 * 重新挂载、串流重启导致宿主重建 Surface。
 * 旧 surface 一旦销毁，之后每帧 eglSwapBuffers 都返回 EGL_BAD_SURFACE(0x300D)，
 * 画面就永久冻结在最后一帧（历史上表现为“眼镜里一片灰/黑，但日志一切正常”）。
 * 所以这里记住 surface 是拿哪个 window 建的，发现 window 换了或 swap 报错就重建。 */
static EGLConfig s_own_cfg;               /* 我们自己 eglChooseConfig 出来的 */
static ANativeWindow *s_surf_win;         /* 当前 surface 绑定的窗口 */
static bool s_surf_dirty;                 /* 需要重建 */
static int s_surf_rebuilds;               /* 重建次数（观察用） */

typedef struct {
    int eye_res;       /* 每眼渲染分辨率 */
    float fov_deg;     /* 半角 */
    float ipd_m;
    int rotate;        /* 0/90/180/270 */
    int flip_v;
    float k1;          /* 桶形预畸变系数（自由曲面镜片需要） */
    float k2;
    int test_pattern;  /* 1 = 不显示 ALVR 图层，画内置测试图 */
    int skip_present;  /* 1 = 我们完全不碰窗口（验证 ALVR 是否自己在画） */
    int probe_swap;    /* 1 = 每秒把 swapchain 纹理读回来，判断是黑纹理还是画错 */
    int capture_alvr;  /* 1 = release 时收；2 = 整屏截图兜底；3 = acquire 时收上一只眼 */
    int mirror_eye;    /* 右眼额外变换：0=同左眼 1=再转180 2=上下翻 3=水平翻 4=再转90 5=再转270 */
    float eye_shift_mm;/* 每眼光心水平偏移（华为 SDK 实测 2.959mm），左右符号相反 */
    float eye_shift_y_mm;/* 每眼光心竖直偏移，左右符号相反 */
    float gain_r;      /* 逐通道增益，用于补偿光学偏色（默认 1.0） */
    float gain_g;
    float gain_b;
    int swap_eyes;     /* 1 = 交换左右眼 swapchain 纹理到相反视口 */
} disp_cfg_t;

static disp_cfg_t g_cfg = {1536, XR_FOV_HALF_DEG, XR_IPD_M, 0, 0, 0.0f, 0.0f, 0, 0, 0, 3, 0,
                           2.959f, 0.0f, 1.0f, 1.0f, 1.0f, 0};
/* 华为 SDK 日志里的 screenMeters=0.065088；每眼视口是半屏，故 UV 偏移乘 2 */
#define XR_SCREEN_METERS 0.065088f

int xr_cfg_eye_res(void) { return g_cfg.eye_res; }
float xr_cfg_fov_deg(void) { return g_cfg.fov_deg; }
float xr_cfg_ipd(void) { return g_cfg.ipd_m; }

#define CFG_FILE_EXT "/sdcard/Android/data/com.lht.huaweivr.alvr/files/xr.cfg"

/* 每 0.5 秒看一眼配置文件有没有被改过，改了就重载（方便实时调参/切测试图） */
void xr_cfg_tick(void) {
    static int64_t last_check;
    static int64_t last_mtime;
    int64_t now = xr_now_ns();
    if (now - last_check < 500000000LL) return;
    last_check = now;
    struct stat st;
    if (stat(CFG_FILE_EXT, &st) != 0) return;
    int64_t mt = (int64_t)st.st_mtime * 1000000000LL + (int64_t)st.st_size;
    if (mt == last_mtime) return;
    last_mtime = mt;
    cfg_load();
}

int xr_cfg_test_pattern(void) {
    xr_cfg_tick();
    return g_cfg.test_pattern;
}

int xr_cfg_skip_present(void) {
    xr_cfg_tick();
    return g_cfg.skip_present;
}

int xr_cfg_probe_swap(void) {
    xr_cfg_tick();
    return g_cfg.probe_swap;
}

int xr_cfg_capture_alvr(void) {
    xr_cfg_tick();
    return g_cfg.capture_alvr;
}

static void cfg_load(void) {
    const char *files[] = {
            "/sdcard/Android/data/com.lht.huaweivr.alvr/files/xr.cfg",
            "/data/data/com.lht.huaweivr.alvr/files/xr.cfg",
    };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(files[i], "r");
        if (!f) continue;
        char line[128];
        XLOG("CFG: 读取 %s", files[i]);
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = line;
            char *v = eq + 1;
            while (*k == ' ') k++;
            while (*v == ' ') v++;
            char *end = v + strlen(v);
            while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
            if (strcmp(k, "eye_res") == 0) g_cfg.eye_res = atoi(v);
            else if (strcmp(k, "fov_deg") == 0) g_cfg.fov_deg = (float)atof(v);
            else if (strcmp(k, "ipd_mm") == 0) g_cfg.ipd_m = (float)atof(v) / 1000.0f;
            else if (strcmp(k, "rotate") == 0) g_cfg.rotate = atoi(v);
            else if (strcmp(k, "flip_v") == 0) g_cfg.flip_v = atoi(v);
            else if (strcmp(k, "k1") == 0) g_cfg.k1 = (float)atof(v);
            else if (strcmp(k, "k2") == 0) g_cfg.k2 = (float)atof(v);
            else if (strcmp(k, "test_pattern") == 0) g_cfg.test_pattern = atoi(v);
            else if (strcmp(k, "skip_present") == 0) g_cfg.skip_present = atoi(v);
            else if (strcmp(k, "probe_swap") == 0) g_cfg.probe_swap = atoi(v);
            else if (strcmp(k, "capture_alvr") == 0) g_cfg.capture_alvr = atoi(v);
            else if (strcmp(k, "mirror_eye") == 0) g_cfg.mirror_eye = atoi(v);
            else if (strcmp(k, "eye_shift_mm") == 0) g_cfg.eye_shift_mm = (float)atof(v);
            else if (strcmp(k, "eye_shift_y_mm") == 0) g_cfg.eye_shift_y_mm = (float)atof(v);
            else if (strcmp(k, "gain_r") == 0) g_cfg.gain_r = (float)atof(v);
            else if (strcmp(k, "gain_g") == 0) g_cfg.gain_g = (float)atof(v);
            else if (strcmp(k, "gain_b") == 0) g_cfg.gain_b = (float)atof(v);
            else if (strcmp(k, "swap_eyes") == 0) g_cfg.swap_eyes = atoi(v);
            else XWARN("CFG: 未知键 %s", k);
        }
        fclose(f);
        break;
    }
    XLOG("CFG: eye_res=%d fov=%.1f ipd=%.1fmm rotate=%d flip_v=%d k1=%.3f k2=%.3f "
         "test=%d skip_present=%d probe_swap=%d capture_alvr=%d mirror_eye=%d "
         "eye_shift=(%.3f,%.3f)mm gain=(%.2f,%.2f,%.2f)",
         g_cfg.eye_res, g_cfg.fov_deg, g_cfg.ipd_m * 1000.0f, g_cfg.rotate, g_cfg.flip_v,
         g_cfg.k1, g_cfg.k2, g_cfg.test_pattern, g_cfg.skip_present, g_cfg.probe_swap,
         g_cfg.capture_alvr, g_cfg.mirror_eye, g_cfg.eye_shift_mm, g_cfg.eye_shift_y_mm,
         g_cfg.gain_r, g_cfg.gain_g, g_cfg.gain_b);
}

/* ---------------- GL ---------------- */

static const char *k_vs =
        "#version 300 es\n"
        "layout(location=0) in vec2 aPos;\n"
        "uniform mat2 uRot;\n"
        "uniform vec2 uScale;\n"
        "uniform vec2 uShift;\n"
        "out vec2 vUv;\n"
        "void main(){\n"
        "  vec2 uv = uRot * (aPos * 0.5);\n"
        "  uv = (uv) * uScale + 0.5 + uShift;\n"
        "  vUv = uv;\n"
        "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";

static const char *k_fs =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 vUv;\n"
        "uniform sampler2D uTex;\n"
        "uniform float uK1;\n"
        "uniform float uK2;\n"
        "uniform vec3 uGain;\n"
        "out vec4 fragColor;\n"
        "void main(){\n"
        "  vec2 p = vUv * 2.0 - 1.0;\n"
        "  float r2 = dot(p, p);\n"
        "  float s = 1.0 + uK1 * r2 + uK2 * r2 * r2;\n"
        "  vec2 uv = (p * s) * 0.5 + 0.5;\n"
        "  fragColor = texture(uTex, uv);\n"
        "  fragColor.rgb *= uGain;\n"
        "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        XERR("着色器编译失败: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool xr_display_attach_window(ANativeWindow *window) {
    if (!window) return false;
    ANativeWindow *old = g_xr.window;
    g_xr.window = window;
    XLOG("DISPLAY: 收到窗口 %dx%d fmt=%d",
         ANativeWindow_getWidth(window), ANativeWindow_getHeight(window),
         ANativeWindow_getFormat(window));
    /* 窗口换了：旧的 EGLSurface 绑在已经销毁的那个 window 上，必须重建，
     * 否则从这一刻起 eglSwapBuffers 全部 EGL_BAD_SURFACE。 */
    if (old && old != window && g_xr.our_surface != EGL_NO_SURFACE) {
        s_surf_dirty = true;
        XWARN("EGL: 窗口已更换(%p -> %p)，标记 surface 重建", (void *)old, (void *)window);
    }
    return true;
}

/* swap 失败时的自愈入口：标脏，下一帧在渲染线程重建（不能在这里重建，
 * 因为调用点通常不在持有我们上下文的线程上）。 */
void xr_display_mark_surface_stale(const char *why) {
    if (g_xr.our_surface == EGL_NO_SURFACE) return;
    if (s_surf_dirty) return;
    s_surf_dirty = true;
    XWARN("EGL: 标记 surface 需重建（%s）", why ? why : "?");
}

bool xr_display_surface_stale(void) { return s_surf_dirty; }

/* 在渲染线程调用：把我们的 EGLSurface 重新绑到当前 g_xr.window 上。
 * 上下文本身保留（shader/VAO 都在里面，不用重建）。 */
bool xr_display_rebuild_surface(void) {
    EGLDisplay dpy = g_xr.egl_display;
    if (dpy == EGL_NO_DISPLAY || !g_xr.our_context) return false;
    if (!g_xr.window) {
        XWARN("EGL: 重建失败，当前没有窗口");
        return false;
    }
    if (!s_own_cfg) {
        XWARN("EGL: 重建失败，config 未记录");
        return false;
    }

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_xr.our_surface != EGL_NO_SURFACE) {
        eglDestroySurface(dpy, g_xr.our_surface);
        g_xr.our_surface = EGL_NO_SURFACE;
    }
    EGLSurface s = eglCreateWindowSurface(dpy, s_own_cfg, g_xr.window, NULL);
    if (s == EGL_NO_SURFACE) {
        XERR("EGL: 重建 eglCreateWindowSurface 失败 0x%x", eglGetError());
        s_surf_dirty = true;   /* 下一帧再试 */
        return false;
    }
    g_xr.our_surface = s;
    s_surf_win = g_xr.window;
    s_surf_dirty = false;
    s_surf_rebuilds++;
    if (!eglMakeCurrent(dpy, s, s, g_xr.our_context)) {
        XWARN("EGL: 重建后 eglMakeCurrent 失败 0x%x", eglGetError());
        return false;
    }
    XLOG("EGL: surface 已重建（第 %d 次）窗口 %dx%d", s_surf_rebuilds,
         ANativeWindow_getWidth(g_xr.window), ANativeWindow_getHeight(g_xr.window));
    return true;
}

bool xr_display_init_gl(void) {
    /* 两个线程会同时进来：Java 的 nativeSetSurface 在 UI 线程，xrCreateSession
     * 在 ALVR 渲染线程。EGL 上下文同一时刻只能在一个线程上 current，并发
     * eglMakeCurrent 会拿到 EGL_BAD_ACCESS(0x3002) 并把整个初始化判失败。 */
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    bool ok = init_gl_locked();
    pthread_mutex_unlock(&lock);
    return ok;
}

static bool init_gl_locked(void) {
    if (g_xr.gl_ready) return true;
    cfg_load();

    /* 诊断模式：我们完全不介入窗口，留给 ALVR 自己画 */
    if (g_cfg.skip_present) {
        XLOG("DISPLAY: skip_present=1，本运行时不创建窗口 surface、不初始化 GL");
        return false;
    }

    EGLDisplay dpy = g_xr.egl_display;
    if (dpy == EGL_NO_DISPLAY) {
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        g_xr.egl_display = dpy;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        XERR("EGL: eglInitialize 失败 0x%x", eglGetError());
        return false;
    }
    XLOG("EGL: %d.%d  vendor=%s", major, minor, eglQueryString(dpy, EGL_VENDOR));

    const EGLint cfg_attr[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE};
    EGLConfig cfg = NULL;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        XERR("EGL: 找不到合适的 config");
        return false;
    }
    if (!g_xr.our_surface && g_xr.window) {
        g_xr.our_surface = eglCreateWindowSurface(dpy, cfg, g_xr.window, NULL);
        if (g_xr.our_surface == EGL_NO_SURFACE) {
            XERR("EGL: eglCreateWindowSurface 失败 0x%x", eglGetError());
            return false;
        }
        s_own_cfg = cfg;
        s_surf_win = g_xr.window;
        s_surf_dirty = false;
        XLOG("EGL: 窗口 surface 就绪");
    }
    if (!g_xr.our_context) {
        EGLContext share = g_xr.app_context ? g_xr.app_context : EGL_NO_CONTEXT;
        const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        g_xr.our_context = eglCreateContext(dpy, cfg, share, ctx_attr);
        if (g_xr.our_context == EGL_NO_CONTEXT) {
            XERR("EGL: eglCreateContext(share=%p) 失败 0x%x", (void *)share, eglGetError());
            return false;
        }
        XLOG("EGL: 自建上下文 ok（share=%s）", share ? "ALVR" : "无");
    }
    if (!g_xr.our_surface) {
        XWARN("EGL: 还没有窗口，先不建 GL 资源");
        g_xr.gl_ready = false;
        return false;
    }
    if (!eglMakeCurrent(dpy, g_xr.our_surface, g_xr.our_surface, g_xr.our_context)) {
        EGLint e = eglGetError();
        XWARN("EGL: eglMakeCurrent 失败 0x%x，尝试重建 surface", e);
        if (!xr_display_rebuild_surface() ||
            !eglMakeCurrent(dpy, g_xr.our_surface, g_xr.our_surface, g_xr.our_context)) {
            XERR("EGL: 重建后仍然 eglMakeCurrent 失败 0x%x", eglGetError());
            return false;
        }
    }
    XLOG("GL: %s | %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    if (!g_xr.comp_ready) {
        GLuint vs = compile(GL_VERTEX_SHADER, k_vs);
        GLuint fs = compile(GL_FRAGMENT_SHADER, k_fs);
        if (!vs || !fs) return false;
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(prog, sizeof(log), NULL, log);
            XERR("链接失败: %s", log);
            return false;
        }
        g_xr.prog = prog;
        g_xr.u_tex = glGetUniformLocation(prog, "uTex");
        g_xr.u_xform = glGetUniformLocation(prog, "uRot");
        glUseProgram(prog);
        glUniform1i(g_xr.u_tex, 0);
        glUniform1f(glGetUniformLocation(prog, "uK1"), g_cfg.k1);
        glUniform1f(glGetUniformLocation(prog, "uK2"), g_cfg.k2);

        static const float quad[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
        glGenVertexArrays(1, &g_xr.vao);
        glBindVertexArray(g_xr.vao);
        glGenBuffers(1, &g_xr.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, g_xr.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        g_xr.comp_ready = true;
    }
    g_xr.gl_ready = true;
    XLOG("DISPLAY: GL 合成器就绪");

    /* 释放当前绑定：EGL 上下文同一时刻只能在一个线程上 current，
     * 这里可能是 UI 线程，必须放还给渲染线程去用。 */
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return true;
}

/* ------------------------------------------------------------------
 * ALVR 直渲捕获
 *
 * 实测结论（PROBE-CTX / PROBE-SWAP）：
 *   这个魔改版 ALVR 每帧都把 fbo 绑成 0、viewport 设成 1536x1536，
 *   画进它自己的 1080x2265 手机窗口 surface；
 *   而它交给 OpenXR 的两张 1536x1536 眼纹理全程是纯黑（非黑=0/64）。
 *   也就是说它压根没走 swapchain 这条路。
 *
 * 所以不必去 hook EGL 抢它的 surface：在 xrEndFrame 入口，ALVR 的上下文
 * 还是 current、fbo=0 正是它刚画完的后备缓冲，直接 glBlitFramebuffer 到
 * 我们自己的一张纹理上就行。纹理同属一个 share group，切回 our_context
 * 立刻能采样。blit 是纯 GPU 搬运，不落内存，代价可以忽略。
 * ------------------------------------------------------------------ */

static GLuint s_cap_tex;
static GLuint s_cap_fbo;
static int s_cap_w;
static int s_cap_h;
static bool s_cap_ok;
static int s_cap_attempts;
static int s_cap_err_logged;

GLuint xr_capture_texture(void) {
    return s_cap_ok ? s_cap_tex : 0;
}

int xr_capture_size(int *w, int *h) {
    if (!s_cap_ok) return 0;
    if (w) *w = s_cap_w;
    if (h) *h = s_cap_h;
    return 1;
}

/*
 * 在 ALVR 的上下文里调用（必须 fbo=0 就是它的后备缓冲）。
 * 尺寸从当前 draw surface 查，surface 变了就重建纹理。
 */
void xr_capture_alvr_frame(void) {
    if (!g_xr.gl_ready || !xr_cfg_capture_alvr()) return;
    EGLSurface d = eglGetCurrentSurface(EGL_DRAW);
    if (d == EGL_NO_SURFACE || g_xr.egl_display == EGL_NO_DISPLAY) return;

    EGLint sw = 0, sh = 0;
    if (!eglQuerySurface(g_xr.egl_display, d, EGL_WIDTH, &sw) ||
        !eglQuerySurface(g_xr.egl_display, d, EGL_HEIGHT, &sh)) {
        return;
    }
    if (sw <= 0 || sh <= 0 || sw > 4096 || sh > 4096) return;

    if (!s_cap_ok || s_cap_w != sw || s_cap_h != sh) {
        if (s_cap_tex) glDeleteTextures(1, &s_cap_tex);
        if (s_cap_fbo) glDeleteFramebuffers(1, &s_cap_fbo);
        s_cap_tex = 0;
        s_cap_fbo = 0;

        glGenTextures(1, &s_cap_tex);
        glBindTexture(GL_TEXTURE_2D, s_cap_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenFramebuffers(1, &s_cap_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s_cap_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_cap_tex, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            XERR("CAPTURE: FBO 不完整 0x%x", st);
            s_cap_ok = false;
            return;
        }
        s_cap_w = sw;
        s_cap_h = sh;
        s_cap_ok = true;
        XLOG("CAPTURE: 捕获目标就绪 %dx%d tex=%u fbo=%u", sw, sh, s_cap_tex, s_cap_fbo);
    }

    /* fbo=0 -> 我们的纹理。NEAREST 足够，两边尺寸相同不走缩放。 */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_cap_fbo);
    glBlitFramebuffer(0, 0, s_cap_w, s_cap_h, 0, 0, s_cap_w, s_cap_h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        s_cap_attempts++;
        if (s_cap_err_logged < 5) {
            XWARN("CAPTURE: blit 失败 glErr=0x%x（第 %d 次）", err, s_cap_attempts);
            s_cap_err_logged++;
        }
    }
}

/*
 * 把当前默认帧缓冲（fbo=0）的内容搬进指定 FBO。
 *
 * 这就是华为官方 runtime 的 "acquire-bound framebuffer path"：
 *   acquire → fbo=0 指向该 swapchain 图像 → 应用渲 fbo=0 → release 时运行时收走。
 *
 * GLES 不允许我们真的把 fbo=0 改成别的目标，所以在 release 这个时间点把
 * fbo=0 已经画好的内容 blit 过去，效果等价，而且完全不用 hook 应用的 GL 调用。
 * 源尺寸取当前 draw surface，目标尺寸取 swapchain，不一致时自动缩放。
 */
bool xr_capture_blit_to(GLuint dst_tex, int dst_w, int dst_h, const char *tag) {
    if (!dst_tex || dst_w <= 0 || dst_h <= 0) return false;
    EGLSurface d = eglGetCurrentSurface(EGL_DRAW);
    if (d == EGL_NO_SURFACE || g_xr.egl_display == EGL_NO_DISPLAY) return false;

    EGLint sw = 0, sh = 0;
    if (!eglQuerySurface(g_xr.egl_display, d, EGL_WIDTH, &sw) ||
        !eglQuerySurface(g_xr.egl_display, d, EGL_HEIGHT, &sh)) {
        return false;
    }
    if (sw <= 0 || sh <= 0) return false;

    /*
     * 【关键坑】Framebuffer Object 不在 EGL share group 里共享 —— 只有
     * texture / buffer 这类对象共享，FBO 和 renderbuffer 是 per-context 的。
     * 所以绝对不能预先在 our_context 里建好 FBO 再拿到 ALVR 上下文里用：
     * 名字在那边根本不存在，glCheckFramebufferStatus 会返回
     * GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT(0x8CD7)，blit 报 0x506。
     *
     * 这里改成「用当前上下文的临时 FBO，现把目标纹理挂上去」，每个上下文
     * 各自缓存一个，切上下文就重建。
     */
    static GLuint s_blit_fbo;
    static EGLContext s_blit_ctx;
    EGLContext cur = eglGetCurrentContext();
    if (!s_blit_fbo || s_blit_ctx != cur) {
        s_blit_fbo = 0;
        glGenFramebuffers(1, &s_blit_fbo);
        s_blit_ctx = cur;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_blit_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    GLenum dst_st = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (dst_st != GL_FRAMEBUFFER_COMPLETE) {
        static int logged;
        if (logged < 4) {
            XWARN("CAPTURE[%s]: 目标 FBO 不完整 0x%x (tex=%u %dx%d)", tag, dst_st, dst_tex,
                  dst_w, dst_h);
            logged++;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    /* 头几次把 ALVR 渲染时的 viewport 打出来：两只眼如果 viewport 不一样，
     * 说明它在同一块 fbo=0 上左右分屏，那我们一次就能收全两只眼。 */
    static int dbg_n;
    if (dbg_n < 6) {
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        XLOG("CAPTURE[%s]: 源 %dx%d → 目标 %dx%d viewport=[%d,%d %dx%d] ctx=%p surf=%p",
             tag, sw, sh, dst_w, dst_h, vp[0], vp[1], vp[2], vp[3],
             (void *)cur, (void *)d);
        dbg_n++;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    /*
     * 只搬「ALVR 真正渲染的那块区域」，不是整块 window surface。
     *
     * ALVR 按我们上报的眼分辨率设 viewport（实测 [0,0 1536x1536]），但这个
     * window surface 只有 1080x2265 —— 1536 宽的 viewport 根本装不进去，
     * 右边会被裁掉、UI 也会错位。所以上报分辨率必须 <= surface 宽度
     * （见 xr.cfg 的 eye_res），这里再按实际 viewport 取源矩形，两边对齐。
     */
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    int sx = vp[0], sy = vp[1];
    int sw2 = vp[2], sh2 = vp[3];
    if (sx < 0) { sw2 += sx; sx = 0; }
    if (sy < 0) { sh2 += sy; sy = 0; }
    if (sx + sw2 > sw) sw2 = sw - sx;
    if (sy + sh2 > sh) sh2 = sh - sy;
    if (sw2 <= 0 || sh2 <= 0) {
        /* viewport 不可信时退回整块 surface */
        sx = 0; sy = 0; sw2 = sw; sh2 = sh;
    }

    GLenum filter = (sw2 == dst_w && sh2 == dst_h) ? GL_NEAREST : GL_LINEAR;
    glBlitFramebuffer(sx, sy, sx + sw2, sy + sh2, 0, 0, dst_w, dst_h, GL_COLOR_BUFFER_BIT, filter);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* 在 ALVR 上下文里把命令推下去：我们随后会切到自己的上下文去采样这张
     * 纹理，不 flush 的话可能读到还没被 GPU 写入的内容。 */
    glFlush();

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        static int logged;
        if (logged < 4) {
            XWARN("CAPTURE[%s]: blit %dx%d -> %dx%d 失败 glErr=0x%x", tag, sw, sh, dst_w, dst_h, err);
            logged++;
        }
        return false;
    }
    return true;
}

/* 把窗口切成左右两个眼球视口 */
static void eye_viewport(int eye, int *x, int *y, int *w, int *h) {    int W = g_xr.window ? ANativeWindow_getWidth(g_xr.window) : XR_PANEL_W;
    int H = g_xr.window ? ANativeWindow_getHeight(g_xr.window) : XR_PANEL_H;
    *w = W / 2;
    *h = H;
    *x = eye == 0 ? 0 : W / 2;
    *y = 0;
}

/*
 * 按眼设置朝向。
 *
 * HUAWEI VR Glass 是单屏 + 双光路，面板本身是 90° 装的
 * （ALVR 二进制里的原话："the headset optics turn this 90-degree surface
 * into the correct wearer-facing orientation"）。实测 rotate=90 时左眼正立，
 * 但右眼上下反 —— 说明两条光路的方向并不一致，必须逐眼单独校正。
 *
 * mirror_eye 给右眼叠加一个额外变换，热重载即可逐个试：
 *   0 同左眼 / 1 再转180 / 2 上下翻 / 3 水平翻 / 4 再转90 / 5 再转270
 */
static void set_rot_uniform(int eye) {
    int rot = g_cfg.rotate;
    bool flip_v = g_cfg.flip_v != 0;
    bool flip_h = false;

    if (eye == 1) {
        switch (g_cfg.mirror_eye) {
            case 1: rot = (rot + 180) % 360; break;
            case 2: flip_v = !flip_v; break;
            case 3: flip_h = true; break;
            case 4: rot = (rot + 90) % 360; break;
            case 5: rot = (rot + 270) % 360; break;
            default: break;
        }
    }

    float c = 1.f, s = 0.f;
    switch (rot) {
        case 90: c = 0.f; s = -1.f; break;
        case 180: c = -1.f; s = 0.f; break;
        case 270: c = 0.f; s = 1.f; break;
        default: break;
    }
    /* mat2 列主序: [c, s, -s, c] */
    float m[4] = {c, s, -s, c};
    glUniformMatrix2fv(g_xr.u_xform, 1, GL_FALSE, m);
    glUniform2f(glGetUniformLocation(g_xr.prog, "uScale"),
                flip_h ? -1.f : 1.f, flip_v ? -1.f : 1.f);
    /* 华为 SDK 的 horizontalShiftViewLeft/Right：左右眼光心各偏 ±2.959mm。
       每眼视口占半屏，所以换算成单眼 UV 要乘 2。符号可在 xr.cfg 用负值翻转。 */
    float shift_uv = (g_cfg.eye_shift_mm / 1000.0f) / XR_SCREEN_METERS * 2.0f;
    float shift_y_uv = (g_cfg.eye_shift_y_mm / 1000.0f) / XR_SCREEN_METERS * 2.0f;
    glUniform2f(glGetUniformLocation(g_xr.prog, "uShift"),
                eye == 0 ? shift_uv : -shift_uv,
                eye == 0 ? shift_y_uv : -shift_y_uv);
    glUniform3f(glGetUniformLocation(g_xr.prog, "uGain"),
                g_cfg.gain_r, g_cfg.gain_g, g_cfg.gain_b);
}

void xr_composite_clear(float r, float g, float b, float a) {
    if (!g_xr.gl_ready) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void xr_composite_projection(const XrCompositionLayerProjection *proj) {
    if (!g_xr.gl_ready || !proj) return;
    glUseProgram(g_xr.prog);
    glBindVertexArray(g_xr.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_xr.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glActiveTexture(GL_TEXTURE0);

    for (uint32_t i = 0; i < proj->viewCount && i < 2; i++) {
        const XrCompositionLayerProjectionView *v = &proj->views[i];
        swapchain_t *sc = xr_swapchain_by_handle(v->subImage.swapchain);
        if (!sc || !sc->allocated) {
            XWARN("COMPOSITE: view %u 的 swapchain 无效", i);
            continue;
        }
        int x, y, w, h;
        eye_viewport((int)i, &x, &y, &w, &h);
        glViewport(x, y, w, h);
        set_rot_uniform((int)i);
        /* swap_eyes: 把 view 1 的纹理画到左视口、view 0 的纹理画到右视口 */
        swapchain_t *use_sc = (g_cfg.swap_eyes && xr_swapchain_by_handle(proj->views[1 - i].subImage.swapchain))
                ? xr_swapchain_by_handle(proj->views[1 - i].subImage.swapchain) : sc;
        glBindTexture(GL_TEXTURE_2D, use_sc->textures[use_sc->acquired_index]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) XWARN("COMPOSITE: eye %u glError=0x%x", i, err);
    }
    glBindVertexArray(0);
    glUseProgram(0);
}

/*
 * 内置测试图案（不依赖 ALVR 的任何纹理）：
 *   左眼 = 蓝绿渐变块，右眼 = 橙红块，各带一个中央白方块 + 四角小方块。
 * 只要眼镜里看到这两个色块，就证明「Surface → EGL → 眼镜面板」整条链路是通的；
 * 如果看不到，那问题在显示链路而不是 ALVR 渲染。
 */
void xr_display_test_pattern(void) {
    if (!g_xr.gl_ready || !g_xr.window) return;
    int W = ANativeWindow_getWidth(g_xr.window);
    int H = ANativeWindow_getHeight(g_xr.window);
    if (W <= 0) W = XR_PANEL_W;
    if (H <= 0) H = XR_PANEL_H;

    /* 测试图必须走与主合成完全相同的 shader，否则 rotate/mirror_eye 不会生效，
       戴镜判据无效。这里生成带左上角白色锚点 + 四角黑块的左右眼纹理。 */
    static GLuint tex[2];
    if (!tex[0]) {
        const int N = 64;
        glGenTextures(2, tex);
        for (int e = 0; e < 2; e++) {
            unsigned char buf[N * N * 4];
            unsigned char base[3];
            /* 对齐测试用同色，避免左右颜色差异干扰判断 */
            base[0] = 26; base[1] = 89; base[2] = 153;
            for (int y = 0; y < N; y++) {
                for (int x = 0; x < N; x++) {
                    int i = (y * N + x) * 4;
                    unsigned char c[3] = {base[0], base[1], base[2]};
                    if (x < 16 && y >= N - 16) { c[0] = c[1] = c[2] = 255; } /* 左上白锚点 */
                    else if ((x < 8 || x >= N - 8) && (y < 8 || y >= N - 8)) {
                        c[0] = c[1] = c[2] = 0;                                /* 四角黑块 */
                    }
                    buf[i] = c[0]; buf[i+1] = c[1]; buf[i+2] = c[2]; buf[i+3] = 255;
                }
            }
            glBindTexture(GL_TEXTURE_2D, tex[e]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(g_xr.prog);
    glBindVertexArray(g_xr.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_xr.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glActiveTexture(GL_TEXTURE0);

    for (int eye = 0; eye < 2; eye++) {
        int x, y, w, h;
        eye_viewport(eye, &x, &y, &w, &h);
        glViewport(x, y, w, h);
        set_rot_uniform(eye);
        glBindTexture(GL_TEXTURE_2D, tex[eye]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindVertexArray(0);
    glUseProgram(0);

    static bool logged;
    if (!logged) {
        XLOG("DISPLAY: 已绘制 shader 测试图案 %dx%d（左蓝 右橙 左上白锚点）", W, H);
        logged = true;
    }
}

void xr_display_present(void) {
    if (!g_xr.gl_ready || !g_xr.our_surface) return;
    if (!eglSwapBuffers(g_xr.egl_display, g_xr.our_surface)) {
        EGLint e = eglGetError();
        static int n;
        /* 失败会每帧刷屏，限流；EGL_BAD_SURFACE 说明 surface 已死，标脏等下一帧重建 */
        if (n < 6 || (n % 500) == 0) {
            XWARN("EGL: eglSwapBuffers 失败 0x%x（累计 %d 次）", e, n + 1);
        }
        n++;
        if (e == EGL_BAD_SURFACE) {
            xr_display_mark_surface_stale("swap 返回 EGL_BAD_SURFACE");
        }
    } else {
        static int ok_logged;
        if (!ok_logged) {
            XLOG("EGL: eglSwapBuffers 正常，画面已上屏");
            ok_logged = 1;
        }
    }
}

/*
 * 调试用：把当前所有 swapchain 纹理各存一份 PPM。
 * 放一个 files/dump_frame 文件就触发一次（触发后自动删掉）。
 * 用来肉眼确认 ALVR 到底画了什么、方向对不对、比例有没有被拉歪 —— 光看日志
 * 判断不了这些，而眼镜屏是 FLAG_SECURE 截不了图。
 */
void xr_display_dump_frames(void) {
    const char *trig = "/sdcard/Android/data/com.lht.huaweivr.alvr/files/dump_frame";
    if (access(trig, F_OK) != 0) return;
    unlink(trig);
    if (!g_xr.gl_ready) return;

    int n = 0;
    for (swapchain_t *sc = g_xr.swapchains; sc; sc = sc->next) {
        if (!sc->allocated) continue;
        int w = (int)sc->width, h = (int)sc->height;
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) continue;

        /* 必须按 RGBA 读：Mali 上 RGBA8 纹理用 GL_RGB 读会报 0x502 */
        size_t full = (size_t)w * (size_t)h * 4;
        unsigned char *buf = (unsigned char *)malloc(full);
        if (!buf) continue;

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               sc->textures[sc->acquired_index], 0);
        GLenum rerr = 0xFFFF;
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glFinish();   /* 等 blit 真正落盘，否则读到旧内容 */
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            rerr = glGetError();
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);

        if (rerr == GL_NO_ERROR) {
            /* 降采样到 512 宽，文件小、拉取快，看内容足够 */
            int tw = 512;
            int th = (int)((long)h * tw / w);
            if (th < 1) th = 1;
            size_t row = (size_t)tw * 3;
            unsigned char *small = (unsigned char *)malloc(row * (size_t)th);
            if (small) {
                double sum = 0;
                unsigned maxv = 0;
                for (int y = 0; y < th; y++) {
                    int sy = (int)((long)y * h / th);
                    for (int x = 0; x < tw; x++) {
                        int sx = (int)((long)x * w / tw);
                        size_t si = ((size_t)sy * w + sx) * 4;
                        size_t di = (size_t)y * row + (size_t)x * 3;
                        small[di] = buf[si];
                        small[di + 1] = buf[si + 1];
                        small[di + 2] = buf[si + 2];
                        int v = (buf[si] + buf[si + 1] + buf[si + 2]) / 3;
                        sum += v;
                        if ((unsigned)v > maxv) maxv = (unsigned)v;
                    }
                }

                char path[256];
                snprintf(path, sizeof(path),
                         "/sdcard/Android/data/com.lht.huaweivr.alvr/files/dump_%d_%dx%d.ppm",
                         n, tw, th);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", tw, th);
                    /* PPM 第一行是图像顶部，GL 原点在左下，所以倒着写 */
                    for (int y = th - 1; y >= 0; y--) fwrite(small + (size_t)y * row, 1, row, f);
                    fclose(f);
                    XLOG("DUMP: %s (源 %dx%d, 均值=%.1f 最大=%u tex=%u)",
                         path, w, h, sum / (double)(tw * th), maxv,
                         sc->textures[sc->acquired_index]);
                }
                free(small);
            }
        } else {
            XWARN("DUMP: tex=%u 读失败 glErr=0x%x", sc->textures[sc->acquired_index], rerr);
        }
        free(buf);
        n++;
    }
    XLOG("DUMP: 共导出 %d 张", n);
}

/*
 * 把捕获到的 ALVR 画面整块贴到眼镜屏上（保持宽高比、黑边居中）。
 * 这一步是为了先看清「ALVR 到底在画什么」——是双目并排、单目预览还是纯黑，
 * 看清之后才知道该怎么切给左右眼。
 */
void xr_display_draw_capture(void) {
    if (!g_xr.gl_ready || !s_cap_ok) return;
    int W = g_xr.window ? ANativeWindow_getWidth(g_xr.window) : XR_PANEL_W;
    int H = g_xr.window ? ANativeWindow_getHeight(g_xr.window) : XR_PANEL_H;
    if (W <= 0) W = XR_PANEL_W;
    if (H <= 0) H = XR_PANEL_H;

    float ar_src = (float)s_cap_w / (float)s_cap_h;
    float ar_dst = (float)W / (float)H;
    int dw = W, dh = H;
    if (ar_src > ar_dst) dh = (int)((float)W / ar_src);
    else dw = (int)((float)H * ar_src);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int dx = (W - dw) / 2;
    int dy = (H - dh) / 2;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, W, H);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glViewport(dx, dy, dw, dh);
    glUseProgram(g_xr.prog);
    glBindVertexArray(g_xr.vao);
    float ident[4] = {1.f, 0.f, 0.f, 1.f};
    glUniformMatrix2fv(g_xr.u_xform, 1, GL_FALSE, ident);
    glUniform2f(glGetUniformLocation(g_xr.prog, "uScale"), 1.f, 1.f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_cap_tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);

    static bool logged;
    if (!logged) {
        XLOG("CAPTURE: 整屏显示 源 %dx%d → 目标 %dx%d @(%d,%d) 窗口 %dx%d",
             s_cap_w, s_cap_h, dw, dh, dx, dy, W, H);
        logged = true;
    }
}

void xr_display_shutdown(void) {
    if (g_xr.our_context != EGL_NO_CONTEXT && g_xr.egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_xr.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(g_xr.egl_display, g_xr.our_context);
        g_xr.our_context = EGL_NO_CONTEXT;
    }
    if (g_xr.our_surface != EGL_NO_SURFACE && g_xr.egl_display != EGL_NO_DISPLAY) {
        eglDestroySurface(g_xr.egl_display, g_xr.our_surface);
        g_xr.our_surface = EGL_NO_SURFACE;
    }
    g_xr.gl_ready = false;
}
