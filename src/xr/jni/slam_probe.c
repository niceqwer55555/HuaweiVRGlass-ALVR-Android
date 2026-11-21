/*
 * slam_probe.c - 华为 6DoF SLAM 管线的运行时探针
 *
 * 目标：在**没有任何华为系统服务**的普通 app 进程里，验证原厂 SLAM 库能否
 * 加载并初始化。这是整条 6DoF 路线的生死判据。
 *
 * 依据（静态逆向结论）：
 *   libruntime_service_module.so 导出了完整 C++ 门面
 *     SlamState::GetInstance() / Init(void(*)(int)) / Start() / Stop()
 *     SlamState::SetValue(const char* key, char type, const void* value)
 *     SlamState::PushSlamData(HxrAlgorithmData*) / PushIMU(HxrAlgorithmData*)
 *     SlamState::GetSlamPosture(HxrAlgorithmData*)
 *     SlamThread::PushImageData(uint8_t*) / PushImuData(uint8_t*) / GetPosture(...)
 *   参数是字符串键：InitPara.Camera.imageWidth / camIntraParas.fx / distCoefs.k1 ...
 *
 * 探针只做"读"，不改变主流程；失败也只是打日志。
 */
#include "xr_shim.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *(*fn_getinstance_t)(void);
typedef int (*fn_init_t)(void *self, void (*cb)(int));
typedef int (*fn_self_t)(void *self);
typedef int (*fn_setval_t)(void *self, const char *key, char type, const void *val);
typedef void *(*fn_getiface_t)(void);

/* 把地址落到 模块名+偏移，用来认出函数指针属于哪个 so */
static void log_ptr(const char *tag, int idx, uintptr_t v) {
    if (v == 0) {
        XLOG("  %s[%02d] = 0", tag, idx);
        return;
    }
    Dl_info di;
    memset(&di, 0, sizeof(di));
    if (dladdr((void *)v, &di) && di.dli_fname) {
        const char *base = strrchr(di.dli_fname, '/');
        base = base ? base + 1 : di.dli_fname;
        XLOG("  %s[%02d] = 0x%08x  -> %s+0x%x %s", tag, idx, (unsigned)v, base,
             (unsigned)(v - (uintptr_t)di.dli_fbase),
             di.dli_sname ? di.dli_sname : "");
    } else {
        XLOG("  %s[%02d] = 0x%08x", tag, idx, (unsigned)v);
    }
}

/* dump 对象的指针表（接口是函数指针结构体，字段都在固定偏移） */
static void dump_obj(const char *tag, const void *obj, int bytes) {
    if (!obj) {
        XWARN("%s: 对象为空", tag);
        return;
    }
    XLOG("%s: 对象 %p，dump %d 字节", tag, obj, bytes);
    const uintptr_t *p = (const uintptr_t *)obj;
    for (int i = 0; i < bytes / 4; i++) log_ptr(tag, i, p[i]);
}

static void slam_cb(int what) {
    XLOG("SLAM 回调: %d", what);
}

static void try_dlopen(const char *name, void **out) {
    *out = dlopen(name, RTLD_NOW);
    if (*out) {
        XLOG("dlopen %-32s OK", name);
    } else {
        const char *e = dlerror();
        XERR("dlopen %-32s 失败: %s", name, e ? e : "?");
    }
}

/* 尝试用 SlamState::SetValue 写一个整数参数 */
static void set_int(void *inst, fn_setval_t fn, const char *key, int v) {
    if (!fn) return;
    int rc = fn(inst, key, 'i', &v);
    XLOG("  SetValue(\"%s\", 'i', %d) -> %d", key, v, rc);
}

static void set_float(void *inst, fn_setval_t fn, const char *key, float v) {
    if (!fn) return;
    int rc = fn(inst, key, 'f', &v);
    XLOG("  SetValue(\"%s\", 'f', %.4f) -> %d", key, v, rc);
}

static void set_str(void *inst, fn_setval_t fn, const char *key, const char *v) {
    if (!fn) return;
    int rc = fn(inst, key, 's', v);
    XLOG("  SetValue(\"%s\", 's', \"%s\") -> %d", key, v, rc);
}

