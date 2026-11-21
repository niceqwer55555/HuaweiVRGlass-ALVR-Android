/*
 * xr_pose.c - 头部姿态驱动层
 *
 * 三个可选驱动（都不依赖华为 SDK/VR 服务）：
 *   1) 6dof : Android 标准传感器 TYPE_POSE_6DOF(34)，有位置
 *   2) hid  : 眼镜自身 IMU，读 /dev/hwvr1（HID 报文 + 互补滤波）
 *   3) 3dof : 手机 游戏旋转矢量/旋转矢量，只有朝向
 *
 * 选择逻辑（auto）：6dof > hid > 3dof。
 * 可用 /sdcard/Android/data/<pkg>/files/pose_mode 强制指定：
 *   auto | 6dof | hid | 3dof
 */
#include "xr_shim.h"

#include <android/sensor.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define POSE_MODE_FILE_EXT \
    "/sdcard/Android/data/com.lht.huaweivr.alvr/files/pose_mode"
#define POSE_MODE_FILE_INT \
    "/data/data/com.lht.huaweivr.alvr/files/pose_mode"

#ifndef ASENSOR_TYPE_POSE_6DOF
#define ASENSOR_TYPE_POSE_6DOF 34
#endif
#ifndef ASENSOR_TYPE_GAME_ROTATION_VECTOR
#define ASENSOR_TYPE_GAME_ROTATION_VECTOR 15
#endif
#ifndef ASENSOR_TYPE_ROTATION_VECTOR
#define ASENSOR_TYPE_ROTATION_VECTOR 11
#endif

#define HID_NODE "/dev/hwvr1"
#define HID_TAG_IMU 3
#define HID_REPORT_LEN 45

/* 实际使用的节点（可能来自 hidraw 扫描，也可能是华为私有节点） */
static char s_hid_node[64] = HID_NODE;

/* 通用 Android 内核里没有 /dev/hwvrN —— 那是华为自家 HID 驱动建的私有节点，
 * 只有华为手机/华为 VR 系统才有。RK3566 这类通用内核上必须走标准 hidraw。
 * 这里按 HID_ID 里的 VID/PID 扫描，命中眼镜本体就返回它的 /dev/hidrawN。 */
