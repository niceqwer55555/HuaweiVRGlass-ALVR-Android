/*
 * ctrlprobe.c —— 手柄姿态探针
 *
 * libvrpose.so 导出三组 C 包装（llvm-nm -D 确认）：
 *   HelmetPoseStart / HelmetPoseStop / HelmetPoseGet              头显 3DoF
 *   PhoneControllerPoseStart / ...Stop / ...Get                  手柄姿态
 *   VrCtrl3Dof_* （在 libctrl3dof.so）
 *
 * 头显那组已实测可用（1000Hz）。这里测手柄那组，按 C 惯例推断签名为：
 *   int PhoneControllerPoseStart(void);
 *   int PhoneControllerPoseStop(void);
 *   int PhoneControllerPoseGet(void *outVrPosture);
 * VrPosture 与 HelmetPoseGet 用同一个结构（前 4 个 float = 四元数 x,y,z,w），
 * 因此直接给 256 字节缓冲区，把前 12 个 float 打出来看结构。
 *
 * 为什么必须借华为自己的实现：
 *   - /dev/hwvr1 实测 6 秒只有 6002 个 0x03 报文（纯头显 IMU 1000Hz），
 *     手柄开机也不会出现新的报文类型；
 *   - /dev/hwvr2 是 0600 root:root，普通 app 打不开；
 *   - 12d1:3A09 那个 "6DoF Kit" 只有 VideoControl 接口、零端点，取不到流。
 * 所以只能让 libvrpose 内部去激活并解析手柄通道，我们再读它的结果。
 */
#include <dlfcn.h>
#include <string.h>
#include <time.h>

#include "xr_shim.h"

typedef int (*start_t)(void);
typedef int (*stop_t)(void);
typedef int (*get_t)(void *out);

void xr_ctrl_probe_run(void) {
    XLOG("======== 手柄姿态探针开始 ========");

    void *lib = dlopen("libvrpose.so", RTLD_NOW);
    if (!lib) {
        XWARN("CTRLPROBE: dlopen libvrpose.so 失败: %s", dlerror());
        return;
    }

    start_t start = (start_t) dlsym(lib, "PhoneControllerPoseStart");
    stop_t stop = (stop_t) dlsym(lib, "PhoneControllerPoseStop");
    get_t getpose = (get_t) dlsym(lib, "PhoneControllerPoseGet");
    XLOG("CTRLPROBE: start=%p stop=%p get=%p", (void *) start, (void *) stop,
         (void *) getpose);
    if (!start || !getpose) {
        XWARN("CTRLPROBE: 符号缺失，退出");
        return;
    }

    int r = start();
    XLOG("CTRLPROBE: PhoneControllerPoseStart() -> %d", r);

    unsigned char pose[256];
    int nonzero = 0;
    for (int i = 0; i < 150; i++) {
        memset(pose, 0, sizeof(pose));
        int ok = getpose(pose);
        const float *f = (const float *) pose;
        int any = 0;
        for (int k = 0; k < 12; k++) {
            if (f[k] != 0.0f) {
                any = 1;
            }
        }
        if (any) {
            nonzero++;
        }
        if (i < 8 || (i % 25) == 0) {
            XLOG("CTRLPROBE: #%d ok=%d q=(%.4f %.4f %.4f %.4f) f4..f7=(%.4f %.4f %.4f %.4f)",
                 i, ok, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        }
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000 * 1000; /* 10ms -> 100Hz */
        nanosleep(&ts, NULL);
    }
    XLOG("CTRLPROBE: 150 次采样中非零 %d 次", nonzero);

    if (stop) {
        XLOG("CTRLPROBE: PhoneControllerPoseStop() -> %d", stop());
    }
    XLOG("======== 手柄姿态探针结束 ========");
}
