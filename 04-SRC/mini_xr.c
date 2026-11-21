/*
 * mini_xr.c —— 为 OnePlus 7T (hotdogb) + 华为 VR Glass (CV10) 自研的最小 OpenXR runtime
 * 阶段一（skeleton）：可被华为定制版 libxr_loader 加载，跑通
 *   instance → system → view configuration → GLES requirements → (session 待实现)
 * 编译目标：arm-linux-musleabihf, -shared -nostdlib（不链 libc，符号由 bionic 进程解析）
 * 输出：libvrsdk.so（放进 APK 的 lib/armeabi-v7a/，loader 默认 library_path 即此名）
 */

typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long long u64;
typedef long long      i64;
typedef unsigned char  u8;
typedef float          f32;

/* ---------- 日志（liblog 由进程已加载，此处仅留未定义符号） ---------- */
extern int __android_log_print(int prio, const char *tag, const char *fmt, ...);
static int s_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
#define LOGI(...) __android_log_print(4, "MINIXR", __VA_ARGS__)
#define LOGE(...) __android_log_print(6, "MINIXR", __VA_ARGS__)

/* ---------- OpenXR 最小 ABI ---------- */
typedef i32 XrResult;
typedef u64 XrFlags64;
typedef u64 XrSystemId;
typedef u64 XrVersion;
typedef u64 XrTime;
typedef u64 XrDuration;
typedef void *XrInstance;
typedef void *XrSession;
typedef void *XrSpace;
typedef void *XrSwapchain;
typedef void *XrActionSet;
typedef void *XrAction;
typedef u64 XrPath;
typedef u32 XrBool32;
typedef u64 XrSwapchainCreateFlags;
typedef u64 XrSwapchainUsageFlags;
typedef u64 XrSessionCreateFlags;
typedef u64 XrSpaceLocationFlags;
typedef u64 XrInstanceCreateFlags;

#define XR_SUCCESS                    0
#define XR_ERROR_VALIDATION_FAILURE  (-1)
#define XR_ERROR_RUNTIME_FAILURE     (-2)
#define XR_ERROR_OUT_OF_MEMORY       (-3)
#define XR_ERROR_API_VERSION_UNSUPPORTED (-4)
#define XR_ERROR_FUNCTION_UNSUPPORTED    (-5)
#define XR_ERROR_FEATURE_UNSUPPORTED     (-6)
#define XR_ERROR_EXTENSION_NOT_PRESENT   (-7)
#define XR_ERROR_PATH_INVALID            (-10)
#define XR_ERROR_SIZE_INSUFFICIENT       (-11)
#define XR_ERROR_HANDLE_INVALID          (-12)
#define XR_ERROR_INITIALIZATION_FAILED   (-35)

#define XR_TRUE  1
#define XR_FALSE 0

#define XR_TYPE_INSTANCE_CREATE_INFO              3
#define XR_TYPE_SYSTEM_GET_INFO                   4
#define XR_TYPE_SYSTEM_PROPERTIES                 5
#define XR_TYPE_VIEW_CONFIGURATION_PROPERTIES     7
#define XR_TYPE_VIEW_CONFIGURATION_VIEW          10
#define XR_TYPE_INSTANCE_PROPERTIES              11
#define XR_TYPE_EVENT_DATA_BUFFER                12
#define XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR 1000023001
#define XR_TYPE_SWAPCHAIN_CREATE_INFO            9
#define XR_TYPE_SESSION_CREATE_INFO              8

#define XR_MAX_APPLICATION_NAME_SIZE 128
#define XR_MAX_SYSTEM_NAME_SIZE      256
#define XR_MAX_EXTENSION_NAME_SIZE   128
#define XR_MAX_API_LAYER_NAME_SIZE   128

#define XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY 1
#define XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO 2
#define XR_ENVIRONMENT_BLEND_MODE_OPAQUE 1

