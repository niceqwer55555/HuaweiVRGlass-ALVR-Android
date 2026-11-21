/*
 * xr_core.c - 极简 OpenXR 运行时核心
 *
 * 只做 ALVR 客户端需要的事：
 *   instance / system / session / space / view / action / 事件 / 帧
 * 画面合成见 xr_display.c，姿态见 xr_pose.c。
 */
#include "xr_shim.h"

#include <stdlib.h>
#include <unistd.h>

xr_state_t g_xr;

int xr_cfg_eye_res(void);
float xr_cfg_fov_deg(void);
float xr_cfg_ipd(void);

/* ------------------------------------------------------------------ */
/* 事件队列                                                            */
/* ------------------------------------------------------------------ */

static void event_push(const XrEventDataBuffer *ev) {
    int next = (g_xr.ev_head + 1) % XR_EVENT_QUEUE;
    if (next == g_xr.ev_tail) {
        XWARN("事件队列满，丢弃");
        return;
    }
    g_xr.events[g_xr.ev_head] = *ev;
    g_xr.ev_head = next;
}

void xr_event_push_state(XrSessionState state) {
    XrEventDataSessionStateChanged ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    ev.session = g_xr.session;
    ev.state = state;
    ev.time = xr_now_ns();
    g_xr.session_state = state;
    event_push((const XrEventDataBuffer *)&ev);
    XLOG("EVENT: session state -> %d (%lld)", (int)state, (long long)ev.time);
}

/* ------------------------------------------------------------------ */
/* 路径表                                                              */
/* ------------------------------------------------------------------ */

XrPath xr_path_intern(const char *str) {
    if (!str) return XR_NULL_PATH;
    for (int i = 0; i < g_xr.path_count; i++) {
        if (strcmp(g_xr.paths[i].str, str) == 0) return g_xr.paths[i].path;
    }
    if (g_xr.path_count >= XR_MAX_PATHS) return XR_NULL_PATH;
    g_xr.paths[g_xr.path_count].str = strdup(str);
    g_xr.paths[g_xr.path_count].path = (XrPath)(g_xr.path_count + 1);
    return g_xr.paths[g_xr.path_count++].path;
}

const char *xr_path_lookup(XrPath path) {
    for (int i = 0; i < g_xr.path_count; i++) {
        if (g_xr.paths[i].path == path) return g_xr.paths[i].str;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 枚举 / 扩展                                                         */
/* ------------------------------------------------------------------ */

static const char *k_extensions[] = {
        "XR_KHR_opengl_es_enable",
        "XR_KHR_convert_timespec_time",
        "XR_KHR_loader_init_android",
        "XR_KHR_android_create_instance",
};

#define EXT_COUNT ((int)(sizeof(k_extensions) / sizeof(k_extensions[0])))

XrResult xrEnumerateApiLayerProperties(uint32_t capacityInput, uint32_t *countOutput,
                                       XrApiLayerProperties *properties) {
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 0;
    if (capacityInput == 0) return XR_SUCCESS;
    return capacityInput >= 0 ? XR_SUCCESS : XR_ERROR_SIZE_INSUFFICIENT;
}

XrResult xrEnumerateInstanceExtensionProperties(const char *layerName, uint32_t capacityInput,
                                                uint32_t *countOutput,
                                                XrExtensionProperties *properties) {
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    (void)layerName;
    *countOutput = EXT_COUNT;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < EXT_COUNT) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < EXT_COUNT; i++) {
        memset(&properties[i], 0, sizeof(properties[i]));
        properties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        properties[i].extensionVersion = 1;
        strncpy(properties[i].extensionName, k_extensions[i],
                XR_MAX_EXTENSION_NAME_SIZE - 1);
    }
    return XR_SUCCESS;
}

XrResult xrInitializeLoaderKHR(const XrLoaderInitInfoBaseHeaderKHR *info) {
    (void)info;
    XLOG("LOADER: xrInitializeLoaderKHR 调用（忽略，平台 API 不需要）");
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Instance / System                                                   */
/* ------------------------------------------------------------------ */

XrResult xrCreateInstance(const XrInstanceCreateInfo *createInfo, XrInstance *instance) {
    if (!createInfo || !instance) return XR_ERROR_VALIDATION_FAILURE;
    XLOG("INSTANCE: 创建 apiVersion=%u.%u.%u name=%s",
         XR_VERSION_MAJOR(createInfo->applicationInfo.apiVersion),
         XR_VERSION_MINOR(createInfo->applicationInfo.apiVersion),
         XR_VERSION_PATCH(createInfo->applicationInfo.apiVersion),
         createInfo->applicationInfo.applicationName);

    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++) {
        XLOG("INSTANCE: 启用扩展 %s", createInfo->enabledExtensionNames[i]);
    }

    /* 注意：不能直接 memset —— Java 侧的眼镜窗口（Dialog/Presentation 的 Surface）
     * 可能比 instance 更早到达（RK3566 盒子走主屏 Dialog 时就是这样），
     * memset 会把 g_xr.window 清掉，导致后面 init_gl 报「还没有窗口」，
     * 整条合成链路直接不启动。这里把显示相关的字段原样保留。 */
    ANativeWindow *keep_window = g_xr.window;
    EGLDisplay keep_dpy = g_xr.egl_display;
    EGLContext keep_app_ctx = g_xr.app_context;
    EGLContext keep_our_ctx = g_xr.our_context;
    EGLSurface keep_our_surf = g_xr.our_surface;
    bool keep_gl_ready = g_xr.gl_ready;
    GLuint keep_prog = g_xr.prog, keep_vbo = g_xr.vbo, keep_vao = g_xr.vao;
    GLint keep_xform = g_xr.u_xform;
    GLint keep_tex = g_xr.u_tex;
    bool keep_comp_ready = g_xr.comp_ready;
    EGLConfig keep_cfg = g_xr.egl_config;

    memset(&g_xr, 0, sizeof(g_xr));

    g_xr.window = keep_window;
    g_xr.egl_display = keep_dpy;
    g_xr.egl_config = keep_cfg;
    g_xr.app_context = keep_app_ctx;
    g_xr.our_context = keep_our_ctx;
    g_xr.our_surface = keep_our_surf;
    g_xr.gl_ready = keep_gl_ready;
    g_xr.prog = keep_prog;
    g_xr.vbo = keep_vbo;
    g_xr.vao = keep_vao;
    g_xr.u_xform = keep_xform;
    g_xr.u_tex = keep_tex;
    g_xr.comp_ready = keep_comp_ready;
    if (keep_window) {
        XLOG("INSTANCE: 保留已到达的眼镜窗口 %dx%d",
             ANativeWindow_getWidth(keep_window), ANativeWindow_getHeight(keep_window));
    }

    g_xr.instance = (XrInstance)0x494E5354;
    g_xr.instance_created = true;
    g_xr.system_id = (XrSystemId)1;
    g_xr.next_handle = 100;
    g_xr.predicted_period_ns = 1000000000LL / XR_REFRESH_HZ;
    *instance = g_xr.instance;

    xr_pose_init();
    return XR_SUCCESS;
}

