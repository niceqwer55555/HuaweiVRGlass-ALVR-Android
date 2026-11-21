/*
 * mini_xr v0.2 —— 为 OnePlus 7T (hotdogb) + 华为 VR Glass (CV10) 自研的最小 OpenXR runtime
 *
 * v0.2 相比 v0.1 增加：
 *   [姿态]   从 127.0.0.1:47110 收 UDP 姿态报文（hwvrctl / 外部 SLAM 桥均可喂）
 *            "w x y z" = 3DoF；"w x y z px py pz 1" = 6DoF（第 8 个数 bit0=位置有效）
 *   [空间]   xrCreateReferenceSpace / xrDestroySpace / xrLocateSpace
 *   [视图]   xrLocateViews —— 真正把 3DoF/6DoF 姿态算进两只眼
 *   [会话]   xrCreateSession(解析 XrGraphicsBindingOpenGLESAndroidKHR) / Begin / End / Destroy
 *   [帧]     xrWaitFrame / xrBeginFrame / xrEndFrame（固定节拍，present 仍是空实现）
 *   [交换链] xrCreateSwapchain 在 App 的 EGL context 里建 RGBA8 纹理；Acquire/Wait/Release 环形
 * 编译：arm-linux-musleabihf, -shared -nostdlib，零 DT_NEEDED
 */

typedef unsigned int   u32;
typedef unsigned short u16;
typedef int            i32;
typedef unsigned long long u64;
typedef long long      i64;
typedef unsigned char  u8;
typedef float          f32;

extern int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "MINIXR", __VA_ARGS__)
#define LOGE(...) __android_log_print(6, "MINIXR", __VA_ARGS__)

static int s_cmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b; }

/* ---------------- 系统调用/库函数（都由 bionic 解析，不产生 DT_NEEDED） ---------------- */
extern int socket(int, int, int);
extern int bind(int, const void *, unsigned int);
extern int recvfrom(int, void *, unsigned int, int, void *, unsigned int *);
extern int fcntl(int, int, ...);
extern int close(int);
extern int clock_gettime(int, void *);
extern int nanosleep(const void *, void *);
extern void *eglGetCurrentContext(void);
extern int eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx);
extern void *eglCreatePbufferSurface(void *dpy, void *cfg, const int *attrib);