#define GL_RGBA8              0x8058
#define GL_SRGB8_ALPHA8       0x8C43
#define GL_DEPTH24_STENCIL8   0x88F0

struct XrStructHeader { i32 type; const void *next; };

typedef struct {
    char applicationName[XR_MAX_APPLICATION_NAME_SIZE];
    u32  applicationVersion;
    char engineName[128];
    u32  engineVersion;
    XrVersion apiVersion;
} XrApplicationInfo;

typedef struct {
    i32 type; const void *next;
    XrInstanceCreateFlags createFlags;
    XrApplicationInfo applicationInfo;
    u32 enabledApiLayerCount; const char *const *enabledApiLayerNames;
    u32 enabledExtensionCount; const char *const *enabledExtensionNames;
} XrInstanceCreateInfo;

/* XR_KHR_android_create_instance */
typedef struct {
    i32 type; const void *next;
    void *applicationVM;
    void *applicationActivity;
} XrInstanceCreateInfoAndroidKHR;

typedef struct { i32 type; const void *next; i32 formFactor; } XrSystemGetInfo;

typedef struct {
    u32 maxSwapchainImageHeight;
    u32 maxSwapchainImageWidth;
    u32 maxLayerCount;
} XrSystemGraphicsProperties;

typedef struct {
    u32 orientationTracking;   /* XrBool32 */
    u32 positionTracking;
} XrSystemTrackingProperties;

typedef struct {
    i32 type; const void *next;
    XrSystemId systemId;
    u32 vendorId;
    char systemName[XR_MAX_SYSTEM_NAME_SIZE];
    XrSystemGraphicsProperties graphicsProperties;
    XrSystemTrackingProperties trackingProperties;
} XrSystemProperties;

typedef struct {
    i32 type; const void *next;
    i32 viewConfigurationType;
    u32 fovMutable;   /* XrBool32 */
} XrViewConfigurationProperties;

typedef struct {
    i32 type; const void *next;
    u32 recommendedImageRectWidth;
    u32 recommendedImageRectHeight;
    u32 maxImageRectWidth;
    u32 maxImageRectHeight;
    u32 recommendedSwapchainSampleCount;
    u32 maxSwapchainSampleCount;
} XrViewConfigurationView;

typedef struct {
    i32 type; const void *next;
    u32 runtimeVersion;
    char runtimeName[XR_MAX_APPLICATION_NAME_SIZE];
    u32 vendorId;
    char vendorName[XR_MAX_APPLICATION_NAME_SIZE];
} XrInstanceProperties;

typedef struct {
    i32 type; const void *next;
    XrVersion minApiVersionSupported;
    XrVersion maxApiVersionSupported;
} XrGraphicsRequirementsOpenGLESKHR;

typedef struct {
    i32 type; const void *next;
    XrSwapchainCreateFlags createFlags;
    XrSwapchainUsageFlags usageFlags;
    i64 format;
    u32 sampleCount;
    u32 width;
    u32 height;
    u32 faceCount;
    u32 arraySize;
    u32 mipCount;
} XrSwapchainCreateInfo;

typedef struct {
    i32 type; const void *next;
    XrSessionCreateFlags createFlags;
    XrSystemId systemId;
} XrSessionCreateInfo;

typedef struct {
    i32 type; const void *next;
    u32 layerCount;
    const char *const *layerNames;
} XrApiLayerProperties;   /* 仅前两字段被读，其余忽略 */

/* ---------- 单实例状态 ---------- */
typedef struct { u32 magic; u32 phase; } Inst;
static Inst g_inst = {0x4d585231u, 0};

/* 扩展清单：够 ALVR 走到 session 之前的所有查询 */
static const char *g_ext[] = {
    "XR_KHR_opengl_es_enable",
    "XR_KHR_android_create_instance",
    "XR_KHR_android_thread_settings",
    "XR_KHR_loader_init",
    "XR_KHR_loader_init_android",
    "XR_HUAWEI_controller_interaction",
    "XR_KHR_composition_layer_depth",
};