XrResult xrDestroyInstance(XrInstance instance) {
    (void)instance;
    XLOG("INSTANCE: 销毁");
    xr_pose_shutdown();
    xr_display_shutdown();
    return XR_SUCCESS;
}

XrResult xrGetInstanceProperties(XrInstance instance, XrInstanceProperties *properties) {
    (void)instance;
    if (!properties) return XR_ERROR_VALIDATION_FAILURE;
    memset(properties, 0, sizeof(*properties));
    properties->type = XR_TYPE_INSTANCE_PROPERTIES;
    properties->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    snprintf(properties->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, "XrShim (no-Huawei)");
    return XR_SUCCESS;
}

XrResult xrGetSystem(XrInstance instance, const XrSystemGetInfo *getInfo, XrSystemId *systemId) {
    (void)instance;
    if (!getInfo || !systemId) return XR_ERROR_VALIDATION_FAILURE;
    if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    }
    *systemId = g_xr.system_id;
    return XR_SUCCESS;
}

XrResult xrGetSystemProperties(XrInstance instance, XrSystemId systemId,
                               XrSystemProperties *properties) {
    (void)instance;
    (void)systemId;
    if (!properties) return XR_ERROR_VALIDATION_FAILURE;
    memset(properties, 0, sizeof(*properties));
    properties->type = XR_TYPE_SYSTEM_PROPERTIES;
    properties->systemId = g_xr.system_id;
    properties->vendorId = 0;
    snprintf(properties->systemName, XR_MAX_SYSTEM_NAME_SIZE, "HUAWEI VR Glass (XrShim)");
    properties->graphicsProperties.maxSwapchainImageWidth = 4096;
    properties->graphicsProperties.maxSwapchainImageHeight = 4096;
    properties->graphicsProperties.maxLayerCount = 4;
    properties->trackingProperties.orientationTracking = XR_TRUE;
    properties->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* 视图配置                                                            */
/* ------------------------------------------------------------------ */

XrResult xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId,
                                       uint32_t capacityInput, uint32_t *countOutput,
                                       XrViewConfigurationType *types) {
    (void)instance;
    (void)systemId;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 1;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < 1) return XR_ERROR_SIZE_INSUFFICIENT;
    types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

XrResult xrGetViewConfigurationProperties(XrInstance instance, XrSystemId systemId,
                                          XrViewConfigurationType type,
                                          XrViewConfigurationProperties *props) {
    (void)instance;
    (void)systemId;
    if (!props) return XR_ERROR_VALIDATION_FAILURE;
    memset(props, 0, sizeof(*props));
    props->type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES;
    props->viewConfigurationType = type;
    props->fovMutable = XR_FALSE;
    return XR_SUCCESS;
}

XrResult xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId,
                                           XrViewConfigurationType type,
                                           uint32_t capacityInput, uint32_t *countOutput,
                                           XrViewConfigurationView *views) {
    (void)instance;
    (void)systemId;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    int res = xr_cfg_eye_res();
    *countOutput = 2;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 2; i++) {
        memset(&views[i], 0, sizeof(views[i]));
        views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        views[i].recommendedImageRectWidth = (uint32_t)res;
        views[i].recommendedImageRectHeight = (uint32_t)res;
        views[i].maxImageRectWidth = (uint32_t)res;
        views[i].maxImageRectHeight = (uint32_t)res;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }
    XLOG("VIEWCONFIG: 上报每眼 %dx%d", res, res);
    return XR_SUCCESS;
}

XrResult xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId,
                                          XrViewConfigurationType type, uint32_t capacityInput,
                                          uint32_t *countOutput,
                                          XrEnvironmentBlendMode *blendModes) {
    (void)instance;
    (void)systemId;
    (void)type;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 1;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < 1) return XR_ERROR_SIZE_INSUFFICIENT;
    blendModes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