void xr_slam_probe_run(JavaVM *vm, jobject activity) {
    XLOG("======== SLAM 探针开始 ========");

    JNIEnv *env = NULL;

    /* --- 0. 补齐 JNI 环境 ---
     * SlamState::Init 内部靠 ArvrRuntimeModuleManager::GetPackageExternelPath
     * 用 JNI 调 Java 拿外部路径；拿不到就 "jni is null"，标定 xml 路径会退化成
     * 根目录 "/SlamXml/vrglass.xml"（无 root 不可写）。
     *
     * 注意：HvrJniClass::InitEnv 是 C++ 成员函数（隐含 this），直接当普通函数
     * 调用会在 +20 处 SIGSEGV，所以这里只把线程 attach 到 JVM（GetEnv 可用），
     * 真正的路径改由下面的 SetValue 参数注入。 */
    if (vm) {
        if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
            if ((*vm)->AttachCurrentThread(vm, (void **)&env, NULL) != JNI_OK) env = NULL;
        }
        XLOG("JNI: env=%p activity=%p", (void *)env, (void *)activity);
    }

    void *h_slam = NULL, *h_6dof = NULL, *h_rsm = NULL;
    try_dlopen("libarvr_slam.so", &h_slam);
    try_dlopen("libVR6Dof.so", &h_6dof);
    try_dlopen("libruntime_service_module.so", &h_rsm);

    /* --- 1. 两个底层入口 --- */
    if (h_slam) {
        fn_getiface_t g = (fn_getiface_t)dlsym(h_slam, "GetSlamInterface");
        XLOG("GetSlamInterface 符号 = %p", (void *)g);
        if (g) {
            void *iface = g();
            dump_obj("SlamIface", iface, 160);
        }
    } else {
        XERR("libarvr_slam.so 加载失败，跳过 SLAM 接口");
    }

    if (h_6dof) {
        fn_getiface_t g = (fn_getiface_t)dlsym(h_6dof, "GetVR6DofInterface");
        XLOG("GetVR6DofInterface 符号 = %p", (void *)g);
        if (g) {
            void *iface = g();
            dump_obj("Vr6Iface", iface, 160);
        }
    } else {
        XERR("libVR6Dof.so 加载失败，跳过 6DoF 接口");
    }

    if (!h_rsm) {
        XERR("libruntime_service_module.so 加载失败，探针结束");
        return;
    }

    /* --- 2. 门面：SlamState --- */
    fn_getinstance_t get_inst =
            (fn_getinstance_t)dlsym(h_rsm, "_ZN9SlamState11GetInstanceEv");
    fn_init_t init = (fn_init_t)dlsym(h_rsm, "_ZN9SlamState4InitEPFviE");
    fn_self_t start = (fn_self_t)dlsym(h_rsm, "_ZN9SlamState5StartEv");
    fn_self_t stop = (fn_self_t)dlsym(h_rsm, "_ZN9SlamState4StopEv");
    fn_self_t flag = (fn_self_t)dlsym(h_rsm, "_ZN9SlamState7GetFlagEv");
    fn_setval_t setval =
            (fn_setval_t)dlsym(h_rsm, "_ZN9SlamState8SetValueEPKcxPKv");

    XLOG("符号: GetInstance=%p Init=%p Start=%p Stop=%p GetFlag=%p SetValue=%p",
         (void *)get_inst, (void *)init, (void *)start, (void *)stop,
         (void *)flag, (void *)setval);

    if (!get_inst) {
        XERR("找不到 SlamState::GetInstance，探针结束");
        return;
    }

    void *inst = get_inst();
    XLOG("SlamState 实例 = %p", inst);
    dump_obj("SlamState", inst, 96);

    /* --- 2.5 把 Java 上下文喂给 native 管理器 ---
     * ArvrRuntimeModuleManager::GetPackageExternelPath 返回
     *   mService.getExternalFilesDir("").getAbsolutePath()
     * 之前没有 Java 对象 → 返回 null → 标定路径退化成 "/SlamXml/vrglass.xml"
     * （根分区是只读 erofs，必然失败）。这里把 Activity 当 Context 传进去，
     * 路径就会变成 /sdcard/Android/data/com.lht.huaweivr.alvr/files，
     * 我们把 vrglass.xml 预置在它下面的 SlamXml/ 里。
     * 注意该 JNI 函数的 thiz 参数在实现里未被使用，可以传 NULL。 */
    if (env && activity) {
        typedef jboolean (*fn_initmm_t)(JNIEnv *, jobject, jobject);
        fn_initmm_t initmm = (fn_initmm_t)dlsym(
                h_rsm,
                "Java_com_huawei_hvrsdkserverapp_module_ArvrRuntimeModuleManager_"
                "initModuleManagerNative");
        XLOG("initModuleManagerNative 符号 = %p", (void *)initmm);
        if (initmm) {
            jboolean ok = initmm(env, NULL, activity);
            XLOG("initModuleManagerNative(activity) -> %d", (int)ok);
        }
    }

    /* --- 3. 写相机参数（宽高 + 内参）---
     * 值先用占位，真正的内参要等 UVC 出图后标定/从原厂读。 */
    if (setval && inst) {
        XLOG("写入相机参数:");
        set_int(inst, setval, "InitPara.Camera.imageWidth", 640);
        set_int(inst, setval, "InitPara.Camera.imageheight", 480);
        set_int(inst, setval, "Common.slamFPS", 30);
        set_int(inst, setval, "InitPara.IMU.accelFPS", 1000);
        set_int(inst, setval, "InitPara.IMU.gyroFPS", 1000);
        set_float(inst, setval, "camIntraParas.fx", 320.0f);
        set_float(inst, setval, "camIntraParas.fy", 320.0f);
        set_float(inst, setval, "camIntraParas.cx", 320.0f);
        set_float(inst, setval, "camIntraParas.cy", 240.0f);
        /* 标定 xml 的基准目录：原厂靠 JNI 拿外部路径，我们这里直接注入，
         * 让它去读 /sdcard/Android/data/<pkg>/files/SlamXml/vrglass.xml */
        set_str(inst, setval, "Test.savePathAndroid",
                "/sdcard/Android/data/com.lht.huaweivr.alvr/files");
        set_str(inst, setval, "HSVar.ParsingPath",
                "/sdcard/Android/data/com.lht.huaweivr.alvr/files");
    }

    /* --- 4. 初始化 --- */
    if (init) {
        XLOG("调用 SlamState::Init(cb) ...");
        int rc = init(inst, slam_cb);
        XLOG("SlamState::Init 返回 %d", rc);
    } else {
        XWARN("没有 Init 符号");
    }

    if (flag) XLOG("GetFlag() = %d", flag(inst));

    if (start) {
        XLOG("调用 SlamState::Start() ...");
        int rc = start(inst);
        XLOG("SlamState::Start 返回 %d", rc);
    }

    if (flag) XLOG("Start 后 GetFlag() = %d", flag(inst));

    /* --- 5. 换一条门面：SlamThread（设备侧封装，自带标定/线程管理）--- */
    fn_getinstance_t get_thread =
            (fn_getinstance_t)dlsym(h_rsm, "_ZN10SlamThread11GetInstanceEv");
    fn_init_t th_init = (fn_init_t)dlsym(h_rsm, "_ZN10SlamThread4InitEPFviE");
    fn_self_t th_start = (fn_self_t)dlsym(h_rsm, "_ZN10SlamThread5StartEv");
    XLOG("SlamThread 符号: GetInstance=%p Init=%p Start=%p", (void *)get_thread,
         (void *)th_init, (void *)th_start);
    if (get_thread) {
        void *th = get_thread();
        XLOG("SlamThread 实例 = %p", th);
        if (th_init) {
            XLOG("调用 SlamThread::Init(cb) ...");
            int rc = th_init(th, slam_cb);
            XLOG("SlamThread::Init 返回 %d", rc);
        }
        if (th_start) {
            XLOG("调用 SlamThread::Start() ...");
            int rc = th_start(th);
            XLOG("SlamThread::Start 返回 %d", rc);
        }
    } else {
        XWARN("找不到 SlamThread::GetInstance");
    }

    XLOG("======== SLAM 探针结束 ========");
}
