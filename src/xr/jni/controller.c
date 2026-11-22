/*
 * controller.c - 手柄 6DoF/3DoF 姿态数据源
 *
 * 数据源：libvrpose.so 的 PhoneControllerPoseStart/Get（单例，当前设备实测有效）
 *   - 返回前 4 个 float = 四元数 (x,y,z,w)
 *   - 位置字段恒 0（3DoF 手柄无位置）
 *
 * 提供给 XrShim 的 action space 使用：
 *   xr_controller_get_pose(hand, &q, &pos) 返回该手是否 tracked
 *
 * 后续升级：若 HvrApi 的 hvrControllerGetPosture 能拿到左右手 6DoF，
 * 在此切换数据源即可，上层 action space 不用改。
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "xr_shim.h"

typedef int (*ctrl_start_t)(void);
typedef int (*ctrl_stop_t)(void);
typedef int (*ctrl_get_t)(void *out);
/* [2026-11-22] hidraw3 OV580 IMU -> PhoneControllerPose::onDataReceived
 * libvrpose.so 是 ARM 指令集; onDataReceived VA=0xe4e0(无 thumb bit)
 * Singleton<PhoneControllerPose>::instance_ VA=0x25060 */
typedef void (*fn_on_data_t)(void *self, const char *path, void *data, unsigned int len);

static void *s_lib;
static ctrl_start_t s_start;
static ctrl_stop_t s_stop;
static ctrl_get_t s_get;
static pthread_t s_th;
static volatile bool s_running;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* 每手一份姿态 */
typedef struct {
    xq_t rot;
    xv3_t pos;
    bool tracked;
    int samples;
} ctrl_state_t;
static ctrl_state_t s_ctrl[2];