XrResult xrGetOpenGLESGraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId,
                                              XrGraphicsRequirementsOpenGLESKHR *reqs) {
    (void)instance;
    (void)systemId;
    if (!reqs) return XR_ERROR_VALIDATION_FAILURE;
    memset(reqs, 0, sizeof(*reqs));
    reqs->type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
    reqs->minApiVersionSupported = XR_MAKE_VERSION(3, 0, 0);
    reqs->maxApiVersionSupported = XR_MAKE_VERSION(3, 2, 0);
    XLOG("GLESREQ: 上报 GLES 3.0 ~ 3.2");
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

XrResult xrCreateSession(XrInstance instance, const XrSessionCreateInfo *createInfo,
                         XrSession *session) {
    (void)instance;
    if (!createInfo || !session) return XR_ERROR_VALIDATION_FAILURE;

    const XrBaseInStructure *next = (const XrBaseInStructure *)createInfo->next;
    while (next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR) {
            const XrGraphicsBindingOpenGLESAndroidKHR *b =
                    (const XrGraphicsBindingOpenGLESAndroidKHR *)next;
            g_xr.egl_display = (EGLDisplay)b->display;
            g_xr.egl_config = (EGLConfig)b->config;
            g_xr.app_context = (EGLContext)b->context;
            XLOG("SESSION: 拿到 ALVR 的 EGL display=%p config=%p context=%p",
                 b->display, b->config, b->context);
        }
        next = next->next;
    }

    g_xr.session = (XrSession)0x53455353;
    *session = g_xr.session;

    /* 眼镜（输出窗口）可能还没插上。
     *
     * 历史教训：这里原来会阻塞等副屏最多 120 秒。但 ALVR 的串流连接线程是在
     * xrCreateSession 返回之后才启动的，长阻塞会让手机侧 9944 端口一直不监听，
     * 服务端永远连不上——表现为"进了 VR 但一直空白/客户端 Disconnected"。
     *
     * 所以这里绝不能阻塞：只做一次尽力而为的初始化，窗口晚到时由 xrEndFrame
     * 的 !gl_ready 分支重试（它会拿 g_xr.window 再调一次 xr_display_init_gl）。 */
    if (!g_xr.window) {
        XWARN("SESSION: 眼镜副屏还没出现，先建会话；窗口到了自动补初始化");
    }

    if (!xr_display_init_gl()) {
        XWARN("SESSION: GL 还没就绪");
    }

    xr_event_push_state(XR_SESSION_STATE_IDLE);
    xr_event_push_state(XR_SESSION_STATE_READY);
    XLOG("SESSION: 创建完成");
    return XR_SUCCESS;
}

XrResult xrDestroySession(XrSession session) {
    (void)session;
    XLOG("SESSION: 销毁");
    g_xr.session = XR_NULL_HANDLE;
    return XR_SUCCESS;
}

XrResult xrBeginSession(XrSession session, const XrSessionBeginInfo *beginInfo) {
    (void)session;
    (void)beginInfo;
    XLOG("SESSION: begin");
    xr_event_push_state(XR_SESSION_STATE_SYNCHRONIZED);
    xr_event_push_state(XR_SESSION_STATE_VISIBLE);
    xr_event_push_state(XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}

XrResult xrEndSession(XrSession session) {
    (void)session;
    XLOG("SESSION: end");
    xr_event_push_state(XR_SESSION_STATE_STOPPING);
    xr_event_push_state(XR_SESSION_STATE_IDLE);
    return XR_SUCCESS;
}

XrResult xrRequestExitSession(XrSession session) {
    (void)session;
    xr_event_push_state(XR_SESSION_STATE_EXITING);
    return XR_SUCCESS;
}

XrResult xrPollEvent(XrInstance instance, XrEventDataBuffer *eventData) {
    (void)instance;
    if (!eventData) return XR_ERROR_VALIDATION_FAILURE;
    if (g_xr.ev_tail == g_xr.ev_head) return XR_EVENT_UNAVAILABLE;
    *eventData = g_xr.events[g_xr.ev_tail];
    g_xr.ev_tail = (g_xr.ev_tail + 1) % XR_EVENT_QUEUE;
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Space                                                               */
/* ------------------------------------------------------------------ */

XrResult xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo *info,
                                XrSpace *space) {
    (void)session;
    if (!info || !space) return XR_ERROR_VALIDATION_FAILURE;
    space_t *s = calloc(1, sizeof(space_t));
    if (!s) return XR_ERROR_OUT_OF_MEMORY;
    switch (info->referenceSpaceType) {
        case XR_REFERENCE_SPACE_TYPE_VIEW: s->kind = SPACE_VIEW; break;
        case XR_REFERENCE_SPACE_TYPE_STAGE: s->kind = SPACE_STAGE; break;
        case XR_REFERENCE_SPACE_TYPE_LOCAL: s->kind = SPACE_LOCAL; break;
        default: s->kind = SPACE_LOCAL; break;
    }
    s->handle = (XrSpace)(uint64_t)(g_xr.next_handle++);
    s->next = g_xr.spaces;
    g_xr.spaces = s;
    *space = s->handle;
    XLOG("SPACE: 创建 type=%d handle=%llu", (int)info->referenceSpaceType,
         (unsigned long long)s->handle);
    return XR_SUCCESS;
}

XrResult xrDestroySpace(XrSpace space) {
    space_t **pp = &g_xr.spaces;
    while (*pp) {
        if ((*pp)->handle == space) {
            space_t *dead = *pp;
            *pp = dead->next;
            free(dead);
            return XR_SUCCESS;
        }
        pp = &(*pp)->next;
    }
    return XR_SUCCESS;
}

static space_t *find_space(XrSpace h) {
    for (space_t *s = g_xr.spaces; s; s = s->next) {
        if (s->handle == h) return s;
    }
    return NULL;
}

/* 某空间在 STAGE 坐标系下的位姿（无效则返回 false） */
static bool space_pose_in_stage(const space_t *s, xq_t *rot, xv3_t *pos, bool *has_pos) {
    if (!s) return false;
    switch (s->kind) {
        case SPACE_STAGE:
            rot->x = rot->y = rot->z = 0;
            rot->w = 1;
            pos->x = pos->y = pos->z = 0;
            *has_pos = true;
            return true;
        case SPACE_VIEW: {
            bool tracked = false;
            xr_pose_get(rot, pos, &tracked);
            /* 3DoF 眼镜没有位置追踪，但必须上报 POSITION_VALID/TRACKED，
             * 否则 SteamVR 认为头显"未佩戴/未追踪"，约每 10 秒进 standby，
             * layers=0 → 眼镜只剩纯色待机画面，默认场景起不来。
             * 这里用稳定的坐姿眼高代替真实位置（stream.rs 本来也会在
             * three_dof 分支把位置压成 (0, height, 0)）。 */
            *has_pos = true;
            return true;
        }
        case SPACE_LOCAL:
            rot->x = rot->y = rot->z = 0;
            rot->w = 1;
            pos->x = pos->y = pos->z = 0;
            *has_pos = true;
            return true;
        case SPACE_ACTION:
        default:
            return false;
    }
}

XrResult xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation *location) {
    (void)time;
    if (!location) return XR_ERROR_VALIDATION_FAILURE;
    memset(location, 0, sizeof(*location));
    location->type = XR_TYPE_SPACE_LOCATION;

    space_t *s = find_space(space);
    space_t *b = find_space(baseSpace);
    xq_t sr, br;
    xv3_t sp, bp;
    bool shp = false, bhp = false;
    if (!space_pose_in_stage(s, &sr, &sp, &shp) || !space_pose_in_stage(b, &br, &bp, &bhp)) {
        /* 动作空间等：未激活 */
        location->locationFlags = 0;
        return XR_SUCCESS;
    }
    xq_t inv_b = xq_conj(br);
    xq_t r = xq_norm(xq_mul(inv_b, sr));
    xv3_t rel = {sp.x - bp.x, sp.y - bp.y, sp.z - bp.z};
    xv3_t out = xq_rotate(inv_b, rel);

    location->pose.orientation.x = r.x;
    location->pose.orientation.y = r.y;
    location->pose.orientation.z = r.z;
    location->pose.orientation.w = r.w;
    location->pose.position.x = out.x;
    location->pose.position.y = out.y;
    location->pose.position.z = out.z;
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if (shp) {
        location->locationFlags |= XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                   XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    }
    return XR_SUCCESS;
}