/* 视图参数：CV10 单片 1600×1600（双目共用一块 3840×1920 面板，SBS 各 1600×1600 有效区） */
#define VW 1600
#define VH 1600

static u32 g_path_next = 1;

/* ---------- 入口点 ---------- */
static XrResult xrCreateInstance(const XrInstanceCreateInfo *ci, XrInstance *out)
{
    if (!ci || !out) return XR_ERROR_VALIDATION_FAILURE;
    LOGI("createInstance: app=%s engine=%s api=0x%llx ext=%u",
         ci->applicationInfo.applicationName,
         ci->applicationInfo.engineName,
         (unsigned long long)ci->applicationInfo.apiVersion,
         ci->enabledExtensionCount);
    const struct XrStructHeader *h = (const struct XrStructHeader *)ci->next;
    while (h) {
        LOGI("  createInfo chain type=%d", h->type);
        if (h->type == 1000000001) {   /* XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR */
            XrInstanceCreateInfoAndroidKHR *a = (XrInstanceCreateInfoAndroidKHR *)h;
            LOGI("  android vm=%p activity=%p", a->applicationVM, a->applicationActivity);
        }
        h = (const struct XrStructHeader *)h->next;
    }
    if (ci->applicationInfo.apiVersion < 0x00010000ull) return XR_ERROR_API_VERSION_UNSUPPORTED;
    g_inst.phase = 1;
    *out = (XrInstance)&g_inst;
    LOGI("createInstance OK (mini_xr stage1)");
    return XR_SUCCESS;
}

static XrResult xrDestroyInstance(XrInstance i)
{
    if (i != (XrInstance)&g_inst) return XR_ERROR_HANDLE_INVALID;
    g_inst.phase = 0;
    return XR_SUCCESS;
}

static XrResult xrGetInstanceProperties(XrInstance i, XrInstanceProperties *p)
{
    if (!p) return XR_ERROR_VALIDATION_FAILURE;
    p->type = 11;                 /* XR_TYPE_INSTANCE_PROPERTIES */
    p->next = 0;
    p->runtimeVersion = 0x00010000ull;
    for (u32 k = 0; k < XR_MAX_APPLICATION_NAME_SIZE; k++) p->runtimeName[k] = 0;
    const char n[] = "mini_xr (7T/CV10)";
    for (u32 k = 0; k < sizeof(n) - 1; k++) p->runtimeName[k] = n[k];
    p->vendorId = 0;
    p->vendorName[0] = 0;
    return XR_SUCCESS;
}

static XrResult xrEnumerateApiLayerProperties(u32 cap, u32 *count, void *props)
{
    (void)props;
    if (count) *count = 0;
    (void)cap;
    return XR_SUCCESS;
}

static XrResult xrEnumerateInstanceExtensionProperties(const char *layer, u32 cap, u32 *count, void *props)
{
    (void)layer; (void)props;
    u32 n = (u32)(sizeof(g_ext) / sizeof(g_ext[0]));
    if (count) *count = n;
    if (*count > cap) return XR_ERROR_SIZE_INSUFFICIENT;   /* 上层会再问一次 */
    return XR_SUCCESS;
}

static XrResult xrGetSystem(XrInstance i, const XrSystemGetInfo *info, XrSystemId *out)
{
    (void)i;
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    LOGI("getSystem formFactor=%d", info->formFactor);
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) return XR_ERROR_FEATURE_UNSUPPORTED;
    *out = 1;
    return XR_SUCCESS;
}

