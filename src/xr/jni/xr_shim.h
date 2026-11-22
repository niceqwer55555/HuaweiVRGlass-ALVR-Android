/*
 * xr_shim.h - 零华为依赖的极简 OpenXR 运行时（内部头文件）
 *
 * 目标：替换华为的 libxr_loader.so + libvrsdk.so，
 *       直接用普通 Android API（EGL/GLES + SensorManager + DisplayManager）
 *       驱动 HUAWEI VR Glass（作为普通 HDMI 副屏）。
 *
 * 只实现 ALVR 客户端实际用到的 OpenXR 子集。
 */
#ifndef XR_SHIM_H
#define XR_SHIM_H

#define LOG_TAG "XrShim"
#include <android/log.h>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include <jni.h>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1

#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#define XLOG(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define XWARN(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define XERR(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ---------- 基本配置 ---------- */

/* 眼镜物理分辨率（HUAWEI VR Display 3200x1600@70Hz） */
#define XR_PANEL_W 3200
#define XR_PANEL_H 1600
/* 每眼渲染分辨率：面板左右各一半，方形 */
#define XR_EYE_W XR_PANEL_H
#define XR_EYE_H XR_PANEL_H
/* 华为官方 Runtime 上报的 FOV / IPD（保持与 ALVR 客户端调参一致） */
#define XR_FOV_HALF_DEG 47.5f
#define XR_IPD_M 0.063f
#define XR_EYE_HEIGHT_M 1.6f
#define XR_REFRESH_HZ 70
#define XR_SWAPCHAIN_IMAGES 3

/* ---------- 数学 ---------- */

typedef struct { float x, y, z, w; } xq_t;
typedef struct { float x, y, z; } xv3_t;

