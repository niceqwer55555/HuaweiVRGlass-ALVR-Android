/*
 * controller_probe.c - 探测 libHvrApi.so 的手柄接口在设备上是否可用
 *
 * 用法（设备上，root 或 app shell）:
 *   controller_probe [libdir]
 * 默认 libdir = /data/app/~~Ro-.../com.lht.huaweivr.alvr-.../lib/arm
 *
 * 只做 dlopen + 调 hvrInit/hvrController*，把结果 printf 出来。
 * 用于确认 6DoF 手柄 pose 能不能拿到，再决定是否接进 XrShim。
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*fn_init_t)(void *);
typedef int (*fn_deinit_t)(void);
typedef int (*fn_get_version_t)(char *, int);
typedef int (*fn_is_available_t)(int);
typedef int (*fn_get_valid_t)(int *, int);
typedef int (*fn_get_type_t)(int);
typedef int (*fn_get_status_t)(int);
typedef int (*fn_get_posture_t)(int, void *);
typedef int (*fn_get_grip_t)(int, void *);
typedef int (*fn_get_trigger_t)(int, void *);

static void *load_sym(void *h, const char *name) {
    void *p = dlsym(h, name);
    printf("[probe] dlsym %-32s = %p\n", name, p);
    return p;
}

int main(int argc, char **argv) {
    const char *libdir = argc > 1 ? argv[1]
        : "/data/app/~~Ro-1dvhNASRXXpZmaUxlLw==/com.lht.huaweivr.alvr-5DgIkfQGwNF9LFD0QCh5FQ==/lib/arm";
    char path[512];
    snprintf(path, sizeof(path), "%s/libHvrApi.so", libdir);

    printf("[probe] dlopen %s ...\n", path);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("[probe] dlopen FAILED: %s\n", dlerror()); return 1; }
    printf("[probe] dlopen OK\n");

    fn_get_version_t get_version = (fn_get_version_t)load_sym(h, "hvrGetVersion");
    fn_init_t init = (fn_init_t)load_sym(h, "hvrInit");
    fn_deinit_t deinit = (fn_deinit_t)load_sym(h, "hvrDeinit");
    fn_is_available_t is_avail = (fn_is_available_t)load_sym(h, "hvrControllerIsAvailable");
    fn_get_valid_t get_valid = (fn_get_valid_t)load_sym(h, "hvrControllerGetValidIndices");
    fn_get_type_t get_type = (fn_get_type_t)load_sym(h, "hvrControllerGetType");
    fn_get_status_t get_status = (fn_get_status_t)load_sym(h, "hvrControllerGetStatus");
    fn_get_posture_t get_posture = (fn_get_posture_t)load_sym(h, "hvrControllerGetPosture");
    fn_get_grip_t get_grip = (fn_get_grip_t)load_sym(h, "hvrControllerGetGripData");
    fn_get_trigger_t get_trigger = (fn_get_trigger_t)load_sym(h, "hvrControllerGetTriggerData");

    if (get_version) {
        char ver[128] = {0};
        int rc = get_version(ver, sizeof(ver));
        printf("[probe] hvrGetVersion rc=%d ver='%s'\n", rc, ver);
    }

    if (!init) { printf("[probe] NO hvrInit, abort\n"); return 2; }
    /* hvrInit 需要上下文；先试 NULL，失败再试其它 */
    printf("[probe] hvrInit(NULL) ...\n");
    int rc = init(NULL);
    printf("[probe] hvrInit(NULL) rc=%d\n", rc);
    if (rc != 0) {
        printf("[probe] hvrInit(NULL) failed; trying dummy ctx\n");
        int dummy[64] = {0};
        rc = init(dummy);
        printf("[probe] hvrInit(dummy) rc=%d\n", rc);
    }

    /* 等 SDK 初始化/扫描手柄 */
    for (int t = 0; t < 3; t++) {
        sleep(1);
        if (is_avail) {
            printf("[probe] t=%d IsAvailable(0)=%d IsAvailable(1)=%d\n",
                   t, is_avail(0), is_avail(1));
        }
        if (get_valid) {
            int idx[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
            int n = get_valid(idx, 8);
            printf("[probe] t=%d GetValidIndices rc=%d idx=[%d,%d,%d,%d]\n",
                   t, n, idx[0], idx[1], idx[2], idx[3]);
        }
    }

    for (int id = 0; id < 2; id++) {
        if (get_type)   printf("[probe] ctrl%d type=%d\n", id, get_type(id));
        if (get_status) printf("[probe] ctrl%d status=%d\n", id, get_status(id));
        if (get_posture) {
            float posture[16] = {0};
            int r = get_posture(id, posture);
            printf("[probe] ctrl%d posture rc=%d: ", id, r);
            for (int i = 0; i < 8 && r == 0; i++) printf("%.4f ", posture[i]);
            printf("\n");
        }
        if (get_grip) {
            float grip[16] = {0};
            int r = get_grip(id, grip);
            printf("[probe] ctrl%d grip rc=%d: ", id, r);
            for (int i = 0; i < 7 && r == 0; i++) printf("%.4f ", grip[i]);
            printf("\n");
        }
        if (get_trigger) {
            float trig[8] = {0};
            int r = get_trigger(id, trig);
            printf("[probe] ctrl%d trigger rc=%d: ", id, r);
            for (int i = 0; i < 4 && r == 0; i++) printf("%.4f ", trig[i]);
            printf("\n");
        }
    }

    if (deinit) { printf("[probe] hvrDeinit ...\n"); deinit(); }
    dlclose(h);
    printf("[probe] done\n");
    return 0;
}