XrResult xrEnumerateReferenceSpaces(XrSession session, uint32_t capacityInput,
                                    uint32_t *countOutput, XrReferenceSpaceType *spaces) {
    (void)session;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 2;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    spaces[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaces[1] = XR_REFERENCE_SPACE_TYPE_STAGE;
    return XR_SUCCESS;
}

XrResult xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType type,
                                       XrExtent2Df *bounds) {
    (void)session;
    (void)type;
    if (!bounds) return XR_ERROR_VALIDATION_FAILURE;
    bounds->width = 2.0f;
    bounds->height = 2.0f;
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* 视图定位                                                            */
/* ------------------------------------------------------------------ */

XrResult xrLocateViews(XrSession session, const XrViewLocateInfo *info, XrViewState *state,
                       uint32_t capacityInput, uint32_t *countOutput, XrView *views) {
    (void)session;
    if (!info || !countOutput || !state) return XR_ERROR_VALIDATION_FAILURE;
    if (info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    *countOutput = 2;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < 2) return XR_ERROR_SIZE_INSUFFICIENT;

    memset(state, 0, sizeof(*state));
    state->type = XR_TYPE_VIEW_STATE;

    xq_t head;
    xv3_t hpos;
    bool tracked = false;
    xr_pose_get(&head, &hpos, &tracked);

    space_t *base = find_space(info->space);
    xq_t brot = {0, 0, 0, 1};
    xv3_t bpos = {0, 0, 0};
    bool bhp = true;
    if (base) space_pose_in_stage(base, &brot, &bpos, &bhp);

    xq_t inv = xq_conj(brot);
    xq_t rel_rot = xq_norm(xq_mul(inv, head));
    xv3_t rel_pos = xq_rotate(inv, (xv3_t){hpos.x - bpos.x, hpos.y - bpos.y, hpos.z - bpos.z});

    float fov = xr_cfg_fov_deg() * 3.14159265f / 180.0f;
    float ipd = xr_cfg_ipd();

    for (uint32_t i = 0; i < 2; i++) {
        memset(&views[i], 0, sizeof(views[i]));
        views[i].type = XR_TYPE_VIEW;
        float dx = (i == 0 ? -0.5f : 0.5f) * ipd;
        xv3_t eye_off = xq_rotate(rel_rot, (xv3_t){dx, 0, 0});
        views[i].pose.orientation.x = rel_rot.x;
        views[i].pose.orientation.y = rel_rot.y;
        views[i].pose.orientation.z = rel_rot.z;
        views[i].pose.orientation.w = rel_rot.w;
        views[i].pose.position.x = rel_pos.x + eye_off.x;
        views[i].pose.position.y = rel_pos.y + eye_off.y;
        views[i].pose.position.z = rel_pos.z + eye_off.z;
        views[i].fov.angleLeft = -fov;
        views[i].fov.angleRight = fov;
        views[i].fov.angleUp = fov;
        views[i].fov.angleDown = -fov;
    }
    /* 3DoF：位置用眼高常量，必须报 POSITION_VALID/TRACKED，否则 SteamVR
     * 判定头显未追踪→待机→layers=0→眼镜纯色。见 space_pose_in_stage 注释。 */
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                            XR_VIEW_STATE_POSITION_VALID_BIT |
                            XR_VIEW_STATE_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* 帧                                                                  */
/* ------------------------------------------------------------------ */

XrResult xrWaitFrame(XrSession session, const XrFrameWaitInfo *frameWaitInfo,
                     XrFrameState *frameState) {
    (void)session;
    (void)frameWaitInfo;
    if (!frameState) return XR_ERROR_VALIDATION_FAILURE;

    /* 按 70Hz 节拍 */
    int64_t now = xr_now_ns();
    int64_t period = g_xr.predicted_period_ns;
    if (g_xr.last_frame_ns > 0) {
        int64_t target = g_xr.last_frame_ns + period;
        while (now < target) {
            int64_t remain = target - now;
            usleep((useconds_t)(remain > 2000000 ? 2000 : remain / 1000));
            now = xr_now_ns();
        }
    }
    g_xr.last_frame_ns = now;

    memset(frameState, 0, sizeof(*frameState));
    frameState->type = XR_TYPE_FRAME_STATE;
    frameState->predictedDisplayTime = now + period;
    frameState->predictedDisplayPeriod = period;
    frameState->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}

XrResult xrBeginFrame(XrSession session, const XrFrameBeginInfo *frameBeginInfo) {
    (void)session;
    (void)frameBeginInfo;
    return XR_SUCCESS;
}

XrResult xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo) {
    (void)session;
    if (!frameEndInfo) return XR_ERROR_VALIDATION_FAILURE;

    static long long stat_frames;
    static unsigned stat_proj;
    static int stat_layers_logged;

    /* skip_present=1：我们完全不碰窗口，用来验证 ALVR 是不是自己在往窗口上画 */
    if (xr_cfg_skip_present()) {
        static bool once;
        if (!once) {
            XLOG("ENDFRAME: skip_present=1，本运行时不再输出到窗口");
            once = true;
        }
        return XR_SUCCESS;
    }

    /*
     * 【诊断】此刻当前上下文还是 ALVR 的，正好可以偷看它把东西渲到哪去了。
     *   fbo=0        → 用默认帧缓冲，也就是它自己的 window surface
     *   drawSurface  → 这个 surface 是不是我们给的眼镜窗口
     *   viewport     → 它实际在渲多大
     * 每 70 帧（约 1 秒）打一次。
     */
    if (xr_cfg_probe_swap()) {
        static int probe_ctx_n;
        if ((probe_ctx_n++ % 70) == 0) {
            GLint fbo = -1, vp[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
            glGetIntegerv(GL_VIEWPORT, vp);
            EGLContext actx = eglGetCurrentContext();
            EGLSurface adraw = eglGetCurrentSurface(EGL_DRAW);
            EGLint sw = 0, sh = 0;
            if (adraw != EGL_NO_SURFACE && g_xr.egl_display != EGL_NO_DISPLAY) {
                eglQuerySurface(g_xr.egl_display, adraw, EGL_WIDTH, &sw);
                eglQuerySurface(g_xr.egl_display, adraw, EGL_HEIGHT, &sh);
            }
            XLOG("PROBE-CTX: ALVR fbo=%d vp=[%d,%d %dx%d] draw=%p %dx%d ctx=%p (我们的 ctx=%p win=%p)",
                 fbo, vp[0], vp[1], vp[2], vp[3], (void *)adraw, sw, sh, (void *)actx,
                 (void *)g_xr.our_context, (void *)g_xr.window);
        }
    }

    if (!g_xr.gl_ready) {
        /* 窗口还没到，重试 */
        if (g_xr.window) xr_display_init_gl();
    }
    if (!g_xr.gl_ready) {
        static int warn_once = 0;
        if (warn_once++ < 3) XWARN("ENDFRAME: GL 未就绪，跳过合成");
        return XR_SUCCESS;
    }

    /* 保存 ALVR 上下文，切到我们的 */
    EGLContext prev_ctx = eglGetCurrentContext();
    EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);

    /* 趁 ALVR 的上下文还 current、fbo=0 还是它刚画完的后备缓冲，先把它抠出来。
     * 必须在下面的 eglMakeCurrent 之前做。
     * 只在兜底模式（capture_alvr=2）才需要；模式 1 已经在 release 时收过了。 */
    if (xr_cfg_capture_alvr() == 2) {
        xr_capture_alvr_frame();
    } else if (xr_cfg_capture_alvr() == 3) {
        /* 收最后一只眼（eye1）；上一只眼已经在 acquire 时收过了 */
        xr_swap_capture_pending();
    }

    /* 宿主换窗口后旧 EGLSurface 会失效（eglSwapBuffers 一直 0x300D，画面冻结），
     * 这里在渲染线程上按需重建一次。 */
    if (xr_display_surface_stale()) {
        xr_display_rebuild_surface();
    }

    if (!eglMakeCurrent(g_xr.egl_display, g_xr.our_surface, g_xr.our_surface,
                        g_xr.our_context)) {
        EGLint egl_err = eglGetError();
        XWARN("ENDFRAME: eglMakeCurrent 失败 0x%x，尝试重建 surface", egl_err);
        if (!xr_display_rebuild_surface() ||
            !eglMakeCurrent(g_xr.egl_display, g_xr.our_surface, g_xr.our_surface,
                            g_xr.our_context)) {
            XWARN("ENDFRAME: 重建后仍失败 0x%x，跳过本帧", eglGetError());
            return XR_SUCCESS;
        }
        XLOG("ENDFRAME: surface 重建成功，恢复出图");
    }

    /* 已经切到我们自己的上下文了。如果开了 probe，先把 ALVR 提交的纹理读回来看看。 */
    if (xr_cfg_probe_swap()) {
        static int probe_swap_n;
        if ((probe_swap_n++ % 70) == 0) {
            XLOG("PROBE-SWAP: 本帧 layerCount=%u", frameEndInfo->layerCount);
            for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
                const XrCompositionLayerBaseHeader *ly = frameEndInfo->layers[i];
                if (!ly) continue;
                if (ly->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    XLOG("PROBE-SWAP: 图层 %u 类型=%d（非投影）", i, (int)ly->type);
                    continue;
                }
                const XrCompositionLayerProjection *pj =
                        (const XrCompositionLayerProjection *)ly;
                XLOG("PROBE-SWAP: 投影图层 viewCount=%u space=%llu", pj->viewCount,
                     (unsigned long long)pj->space);
                for (uint32_t v = 0; v < pj->viewCount && v < 2; v++) {
                    swapchain_t *sc = xr_swapchain_by_handle(pj->views[v].subImage.swapchain);
                    if (!sc) {
                        XWARN("PROBE-SWAP: eye%u swapchain 句柄无效", v);
                        continue;
                    }
                    XLOG("PROBE-SWAP: eye%u subImage rect=(%d,%d %ux%u) 眼姿态 pos=(%.3f,%.3f,%.3f)",
                         v, pj->views[v].subImage.imageRect.offset.x,
                         pj->views[v].subImage.imageRect.offset.y,
                         (unsigned)pj->views[v].subImage.imageRect.extent.width,
                         (unsigned)pj->views[v].subImage.imageRect.extent.height,
                         pj->views[v].pose.position.x, pj->views[v].pose.position.y,
                         pj->views[v].pose.position.z);
                    char tag[32];
                    snprintf(tag, sizeof(tag), "eye%u", v);
                    xr_swapchain_probe_readback(sc, tag);
                }
            }
        }
    }

    xr_display_dump_frames();

    xr_composite_clear(0.02f, 0.02f, 0.05f, 1.0f);   /* 没图层时是深蓝而不是纯黑，便于定位 */
    if (xr_cfg_test_pattern()) {
        xr_display_test_pattern();
    } else if (xr_cfg_capture_alvr() == 2 && xr_capture_texture()) {
        /* 兜底模式：ALVR 完全不碰 swapchain 时，整块贴 fbo=0 的截图 */
        xr_display_draw_capture();
    } else {
        for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
            const XrCompositionLayerBaseHeader *layer = frameEndInfo->layers[i];
            if (!layer) continue;
            if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                xr_composite_projection(
                        (const XrCompositionLayerProjection *)layer);
            } else {
                if (stat_layers_logged < 3) {
                    XLOG("ENDFRAME: 忽略图层类型 %d", (int)layer->type);
                    stat_layers_logged++;
                }
            }
        }
    }
    xr_display_present();

    /* 每 70 帧（≈1 秒）报一次统计，用来确认 ALVR 到底有没有在提交帧 */
    stat_frames++;
    stat_proj += frameEndInfo->layerCount;
    if (stat_frames % 70 == 0) {
        static long long last_acq;
        long long acq = xr_swap_acquire_total();
        XLOG("FRAME: 累计 %lld 帧 最近一秒图层合计=%u acquire/秒=%lld glErr=0x%x",
             (long long)stat_frames, stat_proj, acq - last_acq, (unsigned)glGetError());
        last_acq = acq;
        stat_proj = 0;
    }

    if (prev_ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(g_xr.egl_display, prev_draw, prev_read, prev_ctx);
    }
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* 时间转换                                                            */
/* ------------------------------------------------------------------ */