static void *ctrl_thread(void *arg) {
    (void)arg;
    unsigned char buf[128];

    /* [2026-11-22] 尝试打开 hidraw3 (OV580 6DoF 模组) 作为 IMU 数据源。
     * ctrl_use_phone 文件 -> 回退手机传感器; 否则用 hidraw3。
     * ctrl_hidraw3_only 文件 -> 不调 PhoneControllerPoseStart, 只用 hidraw3 数据
     *   (避免手机传感器与 hidraw3 竞争, 让姿态真正由 OV580 驱动)。 */
    int imu_fd = -1;
    fn_on_data_t on_data = NULL;
    void *vp_self = NULL;
    int hidraw3_only = 0;
    {
        FILE *fb = fopen("/sdcard/Android/data/com.lht.huaweivr.alvr/files/ctrl_use_phone", "r");
        FILE *fo = fopen("/sdcard/Android/data/com.lht.huaweivr.alvr/files/ctrl_hidraw3_only", "r");
        if (fo) { fclose(fo); hidraw3_only = 1; }
        if (fb) {
            fclose(fb);
            XLOG("CTRL: ctrl_use_phone 存在 -> 用手机传感器姿态(回退)");
        } else {
            Dl_info di;
            void *getfn = dlsym(s_lib, "PhoneControllerPoseGet");
            if (getfn && dladdr(getfn, &di) && di.dli_fbase) {
                uintptr_t base = (uintptr_t)di.dli_fbase;
                on_data = (fn_on_data_t)(base + 0xe4e0);           /* ARM, 无 thumb bit */
                imu_fd = open("/dev/hidraw3", O_RDONLY | O_NONBLOCK);
                XLOG("CTRL: hidraw3 open onData=%p fd=%d only=%d (self待Start后读)",
                     (void *)on_data, imu_fd, hidraw3_only);
            }
            if (imu_fd < 0) XWARN("CTRL: hidraw3 打开失败, 回退手机传感器");
        }
    }
    /* Start 初始化单例(PhoneControllerPose 实例), 之后才能读 instance_。
     * hidraw3_only: Start 后立刻 Stop 停掉手机传感器线程, 姿态纯由 hidraw3 驱动。 */
    int rc = -1;
    if (s_start) {
        rc = s_start();
        XLOG("CTRL: PhoneControllerPoseStart -> %d", rc);
    }
    if (on_data && imu_fd >= 0) {
        Dl_info di2;
        void *gf2 = dlsym(s_lib, "PhoneControllerPoseGet");
        if (gf2 && dladdr(gf2, &di2) && di2.dli_fbase) {
            vp_self = *(void **)((uintptr_t)di2.dli_fbase + 0x25060); /* Start后读单例 */
        }
        XLOG("CTRL: hidraw3 feed self=%p onData=%p fd=%d only=%d",
             vp_self, (void *)on_data, imu_fd, hidraw3_only);
    }
    if (hidraw3_only && vp_self) {
        if (s_stop) s_stop();
        XLOG("CTRL: ctrl_hidraw3_only -> Stop手机传感器, 纯hidraw3驱动 self=%p", vp_self);
    }

    int valid = 0, fed = 0;
    while (s_running) {
        /* 先把 hidraw3 的 IMU 帧喂给姿态引擎 */
        if (imu_fd >= 0 && on_data && vp_self) {
            ssize_t n = read(imu_fd, buf, sizeof(buf));
            if (n > 0) { on_data(vp_self, "/dev/hidraw3", buf, (unsigned int)n); fed++; }
        }
        memset(buf, 0, sizeof(buf));
        int r = s_get ? s_get(buf) : -1;
        float f[8];
        memcpy(f, buf, sizeof(f));
        xq_t q = {f[0], f[1], f[2], f[3]};
        float len = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        if (len > 0.5f && len < 1.5f) {
            q = xq_norm(q);
            pthread_mutex_lock(&s_lock);
            for (int h = 0; h < 2; h++) {
                s_ctrl[h].rot = q;
                /* 3DoF：位置固定在腰前，左右分开一点；避免 (0,0,0) 被 SteamVR 判无效 */
                s_ctrl[h].pos.x = h == 0 ? -0.15f : 0.15f;
                s_ctrl[h].pos.y = 1.1f;
                s_ctrl[h].pos.z = -0.2f;
                s_ctrl[h].tracked = true;
                s_ctrl[h].samples++;
            }
            pthread_mutex_unlock(&s_lock);
            valid++;
            if ((valid % 400) == 0) {
                XLOG("CTRL: pose q=(%.4f %.4f %.4f %.4f) fed=%d valid=%d src=%s",
                     q.x, q.y, q.z, q.w, fed, valid,
                     imu_fd >= 0 ? "hidraw3" : "phone");
            }
        }
        usleep(14000); /* ~70Hz, 与头显刷新率一致, 降低CPU压力 */
    }
    if (imu_fd >= 0) close(imu_fd);
    if (s_stop) s_stop();
    XLOG("CTRL: 线程结束（有效 %d 帧, hidraw3 喂入 %d 帧）", valid, fed);
    return NULL;
}

bool xr_controller_init(void) {
    if (s_running) return true;
    s_lib = dlopen("libvrpose.so", RTLD_NOW);
    if (!s_lib) { XWARN("CTRL: dlopen libvrpose.so 失败: %s", dlerror()); return false; }
    s_start = (ctrl_start_t)dlsym(s_lib, "PhoneControllerPoseStart");
    s_stop  = (ctrl_stop_t)dlsym(s_lib, "PhoneControllerPoseStop");
    s_get   = (ctrl_get_t)dlsym(s_lib, "PhoneControllerPoseGet");
    if (!s_start || !s_get) { XWARN("CTRL: 手柄符号缺失"); return false; }
    s_running = true;
    if (pthread_create(&s_th, NULL, ctrl_thread, NULL) != 0) {
        XWARN("CTRL: 线程创建失败");
        s_running = false;
        return false;
    }
    pthread_detach(s_th);
    XLOG("CTRL: 手柄姿态线程已启动");
    return true;
}

bool xr_controller_get_pose(int hand, xq_t *rot, xv3_t *pos) {
    if (hand < 0 || hand > 1) return false;
    pthread_mutex_lock(&s_lock);
    bool t = s_ctrl[hand].tracked;
    if (t) {
        if (rot) *rot = s_ctrl[hand].rot;
        if (pos) *pos = s_ctrl[hand].pos;
    }
    pthread_mutex_unlock(&s_lock);
    return t;
}

int xr_controller_samples(int hand) {
    if (hand < 0 || hand > 1) return 0;
    pthread_mutex_lock(&s_lock);
    int n = s_ctrl[hand].samples;
    pthread_mutex_unlock(&s_lock);
    return n;
}
