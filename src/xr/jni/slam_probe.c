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
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

/* [2026-11-22] GOT hook: 伪造 GetPackageExternelPath 返回 app 外部目录。
 * 原函数走 mService.getExternalFilesDir() 的 JNI，无华为服务时返回 null →
 * SLAM 退化读 /SlamXml/vrglass.xml(根只读) 失败。
 * 签名(arm32 libc++ 非ALTERNATE string): void fn(char* sret, void* self)
 *   std::string 布局 12B: [0]=cap<<1|1 (is_long), [4]=size, [8]=data ptr */
static const char *k_fake_ext_path = "/sdcard/Android/data/com.lht.huaweivr.alvr/files";

static void hook_get_ext_path(char *sret, void *self) {
    (void)self;
    uint32_t len = (uint32_t)strlen(k_fake_ext_path);
    /* data 必须在堆上：SDK 析构这个 std::string 时会 free(data)，
     * 指向静态只读区会触发 Scudo misaligned/invalid free 崩溃。 */
    char *heap = (char *)malloc(len + 1);
    if (!heap) { memset(sret, 0, 12); return; }
    memcpy(heap, k_fake_ext_path, len + 1);
    *(uint32_t *)(sret + 0) = ((len + 16) << 1) | 1;  /* cap<<1 | is_long */
    *(uint32_t *)(sret + 4) = len;                     /* size */
    *(char **)(sret + 8) = heap;                       /* data (heap, freeable) */
    XLOG("GOTHOOK: GetPackageExternelPath -> \"%s\" len=%u heap=%p",
         k_fake_ext_path, len, (void *)heap);
}

/* 在进程所有可写映射里扫描指向 target 的指针并替换为 replacement。
 * 返回替换个数。GOT 可能在 relro/匿名映射里，所以不限 so 名。 */