extern void glGenTextures(int n, unsigned int *ids);
extern void glBindTexture(unsigned int target, unsigned int tex);
extern void glTexImage2D(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
extern void glTexParameteri(unsigned int, unsigned int, int);
extern void glDeleteTextures(int n, const unsigned int *ids);

typedef void *EGLDisplay; typedef void *EGLContext; typedef void *EGLConfig; typedef void *EGLSurface;
#define EGL_NO_SURFACE ((void *)0)
#define EGL_NO_CONTEXT ((void *)0)
#define EGL_WIDTH  0x3057
#define EGL_HEIGHT 0x3056
#define EGL_NONE   0x3038
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA8 0x8058
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800

/* ---------------- OpenXR ABI ---------------- */
typedef i32 XrResult;
typedef u64 XrFlags64; typedef u64 XrSystemId; typedef u64 XrVersion;
typedef u64 XrTime; typedef i64 XrDuration;
typedef void *XrHandle;
typedef u64 XrPath;
typedef u32 XrBool32;

#define XR_SUCCESS 0
#define XR_ERROR_VALIDATION_FAILURE (-1)
#define XR_ERROR_RUNTIME_FAILURE (-2)
#define XR_ERROR_API_VERSION_UNSUPPORTED (-4)
#define XR_ERROR_FUNCTION_UNSUPPORTED (-5)
#define XR_ERROR_FEATURE_UNSUPPORTED (-6)
#define XR_ERROR_SIZE_INSUFFICIENT (-11)
#define XR_ERROR_HANDLE_INVALID (-12)
#define XR_ERROR_CALL_ORDER_INVALID (-16)
#define XR_ERROR_GRAPHICS_DEVICE_INVALID (-19)
#define XR_ERROR_INITIALIZATION_FAILED (-35)

#define XR_TRUE 1
#define XR_FALSE 0

#define XR_TYPE_INSTANCE_CREATE_INFO 3
#define XR_TYPE_SYSTEM_GET_INFO 4
#define XR_TYPE_SYSTEM_PROPERTIES 5
#define XR_TYPE_VIEW_CONFIGURATION_PROPERTIES 7
#define XR_TYPE_SESSION_CREATE_INFO 8
#define XR_TYPE_SWAPCHAIN_CREATE_INFO 9
#define XR_TYPE_VIEW 7
#define XR_TYPE_FRAME_STATE 44
#define XR_TYPE_VIEW_CONFIGURATION_VIEW 10
#define XR_TYPE_INSTANCE_PROPERTIES 11
#define XR_TYPE_FRAME_WAIT_INFO 15
#define XR_TYPE_FRAME_BEGIN_INFO 16
#define XR_TYPE_FRAME_END_INFO 17
#define XR_TYPE_REFERENCE_SPACE_CREATE_INFO 18
#define XR_TYPE_VIEW_LOCATE_INFO 19
#define XR_TYPE_VIEW_STATE 20
#define XR_TYPE_SPACE_LOCATION 22
#define XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR 1000023001
#define XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR 1000024001

#define XR_MAX_APPLICATION_NAME_SIZE 128
#define XR_MAX_SYSTEM_NAME_SIZE 256

#define XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY 1
#define XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO 2
#define XR_ENVIRONMENT_BLEND_MODE_OPAQUE 1
#define XR_REFERENCE_SPACE_TYPE_VIEW 1
#define XR_REFERENCE_SPACE_TYPE_LOCAL 2

struct Hdr { i32 type; const void *next; };
typedef struct { f32 x, y, z, w; } Quat;
typedef struct { f32 x, y, z; } Vec3;
typedef struct { Quat orientation; Vec3 position; } Pose;
typedef struct { f32 angleLeft, angleRight, angleUp, angleDown; } Fov;

typedef struct {
    char applicationName[XR_MAX_APPLICATION_NAME_SIZE]; u32 applicationVersion;
    char engineName[128]; u32 engineVersion; XrVersion apiVersion;
} XrApplicationInfo;

typedef struct {
    i32 type; const void *next; XrFlags64 createFlags; XrApplicationInfo applicationInfo;
    u32 enabledApiLayerCount; const char *const *enabledApiLayerNames;
    u32 enabledExtensionCount; const char *const *enabledExtensionNames;
} XrInstanceCreateInfo;

typedef struct { i32 type; const void *next; void *applicationVM; void *applicationActivity; } XrInstanceCreateInfoAndroidKHR;
typedef struct { i32 type; const void *next; i32 formFactor; } XrSystemGetInfo;

typedef struct { u32 maxSwapchainImageHeight, maxSwapchainImageWidth, maxLayerCount; } XrSystemGraphicsProperties;
typedef struct { u32 orientationTracking, positionTracking; } XrSystemTrackingProperties;
typedef struct {
    i32 type; const void *next; XrSystemId systemId; u32 vendorId; char systemName[XR_MAX_SYSTEM_NAME_SIZE];
    XrSystemGraphicsProperties graphicsProperties; XrSystemTrackingProperties trackingProperties;
} XrSystemProperties;

typedef struct { i32 type; const void *next; i32 viewConfigurationType; u32 fovMutable; } XrViewConfigurationProperties;
typedef struct {
    i32 type; const void *next;
    u32 recommendedImageRectWidth, recommendedImageRectHeight, maxImageRectWidth, maxImageRectHeight,
        recommendedSwapchainSampleCount, maxSwapchainSampleCount;
} XrViewConfigurationView;

typedef struct { i32 type; const void *next; u32 runtimeVersion; char runtimeName[128]; u32 vendorId; char vendorName[128]; } XrInstanceProperties;
typedef struct { i32 type; const void *next; XrVersion minApiVersionSupported, maxApiVersionSupported; } XrGraphicsRequirementsOpenGLESKHR;

typedef struct { i32 type; const void *next; EGLDisplay display; EGLContext context; EGLConfig config; } XrGraphicsBindingOpenGLESAndroidKHR;
typedef struct { i32 type; const void *next; XrFlags64 createFlags; XrSystemId systemId; } XrSessionCreateInfo;
typedef struct {
    i32 type; const void *next; XrFlags64 createFlags; XrFlags64 usageFlags; i64 format;
    u32 sampleCount, width, height, faceCount, arraySize, mipCount;
} XrSwapchainCreateInfo;

typedef struct { i32 type; const void *next; i32 referenceSpaceType; Pose poseInReferenceSpace; } XrReferenceSpaceCreateInfo;
typedef struct { i32 type; const void *next; XrTime displayTime; i64 space; XrFlags64 locateFlags; } XrViewLocateInfo;
typedef struct {
    i32 type; const void *next; u32 viewStateFlags;   /* bit0 = orientation valid, bit1 = position valid */
} XrViewState;
typedef struct { i32 type; const void *next; Pose pose; Fov fov; } XrView;
typedef struct { i32 type; const void *next; Pose pose; u64 locationFlags; } XrSpaceLocation;
typedef struct {
    i32 type; const void *next; i64 predictedDisplayTime, predictedDisplayPeriod; u32 shouldRender;
} XrFrameState;
typedef struct { i32 type; const void *next; } XrFrameWaitInfo;
typedef struct { i32 type; const void *next; } XrFrameBeginInfo;
typedef struct {
    i32 type; const void *next; XrTime displayTime; i32 environmentBlendMode;
    u32 layerCount; const void *const *layers;
} XrFrameEndInfo;
typedef struct { i32 type; const void *next; u32 image; } XrSwapchainImageOpenGLESKHR;

/* ---------------- 全局状态 ---------------- */
#define MAX_TEX 8
typedef struct { u32 magic; u32 phase; } Inst;
static Inst g_inst = {0x4d585232u, 0};

typedef struct { u32 magic; i32 type; } Space;
static Space g_space_view = {0x53505831u, XR_REFERENCE_SPACE_TYPE_VIEW};
static Space g_space_local = {0x53505832u, XR_REFERENCE_SPACE_TYPE_LOCAL};

typedef struct {
    u32 magic;
    EGLDisplay dpy; EGLContext ctx; EGLConfig cfg;
    int running;
} Sess;
static Sess g_sess = {0x53455353u, 0, 0, 0, 0};

typedef struct {
    u32 magic; i64 format; u32 w, h, n, idx, created;
    u32 tex[MAX_TEX];
} Swap;
static Swap g_swaps[4];
static u32 g_swap_count;

static const char *g_ext[] = {
    "XR_KHR_opengl_es_enable", "XR_KHR_android_create_instance", "XR_KHR_android_thread_settings",
    "XR_KHR_loader_init", "XR_KHR_loader_init_android", "XR_HUAWEI_controller_interaction",
    "XR_KHR_composition_layer_depth",
};

#define VW 1600
#define VH 1600
#define IPD 0.064f

/* ---------------- 姿态输入（UDP） ---------------- */
#define POSE_PORT 47110
static int g_pose_fd = -1;
static Quat g_q = {0, 0, 0, 1};      /* x,y,z,w —— 默认单位四元数（正前） */
static Vec3 g_pos = {0, 0, 0};
static int  g_pos_valid = 0;
static int  g_pose_seen = 0;
static int  g_pose_log_div = 0;

static f32 parse_f32(const char **p)
{
    const char *s = *p;
    while (*s == ' ') s++;
    int neg = 0, seen = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    f32 v = 0; f32 frac = 0; f32 div = 1;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (f32)(*s - '0'); s++; seen = 1; }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { frac = frac * 10 + (f32)(*s - '0'); div *= 10; s++; seen = 1; } }
    if (!seen) { *p = s; return 0; }
    *p = s;
    return (neg ? -1.0f : 1.0f) * (v + frac / div);
}