static const char *hid_find_node(void) {
    static char cand[8][32];
    static char found[64];
    int nc = 0;

    /* 收集所有属于华为（VID 12D1）的 hidraw */
    for (int i = 0; i < 32 && nc < 8; i++) {
        char uev[96], line[256];
        snprintf(uev, sizeof uev, "/sys/class/hidraw/hidraw%d/device/uevent", i);
        FILE *f = fopen(uev, "r");
        if (!f) continue;
        unsigned vid = 0, pid = 0;
        while (fgets(line, sizeof line, f)) {
            const char *p = strstr(line, "HID_ID=");
            if (p) {
                unsigned bus = 0;
                if (sscanf(p + 7, "%x:%x:%x", &bus, &vid, &pid) != 3) {
                    vid = pid = 0;
                }
            }
        }
        fclose(f);
        if (vid != 0x12D1) continue;
        snprintf(cand[nc], sizeof cand[0], "/dev/hidraw%d", i);
        XLOG("HID: 候选 %s (%04x:%04x)", cand[nc], vid, pid);
        nc++;
    }
    if (nc == 0) return NULL;

    /* 眼镜的 IMU 挂在哪个接口上并不固定（1092 本体 / 107E 复合 HID 都可能有），
     * 所以这里实打实测一下：谁的 poll 立刻有数据就用谁。 */
    const char *first_ok = NULL;
    for (int k = 0; k < nc; k++) {
        int fd = open(cand[k], O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (!first_ok) first_ok = cand[k];
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = poll(&pfd, 1, 250);
        if (r > 0 && (pfd.revents & POLLIN)) {
            close(fd);
            snprintf(found, sizeof found, "%s", cand[k]);
            XLOG("HID: %s 有数据流（%d 个候选里实测选中）", found, nc);
            return found;
        }
        close(fd);
    }
    if (first_ok) {
        snprintf(found, sizeof found, "%s", first_ok);
        XLOG("HID: %d 个候选 250ms 内都无数据，退回 %s（IMU 可能未启动）",
             nc, found);
        return found;
    }
    return NULL;
}

typedef enum {
    DRV_NONE = 0,
    DRV_SENSOR_3DOF,   /* 手机旋转矢量 */
    DRV_SENSOR_6DOF,   /* TYPE_POSE_6DOF */
    DRV_HID_3DOF,      /* 眼镜 HID IMU（自己解析报文） */
    DRV_HUAWEI_3DOF,   /* 原厂 libvrpose.so（HelmetPose） */
    DRV_HUAWEI_6DOF,   /* 原厂 libVR6Dof.so（SLAM） */
} drv_t;

/* 探测结果位掩码 */
#define PROBE_3DOF (1 << 0)
#define PROBE_6DOF (1 << 1)
#define PROBE_HID (1 << 2)
#define PROBE_HW_3DOF (1 << 3)
#define PROBE_HW_6DOF (1 << 4)

typedef struct {
    pthread_mutex_t lock;
    xq_t rot;              /* 已转换到渲染坐标系 */
    xv3_t pos;             /* 米 */
    bool position_valid;
    bool running;
    drv_t driver;

    /* 传感器 */
    ASensorManager *sensor_mgr;
    ASensorEventQueue *sensor_q;
    const ASensor *sensor_rot;
    const ASensor *sensor_pose6;
    ALooper *looper;
    pthread_t thread;

    /* HID */
    int hid_fd;
    pthread_t hid_thread;
    bool hid_running;

    /* 最近一次原始值（调试） */
    float last_gyro[3];
    int64_t last_ts_ns;
    int sample_count;

    /* 自检线程 */
    pthread_t watch_thread;
    bool watch_running;
} pose_state_t;

static pose_state_t g_pose;
static bool g_pose_inited;

static const xq_t k_world_to_render = {0.70710678f, 0.0f, 0.0f, 0.70710678f};
/* = axisAngle(1,0,0, +90°)，即 +PI/2 */

static int64_t now_ns(void) { return xr_now_ns(); }

/* ---------------- HID 互补滤波 ---------------- */

typedef struct {
    xq_t q;
    bool have;
    xq_t bias_q;      /* 初始朝向（yaw 归零用） */
} hid_fusion_t;

static hid_fusion_t g_fusion;

static void fusion_reset(void) {
    g_fusion.q.x = 0;
    g_fusion.q.y = 0;
    g_fusion.q.z = 0;
    g_fusion.q.w = 1;
    g_fusion.bias_q.x = 0;
    g_fusion.bias_q.y = 0;
    g_fusion.bias_q.z = 0;
    g_fusion.bias_q.w = 1;
    g_fusion.have = false;
}

/* accel(设备系, m/s^2) + gyro(rad/s) 互补滤波 */
static void fusion_update(float ax, float ay, float az,
                          float gx, float gy, float gz, float dt) {
    if (!g_fusion.have) {
        /* 用重力对齐初始姿态：把设备 -Y(或 +Z) 对齐世界 +Y 的近似 */
        float n = sqrtf(ax * ax + ay * ay + az * az);
        if (n > 1e-3f) {
            /* 求把测量到的重力方向转到世界 +Y 的最小旋转 */
            float ux = ax / n, uy = ay / n, uz = az / n;
            /* 世界重力在设备系应读作 (0, +1, 0)（Android: 设备静止且屏幕朝上时 g=(0,9.8,0)） */
            xv3_t from = {ux, uy, uz};
            xv3_t to = {0.0f, 1.0f, 0.0f};
            float dot = from.x * to.x + from.y * to.y + from.z * to.z;
            xq_t q = {0, 0, 0, 1};
            if (dot < 0.9999f) {
                float cx = from.y * to.z - from.z * to.y;
                float cy = from.z * to.x - from.x * to.z;
                float cz = from.x * to.y - from.y * to.x;
                float s = sqrtf(cx * cx + cy * cy + cz * cz);
                float ang = atan2f(s, dot);
                if (s > 1e-6f) q = xq_axis_angle(cx / s, cy / s, cz / s, ang);
            }
            g_fusion.q = q;
            g_fusion.have = true;
        }
        return;
    }

    /* 陀螺积分 */
    float omega = sqrtf(gx * gx + gy * gy + gz * gz);
    if (omega > 1e-5f && dt > 1e-5f && dt < 0.2f) {
        xq_t dq = xq_axis_angle(gx, gy, gz, omega * dt);
        g_fusion.q = xq_norm(xq_mul(g_fusion.q, dq));
    }

    /* 用重力做缓慢校正（PI 中的 P） */
    float n = sqrtf(ax * ax + ay * ay + az * az);
    if (n > 1e-3f) {
        xv3_t ga = {ax / n, ay / n, az / n};
        /* 当前姿态下预测的重力方向（设备系） */
        xv3_t pred = xq_rotate(xq_conj(g_fusion.q), (xv3_t){0.0f, 1.0f, 0.0f});
        float cx = pred.y * ga.z - pred.z * ga.y;
        float cy = pred.z * ga.x - pred.x * ga.z;
        float cz = pred.x * ga.y - pred.y * ga.x;
        float k = 0.02f; /* 校正增益，约几秒收敛 */
        float cs = sqrtf(cx * cx + cy * cy + cz * cz);
        if (cs > 1e-6f) {
            xq_t corr = xq_axis_angle(cx / cs, cy / cs, cz / cs, cs * k);
            g_fusion.q = xq_norm(xq_mul(corr, g_fusion.q));
        }
    }
}

/* ---------------- HID 报文解析 ---------------- */

static float le_f32(const unsigned char *p) {
    int32_t bits = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                             ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void hid_publish(void) {
    pthread_mutex_lock(&g_pose.lock);
    g_pose.rot = xq_norm(xq_mul(k_world_to_render, g_fusion.q));
    g_pose.position_valid = false;
    g_pose.pos.x = 0;
    g_pose.pos.y = XR_EYE_HEIGHT_M;
    g_pose.pos.z = 0;
    g_pose.sample_count++;
    pthread_mutex_unlock(&g_pose.lock);
}

static void *hid_thread_fn(void *arg) {
    (void)arg;
    unsigned char buf[256];
    int64_t last = 0;
    int pkt = 0;
    int raw_pkt = 0;      /* 所有读到的包（不论能不能解析） */
    int skip_short = 0;   /* 太短的包 */
    int skip_tag = 0;     /* tag 不是 IMU 的包 */
    int idle_polls = 0;   /* 连续 poll 超时次数 */
    XLOG("HID: 开始读取 %s", s_hid_node);
    while (g_pose.hid_running) {
        /* 用 poll 带超时，这样停止时（关闭 fd 不一定能唤醒阻塞 read）能及时退出 */
        struct pollfd pfd;
        pfd.fd = g_pose.hid_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 200);
        if (pr == 0) {
            /* 5 秒没有任何数据就报一次，说明节点能开但没在推流 */
            if (++idle_polls == 25) {
                XWARN("HID: 已 5 秒无数据流（raw=%d 跳过短包=%d 跳过tag=%d）"
                      " —— 节点可打开但没有报文推送，可能需要先下发使能命令",
                      raw_pkt, skip_short, skip_tag);
            } else if (idle_polls % 150 == 0) {
                XWARN("HID: 已 %d 秒无数据流", idle_polls / 5);
            }
            continue;
        }
        idle_polls = 0;
        if (pr < 0) {
            if (errno == EINTR) continue;
            XWARN("HID: poll 失败 errno=%d (%s)", errno, strerror(errno));
            break;
        }
        if (!(pfd.revents & POLLIN)) continue;

        ssize_t n = read(g_pose.hid_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            XWARN("HID: read 失败 errno=%d (%s)", errno, strerror(errno));
            break;
        }
        raw_pkt++;

        /* 前 8 个包无条件打印，用来确认真实报文协议（tag / 长度 / 字节序） */
        if (raw_pkt <= 8) {
            int show = (int)n < 48 ? (int)n : 48;
            char hex[48 * 3 + 1];
            char *p = hex;
            for (int i = 0; i < show; i++) p += snprintf(p, 4, "%02x ", buf[i]);
            XLOG("HID: 原始包#%d len=%d tag=0x%02x: %s", raw_pkt, (int)n, buf[0], hex);
        }

        if (n < HID_REPORT_LEN) { skip_short++; continue; }
        if ((buf[0] & 0xFF) != HID_TAG_IMU) { skip_tag++; continue; }

        float ax = le_f32(buf + 1), ay = le_f32(buf + 5), az = le_f32(buf + 9);
        float gx = le_f32(buf + 13), gy = le_f32(buf + 17), gz = le_f32(buf + 21);
        float ts = le_f32(buf + 37) / 1e6f;
        int64_t ns = (int64_t)(ts * 1e9f);
        float dt = (last > 0 && ns > last) ? (float)(ns - last) * 1e-9f : 0.0025f;
        last = ns;

        pkt++;
        if (pkt <= 3) {
            char hex[HID_REPORT_LEN * 3 + 1];
            char *p = hex;
            for (int i = 0; i < HID_REPORT_LEN; i++) {
                p += snprintf(p, 4, "%02x ", buf[i]);
            }
            XLOG("HID: 报文#%d len=%d 前45字节: %s", pkt, (int)n, hex);
            XLOG("HID: 解析 accel=(%.2f, %.2f, %.2f) gyro=(%.3f, %.3f, %.3f) t=%.3fs",
                 ax, ay, az, gx, gy, gz, ts);
        } else if (pkt % 200 == 0) {
            XLOG("HID: 已收 %d 报文 dt=%.2fms accel=(%.2f, %.2f, %.2f)",
                 pkt, dt * 1000.0f, ax, ay, az);
        }

        pthread_mutex_lock(&g_pose.lock);
        g_pose.last_gyro[0] = gx;
        g_pose.last_gyro[1] = gy;
        g_pose.last_gyro[2] = gz;
        pthread_mutex_unlock(&g_pose.lock);

        fusion_update(ax, ay, az, gx, gy, gz, dt);
        hid_publish();
    }
    XWARN("HID: 读取线程结束（共 %d 报文）", pkt);
    return NULL;
}

static bool hid_start(void) {
    /* 候选顺序：hidraw 扫描结果（通用内核）→ 华为私有 hwvr1 / hwvr0（华为系统） */
    const char *scan = hid_find_node();
    const char *nodes[4];
    int nn = 0;
    if (scan) nodes[nn++] = scan;
    nodes[nn++] = HID_NODE;
    nodes[nn++] = "/dev/hwvr0";
    nodes[nn] = NULL;

    int fd = -1;
    const char *used = NULL;
    for (int i = 0; i < nn; i++) {
        fd = open(nodes[i], O_RDONLY | O_NONBLOCK);
        if (fd >= 0) { used = nodes[i]; break; }
        /* ENOENT 是常态（华为私有节点在通用内核上本来就不存在），不刷日志 */
        if (errno != ENOENT) {
            XWARN("HID: 打不开 %s errno=%d (%s)", nodes[i], errno, strerror(errno));
        }
    }
    if (fd < 0) return false;
    snprintf(s_hid_node, sizeof s_hid_node, "%s", used);
    XLOG("HID: 使用节点 %s fd=%d", used, fd);

    /* 清掉 O_NONBLOCK，改成阻塞读（由 poll 控制超时） */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    g_pose.hid_fd = fd;
    fusion_reset();
    g_pose.hid_running = true;
    if (pthread_create(&g_pose.hid_thread, NULL, hid_thread_fn, NULL) != 0) {
        XWARN("HID: 线程创建失败");
        g_pose.hid_running = false;
        close(fd);
        g_pose.hid_fd = -1;
        return false;
    }
    return true;
}

static void hid_stop(void) {
    if (!g_pose.hid_running) return;
    g_pose.hid_running = false;
    if (g_pose.hid_fd >= 0) {
        close(g_pose.hid_fd); /* 让 read 返回错误以退出 */
        g_pose.hid_fd = -1;
    }
    pthread_join(g_pose.hid_thread, NULL);
}

/* ---------------- 原厂 libvrpose.so（3DoF 头追） ---------------- */
/*
 * libvrpose.so 导出纯 C 接口（已用 llvm-nm 确认）：
 *   int  HelmetPoseStart(void);
 *   int  HelmetPoseStop(void);
 *   int  HelmetPoseGet(void *outVrPosture);   // out 前 4 个 float = 四元数
 * 依赖只有 libWalleAlg / libWalleDev / libMobileSensor（都在 APK 里），
 * 不连任何华为系统服务，所以可以直接 dlopen。
 */

typedef int (*vrpose_start_t)(void);
typedef int (*vrpose_stop_t)(void);
typedef int (*vrpose_get_t)(void *out);

static struct {
    void *lib;
    vrpose_start_t start;
    vrpose_stop_t stop;
    vrpose_get_t get;
    pthread_t th;
    bool running;
    xq_t ref;          /* recenter 基准：数据开始流动那一刻的姿态 */
    bool have_ref;
    xq_t prev;         /* 上一帧原始值，用于判断数据是否开始流动 */
    bool have_prev;
} g_vrp;

static bool vrpose_load(void) {
    if (g_vrp.lib) return true;
    g_vrp.lib = dlopen("libvrpose.so", RTLD_NOW);
    if (!g_vrp.lib) {
        XWARN("VRPOSE: dlopen libvrpose.so 失败: %s", dlerror());
        return false;
    }
    g_vrp.start = (vrpose_start_t)dlsym(g_vrp.lib, "HelmetPoseStart");
    g_vrp.stop = (vrpose_stop_t)dlsym(g_vrp.lib, "HelmetPoseStop");
    g_vrp.get = (vrpose_get_t)dlsym(g_vrp.lib, "HelmetPoseGet");
    XLOG("VRPOSE: 符号 start=%p stop=%p get=%p",
         (void *)g_vrp.start, (void *)g_vrp.stop, (void *)g_vrp.get);
    if (!g_vrp.get) {
        dlclose(g_vrp.lib);
        g_vrp.lib = NULL;
        return false;
    }
    return true;
}

static void *vrpose_thread_fn(void *arg) {
    (void)arg;
    unsigned char buf[128];
    int rc = g_vrp.start ? g_vrp.start() : -1;
    XLOG("VRPOSE: HelmetPoseStart() 返回 %d", rc);

    int n = 0, valid = 0, bad = 0;
    while (g_vrp.running) {
        memset(buf, 0, sizeof(buf));
        int r = g_vrp.get(buf);
        float f[8];
        memcpy(f, buf, sizeof(f));

        /* 前 8 帧把原始缓冲打出来，用来确认四元数/位置的字段偏移 */
        if (n < 8) {
            XLOG("VRPOSE: #%d ret=%d raw: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                 n, r, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            n++;
        }

        xq_t q = {f[0], f[1], f[2], f[3]};
        float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (len > 0.5f && len < 1.5f) {
            q = xq_norm(q);

            /* 数据开始流动（四元数相对上一帧变化了）的那一刻抓基准：
             *   未插镜时原厂一直吐单位四元数（= 眼镜平放桌面），
             *   插镜瞬间开始出真实姿态，此时头基本是摆正的 —— 就以它为零点。 */
            if (!g_vrp.have_ref) {
                bool changed = !g_vrp.have_prev ||
                               fabsf(q.x - g_vrp.prev.x) > 1e-4f ||
                               fabsf(q.y - g_vrp.prev.y) > 1e-4f ||
                               fabsf(q.z - g_vrp.prev.z) > 1e-4f ||
                               fabsf(q.w - g_vrp.prev.w) > 1e-4f;
                if (changed) {
                    g_vrp.ref = q;
                    g_vrp.have_ref = true;
                    XLOG("VRPOSE: 数据开始流动，抓基准 q=(%.4f, %.4f, %.4f, %.4f)",
                         q.x, q.y, q.z, q.w);
                }
            }
            g_vrp.prev = q;
            g_vrp.have_prev = true;

            if (g_vrp.have_ref) {
                xq_t rel = xq_norm(xq_mul(xq_conj(g_vrp.ref), q));
                pthread_mutex_lock(&g_pose.lock);
                g_pose.rot = rel;
                g_pose.position_valid = false;
                g_pose.pos.x = 0;
                g_pose.pos.y = XR_EYE_HEIGHT_M;
                g_pose.pos.z = 0;
                g_pose.sample_count++;
                pthread_mutex_unlock(&g_pose.lock);
                valid++;
            }
        } else {
            if (bad < 3) {
                XWARN("VRPOSE: 四元数模长异常 len=%.4f（丢弃）", len);
                bad++;
            }
        }
        usleep(5000); /* 200Hz */
    }

    if (g_vrp.stop) g_vrp.stop();
    XLOG("VRPOSE: 已停止（有效 %d 帧）", valid);
    return NULL;
}

static bool vrpose_start(void) {
    if (!vrpose_load()) return false;
    g_vrp.have_ref = false;
    g_vrp.running = true;
    if (pthread_create(&g_vrp.th, NULL, vrpose_thread_fn, NULL) != 0) {
        g_vrp.running = false;
        XWARN("VRPOSE: 线程创建失败");
        return false;
    }
    return true;
}

static void vrpose_stop(void) {
    if (!g_vrp.running) return;
    g_vrp.running = false;
    pthread_join(g_vrp.th, NULL);
}

/* ---------------- 原厂 libVR6Dof.so（SLAM 6DoF） ---------------- */
/*
 * 只导出一个 C 符号 GetVR6DofInterface()，内部是 C++ 的 HW_SLAM 实现，
 * 需要外部把相机帧（HwSlamImage）喂进去才会产生位姿。
 * 这里只做「能不能加载 + 接口能不能拿到」的探测，真正的 SLAM 管线还没接。
 */
static void *vr6dof_probe(void) {
    void *lib = dlopen("libVR6Dof.so", RTLD_NOW);
    if (!lib) {
        XWARN("VR6DOF: dlopen libVR6Dof.so 失败: %s", dlerror());
        return NULL;
    }
    void *(*getit)(void) = (void *(*)(void))dlsym(lib, "GetVR6DofInterface");
    if (!getit) {
        XWARN("VR6DOF: 找不到 GetVR6DofInterface: %s", dlerror());
        return NULL;
    }
    XLOG("VR6DOF: libVR6Dof.so 加载成功，准备调用 GetVR6DofInterface()");
    void *itf = getit();
    XLOG("VR6DOF: GetVR6DofInterface() = %p", itf);
    return itf;
}

/* ---------------- 自检（每秒打印一次姿态快照） ---------------- */

static void sensor_stop(void);
static bool read_mode_file_raw(char *out, size_t n);
static void pose_apply_mode(const char *mode);

static void *watch_thread_fn(void *arg) {
    (void)arg;
    int last_count = 0;
    int idle_ticks = 0;
    while (g_pose.watch_running) {
        usleep(1000 * 1000);
        if (!g_pose.watch_running) break;

        /* 模式热切换：外部 adb 改 pose_mode 文件即可换驱动，不用重启应用 */
        char cur[16];
        if (read_mode_file_raw(cur, sizeof(cur)) && cur[0] &&
            strcmp(cur, g_xr.pose_mode) != 0) {
            XLOG("POSE: 发现 pose_mode 变化 \"%s\" -> \"%s\"，热切换驱动",
                 g_xr.pose_mode, cur);
            snprintf(g_xr.pose_mode, sizeof(g_xr.pose_mode), "%s", cur);
            pose_apply_mode(cur);
            pthread_mutex_lock(&g_pose.lock);
            last_count = g_pose.sample_count;   /* 重置基线，避免把历史累计当速率 */
            pthread_mutex_unlock(&g_pose.lock);
            idle_ticks = 0;
            continue;
        }

        xq_t rot;
        xv3_t pos;
        bool tracked;
        int count;
        pthread_mutex_lock(&g_pose.lock);
        rot = g_pose.rot;
        pos = g_pose.pos;
        tracked = g_pose.position_valid;
        count = g_pose.sample_count;
        pthread_mutex_unlock(&g_pose.lock);

        int delta = count - last_count;
        last_count = count;

        if (delta > 0) {
            idle_ticks = 0;
            /* 顺便算欧拉角，方便验证轴向映射（yaw 绕 Y / pitch 绕 X / roll 绕 Z） */
            float yaw = atan2f(2.f * (rot.w * rot.y + rot.x * rot.z),
                               1.f - 2.f * (rot.y * rot.y + rot.z * rot.z)) * 57.29578f;
            float pitch = asinf(fmaxf(-1.f, fminf(1.f, 2.f * (rot.w * rot.x - rot.z * rot.y))))
                          * 57.29578f;
            float roll = atan2f(2.f * (rot.w * rot.z + rot.x * rot.y),
                                1.f - 2.f * (rot.x * rot.x + rot.y * rot.y)) * 57.29578f;
            XLOG("POSE-TICK: drv=%s +%d samples/秒  q=(%.3f, %.3f, %.3f, %.3f) "
                 "yaw=%.1f° pitch=%.1f° roll=%.1f° pos=(%.3f, %.3f, %.3f) tracked=%d",
                 xr_pose_driver_name(), delta, rot.x, rot.y, rot.z, rot.w,
                 yaw, pitch, roll, pos.x, pos.y, pos.z, tracked);
        } else if (++idle_ticks % 5 == 0) {
            XWARN("POSE-TICK: drv=%s 已 %d 秒没有新样本（传感器可能被挂起）",
                  xr_pose_driver_name(), idle_ticks);
        }
    }
    return NULL;
}

static void watch_start(void) {
    g_pose.watch_running = true;
    if (pthread_create(&g_pose.watch_thread, NULL, watch_thread_fn, NULL) != 0) {
        g_pose.watch_running = false;
        XWARN("POSE: 自检线程创建失败");
    }
}

static void watch_stop(void) {
    if (!g_pose.watch_running) return;
    g_pose.watch_running = false;
    pthread_join(g_pose.watch_thread, NULL);
}

/* ---------------- Android 传感器 ---------------- */

static const ASensor *g_use_sensor;

/*
 * 注意：ALooper_pollAll 只作用于「当前线程」prepare 出来的 looper。
 * 所以 looper / 事件队列 / 注册传感器 必须都在传感器线程里建，
 * 否则 pollAll 会一直返回 ALOOPER_POLL_ERROR，姿态永远不会更新。
 */
static void *sensor_thread_fn(void *arg) {
    (void)arg;
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (!looper) {
        XERR("SENSOR: ALooper_prepare 失败");
        g_pose.running = false;
        return NULL;
    }
    ASensorEventQueue *q = ASensorManager_createEventQueue(
            g_pose.sensor_mgr, looper, 3, NULL, NULL);
    if (!q) {
        XERR("SENSOR: 事件队列创建失败");
        g_pose.running = false;
        return NULL;
    }
    g_pose.sensor_q = q;

    int rc = ASensorEventQueue_registerSensor(q, g_use_sensor, 1000000 /* 1000Hz */, 0);
    if (rc < 0) {
        XERR("SENSOR: registerSensor 失败 rc=%d", rc);
        g_pose.running = false;
        return NULL;
    }
    XLOG("SENSOR: 线程已启动 type=%d name=%s 请求 1000Hz",
         ASensor_getType(g_use_sensor), ASensor_getName(g_use_sensor));

    ASensorEvent event;
    int raw = 0;
    while (g_pose.running) {
        int ident = ALooper_pollAll(50, NULL, NULL, NULL);
        if (ident < 0) {
            /* -1 超时正常；-3/-4 是错误，报一次就好 */
            static int err_logged;
            if (ident != ALOOPER_POLL_TIMEOUT && err_logged < 3) {
                XWARN("SENSOR: pollAll 返回 %d", ident);
                err_logged++;
            }
            continue;
        }
        while (ASensorEventQueue_getEvents(q, &event, 1) > 0) {
            raw++;
            if ((int)event.type == ASENSOR_TYPE_GAME_ROTATION_VECTOR ||
                (int)event.type == ASENSOR_TYPE_ROTATION_VECTOR) {
                /* Android 旋转矢量: values[0..2]=xyz 虚部, [3]=w */
                xq_t qq = {event.data[0], event.data[1], event.data[2], event.data[3]};
                qq = xq_norm(qq);
                pthread_mutex_lock(&g_pose.lock);
                g_pose.rot = xq_norm(xq_mul(k_world_to_render, qq));
                g_pose.position_valid = false;
                g_pose.pos.x = 0;
                g_pose.pos.y = XR_EYE_HEIGHT_M;
                g_pose.pos.z = 0;
                g_pose.sample_count++;
                g_pose.last_ts_ns = (int64_t)event.timestamp;
                pthread_mutex_unlock(&g_pose.lock);
            } else if ((int)event.type == ASENSOR_TYPE_POSE_6DOF) {
                /* 标准 6DOF: [0..3]=quat(xyzw), [4..6]=position */
                xq_t qq = {event.data[0], event.data[1], event.data[2], event.data[3]};
                qq = xq_norm(qq);
                xv3_t p = {event.data[4], event.data[5], event.data[6]};
                xv3_t pr = xq_rotate(k_world_to_render, p);
                pthread_mutex_lock(&g_pose.lock);
                g_pose.rot = xq_norm(xq_mul(k_world_to_render, qq));
                g_pose.pos.x = pr.x;
                g_pose.pos.y = pr.y + XR_EYE_HEIGHT_M;
                g_pose.pos.z = pr.z;
                g_pose.position_valid = true;
                g_pose.sample_count++;
                g_pose.last_ts_ns = (int64_t)event.timestamp;
                pthread_mutex_unlock(&g_pose.lock);
            }
        }
    }
    ASensorEventQueue_disableSensor(q, g_use_sensor);
    ASensorManager_destroyEventQueue(g_pose.sensor_mgr, q);
    g_pose.sensor_q = NULL;
    XLOG("SENSOR: 线程结束（共 %d 个事件）", raw);
    return NULL;
}

static bool sensor_start(bool want_6dof) {
    g_pose.sensor_mgr = ASensorManager_getInstanceForPackage("com.lht.huaweivr.alvr");
    if (!g_pose.sensor_mgr) {
        XWARN("SENSOR: 拿不到 SensorManager");
        return false;
    }
    g_pose.sensor_rot = ASensorManager_getDefaultSensor(
            g_pose.sensor_mgr, ASENSOR_TYPE_GAME_ROTATION_VECTOR);
    if (!g_pose.sensor_rot) {
        g_pose.sensor_rot = ASensorManager_getDefaultSensor(
                g_pose.sensor_mgr, ASENSOR_TYPE_ROTATION_VECTOR);
    }
    g_pose.sensor_pose6 = ASensorManager_getDefaultSensor(
            g_pose.sensor_mgr, ASENSOR_TYPE_POSE_6DOF);

    const ASensor *use = NULL;
    if (want_6dof && g_pose.sensor_pose6) {
        use = g_pose.sensor_pose6;
    } else if (g_pose.sensor_rot) {
        use = g_pose.sensor_rot;
    }
    if (!use) {
        XWARN("SENSOR: 没有可用的姿态传感器");
        return false;
    }
    g_use_sensor = use;

    g_pose.running = true;
    if (pthread_create(&g_pose.thread, NULL, sensor_thread_fn, NULL) != 0) {
        g_pose.running = false;
        XWARN("SENSOR: 线程创建失败");
        return false;
    }
    return true;
}

static void sensor_stop(void) {
    if (!g_pose.running) return;
    g_pose.running = false;
    pthread_join(g_pose.thread, NULL);
}

/* ---------------- 对外接口 ---------------- */

/* 静默读取模式文件（watch 线程每秒会调用，不能刷日志） */
static bool read_mode_file_raw(char *out, size_t n) {
    const char *candidates[] = {POSE_MODE_FILE_EXT, POSE_MODE_FILE_INT};
    out[0] = '\0';
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(candidates[i], "r");
        if (!f) continue;
        if (fgets(out, (int)n, f)) {
            /* 去掉空白 */
            for (char *p = out; *p; p++) {
                if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') {
                    *p = '\0';
                    break;
                }
            }
        }
        fclose(f);
        if (out[0]) return true;
    }
    return false;
}