static int patch_got_for(void *target, void *replacement, const char *so_hint) {
    (void)so_hint;
    uintptr_t tgt = (uintptr_t)target;
    int patched = 0;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) { XWARN("GOTHOOK: 打不开 /proc/self/maps"); return 0; }
    char line[512];
    long page = sysconf(_SC_PAGESIZE);
    while (fgets(line, sizeof(line), maps)) {
        char perms[8] = {0};
        sscanf(line, "%*x-%*x %7s", perms);
        if (strchr(perms, 'w') == NULL) continue;   /* 只扫可写映射 */
        unsigned long lo, hi;
        sscanf(line, "%lx-%lx", &lo, &hi);
        uint32_t *p = (uint32_t *)lo;
        uint32_t *end = (uint32_t *)(hi - 4);
        for (; p < end; p++) {
            uint32_t v = *p;
            /* GOT 里函数指针通常带 thumb bit(=target 原值)，也可能是清 bit 的入口 */
            if (v == (uint32_t)tgt || v == (uint32_t)(tgt & ~1u)) {
                uintptr_t pg = ((uintptr_t)p / page) * page;
                mprotect((void *)pg, page, PROT_READ | PROT_WRITE | PROT_EXEC);
                *p = (uint32_t)(uintptr_t)replacement;
                __builtin___clear_cache((char *)p, (char *)p + 4);
                XLOG("GOTHOOK: patched ptr @%p (was 0x%x) -> %p", (void *)p, v, replacement);
                patched++;
                if (patched >= 8) break;  /* 防止误伤太多 */
            }
        }
        if (patched >= 8) break;
    }
    fclose(maps);
    return patched;
}

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

    /* --- 0.5 [2026-11-21] 用 HvrApi C API 打开 debugxml，绕开 setprop 无效 ---
     * hvrSetProperty 是纯转发代理(2 参数: const char* key, const void* value)，
     * 设的是进程内变量，而 adb setprop hvr.debugxml 只改系统 property(native 读不到)。
     * hvrInit 已由 hvr_bridge 调过，这里 dlopen 同一个 so 拿现成句柄。
     * 对同一 key 试字符串"1"和整数1，读回看哪个生效。 */
    {
        void *h_api = dlopen("libHvrApi.so", RTLD_NOW);
        XLOG("HVRPROP: dlopen libHvrApi.so = %p", h_api);
        if (h_api) {
            typedef int (*fn_setprop_t)(const char *key, const void *val);
            typedef int (*fn_getprop_t)(const char *key, void *out, int *len);
            fn_setprop_t setp = (fn_setprop_t)dlsym(h_api, "hvrSetProperty");
            fn_getprop_t getp = (fn_getprop_t)dlsym(h_api, "hvrGetProperty");
            XLOG("HVRPROP: hvrSetProperty=%p hvrGetProperty=%p", (void *)setp, (void *)getp);
            if (setp) {
                /* 试法1: value 指字符串 "1" */
                const char *one = "1";
                int r1 = setp("hvr.debugxml", one);
                XLOG("HVRPROP: set(\"hvr.debugxml\", \"1\") -> %d", r1);
                /* 试法2: value 指整数 1 */
                int ione = 1;
                int r2 = setp("hvr.debugxml", &ione);
                XLOG("HVRPROP: set(\"hvr.debugxml\", &int1) -> %d", r2);
                /* 试其它高价值开关 */
                int r3 = setp("hvr.runtimeToolLog", one);
                XLOG("HVRPROP: set(\"hvr.runtimeToolLog\", \"1\") -> %d", r3);
                /* 试直接给路径 key */
                const char *p = "/sdcard/Android/data/com.lht.huaweivr.alvr/files";
                int r4 = setp("HSVar.ParsingPath", p);
                XLOG("HVRPROP: set(\"HSVar.ParsingPath\", path) -> %d", r4);
                int r5 = setp("Test.savePathAndroid", p);
                XLOG("HVRPROP: set(\"Test.savePathAndroid\", path) -> %d", r5);
            }
            if (getp) {
                char buf[256] = {0};
                int len = sizeof(buf);
                int gr = getp("hvr.debugxml", buf, &len);
                XLOG("HVRPROP: get(\"hvr.debugxml\") -> %d len=%d val=\"%s\"", gr, len, buf);
            }
        }
    }

    /* --- 0.6 [2026-11-22] 直接查 SDK 内部的路径/调试状态 ---
     * 静态符号(libruntime_service_module.so):
     *   ArvrRuntimeModuleManager::GetPackageExternelPath() @0x7656d (返回 std::string)
     *   VrConfiguration::GetXmlDebugingStatus() @0x90035 (无参, 读全局)
     * 目的: 确认 JNI 路径到底返回什么, 以及 debugxml 真实状态。 */
    if (h_rsm) {
        typedef int (*fn_dbgstatus_t)(void);
        void *dbg = dlsym(h_rsm, "_ZN15VrConfiguration20GetXmlDebugingStatusEv");
        /* 不要直接调 dbg()：它是 C++ 成员函数(隐含 this)，无实例调用会 SIGSEGV
         * (实测 GetXmlDebugingStatus()+6 崩溃，打断整个探针)。只打印符号。 */
        XLOG("PATHDBG: GetXmlDebugingStatus=%p (符号存在,未调用)", dbg);
        /* GetPackageExternelPath 返回 std::string(arm32: r0=sret指针, this在r0)——
         * 需要实例, 先只打印符号存在性, 不强调用避免 ABI 崩溃。 */
        void *gep = dlsym(h_rsm,
                "_ZN24ArvrRuntimeModuleManager22GetPackageExternelPathEv");
        XLOG("PATHDBG: GetPackageExternelPath=%p", gep);
        void *setpath = dlsym(h_rsm,
                "_ZN6XmlOpt7SetPathENSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE");
        XLOG("PATHDBG: XmlOpt::SetPath=%p", setpath);

        /* [2026-11-22] 核心修复: GOT hook GetPackageExternelPath, 让它返回
         * app 外部目录而不是 null。默认开启; 放 slam_no_hook 文件可关闭回退。
         * 必须在 SlamState::Init(=InitPara) 之前 patch。
         * 精确 GOT: R_ARM_JUMP_SLOT @ vaddr 0x15e288 (objdump -R 确认)。
         * lazy binding 下该槽在首次调用前可能还是 0/PLT, 所以不能靠扫描值,
         * 直接用 so_base+0x15e288 定位写入。 */
        {
            FILE *nf = fopen("/sdcard/Android/data/com.lht.huaweivr.alvr/files/slam_no_hook", "r");
            if (nf) { fclose(nf); XLOG("GOTHOOK: slam_no_hook 存在,跳过 hook"); }
            else if (gep) {
                Dl_info di;
                memset(&di, 0, sizeof(di));
                if (dladdr(gep, &di) && di.dli_fbase) {
                    uintptr_t got_addr = (uintptr_t)di.dli_fbase + 0x15e288u;
                    long page = sysconf(_SC_PAGESIZE);
                    uintptr_t pg = (got_addr / page) * page;
                    uint32_t old = *(uint32_t *)got_addr;
                    int pr = mprotect((void *)pg, page, PROT_READ | PROT_WRITE | PROT_EXEC);
                    XLOG("GOTHOOK: so_base=%p got=%p old=0x%x mprotect=%d",
                         di.dli_fbase, (void *)got_addr, old, pr);
                    *(uint32_t *)got_addr = (uint32_t)(uintptr_t)hook_get_ext_path;
                    __builtin___clear_cache((char *)got_addr, (char *)got_addr + 4);
                    XLOG("GOTHOOK: 写入 hook -> 0x%x (now=0x%x)",
                         (uint32_t)(uintptr_t)hook_get_ext_path, *(uint32_t *)got_addr);
                } else {
                    XWARN("GOTHOOK: dladdr 失败,回退扫描");
                    int n = patch_got_for(gep, (void *)hook_get_ext_path, "libruntime_service_module.so");
                    XLOG("GOTHOOK: 扫描 patch %d 个", n);
                }
            }
        }
    }

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
    /* 【2026-11-21 订正】hvrInit 内部已通过 JNI 初始化过 module manager，
     * 这里再调 initModuleManagerNative 会重复初始化导致线程卡死（探针曾在此
     * 静默挂住，SetValue/Init/Start 全部未执行）。因此默认跳过；只有设置
     * files/slam_force_mm 时才强制再调一次。 */
    {
        int force_mm = 0;
        FILE *f = fopen("/sdcard/Android/data/com.lht.huaweivr.alvr/files/slam_force_mm", "r");
        if (f) { force_mm = 1; fclose(f); }
        if (force_mm && env && activity) {
            typedef jboolean (*fn_initmm_t)(JNIEnv *, jobject, jobject);
            fn_initmm_t initmm = (fn_initmm_t)dlsym(
                    h_rsm,
                    "Java_com_huawei_hvrsdkserverapp_module_ArvrRuntimeModuleManager_"
                    "initModuleManagerNative");
            XLOG("initModuleManagerNative 符号 = %p (FORCE)", (void *)initmm);
            if (initmm) {
                jboolean ok = initmm(env, NULL, activity);
                XLOG("initModuleManagerNative(activity) -> %d", (int)ok);
            }
        } else {
            XLOG("跳过 initModuleManagerNative（hvrInit 已初始化）");
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

    /* --- 5.5 [2026-11-22] dump libvrsdk.so 关键全局, 定位 ControllerManager --- */
    {
        void *hv = dlopen("libvrsdk.so", RTLD_NOW);
        Dl_info di;
        if (hv && dladdr(dlsym(hv, "hvrInit"), &di) && di.dli_fbase) {
            uintptr_t base = (uintptr_t)di.dli_fbase;
            /* hvrControllerGetValidIndices@0x44534 检查的全局 VA=0x11f634
             * hvrInit 里 new 对象存到 VA=0x11f650
             * 初始化标志 VA=0x11f620 */
            uint32_t cm   = *(uint32_t *)(base + 0x11f634);
            uint32_t obj  = *(uint32_t *)(base + 0x11f650);
            uint8_t  flag = *(uint8_t  *)(base + 0x11f620);
            XLOG("GLOBALS: so_base=%p CM[0x11f634]=0x%x obj[0x11f650]=0x%x initflag[0x11f620]=%u",
                 (void *)base, cm, obj, flag);
        } else {
            XWARN("GLOBALS: libvrsdk.so dladdr 失败 hv=%p", hv);
        }
    }

    /* --- 6. [2026-11-22] 攻第二道门: ControllerManager not start! ---
     * hvrControllerGetValidIndices 内部检查一个全局 ControllerManager 指针,
     * null 就打 "ControllerManager not start!" 返回 -1。hvrController* 全是
     * getter/振动, 没有 Init/Start —— ControllerManager 只能由 hvrTrackingStart
     * 间接启动。当前桥接只调了 hvrInit, 从没调 hvrTrackingStart。
     * 这里实验: 调 hvrTrackingStart, 再查手柄索引是否恢复。 */
    {
        void *h_api2 = dlopen("libHvrApi.so", RTLD_NOW);
        if (h_api2) {
            typedef int (*fn_trackstart_t)(void);
            typedef int (*fn_getvalid_t)(int *out, int max);
            fn_trackstart_t tstart = (fn_trackstart_t)dlsym(h_api2, "hvrTrackingStart");
            fn_getvalid_t gv = (fn_getvalid_t)dlsym(h_api2, "hvrControllerGetValidIndices");
            XLOG("TRACK: hvrTrackingStart=%p GetValidIndices=%p", (void *)tstart, (void *)gv);

            /* before: 手柄索引 */
            if (gv) {
                int idx[8] = {0};
                int n0 = gv(idx, 8);
                XLOG("TRACK: before GetValidIndices -> %d idx=[%d,%d]", n0, idx[0], idx[1]);
            }
            if (tstart) {
                int trc = tstart();
                XLOG("TRACK: hvrTrackingStart() -> %d", trc);
                /* 等 ControllerManager 扫描设备 */
                usleep(1500000);
                if (gv) {
                    int idx[8] = {0};
                    int n1 = gv(idx, 8);
                    XLOG("TRACK: after GetValidIndices -> %d idx=[%d,%d]", n1, idx[0], idx[1]);
                }
            }
        } else {
            XWARN("TRACK: dlopen libHvrApi.so 失败");
        }
    }

    /* --- 7. [2026-11-22] FEED 实验: 把 hidraw3 帧喂给 libvrpose 姿态引擎 ---
     * PhoneControllerPose::onDataReceived(this, path, data, len) @0xe4e0
     * Singleton<PhoneControllerPose>::instance_ @0x25060 (运行时为实例指针)
     * PhoneControllerPoseGet @0xe924 (C API, 输出 posture)
     * 若喂 hidraw3 帧后姿态变化, 说明 libvrpose 能解析 OV580 帧 → A 方案可行。 */
    {
        void *hvp = dlopen("libvrpose.so", RTLD_NOW);
        Dl_info di;
        void *getfn = hvp ? dlsym(hvp, "PhoneControllerPoseGet") : NULL;
        if (hvp && getfn && dladdr(getfn, &di) && di.dli_fbase) {
            uintptr_t base = (uintptr_t)di.dli_fbase;
            void **inst = (void **)(base + 0x25060);   /* Singleton instance_ */
            void *self = *inst;
            typedef void (*fn_on_data_t)(void *, const char *, void *, unsigned int);
            typedef int  (*fn_get_t)(void *);
            fn_on_data_t ondata = (fn_on_data_t)(base + 0xe4e0); /* ARM 代码, 无 thumb bit */
            XLOG("FEED: hvp=%p base=%p self=%p onData=%p get=%p",
                 (void *)hvp, (void *)base, self, (void *)ondata, getfn);

            float before[8] = {0};
            ((fn_get_t)getfn)(before);
            XLOG("FEED: before q=(%.4f %.4f %.4f %.4f) pos=(%.4f %.4f %.4f)",
                 before[0],before[1],before[2],before[3],before[4],before[5],before[6]);

            int fd = open("/dev/hidraw3", O_RDONLY | O_NONBLOCK);
            XLOG("FEED: open hidraw3 fd=%d", fd);
            if (fd >= 0) {
                unsigned char buf[256];
                int fed = 0;
                for (int i = 0; i < 200; i++) {
                    ssize_t n = read(fd, buf, sizeof(buf));
                    if (n > 0) { ondata(self, "/dev/hidraw3", buf, (unsigned int)n); fed++; }
                    usleep(1000);
                }
                close(fd);
                float after[8] = {0};
                ((fn_get_t)getfn)(after);
                XLOG("FEED: fed=%d frames after q=(%.4f %.4f %.4f %.4f) pos=(%.4f %.4f %.4f)",
                     fed, after[0],after[1],after[2],after[3],after[4],after[5],after[6]);
                int changed = 0;
                for (int i = 0; i < 7; i++) if (before[i] != after[i]) changed++;
                XLOG("FEED: changed_fields=%d/7", changed);
            }
        } else {
            XWARN("FEED: libvrpose 不可用 hvp=%p get=%p", hvp, getfn);
        }
    }

    XLOG("======== SLAM 探针结束 ========");
}