XrResult xrConvertTimespecTimeToTimeKHR(XrInstance instance, const struct timespec *timespecTime,
                                        XrTime *time) {
    (void)instance;
    if (!timespecTime || !time) return XR_ERROR_VALIDATION_FAILURE;
    *time = (XrTime)timespecTime->tv_sec * 1000000000LL + timespecTime->tv_nsec;
    return XR_SUCCESS;
}

XrResult xrConvertTimeToTimespecTimeKHR(XrInstance instance, XrTime time,
                                        struct timespec *timespecTime) {
    (void)instance;
    if (!timespecTime) return XR_ERROR_VALIDATION_FAILURE;
    timespecTime->tv_sec = (time_t)(time / 1000000000LL);
    timespecTime->tv_nsec = (long)(time % 1000000000LL);
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Action                                                              */
/* ------------------------------------------------------------------ */

XrResult xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo *createInfo,
                           XrActionSet *actionSet) {
    (void)instance;
    if (!createInfo || !actionSet) return XR_ERROR_VALIDATION_FAILURE;
    actionset_t *s = calloc(1, sizeof(actionset_t));
    if (!s) return XR_ERROR_OUT_OF_MEMORY;
    s->handle = (XrActionSet)(uint64_t)(g_xr.next_handle++);
    s->next = g_xr.action_sets;
    g_xr.action_sets = s;
    *actionSet = s->handle;
    XLOG("ACTION: 新建 actionSet %s", createInfo->actionSetName);
    return XR_SUCCESS;
}