static void pose_open(void)
{
    if (g_pose_fd >= 0) return;
    g_pose_fd = socket(2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 0);
    if (g_pose_fd < 0) { LOGE("pose: socket 失败 %d", g_pose_fd); return; }
    /* sockaddr_in: family(2) port(2) addr(4) */
    u32 sa[4] = {0, 0, 0, 0};
    u16 family = 2; u16 port = POSE_PORT;
    u8 *p = (u8 *)sa;
    *(u16 *)(p + 0) = family;
    *(u16 *)(p + 2) = port;
    *(u32 *)(p + 4) = 0x7f000001u;     /* 127.0.0.1 小端内存里就是 01 00 00 7f */
    if (bind(g_pose_fd, sa, 16) < 0) {
        LOGE("pose: bind 47110 失败（App 侧可能无权限，改用文件/模块直供）");
        close(g_pose_fd); g_pose_fd = -1; return;
    }
    fcntl(g_pose_fd, 4 /*F_SETFL*/, 0x800 /*O_NONBLOCK*/);
    LOGI("pose: 监听 udp 127.0.0.1:%d", POSE_PORT);
}

static void pose_poll(void)
{
    if (g_pose_fd < 0) pose_open();
    if (g_pose_fd < 0) return;
    char buf[160];
    for (int i = 0; i < 8; i++) {
        u32 sa[4]; u32 salen = 16;
        int n = recvfrom(g_pose_fd, buf, sizeof(buf) - 1, 0, (void *)sa, &salen);
        if (n <= 0) break;
        buf[n] = 0;
        const char *p = buf;
        f32 w = parse_f32(&p), x = parse_f32(&p), y = parse_f32(&p), z = parse_f32(&p);
        f32 px = parse_f32(&p), py = parse_f32(&p), pz = parse_f32(&p), flag = parse_f32(&p);
        f32 norm = w * w + x * x + y * y + z * z;
        if (norm > 0.000001f && norm < 4.0f) {
            g_q.x = x; g_q.y = y; g_q.z = z; g_q.w = w;
            g_pos.x = px; g_pos.y = py; g_pos.z = pz;
            int nv = ((int)flag) & 1;
            if (nv != g_pos_valid) LOGI("pose: 6DoF 位置%s", nv ? "有效" : "无效");
            g_pos_valid = nv;
            if (!g_pose_seen) { g_pose_seen = 1; LOGI("pose: 收到首个姿态包"); }
            if (++g_pose_log_div >= 100) {   /* 约每 100 帧打一次，避免刷屏 */
                g_pose_log_div = 0;
                LOGI("pose: q=(%.3f %.3f %.3f %.3f) pos=%s", g_q.x, g_q.y, g_q.z, g_q.w,
                     g_pos_valid ? "6DoF" : "3DoF");
            }
        }
    }
}