static void read_mode_file(char *out, size_t n) {
    if (read_mode_file_raw(out, n) && out[0]) {
        XLOG("POSE: 模式来自文件 -> \"%s\"", out);
    } else {
        out[0] = '\0';
    }
}

int xr_pose_probe(void) {
    int mask = 0;
    ASensorManager *mgr = ASensorManager_getInstanceForPackage("com.lht.huaweivr.alvr");
    if (mgr) {
        const ASensor *s3 = ASensorManager_getDefaultSensor(
                mgr, ASENSOR_TYPE_GAME_ROTATION_VECTOR);
        if (!s3) s3 = ASensorManager_getDefaultSensor(mgr, ASENSOR_TYPE_ROTATION_VECTOR);
        const ASensor *s6 = ASensorManager_getDefaultSensor(mgr, ASENSOR_TYPE_POSE_6DOF);
        XLOG("PROBE: 3dof(rotvec)=%s  6dof(pose6dof)=%s",
             s3 ? ASensor_getName(s3) : "无", s6 ? ASensor_getName(s6) : "无");
        if (s3) mask |= PROBE_3DOF;
        if (s6) mask |= PROBE_6DOF;
    } else {
        XWARN("PROBE: SensorManager 不可用");
    }

    XLOG("PROBE: 进程 uid=%d gid=%d egid=%d", (int)getuid(), (int)getgid(), (int)getegid());

    /* 候选：hidraw 扫描结果（通用内核）→ hwvr1 / hwvr0（华为私有驱动） */
    const char *hid_scan = hid_find_node();
    const char *cand[4];
    int nc = 0;
    if (hid_scan) cand[nc++] = hid_scan;
    cand[nc++] = HID_NODE;
    cand[nc++] = "/dev/hwvr0";
    cand[nc] = NULL;

    bool hid_ok = false;
    for (int i = 0; i < nc; i++) {
        int fd = open(cand[i], O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            XLOG("PROBE: HID %s 可打开 ✓", cand[i]);
            mask |= PROBE_HID;
            hid_ok = true;
            close(fd);
            break;
        }
        int e = errno;
        if (e == EACCES || e == EPERM) {
            XWARN("PROBE: HID %s 被拒绝 errno=%d（SELinux 或 group 不符），不是节点缺失",
                  cand[i], e);
        } else if (e != ENOENT) {
            XWARN("PROBE: HID %s 打不开 errno=%d (%s)", cand[i], e, strerror(e));
        }
    }
    if (!hid_ok) {
        XWARN("PROBE: 没有可用 HID 节点（hidraw 扫描与 hwvr0/1 都未命中）");
    }
    /* 原厂库探测：libvrpose 能 dlopen + 有 HelmetPoseGet 就算 3DoF 可用 */
    if (vrpose_load()) {
        XLOG("PROBE: 原厂 3dof(libvrpose HelmetPose) 可用 ✓");
        mask |= PROBE_HW_3DOF;
    }
    /* libVR6Dof 只检查能不能 dlopen（不主动调 GetVR6DofInterface，避免无谓初始化） */
    {
        void *lib6 = dlopen("libVR6Dof.so", RTLD_NOW);
        if (lib6) {
            if (dlsym(lib6, "GetVR6DofInterface")) {
                XLOG("PROBE: 原厂 6dof(libVR6Dof) 库可加载 ✓（SLAM 管线未接）");
                mask |= PROBE_HW_6DOF;
            }
            dlclose(lib6);
        } else {
            XWARN("PROBE: libVR6Dof.so 加载失败: %s", dlerror());
        }
    }

    XLOG("PROBE: mask=0x%x (手机3dof=%d 手机6dof=%d hid=%d 原厂3dof=%d 原厂6dof=%d)", mask,
         !!(mask & PROBE_3DOF), !!(mask & PROBE_6DOF), !!(mask & PROBE_HID),
         !!(mask & PROBE_HW_3DOF), !!(mask & PROBE_HW_6DOF));
    return mask;
}