XrResult xrDestroyActionSet(XrActionSet actionSet) {
    (void)actionSet;
    return XR_SUCCESS;
}

XrResult xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo *createInfo,
                        XrAction *action) {
    (void)actionSet;
    if (!createInfo || !action) return XR_ERROR_VALIDATION_FAILURE;
    action_t *a = calloc(1, sizeof(action_t));
    if (!a) return XR_ERROR_OUT_OF_MEMORY;
    switch (createInfo->actionType) {
        case XR_ACTION_TYPE_BOOLEAN_INPUT: a->type = ACT_BOOL; break;
        case XR_ACTION_TYPE_FLOAT_INPUT: a->type = ACT_FLOAT; break;
        case XR_ACTION_TYPE_VECTOR2F_INPUT: a->type = ACT_VEC2; break;
        case XR_ACTION_TYPE_POSE_INPUT: a->type = ACT_POSE; break;
        case XR_ACTION_TYPE_VIBRATION_OUTPUT: a->type = ACT_VIBRATE; break;
        default: a->type = ACT_BOOL; break;
    }
    a->handle = (XrAction)(uint64_t)(g_xr.next_handle++);
    a->path = xr_path_intern(createInfo->actionName);
    a->next = g_xr.actions;
    g_xr.actions = a;
    *action = a->handle;
    return XR_SUCCESS;
}

XrResult xrDestroyAction(XrAction action) {
    (void)action;
    return XR_SUCCESS;
}

XrResult xrSuggestInteractionProfileBindings(XrInstance instance,
                                             const XrInteractionProfileSuggestedBinding *bindings) {
    (void)instance;
    if (bindings) {
        const char *prof = xr_path_lookup(bindings->interactionProfile);
        XLOG("ACTION: 建议绑定 profile=%s 数量=%u", prof ? prof : "?",
             bindings->countSuggestedBindings);
    }
    return XR_SUCCESS;
}