/* ---------------- 实例 ---------------- */
static XrResult xrCreateInstance(const XrInstanceCreateInfo *ci, void **out)
{
    if (!ci || !out) return XR_ERROR_VALIDATION_FAILURE;
    LOGI("createInstance: app=%s engine=%s api=0x%llx ext=%u", ci->applicationInfo.applicationName,
         ci->applicationInfo.engineName, (unsigned long long)ci->applicationInfo.apiVersion,
         ci->enabledExtensionCount);
    const struct Hdr *h = (const struct Hdr *)ci->next;
    while (h) {
        if (h->type == 1000000001) {
            XrInstanceCreateInfoAndroidKHR *a = (XrInstanceCreateInfoAndroidKHR *)h;
            LOGI("  android vm=%p activity=%p", a->applicationVM, a->applicationActivity);
        }
        h = (const struct Hdr *)h->next;
    }
    if (ci->applicationInfo.apiVersion < 0x00010000ull) return XR_ERROR_API_VERSION_UNSUPPORTED;
    g_inst.phase = 1;
    *out = &g_inst;
    pose_open();
    LOGI("createInstance OK (mini_xr v0.2)");
    return XR_SUCCESS;
}

static XrResult xrDestroyInstance(void *i)
{ if (i != &g_inst) return XR_ERROR_HANDLE_INVALID; g_inst.phase = 0; return XR_SUCCESS; }

static XrResult xrGetInstanceProperties(void *i, XrInstanceProperties *p)
{
    (void)i;
    if (!p) return XR_ERROR_VALIDATION_FAILURE;
    p->type = XR_TYPE_INSTANCE_PROPERTIES; p->next = 0; p->runtimeVersion = 0x00020000ull;
    for (u32 k = 0; k < 128; k++) p->runtimeName[k] = 0;
    const char n[] = "mini_xr 0.2 (7T/CV10 3DoF+6DoF-ready)";
    for (u32 k = 0; k < sizeof(n) - 1 && k < 128; k++) p->runtimeName[k] = n[k];
    p->vendorId = 0; p->vendorName[0] = 0;
    return XR_SUCCESS;
}

static XrResult xrEnumerateApiLayerProperties(u32 cap, u32 *count, void *p)
{ (void)cap; (void)p; if (count) *count = 0; return XR_SUCCESS; }

static XrResult xrEnumerateInstanceExtensionProperties(const char *l, u32 cap, u32 *count, void *p)
{
    (void)l; (void)p;
    u32 n = (u32)(sizeof(g_ext) / sizeof(g_ext[0]));
    if (count) *count = n;
    if (n > cap) return XR_ERROR_SIZE_INSUFFICIENT;
    return XR_SUCCESS;
}

static XrResult xrGetSystem(void *i, const XrSystemGetInfo *info, XrSystemId *out)
{
    (void)i;
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) return XR_ERROR_FEATURE_UNSUPPORTED;
    *out = 1;
    LOGI("getSystem OK");
    return XR_SUCCESS;
}