void xr_pose_set_mode(const char *mode) {
    if (!mode) return;
    snprintf(g_xr.pose_mode, sizeof(g_xr.pose_mode), "%s", mode);
    XLOG("POSE: 模式被设置为 %s（热切换）", g_xr.pose_mode);
    pose_apply_mode(mode);
}

/* 停掉当前驱动，按 mode 重新选一个（init 与热切换共用） */
static void pose_apply_mode(const char *mode) {
    hid_stop();
    sensor_stop();
    vrpose_stop();
    g_pose.driver = DRV_NONE;

    int mask = xr_pose_probe();

    if (strcmp(mode, "hid") == 0) {
        if (hid_start()) { g_pose.driver = DRV_HID_3DOF; }
        else XWARN("POSE: 指定 hid 但 %s 打不开", HID_NODE);
    } else if (strcmp(mode, "3dof") == 0) {
        if (sensor_start(false)) { g_pose.driver = DRV_SENSOR_3DOF; }
        else XWARN("POSE: 指定 3dof 但手机旋转矢量不可用");
    } else if (strcmp(mode, "huawei") == 0 || strcmp(mode, "vrpose") == 0) {
        if (vrpose_start()) { g_pose.driver = DRV_HUAWEI_3DOF; }
        else XWARN("POSE: 指定 huawei 但 libvrpose 用不了");
    } else if (strcmp(mode, "6dof") == 0) {
        /* 优先原厂 SLAM；本机没有 SLAM 管线，退化到标准传感器 */
        if ((mask & PROBE_HW_6DOF) && vr6dof_probe()) {
            XWARN("POSE: libVR6Dof 接口已拿到，但相机帧还没接入，暂不可用");
        }
        if ((mask & PROBE_6DOF) && sensor_start(true)) {
            g_pose.driver = DRV_SENSOR_6DOF;
        } else {
            XWARN("POSE: 6dof 不可用（无 TYPE_POSE_6DOF，SLAM 管线未接）");
        }
    } else {
        /* auto：原厂眼镜 IMU > 标准 6dof > HID > 手机 IMU */
        if ((mask & PROBE_HW_3DOF) && vrpose_start()) {
            g_pose.driver = DRV_HUAWEI_3DOF;
        } else if ((mask & PROBE_6DOF) && sensor_start(true)) {
            g_pose.driver = DRV_SENSOR_6DOF;
        } else if ((mask & PROBE_HID) && hid_start()) {
            g_pose.driver = DRV_HID_3DOF;
        } else if (sensor_start(false)) {
            g_pose.driver = DRV_SENSOR_3DOF;
        }
    }

    XLOG("POSE: 选用驱动 = %s", xr_pose_driver_name());
}