static XrResult xrGetSystemProperties(XrInstance i, XrSystemId s, XrSystemProperties *p)
{
    (void)i; (void)s;
    if (!p) return XR_ERROR_VALIDATION_FAILURE;
    p->type = XR_TYPE_SYSTEM_PROPERTIES; p->next = 0;
    p->systemId = 1; p->vendorId = 0;
    for (u32 k = 0; k < XR_MAX_SYSTEM_NAME_SIZE; k++) p->systemName[k] = 0;
    const char n[] = "Huawei VR Glass (CV10) via mini_xr";
    for (u32 k = 0; k < sizeof(n) - 1 && k < XR_MAX_SYSTEM_NAME_SIZE; k++) p->systemName[k] = n[k];
    p->graphicsProperties.maxSwapchainImageHeight = 3200;
    p->graphicsProperties.maxSwapchainImageWidth  = 3200;
    p->graphicsProperties.maxLayerCount = 4;
    p->trackingProperties.orientationTracking = XR_TRUE;   /* 3DoF 有 */
    p->trackingProperties.positionTracking    = XR_FALSE;  /* 6DoF 无（SLAM 绑麒麟 NPU） */
    LOGI("getSystemProperties OK");
    return XR_SUCCESS;
}

static XrResult xrEnumerateViewConfigurations(XrInstance i, XrSystemId s, u32 cap, u32 *count, i32 *types)
{
    (void)i; (void)s;
    if (count) *count = 1;
    if (types && cap >= 1) types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

static XrResult xrGetViewConfigurationProperties(XrInstance i, XrSystemId s, i32 t, XrViewConfigurationProperties *p)
{
    (void)i; (void)s;
    if (!p) return XR_ERROR_VALIDATION_FAILURE;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_FEATURE_UNSUPPORTED;
    p->type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES; p->next = 0;
    p->viewConfigurationType = t; p->fovMutable = XR_TRUE;
    return XR_SUCCESS;
}

static XrResult xrEnumerateViewConfigurationViews(XrInstance i, XrSystemId s, i32 t, u32 cap, u32 *count, XrViewConfigurationView *views)
{
    (void)i; (void)s; (void)t;
    u32 n = 2;
    if (count) *count = n;
    if (views) {
        u32 fill = cap < n ? cap : n;
        for (u32 k = 0; k < fill; k++) {
            views[k].type = XR_TYPE_VIEW_CONFIGURATION_VIEW; views[k].next = 0;
            views[k].recommendedImageRectWidth = VW;
            views[k].recommendedImageRectHeight = VH;
            views[k].maxImageRectWidth = VW + 64;
            views[k].maxImageRectHeight = VH + 64;
            views[k].recommendedSwapchainSampleCount = 1;
            views[k].maxSwapchainSampleCount = 1;
        }
    }
    LOGI("enumerateViewConfigurationViews: %u views %dx%d", n, VW, VH);
    return XR_SUCCESS;
}

static XrResult xrEnumerateEnvironmentBlendModes(XrInstance i, XrSystemId s, i32 t, u32 cap, u32 *count, i32 *modes)
{
    (void)i; (void)s; (void)t;
    if (count) *count = 1;
    if (modes && cap >= 1) modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

static XrResult xrGetOpenGLESGraphicsRequirementsKHR(XrInstance i, XrSystemId s, XrGraphicsRequirementsOpenGLESKHR *r)
{
    (void)i; (void)s;
    if (!r) return XR_ERROR_VALIDATION_FAILURE;
    r->type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR; r->next = 0;
    r->minApiVersionSupported = 0x00030000ull;   /* GLES 3.0 */
    r->maxApiVersionSupported = 0x00030002ull;   /* GLES 3.2 —— Adreno 640 够 */
    LOGI("GLES requirements 3.0 .. 3.2");
    return XR_SUCCESS;
}

static XrResult xrEnumerateSwapchainFormats(XrSession s, u32 cap, u32 *count, i64 *fmts)
{
    (void)s;
    u32 n = 3;
    if (count) *count = n;
    if (fmts) {
        u32 fill = cap < n ? cap : n;
        if (fill > 0) fmts[0] = GL_RGBA8;          /* ← ALVR 硬要求 */
        if (fill > 1) fmts[1] = GL_SRGB8_ALPHA8;
        if (fill > 2) fmts[2] = GL_DEPTH24_STENCIL8;
    }
    return XR_SUCCESS;
}

static XrResult xrStringToPath(XrInstance i, const char *str, XrPath *out)
{
    (void)i; (void)str;
    if (!out) return XR_ERROR_VALIDATION_FAILURE;
    *out = g_path_next++;
    return XR_SUCCESS;
}

static XrResult xrPathToString(XrInstance i, XrPath p, u32 cap, u32 *count, char *buf)
{
    (void)i; (void)p;
    char tmp[64];
    tmp[0] = '/'; tmp[1] = 'x'; tmp[2] = 0;
    if (count) *count = 2;
    if (!buf || cap < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    buf[0] = '/'; buf[1] = 'x'; buf[2] = 0;
    return XR_SUCCESS;
}

/* 未实现但必须存在的入口点 → 统一返回 FUNCTION_UNSUPPORTED，让 ALVR 打日志而不是崩 */
#define STUB(nm) \
static XrResult nm(void *a, ...) { LOGI("stub: " #nm " -> unsupported"); return XR_ERROR_FUNCTION_UNSUPPORTED; }

STUB(xrCreateSession) STUB(xrDestroySession) STUB(xrBeginSession) STUB(xrEndSession)
STUB(xrWaitFrame) STUB(xrBeginFrame) STUB(xrEndFrame) STUB(xrPollEvent)
STUB(xrCreateSwapchain) STUB(xrDestroySwapchain) STUB(xrAcquireSwapchainImage)
STUB(xrWaitSwapchainImage) STUB(xrReleaseSwapchainImage) STUB(xrEnumerateSwapchainImages)
STUB(xrLocateViews) STUB(xrCreateReferenceSpace) STUB(xrLocateSpace) STUB(xrDestroySpace)
STUB(xrCreateActionSet) STUB(xrAttachSessionActionSets) STUB(xrSyncActions) STUB(xrGetActionStateBoolean)

/* ---------- 派发 ---------- */
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
    M(xrEnumerateSwapchainFormats)
    M(xrStringToPath) M(xrPathToString)
    M(xrCreateSession) M(xrDestroySession) M(xrBeginSession) M(xrEndSession)
    M(xrWaitFrame) M(xrBeginFrame) M(xrEndFrame) M(xrPollEvent)
    M(xrCreateSwapchain) M(xrDestroySwapchain) M(xrAcquireSwapchainImage)
    M(xrWaitSwapchainImage) M(xrReleaseSwapchainImage) M(xrEnumerateSwapchainImages)
    M(xrLocateViews) M(xrCreateReferenceSpace) M(xrLocateSpace) M(xrDestroySpace)
    M(xrCreateActionSet) M(xrAttachSessionActionSets) M(xrSyncActions) M(xrGetActionStateBoolean)
    LOGI("loader asked for unimplemented proc: %s", name);
    return 0;
}

__attribute__((visibility("default")))
XrResult xrGetInstanceProcAddr(XrInstance inst, const char *name, PFN *out)
{
    if (!out) return XR_ERROR_VALIDATION_FAILURE;
    if (!name) { *out = 0; return XR_SUCCESS; }
    if (!s_cmp(name, "xrGetInstanceProcAddr")) { *out = (PFN)xrGetInstanceProcAddr; return XR_SUCCESS; }
    (void)inst;
    PFN p = resolve(name);
    *out = p;
    return p ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}

__attribute__((visibility("default")))
XrResult xrNegotiateLoaderRuntimeInterface(const void *li, void **ri)
{
    (void)li; (void)ri;
    LOGI("NegotiateLoaderRuntimeInterface");
    return XR_ERROR_FUNCTION_UNSUPPORTED;   /* 华为 loader 走 manifest 兜底路径 */
}