static XrResult xrGetSystemProperties(void *i, XrSystemId s, XrSystemProperties *p)
{
    (void)i; (void)s;
    if (!p) return XR_ERROR_VALIDATION_FAILURE;
    p->type = XR_TYPE_SYSTEM_PROPERTIES; p->next = 0; p->systemId = 1; p->vendorId = 0;
    for (u32 k = 0; k < XR_MAX_SYSTEM_NAME_SIZE; k++) p->systemName[k] = 0;
    const char n[] = "Huawei VR Glass CV10 (mini_xr)";
    for (u32 k = 0; k < sizeof(n) - 1; k++) p->systemName[k] = n[k];
    p->graphicsProperties.maxSwapchainImageHeight = 3200;
    p->graphicsProperties.maxSwapchainImageWidth = 3200;
    p->graphicsProperties.maxLayerCount = 4;
    p->trackingProperties.orientationTracking = XR_TRUE;              /* 3DoF 恒定有 */
    p->trackingProperties.positionTracking = g_pos_valid ? XR_TRUE : XR_FALSE;   /* 6DoF 看源 */
    LOGI("getSystemProperties: orientation=1 position=%d", (int)(g_pos_valid));
    return XR_SUCCESS;
}

static XrResult xrEnumerateViewConfigurations(void *i, XrSystemId s, u32 cap, u32 *count, i32 *t)
{
    (void)i; (void)s;
    if (count) *count = 1;
    if (t && cap >= 1) t[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

static XrResult xrGetViewConfigurationProperties(void *i, XrSystemId s, i32 type, XrViewConfigurationProperties *p)
{
    (void)i; (void)s;
    if (!p) return XR_ERROR_VALIDATION_FAILURE;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_FEATURE_UNSUPPORTED;
    p->type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES; p->next = 0;
    p->viewConfigurationType = type; p->fovMutable = XR_TRUE;
    return XR_SUCCESS;
}

static XrResult xrEnumerateViewConfigurationViews(void *i, XrSystemId s, i32 t, u32 cap, u32 *count, XrViewConfigurationView *v)
{
    (void)i; (void)s; (void)t;
    if (count) *count = 2;
    if (v) {
        u32 fill = cap < 2 ? cap : 2;
        for (u32 k = 0; k < fill; k++) {
            v[k].type = XR_TYPE_VIEW_CONFIGURATION_VIEW; v[k].next = 0;
            v[k].recommendedImageRectWidth = VW; v[k].recommendedImageRectHeight = VH;
            v[k].maxImageRectWidth = VW + 64; v[k].maxImageRectHeight = VH + 64;
            v[k].recommendedSwapchainSampleCount = 1; v[k].maxSwapchainSampleCount = 1;
        }
    }
    return XR_SUCCESS;
}

static XrResult xrEnumerateEnvironmentBlendModes(void *i, XrSystemId s, i32 t, u32 cap, u32 *count, i32 *m)
{
    (void)i; (void)s; (void)t;
    if (count) *count = 1;
    if (m && cap >= 1) m[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

static XrResult xrGetOpenGLESGraphicsRequirementsKHR(void *i, XrSystemId s, XrGraphicsRequirementsOpenGLESKHR *r)
{
    (void)i; (void)s;
    if (!r) return XR_ERROR_VALIDATION_FAILURE;
    r->type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR; r->next = 0;
    r->minApiVersionSupported = 0x00030000ull; r->maxApiVersionSupported = 0x00030002ull;
    return XR_SUCCESS;
}

/* ---------------- 会话 ---------------- */
static XrResult xrCreateSession(void *inst, const XrSessionCreateInfo *ci, void **out)
{
    (void)inst;
    if (!ci || !out) return XR_ERROR_VALIDATION_FAILURE;
    EGLDisplay dpy = 0; EGLContext ctx = 0; EGLConfig cfg = 0;
    const struct Hdr *h = (const struct Hdr *)ci->next;
    while (h) {
        if (h->type == 1000023000 /* XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR */) {
            XrGraphicsBindingOpenGLESAndroidKHR *b = (XrGraphicsBindingOpenGLESAndroidKHR *)h;
            dpy = b->display; ctx = b->context; cfg = b->config;
        }
        h = (const struct Hdr *)h->next;
    }
    if (!dpy || !ctx) {
        LOGE("createSession: 缺 GLES 图形绑定（dpy=%p ctx=%p）", dpy, ctx);
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    g_sess.dpy = dpy; g_sess.ctx = ctx; g_sess.cfg = cfg; g_sess.running = 0;
    *out = &g_sess;
    LOGI("createSession OK: dpy=%p ctx=%p cfg=%p", dpy, ctx, cfg);
    return XR_SUCCESS;
}

static XrResult xrDestroySession(void *s)
{ (void)s; g_sess.magic = 0; return XR_SUCCESS; }

static XrResult xrBeginSession(void *s, const void *ci)
{
    (void)ci;
    if (s != &g_sess) return XR_ERROR_HANDLE_INVALID;
    g_sess.running = 1;
    LOGI("beginSession: 姿态源=%s", g_pose_seen ? "已收到数据" : "尚无数据(检查 hwvrctl)");
    return XR_SUCCESS;
}

static XrResult xrEndSession(void *s)
{ if (s != &g_sess) return XR_ERROR_HANDLE_INVALID; g_sess.running = 0; return XR_SUCCESS; }

/* ---------------- 空间 / 视图 ---------------- */
static XrResult xrCreateReferenceSpace(void *s, const XrReferenceSpaceCreateInfo *ci, void **out)
{
    (void)s;
    if (!ci || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) *out = &g_space_view;
    else if (ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) *out = &g_space_local;
    else return XR_ERROR_FEATURE_UNSUPPORTED;
    return XR_SUCCESS;
}

static XrResult xrDestroySpace(void *sp) { (void)sp; return XR_SUCCESS; }

static XrResult xrLocateSpace(void *space, void *base, XrTime t, XrSpaceLocation *out)
{
    (void)base; (void)t;
    if (!out) return XR_ERROR_VALIDATION_FAILURE;
    pose_poll();
    out->type = XR_TYPE_SPACE_LOCATION; out->next = 0;
    Space *sp = (Space *)space;
    u64 flags = 0;
    if (sp && sp->type == XR_REFERENCE_SPACE_TYPE_VIEW) {
        out->pose.orientation = g_q;
        out->pose.position = g_pos;
        flags |= (1ull << 0) | (1ull << 1);              /* ORIENTATION_VALID | POSITION_VALID */
        if (!g_pos_valid) flags &= ~(1ull << 1);         /* 纯 3DoF：位置无效 */
        if (!g_pose_seen) flags = 0;
    } else {
        out->pose.orientation.x = 0; out->pose.orientation.y = 0;
        out->pose.orientation.z = 0; out->pose.orientation.w = 1;
        out->pose.position.x = 0; out->pose.position.y = 0; out->pose.position.z = 0;
        flags = (1ull << 0) | (1ull << 1);
    }
    out->locationFlags = flags;
    return XR_SUCCESS;
}

static XrResult xrLocateViews(void *s, const XrViewLocateInfo *li, XrViewState *state, u32 cap, u32 *count, XrView *views)
{
    (void)s; (void)li;
    pose_poll();
    if (count) *count = 2;
    if (state) {
        state->type = XR_TYPE_VIEW_STATE; state->next = 0;
        state->viewStateFlags = g_pose_seen ? (g_pos_valid ? 3u : 1u) : 0u;
    }
    if (views) {
        u32 fill = cap < 2 ? cap : 2;
        for (u32 k = 0; k < fill; k++) {
            views[k].type = XR_TYPE_VIEW; views[k].next = 0;
            views[k].pose.orientation = g_q;
            views[k].pose.position = g_pos;
            if (g_pos_valid) {
                /* 6DoF：左右眼按 IPD 在头系 x 轴分开 */
                views[k].pose.position.x += (k == 0 ? -0.5f : 0.5f) * IPD * g_q.w;
            }
            views[k].fov.angleLeft = -0.85f;   /* ≈ ±48.7°，CV10 单眼约 100° 对角 */
            views[k].fov.angleRight = 0.85f;
            views[k].fov.angleUp = 0.85f;
            views[k].fov.angleDown = -0.85f;
        }
    }
    return XR_SUCCESS;
}

/* ---------------- 交换链（真纹理） ---------------- */
static XrResult xrEnumerateSwapchainFormats(void *s, u32 cap, u32 *count, i64 *fmts)
{
    (void)s;
    if (count) *count = 3;
    if (fmts) {
        u32 fill = cap < 3 ? cap : 3;
        if (fill > 0) fmts[0] = GL_RGBA8;
        if (fill > 1) fmts[1] = 0x8C43;   /* GL_SRGB8_ALPHA8 */
        if (fill > 2) fmts[2] = 0x88F0;   /* GL_DEPTH24_STENCIL8 */
    }
    return XR_SUCCESS;
}

static void swap_ensure_textures(Swap *sw)
{
    if (sw->created) return;
    if (!g_sess.dpy || !g_sess.ctx) return;
    int pbuf[] = {EGL_WIDTH, (int)sw->w, EGL_HEIGHT, (int)sw->h, EGL_NONE};
    EGLSurface sf = eglCreatePbufferSurface(g_sess.dpy, g_sess.cfg, pbuf);
    if (sf && eglMakeCurrent(g_sess.dpy, sf, sf, g_sess.ctx) != 0) {
        glGenTextures((int)sw->n, sw->tex);
        for (u32 i = 0; i < sw->n; i++) {
            glBindTexture(GL_TEXTURE_2D, sw->tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (int)sw->w, (int)sw->h, 0, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        sw->created = 1;
        LOGI("swapchain %ux%u 建出 %u 张 RGBA8 纹理", sw->w, sw->h, sw->n);
    } else {
        LOGE("swapchain: eglMakeCurrent 失败，纹理延后到下次创建");
    }
}

static XrResult xrCreateSwapchain(void *s, const XrSwapchainCreateInfo *ci, void **out)
{
    (void)s;
    if (!ci || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (g_swap_count >= 4) return (-3);
    Swap *sw = &g_swaps[g_swap_count];
    sw->magic = 0x53574150u; sw->format = ci->format;
    sw->w = ci->width; sw->h = ci->height; sw->n = (ci->sampleCount > 1 ? 1 : 3);
    if (sw->n > MAX_TEX) sw->n = MAX_TEX;
    sw->idx = 0; sw->created = 0;
    g_swap_count++;
    *out = sw;
    swap_ensure_textures(sw);
    return XR_SUCCESS;
}

static XrResult xrDestroySwapchain(void *h) { (void)h; return XR_SUCCESS; }

static XrResult xrEnumerateSwapchainImages(void *h, u32 cap, u32 *count, XrSwapchainImageOpenGLESKHR *imgs)
{
    Swap *sw = (Swap *)h;
    if (!sw || sw->magic != 0x53574150u) return XR_ERROR_HANDLE_INVALID;
    swap_ensure_textures(sw);
    if (count) *count = sw->n;
    if (imgs) {
        u32 fill = cap < sw->n ? cap : sw->n;
        for (u32 k = 0; k < fill; k++) {
            imgs[k].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
            imgs[k].next = 0;
            imgs[k].image = sw->tex[k];
        }
    }
    return XR_SUCCESS;
}

static XrResult xrAcquireSwapchainImage(void *h, const void *ai, u32 *index)
{
    Swap *sw = (Swap *)h;
    if (!sw || sw->magic != 0x53574150u) return XR_ERROR_HANDLE_INVALID;
    if (index) *index = sw->idx;
    return XR_SUCCESS;
}

static XrResult xrWaitSwapchainImage(void *h, const void *wi)
{ (void)h; (void)wi; return XR_SUCCESS; }

static XrResult xrReleaseSwapchainImage(void *h, const void *ri)
{
    Swap *sw = (Swap *)h;
    if (!sw || sw->magic != 0x53574150u) return XR_ERROR_HANDLE_INVALID;
    sw->idx++; if (sw->n && sw->idx >= sw->n) sw->idx = 0;
    return XR_SUCCESS;
}

/* ---------------- 帧循环 ---------------- */
#define FRAME_NS 16666667ll   /* 60 Hz */
static i64 g_next_frame_ns;

static i64 now_ns(void)
{
    struct { i64 s; i64 ns; } ts;
    clock_gettime(1 /*CLOCK_MONOTONIC*/, &ts);
    return ts.s * 1000000000ll + ts.ns;
}

static XrResult xrWaitFrame(void *s, const XrFrameWaitInfo *wi, XrFrameState *fs)
{
    (void)s; (void)wi;
    pose_poll();
    if (!fs) return XR_ERROR_VALIDATION_FAILURE;
    i64 t = now_ns();
    if (g_next_frame_ns == 0 || t > g_next_frame_ns + FRAME_NS * 4) g_next_frame_ns = t + FRAME_NS;
    if (g_next_frame_ns > t) {
        /* 不用 64 位除法（ARM32 会引入 __aeabi_uldivmod 未定义符号）：
         * 固定 16.6ms 片，最多等 1 片，节拍偏差由 g_next_frame_ns 兜底 */
        struct { i64 s; i64 ns; } req = {0, FRAME_NS};
        nanosleep(&req, 0);
    }
    g_next_frame_ns += FRAME_NS;
    fs->type = XR_TYPE_FRAME_STATE; fs->next = 0;
    fs->predictedDisplayTime = now_ns();
    fs->predictedDisplayPeriod = FRAME_NS;
    fs->shouldRender = 1;
    return XR_SUCCESS;
}

static XrResult xrBeginFrame(void *s, const XrFrameBeginInfo *bi) { (void)s; (void)bi; return XR_SUCCESS; }

static XrResult xrEndFrame(void *s, const XrFrameEndInfo *fi)
{
    (void)s;
    if (fi && fi->layerCount) {
        /* v0.2 只做节拍与姿态；合成/呈现留到 v0.3（窗口获取后做 SBS 输出） */
    }
    return XR_SUCCESS;
}

/* ---------------- 路径 ---------------- */
static u32 g_path_next = 1;
static XrResult xrStringToPath(void *i, const char *str, XrPath *out)
{ (void)i; (void)str; if (!out) return XR_ERROR_VALIDATION_FAILURE; *out = g_path_next++; return XR_SUCCESS; }
static XrResult xrPathToString(void *i, XrPath p, u32 cap, u32 *count, char *b)
{
    (void)i; (void)p;
    if (count) *count = 2;
    if (!b || cap < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    b[0] = '/'; b[1] = 'x'; b[2] = 0;
    return XR_SUCCESS;
}

/* ---------------- 未实现项 ---------------- */
#define STUB(nm) static XrResult nm(void *a, ...) { LOGI("stub: " #nm); return XR_ERROR_FUNCTION_UNSUPPORTED; }
STUB(xrCreateActionSet) STUB(xrAttachSessionActionSets) STUB(xrSyncActions)
STUB(xrCreateActionSpace) STUB(xrLocateSpaceForAction)

/* ---------------- 派发 ---------------- */
typedef XrResult (*PFN)(void);
#define M(nm) if (!s_cmp(name, #nm)) return (PFN)nm;

static PFN resolve(const char *name)
{
    M(xrCreateInstance) M(xrDestroyInstance) M(xrGetInstanceProperties)
    M(xrEnumerateApiLayerProperties) M(xrEnumerateInstanceExtensionProperties)
    M(xrGetSystem) M(xrGetSystemProperties)
    M(xrEnumerateViewConfigurations) M(xrGetViewConfigurationProperties)
    M(xrEnumerateViewConfigurationViews) M(xrEnumerateEnvironmentBlendModes)
    M(xrGetOpenGLESGraphicsRequirementsKHR)
    M(xrCreateSession) M(xrDestroySession) M(xrBeginSession) M(xrEndSession)
    M(xrCreateReferenceSpace) M(xrDestroySpace) M(xrLocateSpace) M(xrLocateViews)
    M(xrEnumerateSwapchainFormats) M(xrCreateSwapchain) M(xrDestroySwapchain)
    M(xrEnumerateSwapchainImages) M(xrAcquireSwapchainImage) M(xrWaitSwapchainImage)
    M(xrReleaseSwapchainImage)
    M(xrWaitFrame) M(xrBeginFrame) M(xrEndFrame)
    M(xrStringToPath) M(xrPathToString)
    M(xrCreateActionSet) M(xrAttachSessionActionSets) M(xrSyncActions) M(xrCreateActionSpace)
    LOGI("loader asked for unimplemented proc: %s", name);
    return 0;
}

__attribute__((visibility("default")))
XrResult xrGetInstanceProcAddr(void *inst, const char *name, PFN *out)
{
    (void)inst;
    if (!out) return XR_ERROR_VALIDATION_FAILURE;
    if (!name) { *out = 0; return XR_SUCCESS; }
    if (!s_cmp(name, "xrGetInstanceProcAddr")) { *out = (PFN)xrGetInstanceProcAddr; return XR_SUCCESS; }
    PFN p = resolve(name);
    *out = p;
    return p ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}

__attribute__((visibility("default")))
XrResult xrNegotiateLoaderRuntimeInterface(const void *li, void **ri)
{ (void)li; (void)ri; return XR_ERROR_FUNCTION_UNSUPPORTED; }