XrResult xrAttachSessionActionSets(XrSession session,
                                   const XrSessionActionSetsAttachInfo *attachInfo) {
    (void)session;
    (void)attachInfo;
    XLOG("ACTION: 绑定到会话");
    return XR_SUCCESS;
}

XrResult xrSyncActions(XrSession session, const XrActionsSyncInfo *syncInfo) {
    (void)session;
    (void)syncInfo;
    return XR_SUCCESS;
}

static action_t *find_action(XrAction h) {
    for (action_t *a = g_xr.actions; a; a = a->next) {
        if (a->handle == h) return a;
    }
    return NULL;
}

XrResult xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo *getInfo,
                                 XrActionStateBoolean *state) {
    (void)session;
    (void)getInfo;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    memset(state, 0, sizeof(*state));
    state->type = XR_TYPE_ACTION_STATE_BOOLEAN;
    state->currentState = XR_FALSE;
    state->isActive = XR_FALSE;
    return XR_SUCCESS;
}

XrResult xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo *getInfo,
                               XrActionStateFloat *state) {
    (void)session;
    (void)getInfo;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    memset(state, 0, sizeof(*state));
    state->type = XR_TYPE_ACTION_STATE_FLOAT;
    state->currentState = 0.0f;
    state->isActive = XR_FALSE;
    return XR_SUCCESS;
}

XrResult xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo *getInfo,
                                  XrActionStateVector2f *state) {
    (void)session;
    (void)getInfo;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    memset(state, 0, sizeof(*state));
    state->type = XR_TYPE_ACTION_STATE_VECTOR2F;
    state->currentState.x = 0.0f;
    state->currentState.y = 0.0f;
    state->isActive = XR_FALSE;
    return XR_SUCCESS;
}

XrResult xrGetActionStatePose(XrSession session, const XrActionStateGetInfo *getInfo,
                              XrActionStatePose *state) {
    (void)session;
    (void)getInfo;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    memset(state, 0, sizeof(*state));
    state->type = XR_TYPE_ACTION_STATE_POSE;
    state->isActive = XR_FALSE;
    return XR_SUCCESS;
}

XrResult xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo *createInfo,
                             XrSpace *space) {
    (void)session;
    if (!createInfo || !space) return XR_ERROR_VALIDATION_FAILURE;
    space_t *s = calloc(1, sizeof(space_t));
    if (!s) return XR_ERROR_OUT_OF_MEMORY;
    s->kind = SPACE_ACTION;
    s->action = createInfo->action;
    s->subaction = createInfo->subactionPath;
    s->handle = (XrSpace)(uint64_t)(g_xr.next_handle++);
    s->next = g_xr.spaces;
    g_xr.spaces = s;
    *space = s->handle;
    return XR_SUCCESS;
}

XrResult xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                        XrInteractionProfileState *state) {
    (void)session;
    (void)topLevelUserPath;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    memset(state, 0, sizeof(*state));
    state->type = XR_TYPE_INTERACTION_PROFILE_STATE;
    state->interactionProfile = xr_path_intern("/interaction_profiles/khr/simple_controller");
    return XR_SUCCESS;
}

XrResult xrEnumerateBoundSourcesForAction(XrSession session,
                                          const XrBoundSourcesForActionEnumerateInfo *info,
                                          uint32_t capacityInput, uint32_t *countOutput,
                                          XrPath *sources) {
    (void)session;
    (void)info;
    (void)sources;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 0;
    return XR_SUCCESS;
}

XrResult xrGetInputSourceLocalizedName(XrSession session,
                                       const XrInputSourceLocalizedNameGetInfo *info,
                                       uint32_t capacityInput, uint32_t *countOutput,
                                       char *buffer) {
    (void)session;
    (void)info;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 1;
    if (capacityInput >= 1 && buffer) buffer[0] = '\0';
    return XR_SUCCESS;
}

XrResult xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo *info,
                               const XrHapticBaseHeader *feedback) {
    (void)session;
    (void)info;
    (void)feedback;
    return XR_SUCCESS;
}

XrResult xrStopHapticFeedback(XrSession session, const XrHapticActionInfo *info) {
    (void)session;
    (void)info;
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* 字符串路径 / 结果字符串                                             */
/* ------------------------------------------------------------------ */

XrResult xrStringToPath(XrInstance instance, const char *pathString, XrPath *path) {
    (void)instance;
    if (!pathString || !path) return XR_ERROR_VALIDATION_FAILURE;
    *path = xr_path_intern(pathString);
    return *path == XR_NULL_PATH ? XR_ERROR_PATH_FORMAT_INVALID : XR_SUCCESS;
}

XrResult xrPathToString(XrInstance instance, XrPath path, uint32_t capacityInput,
                        uint32_t *countOutput, char *buffer) {
    (void)instance;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    const char *s = xr_path_lookup(path);
    if (!s) return XR_ERROR_PATH_INVALID;
    uint32_t len = (uint32_t)strlen(s) + 1;
    *countOutput = len;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < len) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buffer, s, len);
    return XR_SUCCESS;
}

XrResult xrResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    (void)instance;
    snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "XrResult(%d)", (int)value);
    return XR_SUCCESS;
}

XrResult xrStructureTypeToString(XrInstance instance, XrStructureType value,
                                 char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    (void)instance;
    snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XrStructureType(%d)", (int)value);
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* 未实现函数的兜底                                                    */
/* ------------------------------------------------------------------ */

static XrResult xr_unsupported_stub(void) {
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

typedef struct {
    const char *name;
    void *fn;
} fn_entry_t;

extern XrResult xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo *, XrSwapchain *);
extern XrResult xrDestroySwapchain(XrSwapchain);
extern XrResult xrEnumerateSwapchainFormats(XrSession, uint32_t, uint32_t *, int64_t *);
extern XrResult xrEnumerateSwapchainImages(XrSwapchain, uint32_t, uint32_t *,
                                           XrSwapchainImageBaseHeader *);
extern XrResult xrAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo *,
                                        uint32_t *);
