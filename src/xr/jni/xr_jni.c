/*
 * xr_jni.c - Java 侧桥梁
 *
 * Java 类 com.lht.huaweivr.alvr.XrDisplayHost 通过 System.loadLibrary("xr_loader")
 * 显式加载本库（JNI_OnLoad 会被调用），把眼镜副屏的 Surface 交进来，
 * 并提供姿态驱动探测/切换。
 */
#include "xr_shim.h"

#include <jni.h>
#include <android/native_window_jni.h>

static JavaVM *g_vm;

static ANativeWindow *surface_to_window(JNIEnv *env, jobject surface) {
    if (!surface) return NULL;
    /* 标准途径：android/native_window_jni.h */
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    if (!win) {
        XERR("JNI: ANativeWindow_fromSurface 失败");
    }
    return win;
}

static void jni_set_surface(JNIEnv *env, jclass cls, jobject surface) {
    (void)cls;
    ANativeWindow *win = surface_to_window(env, surface);
    if (!win) {
        XERR("JNI: 拿不到 ANativeWindow");
        return;
    }
    /* 我们自己持有一份引用，避免 Java 侧回收 */
    ANativeWindow_acquire(win);
    xr_display_attach_window(win);
    if (g_xr.app_context) {
        xr_display_init_gl();
    }
}

static jint jni_probe(JNIEnv *env, jclass cls) {
    (void)env;
    (void)cls;
    return (jint)xr_pose_probe();
}

static jstring jni_driver(JNIEnv *env, jclass cls) {
    (void)cls;
    return (*env)->NewStringUTF(env, xr_pose_driver_name());
}

static void jni_recenter(JNIEnv *env, jclass cls) {
    (void)env;
    (void)cls;
    xr_pose_recenter();
}

static void jni_set_mode(JNIEnv *env, jclass cls, jstring mode) {
    (void)cls;
    if (!mode) return;
    const char *c = (*env)->GetStringUTFChars(env, mode, NULL);
    if (c) {
        xr_pose_set_mode(c);
        (*env)->ReleaseStringUTFChars(env, mode, c);
    }
}

/* 6DoF SLAM 探针：可能阻塞（Init 会起内部线程），放到独立线程里跑，
 * 保证 Java 调用立即返回，不卡主流程。
 * 需要 Activity 是为了补齐库内部 GetPackageExternelPath 依赖的 JNI 环境。 */
static jobject g_probe_activity;

static void *slam_probe_thread(void *arg) {
    (void)arg;
    xr_slam_probe_run(g_vm, g_probe_activity);
    return NULL;
}

static void jni_slam_probe(JNIEnv *env, jclass cls, jobject activity) {
    (void)cls;
    if (activity) {
        if (g_probe_activity) (*env)->DeleteGlobalRef(env, g_probe_activity);
        g_probe_activity = (*env)->NewGlobalRef(env, activity);
    }
    pthread_t t;
    if (pthread_create(&t, NULL, slam_probe_thread, NULL) == 0) {
        pthread_detach(t);
    } else {
        XERR("JNI: SLAM 探针线程创建失败");
    }
}

static void *hid_probe_thread(void *arg) {
    (void)arg;
    xr_hid_probe_run();
    return NULL;
}

static void jni_hid_probe(JNIEnv *env, jclass cls) {
    (void)env;
    (void)cls;
    pthread_t t;
    if (pthread_create(&t, NULL, hid_probe_thread, NULL) == 0) {
        pthread_detach(t);
    } else {
        XERR("JNI: HID 探针线程创建失败");
    }
}

static void *ctrl_probe_thread(void *arg) {
    (void)arg;
    xr_ctrl_probe_run();
    return NULL;
}

static void jni_ctrl_probe(JNIEnv *env, jclass cls) {
    (void)env;
    (void)cls;
    pthread_t t;
    if (pthread_create(&t, NULL, ctrl_probe_thread, NULL) == 0) {
        pthread_detach(t);
    } else {
        XERR("JNI: 手柄探针线程创建失败");
    }
}

static const JNINativeMethod k_methods[] = {
        {"nativeSetSurface", "(Landroid/view/Surface;)V", (void *)jni_set_surface},
        {"nativeProbe", "()I", (void *)jni_probe},
        {"nativePoseDriver", "()Ljava/lang/String;", (void *)jni_driver},
        {"nativeRecenter", "()V", (void *)jni_recenter},
        {"nativeSetPoseMode", "(Ljava/lang/String;)V", (void *)jni_set_mode},
        {"nativeSlamProbe", "(Landroid/app/Activity;)V", (void *)jni_slam_probe},
        {"nativeHidProbe", "()V", (void *)jni_hid_probe},
        {"nativeCtrlProbe", "()V", (void *)jni_ctrl_probe},
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        XERR("JNI: GetEnv 失败");
        return JNI_ERR;
    }
    jclass cls = (*env)->FindClass(env, "com/lht/huaweivr/alvr/XrDisplayHost");
    if (!cls) {
        XWARN("JNI: 找不到 XrDisplayHost（可以忽略）");
        (*env)->ExceptionClear(env);
        return JNI_VERSION_1_6;
    }
    if ((*env)->RegisterNatives(env, cls, k_methods,
                                (jint)(sizeof(k_methods) / sizeof(k_methods[0]))) != JNI_OK) {
        XERR("JNI: RegisterNatives 失败");
        return JNI_ERR;
    }
    XLOG("JNI: XrDisplayHost 已注册");
    return JNI_VERSION_1_6;
}

/* 提供一个 C 入口给 Java 之外调用（调试用） */
void xr_jni_set_vm(JavaVM *vm) { g_vm = vm; }