static inline xq_t xq_mul(xq_t a, xq_t b) {
    xq_t r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

static inline xq_t xq_conj(xq_t a) {
    xq_t r = {-a.x, -a.y, -a.z, a.w};
    return r;
}

static inline xq_t xq_norm(xq_t a) {
    float n = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
    if (n < 1e-8f) {
        xq_t id = {0, 0, 0, 1};
        return id;
    }
    xq_t r = {a.x / n, a.y / n, a.z / n, a.w / n};
    return r;
}

static inline xq_t xq_axis_angle(float ax, float ay, float az, float rad) {
    float n = sqrtf(ax * ax + ay * ay + az * az);
    if (n < 1e-8f) {
        xq_t id = {0, 0, 0, 1};
        return id;
    }
    float s = sinf(rad * 0.5f) / n;
    xq_t r = {ax * s, ay * s, az * s, cosf(rad * 0.5f)};
    return r;
}

static inline xv3_t xq_rotate(xq_t q, xv3_t v) {
    /* v' = q * v * q^-1 */
    xq_t p = {v.x, v.y, v.z, 0};
    xq_t r = xq_mul(xq_mul(q, p), xq_conj(q));
    xv3_t out = {r.x, r.y, r.z};
    return out;
}

static inline int64_t xr_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---------- 对象定义 ---------- */

typedef enum {
    SPACE_STAGE = 0,
    SPACE_VIEW,
    SPACE_LOCAL,
    SPACE_ACTION,
} space_kind_t;

typedef struct space_s {
    XrSpace handle;
    space_kind_t kind;
    XrAction action;
    XrPath subaction;
    struct space_s *next;
} space_t;

typedef struct swapchain_s {
    XrSwapchain handle;
    uint32_t width;
    uint32_t height;
    int64_t format;                       /* GL enum */
    uint32_t image_count;
    GLuint textures[XR_SWAPCHAIN_IMAGES];
    bool allocated;
    bool acquired[XR_SWAPCHAIN_IMAGES];
    uint32_t next_acquire;
    uint32_t acquired_index;
    bool has_acquired;
    struct swapchain_s *next;
} swapchain_t;

typedef enum {
    ACT_BOOL = 0,
    ACT_FLOAT,
    ACT_VEC2,
    ACT_POSE,
    ACT_VIBRATE,
} action_type_t;

typedef struct action_s {
    XrAction handle;
    action_type_t type;
    XrPath path;          /* 动作名路径 */
    struct action_s *next;
} action_t;

typedef struct actionset_s {
    XrActionSet handle;
    struct actionset_s *next;
} actionset_t;

/* 路径表：字符串 <-> XrPath */
#define XR_MAX_PATHS 512
typedef struct {
    char *str;
    XrPath path;
} path_entry_t;

/* 事件队列 */
#define XR_EVENT_QUEUE 32

/* ---------- 全局运行时状态 ---------- */

typedef struct {
    /* instance / system */
    bool instance_created;
    XrInstance instance;
    XrSystemId system_id;
    XrSession session;
    XrSessionState session_state;

    /* 图形绑定（由 ALVR 提供） */
    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLContext app_context;      /* ALVR/wgpu 的上下文 */
    EGLContext our_context;      /* 我们自己的（与其共享） */
    EGLSurface our_surface;      /* 眼镜屏窗口 */
    ANativeWindow *window;       /* Java 传进来的 Surface */
    bool gl_ready;

    /* 合成器 */
    GLuint prog;
    GLuint vbo;
    GLuint vao;
    GLint u_tex;
    GLint u_xform;
    bool comp_ready;

    /* 帧节拍 */
    int64_t last_frame_ns;
    int64_t predicted_period_ns;

    /* 事件 */
    XrEventDataBuffer events[XR_EVENT_QUEUE];
    int ev_head;
    int ev_tail;

    /* 动作 */
    actionset_t *action_sets;
    action_t *actions;
    uint32_t next_handle;

    /* 空间 */
    space_t *spaces;

    /* swapchain */
    swapchain_t *swapchains;

    /* 路径 */
    path_entry_t paths[XR_MAX_PATHS];
    int path_count;

    /* 姿态模式（测试用）：auto / 3dof / 6dof / hid */
    char pose_mode[16];
} xr_state_t;

extern xr_state_t g_xr;

/* ---------- pose ---------- */
void xr_pose_init(void);
void xr_pose_shutdown(void);
/* 返回头部姿态：stage 坐标系下的朝向 + 位置（米） */
void xr_pose_get(xq_t *out_rot, xv3_t *out_pos, bool *out_position_tracked);
void xr_pose_recenter(void);
const char *xr_pose_driver_name(void);
/* 探测各驱动可用性，返回位掩码并打印 */
int xr_pose_probe(void);
void xr_pose_set_mode(const char *mode);

/* ---------- display / compositor ---------- */
bool xr_display_attach_window(ANativeWindow *window);
bool xr_display_init_gl(void);
bool xr_display_surface_stale(void);
bool xr_display_rebuild_surface(void);
void xr_display_mark_surface_stale(const char *why);
void xr_display_shutdown(void);
void xr_composite_projection(const XrCompositionLayerProjection *proj);
void xr_composite_clear(float r, float g, float b, float a);
void xr_display_present(void);
/* 内置测试图案：不经 ALVR 图层，直接画左右两色块 + 中央白块，用来验证显示链路 */
void xr_display_test_pattern(void);
/* ALVR 直渲 fbo=0 的捕获：必须在 ALVR 的上下文里调用（换上下文之前） */
void xr_capture_alvr_frame(void);
GLuint xr_capture_texture(void);
int xr_capture_size(int *w, int *h);
/* 把当前 fbo=0 的内容搬进指定纹理（华为 acquire-bound framebuffer path）。
 * 传纹理而不是 FBO —— FBO 不跨上下文共享，必须在使用它的上下文里现建。 */
bool xr_capture_blit_to(GLuint dst_tex, int w, int h, const char *tag);
/* acquire 累计次数，用来确认 ALVR 是不是每帧每只眼各 acquire 一次 */
long long xr_swap_acquire_total(void);
/* 模式 3：在 xrEndFrame 开头把 fbo=0 里最后一只眼的画面收进它的纹理 */
void xr_swap_capture_pending(void);
/* 把捕获到的画面整块贴到眼镜屏，用于观察 ALVR 实际在画什么 */
void xr_display_draw_capture(void);
/* 调试：把眼纹理导出成 PPM（放 files/dump_frame 触发一次） */
void xr_display_dump_frames(void);
int xr_cfg_test_pattern(void);
int xr_cfg_skip_present(void);
int xr_cfg_probe_swap(void);
int xr_cfg_capture_alvr(void);
int xr_cfg_eye_res(void);
float xr_cfg_fov_deg(void);
float xr_cfg_ipd(void);
swapchain_t *xr_swapchain_by_handle(XrSwapchain h);
/* 把 swapchain 当前纹理读回来统计亮度，用来判断 ALVR 到底有没有往纹理里写东西 */
bool xr_swapchain_probe_readback(swapchain_t *sc, const char *tag);

/* ---------- helpers ---------- */
void xr_event_push_state(XrSessionState state);
XrPath xr_path_intern(const char *str);
const char *xr_path_lookup(XrPath path);

/* ---------- 6DoF SLAM 探针（slam_probe.c） ----------
 * 在普通 app 进程里尝试加载原厂 SLAM 库并初始化，只用于验证可行性。
 * vm/activity 用于补齐库内部依赖的 JNI 环境。 */
void xr_slam_probe_run(JavaVM *vm, jobject activity);

/* ---------- /dev/hwvr* 原始 HID 报文探针（hidprobe.c） ----------
 * 不需要任何 Android 权限，直接用 hidraw ioctl 读报文，
 * 用来确认手柄数据是否复用眼镜那条 HID 流（靠 tag 区分）。 */
void xr_hid_probe_run(void);

/* ---------- 手柄姿态探针（ctrlprobe.c） ----------
 * 走 Huawei 自己的 Singleton<PhoneControllerPose>，start 之后 getPose 取值。 */
void xr_ctrl_probe_run(void);

/* ---------- 手柄姿态数据源（controller.c） ----------
 * 持续采样 PhoneControllerPose，供 action space / locate space 使用。 */
bool xr_controller_init(void);
bool xr_controller_get_pose(int hand, xq_t *rot, xv3_t *pos);
int xr_controller_samples(int hand);

/* ---------- HvrApi 6DoF 桥接（hvr_bridge.c） ---------- */
void hvr_bridge_init(JNIEnv *env, jobject activity);
void hvr_bridge_stop(void);

#endif /* XR_SHIM_H */