void xr_pose_init(void) {
    if (g_pose_inited) return;
    memset(&g_pose, 0, sizeof(g_pose));
    pthread_mutex_init(&g_pose.lock, NULL);
    g_pose.hid_fd = -1;
    g_pose.rot.w = 1.0f;
    g_pose.pos.y = XR_EYE_HEIGHT_M;
    g_pose_inited = true;

    char mode[16] = {0};
    read_mode_file(mode, sizeof(mode));
    if (!mode[0]) snprintf(mode, sizeof(mode), "auto");
    snprintf(g_xr.pose_mode, sizeof(g_xr.pose_mode), "%s", mode);

    pose_apply_mode(mode);
    watch_start();
}

void xr_pose_shutdown(void) {
    watch_stop();
    vrpose_stop();
    hid_stop();
    sensor_stop();
}

const char *xr_pose_driver_name(void) {
    switch (g_pose.driver) {
        case DRV_SENSOR_3DOF: return "3dof(手机旋转矢量)";
        case DRV_SENSOR_6DOF: return "6dof(TYPE_POSE_6DOF)";
        case DRV_HID_3DOF:    return "3dof(眼镜 HID IMU)";
        case DRV_HUAWEI_3DOF: return "3dof(原厂 libvrpose HelmetPose)";
        case DRV_HUAWEI_6DOF: return "6dof(原厂 libVR6Dof SLAM)";
        default:              return "无(姿态静止)";
    }
}

void xr_pose_get(xq_t *out_rot, xv3_t *out_pos, bool *out_position_tracked) {
    pthread_mutex_lock(&g_pose.lock);
    *out_rot = g_pose.rot;
    *out_pos = g_pose.pos;
    *out_position_tracked = g_pose.position_valid;
    pthread_mutex_unlock(&g_pose.lock);
}

void xr_pose_recenter(void) {
    pthread_mutex_lock(&g_pose.lock);
    g_pose.rot = xq_norm(xq_mul(k_world_to_render, (xq_t){0, 0, 0, 1}));
    pthread_mutex_unlock(&g_pose.lock);
    /* 原厂驱动：丢掉基准，下一帧的姿态就成为新的「正前方」 */
    g_vrp.have_ref = false;
    XLOG("POSE: 已归零（下一帧作为基准）");
}