extern XrResult xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo *);
extern XrResult xrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo *);

static const fn_entry_t k_fns[] = {
        {"xrGetInstanceProcAddr", (void *)xrGetInstanceProcAddr},
        {"xrEnumerateApiLayerProperties", (void *)xrEnumerateApiLayerProperties},
        {"xrEnumerateInstanceExtensionProperties", (void *)xrEnumerateInstanceExtensionProperties},
        {"xrInitializeLoaderKHR", (void *)xrInitializeLoaderKHR},
        {"xrCreateInstance", (void *)xrCreateInstance},
        {"xrDestroyInstance", (void *)xrDestroyInstance},
        {"xrGetInstanceProperties", (void *)xrGetInstanceProperties},
        {"xrGetSystem", (void *)xrGetSystem},
        {"xrGetSystemProperties", (void *)xrGetSystemProperties},
        {"xrEnumerateViewConfigurations", (void *)xrEnumerateViewConfigurations},
        {"xrGetViewConfigurationProperties", (void *)xrGetViewConfigurationProperties},
        {"xrEnumerateViewConfigurationViews", (void *)xrEnumerateViewConfigurationViews},
        {"xrEnumerateEnvironmentBlendModes", (void *)xrEnumerateEnvironmentBlendModes},
        {"xrGetOpenGLESGraphicsRequirementsKHR", (void *)xrGetOpenGLESGraphicsRequirementsKHR},
        {"xrCreateSession", (void *)xrCreateSession},
        {"xrDestroySession", (void *)xrDestroySession},
        {"xrBeginSession", (void *)xrBeginSession},
        {"xrEndSession", (void *)xrEndSession},
        {"xrRequestExitSession", (void *)xrRequestExitSession},
        {"xrPollEvent", (void *)xrPollEvent},
        {"xrCreateReferenceSpace", (void *)xrCreateReferenceSpace},
        {"xrDestroySpace", (void *)xrDestroySpace},
        {"xrLocateSpace", (void *)xrLocateSpace},
        {"xrEnumerateReferenceSpaces", (void *)xrEnumerateReferenceSpaces},
        {"xrGetReferenceSpaceBoundsRect", (void *)xrGetReferenceSpaceBoundsRect},
        {"xrLocateViews", (void *)xrLocateViews},
        {"xrWaitFrame", (void *)xrWaitFrame},
        {"xrBeginFrame", (void *)xrBeginFrame},
        {"xrEndFrame", (void *)xrEndFrame},
        {"xrConvertTimespecTimeToTimeKHR", (void *)xrConvertTimespecTimeToTimeKHR},
        {"xrConvertTimeToTimespecTimeKHR", (void *)xrConvertTimeToTimespecTimeKHR},
        {"xrCreateActionSet", (void *)xrCreateActionSet},
        {"xrDestroyActionSet", (void *)xrDestroyActionSet},
        {"xrCreateAction", (void *)xrCreateAction},
        {"xrDestroyAction", (void *)xrDestroyAction},
        {"xrSuggestInteractionProfileBindings", (void *)xrSuggestInteractionProfileBindings},
        {"xrAttachSessionActionSets", (void *)xrAttachSessionActionSets},
        {"xrSyncActions", (void *)xrSyncActions},
        {"xrGetActionStateBoolean", (void *)xrGetActionStateBoolean},
        {"xrGetActionStateFloat", (void *)xrGetActionStateFloat},
        {"xrGetActionStateVector2f", (void *)xrGetActionStateVector2f},
        {"xrGetActionStatePose", (void *)xrGetActionStatePose},
        {"xrCreateActionSpace", (void *)xrCreateActionSpace},
        {"xrGetCurrentInteractionProfile", (void *)xrGetCurrentInteractionProfile},
        {"xrEnumerateBoundSourcesForAction", (void *)xrEnumerateBoundSourcesForAction},
        {"xrGetInputSourceLocalizedName", (void *)xrGetInputSourceLocalizedName},
        {"xrApplyHapticFeedback", (void *)xrApplyHapticFeedback},
        {"xrStopHapticFeedback", (void *)xrStopHapticFeedback},
        {"xrStringToPath", (void *)xrStringToPath},
        {"xrPathToString", (void *)xrPathToString},
        {"xrResultToString", (void *)xrResultToString},
        {"xrStructureTypeToString", (void *)xrStructureTypeToString},
        {"xrCreateSwapchain", (void *)xrCreateSwapchain},
        {"xrDestroySwapchain", (void *)xrDestroySwapchain},
        {"xrEnumerateSwapchainFormats", (void *)xrEnumerateSwapchainFormats},
        {"xrEnumerateSwapchainImages", (void *)xrEnumerateSwapchainImages},
        {"xrAcquireSwapchainImage", (void *)xrAcquireSwapchainImage},
        {"xrWaitSwapchainImage", (void *)xrWaitSwapchainImage},
        {"xrReleaseSwapchainImage", (void *)xrReleaseSwapchainImage},
};

XrResult xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
    (void)instance;
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    int n = (int)(sizeof(k_fns) / sizeof(k_fns[0]));
    for (int i = 0; i < n; i++) {
        if (strcmp(k_fns[i].name, name) == 0) {
            *function = (PFN_xrVoidFunction)k_fns[i].fn;
            return XR_SUCCESS;
        }
    }
    if (strncmp(name, "xr", 2) == 0) {
        *function = (PFN_xrVoidFunction)xr_unsupported_stub;
        return XR_SUCCESS;
    }
    *function = NULL;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
