/*
 * hvr_bridge.c - 华为 HvrApi 手柄桥接（6DoF）
 *
 * libHvrApi.so 的 hvrInit 需要一个 Android Activity（JNI jobject），
 * 它通过 com.huawei.hvr.LibUpdateClient.getVrSdkLibPath(Context) 拿到
 * SDK so 目录，再 dlopen libvrunityplugin.so。
 *
 * 本文件：
 *   hvr_bridge_init(JNIEnv*, jobject activity)  - 调 hvrInit + 启动轮询线程
 *   轮询线程读左右手柄 posture/getValidIndices 打到 logcat（验证阶段）
 *
 * 后续把 pose 送进 XrShim action space 即完成 SteamVR 手柄识别。
 */
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xr_shim.h"

typedef int (*fn_init_t)(void *);
typedef int (*fn_deinit_t)(void);
typedef int (*fn_get_valid_t)(int *, int);
typedef int (*fn_is_avail_t)(int);
typedef int (*fn_get_posture_t)(int, void *);
typedef int (*fn_get_grip_t)(int, void *);
typedef int (*fn_get_type_t)(int);
typedef int (*fn_get_status_t)(int);

static void *s_hvr = NULL;
static fn_get_valid_t s_get_valid;
static fn_is_avail_t s_is_avail;
static fn_get_posture_t s_get_posture;
static fn_get_grip_t s_get_grip;
static fn_get_type_t s_get_type;
static fn_get_status_t s_get_status;
static fn_deinit_t s_deinit;
static volatile int s_running;

static void *poll_thread(void *arg) {
    (void)arg;
    XLOG("HVRBRIDGE: 轮询线程启动");
    for (int n = 0; n < 300 && s_running; n++) {
        int idx[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
        int nv = s_get_valid ? s_get_valid(idx, 8) : -1;
        if ((n % 20) == 0) {
            XLOG("HVRBRIDGE: t=%d GetValidIndices rc=%d idx=[%d,%d,%d,%d] avail=(%d,%d)",
                 n, nv, idx[0], idx[1], idx[2], idx[3],
                 s_is_avail ? s_is_avail(0) : -1, s_is_avail ? s_is_avail(1) : -1);
        }
        for (int id = 0; id < 2; id++) {
            if (s_get_posture) {
                float p[16];
                memset(p, 0, sizeof(p));
                int rc = s_get_posture(id, p);
                if ((n % 20) == 0 || (p[0] != 0 || p[4] != 0)) {
                    XLOG("HVRBRIDGE: t=%d ctrl%d posture rc=%d q=(%.4f %.4f %.4f %.4f) pos=(%.4f %.4f %.4f)",
                         n, id, rc, p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
                }
            }
        }
        usleep(100000); /* 10Hz */
    }
    XLOG("HVRBRIDGE: 轮询线程结束");
    return NULL;
}

void hvr_bridge_init(JNIEnv *env, jobject activity) {
    if (s_hvr) { XLOG("HVRBRIDGE: 已初始化"); return; }
    XLOG("HVRBRIDGE: dlopen libHvrApi.so ...");
    s_hvr = dlopen("libHvrApi.so", RTLD_NOW);
    if (!s_hvr) { XWARN("HVRBRIDGE: dlopen 失败 %s", dlerror()); return; }
    XLOG("HVRBRIDGE: dlopen OK");

    fn_init_t init = (fn_init_t)dlsym(s_hvr, "hvrInit");
    s_deinit = (fn_deinit_t)dlsym(s_hvr, "hvrDeinit");
    s_get_valid = (fn_get_valid_t)dlsym(s_hvr, "hvrControllerGetValidIndices");
    s_is_avail = (fn_is_avail_t)dlsym(s_hvr, "hvrControllerIsAvailable");
    s_get_posture = (fn_get_posture_t)dlsym(s_hvr, "hvrControllerGetPosture");
    s_get_grip = (fn_get_grip_t)dlsym(s_hvr, "hvrControllerGetGripData");
    s_get_type = (fn_get_type_t)dlsym(s_hvr, "hvrControllerGetType");
    s_get_status = (fn_get_status_t)dlsym(s_hvr, "hvrControllerGetStatus");
    XLOG("HVRBRIDGE: init=%p valid=%p avail=%p posture=%p grip=%p",
         (void*)init, (void*)s_get_valid, (void*)s_is_avail,
         (void*)s_get_posture, (void*)s_get_grip);

    if (!init) { XWARN("HVRBRIDGE: 无 hvrInit"); return; }
    if (!activity) { XWARN("HVRBRIDGE: activity 为空，跳过 hvrInit"); return; }
    XLOG("HVRBRIDGE: hvrInit(activity) ...");
    int rc = init(activity);   /* hvrInit 参数是 jobject (Android Activity) */
    XLOG("HVRBRIDGE: hvrInit rc=%d", rc);
    if (rc != 0) { XWARN("HVRBRIDGE: hvrInit 失败，保留库句柄供重试"); return; }

    s_running = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, poll_thread, NULL) == 0) {
        pthread_detach(th);
    } else {
        XWARN("HVRBRIDGE: 轮询线程创建失败");
    }
}

void hvr_bridge_stop(void) {
    s_running = 0;
    if (s_deinit) { s_deinit(); s_deinit = NULL; }
    if (s_hvr) { dlclose(s_hvr); s_hvr = NULL; }
}
