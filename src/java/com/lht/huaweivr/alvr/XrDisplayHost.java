package com.lht.huaweivr.alvr;

import android.app.Activity;
import android.app.Dialog;
import android.app.Presentation;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.view.WindowManager;

import java.io.File;

/**
 * 零华为依赖的眼镜显示宿主。
 *
 * <p>把 HUAWEI VR Glass 当成普通 HDMI 副屏：用 DisplayManager 找到它，
 * 在上面挂一个 Presentation + SurfaceView，把 Surface 交给
 * libxr_loader.so（我们自写的极简 OpenXR 运行时）去合成左右眼画面。</p>
 *
 * <p>不需要 com.huawei.android.permission.VR，也不需要 VR SDK Service。</p>
 */
public final class XrDisplayHost {

    private static final String TAG = "XrDisplayHost";

    private static native void nativeSetSurface(Surface surface);
    private static native int nativeProbe();
    private static native String nativePoseDriver();
    private static native void nativeRecenter();
    private static native void nativeSetPoseMode(String mode);
    /** 在普通进程里尝试加载并初始化原厂 6DoF SLAM 库（只打日志，不参与渲染）。 */
    private static native void nativeSlamProbe(Activity activity);
    /** 直接读 /dev/hwvr* 的原始 HID 报文（不依赖任何 Android 权限）。 */
    private static native void nativeHidProbe();
    /** 调华为自己的 PhoneControllerPose 单例拿手柄姿态。 */
    private static native void nativeCtrlProbe();

    static {
        // 原厂 SLAM 库的全局 JNI 环境是在 JNI_OnLoad 里初始化的，而 dlopen 不会
        // 触发 JNI_OnLoad，所以必须先 System.loadLibrary 把它们加载进来，
        // 否则 SlamState::Init 里会一路 "jni is null"、标定路径退化成根目录。
        String[] preload = {"vrsdk", "runtime_service_module"};
        for (int i = 0; i < preload.length; i++) {
            String name = preload[i];
            try {
                System.loadLibrary(name);
                Log.i(TAG, "lib" + name + ".so 已加载（JNI_OnLoad 已执行）");
            } catch (Throwable t) {
                Log.w(TAG, "lib" + name + ".so 加载异常（库可能已映射）", t);
            }
        }
        try {
            System.loadLibrary("xr_loader");
            Log.i(TAG, "libxr_loader.so 已加载");
        } catch (Throwable t) {
            Log.e(TAG, "libxr_loader.so 加载失败", t);
        }
    }

    private static boolean sStarted;
    private static Activity sActivity;
    private static DisplayManager sDisplayManager;
    private static GlassPresentation sPresentation;
    private static GlassDialog sDialog;
    /** 当前承载 Surface 的屏幕 id，-1 表示还没挂窗口。 */
    private static int sSurfaceDisplayId = -1;

    private static final DisplayManager.DisplayListener DISPLAY_LISTENER =
            new DisplayManager.DisplayListener() {
                @Override
                public void onDisplayAdded(int displayId) {
                    Log.i(TAG, "屏幕接入 id=" + displayId);
                    attachToGlass("display-added");
                }

                @Override
                public void onDisplayRemoved(int displayId) {
                    Log.i(TAG, "屏幕移除 id=" + displayId);
                    if (sPresentation != null
                            && sPresentation.getDisplay() != null
                            && sPresentation.getDisplay().getDisplayId() == displayId) {
                        dismissPresentation();
                    }
                }

                @Override
                public void onDisplayChanged(int displayId) {
                    // 忽略
                }
            };

    private XrDisplayHost() {
    }

    /** 由 VR Activity 在 onCreate 里调用。 */
    public static void start(Activity activity) {
        if (activity == null) {
            return;
        }
        sActivity = activity;
        if (sStarted) {
            return;
        }
        sStarted = true;
        int mask = 0;
        try {
            mask = nativeProbe();
            Log.i(TAG, "姿态驱动探测 mask=0x" + Integer.toHexString(mask)
                    + " (3dof=" + ((mask & 1) != 0)
                    + " 6dof=" + ((mask & 2) != 0)
                    + " hid=" + ((mask & 4) != 0) + ")");
        } catch (Throwable t) {
            Log.w(TAG, "姿态探测失败", t);
        }
        sDisplayManager = (DisplayManager) activity.getSystemService(Context.DISPLAY_SERVICE);
        if (sDisplayManager == null) {
            Log.e(TAG, "没有 DisplayManager");
            return;
        }
        sDisplayManager.registerDisplayListener(DISPLAY_LISTENER, null);
        logDisplays();
        attachToGlass("start");
        startSlamProbeWatcher(activity, mask);
    }

    /**
     * SLAM 探针触发：监听 files/slam_probe 触发文件。
     *
     * <p>注意：<b>不再自动跑一次</b>。原厂 libruntime_service_module.so 的
     * initModuleManagerNative 在非华为设备（没有 HwVrService 的普通盒子）上会
     * 卡在 ART 的类初始化锁里，主线程随后调 Activity.finish() 抢同一把锁 →
     * 直接 ANR 被杀。所以只有在探到真 6DoF（mask&2）时才自动跑一次，
     * 其它设备只能靠触发文件手动跑。</p>
     */
    private static void startSlamProbeWatcher(final Activity activity, final int poseMask) {
        final File extDir = activity.getExternalFilesDir(null);
        final File intDir = activity.getFilesDir();
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Thread.sleep(8000);
                } catch (InterruptedException ignored) {
                    return;
                }
                boolean autoDone = (poseMask & 2) == 0;
                Log.i(TAG, "SLAM 探针监听已启动，自动探测=" + (!autoDone));
                while (sStarted) {
                    boolean hit = false;
                    if (!autoDone) {
                        autoDone = true;
                        hit = true;
                    }
                    File a = extDir == null ? null : new File(extDir, "slam_probe");
                    File b = intDir == null ? null : new File(intDir, "slam_probe");
                    if (a != null && a.exists()) {
                        a.delete();
                        hit = true;
                    }
                    if (b != null && b.exists()) {
                        b.delete();
                        hit = true;
                    }
                    if (hit) {
                        Log.i(TAG, "触发 SLAM 探针");
                        try {
                            nativeSlamProbe(activity);
                        } catch (Throwable t2) {
                            Log.w(TAG, "SLAM 探针异常", t2);
                        }
                    }
                    // files/usb_probe -> 跑一次 USB 地面真相探测
                    File ua = extDir == null ? null : new File(extDir, "usb_probe");
                    File ub = intDir == null ? null : new File(intDir, "usb_probe");
                    boolean usbHit = (ua != null && ua.exists()) || (ub != null && ub.exists());
                    if (usbHit) {
                        if (ua != null && ua.exists()) {
                            ua.delete();
                        }
                        if (ub != null && ub.exists()) {
                            ub.delete();
                        }
                        Log.i(TAG, "触发 USB 探针");
                        try {
                            UsbProbe.trigger(activity);
                        } catch (Throwable t3) {
                            Log.w(TAG, "USB 探针异常", t3);
                        }
                    }
                    // files/ctrl_probe -> 走 PhoneControllerPose 单例拿手柄姿态
                    File ca = extDir == null ? null : new File(extDir, "ctrl_probe");
                    File cb = intDir == null ? null : new File(intDir, "ctrl_probe");
                    if ((ca != null && ca.exists()) || (cb != null && cb.exists())) {
                        if (ca != null && ca.exists()) {
                            ca.delete();
                        }
                        if (cb != null && cb.exists()) {
                            cb.delete();
                        }
                        Log.i(TAG, "触发手柄姿态探针");
                        try {
                            nativeCtrlProbe();
                        } catch (Throwable t5) {
                            Log.w(TAG, "手柄姿态探针异常", t5);
                        }
                    }
                    // files/hid_probe -> 直接读 /dev/hwvr* 原始报文（无需权限）
                    File ha = extDir == null ? null : new File(extDir, "hid_probe");
                    File hb = intDir == null ? null : new File(intDir, "hid_probe");
                    if ((ha != null && ha.exists()) || (hb != null && hb.exists())) {
                        if (ha != null && ha.exists()) {
                            ha.delete();
                        }
                        if (hb != null && hb.exists()) {
                            hb.delete();
                        }
                        Log.i(TAG, "触发 HID 探针");
                        try {
                            nativeHidProbe();
                        } catch (Throwable t4) {
                            Log.w(TAG, "HID 探针异常", t4);
                        }
                    }
                    try {
                        Thread.sleep(1000);
                    } catch (InterruptedException ignored) {
                        return;
                    }
                }
            }
        }, "slam-probe-watch");
        t.setDaemon(true);
        t.start();
    }

    public static void stop() {
        if (sDisplayManager != null) {
            try {
                sDisplayManager.unregisterDisplayListener(DISPLAY_LISTENER);
            } catch (Throwable ignored) {
            }
        }
        dismissPresentation();
        sStarted = false;
        sActivity = null;
    }

    public static String poseDriver() {
        try {
            return nativePoseDriver();
        } catch (Throwable t) {
            return "未知";
        }
    }

    public static void recenter() {
        try {
            nativeRecenter();
        } catch (Throwable ignored) {
        }
    }

    public static void setPoseMode(String mode) {
        try {
            nativeSetPoseMode(mode);
        } catch (Throwable ignored) {
        }
    }

    private static void logDisplays() {
        try {
            Display[] all = sDisplayManager.getDisplays();
            Log.i(TAG, "共 " + all.length + " 块屏幕");
            for (Display d : all) {
                Log.i(TAG, "  #" + d.getDisplayId() + " " + d.getName()
                        + " " + d.getWidth() + "x" + d.getHeight()
                        + " state=" + d.getState()
                        + " flags=0x" + Integer.toHexString(d.getFlags()));
            }
        } catch (Throwable t) {
            Log.w(TAG, "枚举屏幕失败", t);
        }
    }

    private static Display findGlassDisplay() {
        if (sDisplayManager == null) {
            return null;
        }
        Display[] all;
        try {
            all = sDisplayManager.getDisplays();
        } catch (Throwable t) {
            return null;
        }
        // 1) 名字命中
        for (Display d : all) {
            if (d == null || d.getDisplayId() == Display.DEFAULT_DISPLAY) {
                continue;
            }
            String name = d.getName() == null ? "" : d.getName().toUpperCase();
            if (name.contains("VR") || name.contains("GLASS") || name.contains("HUAWEI")) {
                return d;
            }
        }
        // 2) FLAG_PRESENTATION 且不是虚拟小屏
        for (Display d : all) {
            if (d == null || d.getDisplayId() == Display.DEFAULT_DISPLAY) {
                continue;
            }
            if (d.getFlags() == 0) {
                continue;
            }
            if ((d.getFlags() & Display.FLAG_PRESENTATION) != 0 && d.getWidth() >= 1280) {
                return d;
            }
        }
        // 3) 够大的非默认屏
        for (Display d : all) {
            if (d == null || d.getDisplayId() == Display.DEFAULT_DISPLAY) {
                continue;
            }
            if (d.getWidth() >= 1280 && d.getHeight() >= 720) {
                return d;
            }
        }
        // 4) 兜底：压根没有副屏 —— 眼镜直接就是主屏（HDMI 接盒子/PC 显卡的场景）。
        //    这时就在主屏上挂 Presentation，效果等价于「全屏 SBS 合成器」。
        Display def = sDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
        if (def != null) {
            Log.w(TAG, "没有副屏，回退用主屏 #" + def.getDisplayId()
                    + " " + def.getWidth() + "x" + def.getHeight());
        }
        return def;
    }

    private static void attachToGlass(String why) {
        if (sActivity == null || sActivity.isFinishing()) {
            return;
        }
        final Display glass = findGlassDisplay();
        if (glass == null) {
            Log.w(TAG, "[" + why + "] 没找到可用屏幕，等它出现");
            return;
        }
        // 已经在目标屏上就不重复挂；换了屏（比如眼镜刚插上）就换过去
        if (sSurfaceDisplayId == glass.getDisplayId() && (sPresentation != null || sDialog != null)) {
            return;
        }
        if (sPresentation != null || sDialog != null) {
            Log.i(TAG, "[" + why + "] 目标屏变化，重建窗口");
            dismissPresentation();
        }
        Log.i(TAG, "[" + why + "] 使用屏幕 #" + glass.getDisplayId() + " " + glass.getName()
                + " " + glass.getWidth() + "x" + glass.getHeight());
        sActivity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                boolean isDefault = glass.getDisplayId() == Display.DEFAULT_DISPLAY;
                try {
                    if (isDefault) {
                        // 主屏不能用 Presentation：它的窗口类型 TYPE_PRESENTATION
                        // 只有副屏才被 WindowManager 接受，主屏会抛 InvalidDisplayException。
                        GlassDialog d = new GlassDialog(sActivity);
                        d.show();
                        sDialog = d;
                    } else {
                        GlassPresentation p = new GlassPresentation(sActivity, glass);
                        p.show();
                        sPresentation = p;
                    }
                    sSurfaceDisplayId = glass.getDisplayId();
                } catch (Throwable t) {
                    Log.e(TAG, "窗口创建失败 (display=" + glass.getDisplayId() + ")", t);
                }
            }
        });
    }

    private static void dismissPresentation() {
        final GlassPresentation p = sPresentation;
        final GlassDialog d = sDialog;
        sPresentation = null;
        sDialog = null;
        sSurfaceDisplayId = -1;
        if (p != null) {
            try {
                p.dismiss();
            } catch (Throwable ignored) {
            }
        }
        if (d != null) {
            try {
                d.dismiss();
            } catch (Throwable ignored) {
            }
        }
    }

    /** 主屏兜底窗口：普通 Dialog（TYPE_APPLICATION），眼镜直接当主屏时用它。 */
    private static final class GlassDialog extends Dialog
            implements SurfaceHolder.Callback {

        private SurfaceView surfaceView;

        GlassDialog(Context context) {
            super(context, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        }

        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.width = WindowManager.LayoutParams.MATCH_PARENT;
            lp.height = WindowManager.LayoutParams.MATCH_PARENT;
            lp.flags |= WindowManager.LayoutParams.FLAG_FULLSCREEN
                    | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                    | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON;
            getWindow().setAttributes(lp);
            surfaceView = new SurfaceView(getContext());
            surfaceView.getHolder().addCallback(this);
            setContentView(surfaceView, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
            Log.i(TAG, "主屏 Dialog 已创建");
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            Surface s = holder.getSurface();
            Log.i(TAG, "主屏 Surface 就绪 valid=" + (s != null && s.isValid()));
            if (s != null) {
                nativeSetSurface(s);
            }
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            Log.i(TAG, "主屏 Surface 尺寸 " + width + "x" + height);
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            Log.w(TAG, "主屏 Surface 销毁");
            try {
                nativeSetSurface(null);
            } catch (Throwable ignored) {
            }
        }
    }

    /** 眼镜屏上的呈现窗口。 */
    private static final class GlassPresentation extends Presentation
            implements SurfaceHolder.Callback {

        private SurfaceView surfaceView;

        GlassPresentation(Context context, Display display) {
            super(context, display, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        }

        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.width = WindowManager.LayoutParams.MATCH_PARENT;
            lp.height = WindowManager.LayoutParams.MATCH_PARENT;
            lp.flags |= WindowManager.LayoutParams.FLAG_FULLSCREEN
                    | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
            getWindow().setAttributes(lp);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

            surfaceView = new SurfaceView(getContext());
            surfaceView.getHolder().addCallback(this);
            setContentView(surfaceView, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
            Log.i(TAG, "眼镜 Presentation 已创建");
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            Surface s = holder.getSurface();
            Log.i(TAG, "眼镜 Surface 就绪 valid=" + (s != null && s.isValid()));
            if (s != null) {
                nativeSetSurface(s);
            }
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            Log.i(TAG, "眼镜 Surface 尺寸 " + width + "x" + height);
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            Log.w(TAG, "眼镜 Surface 销毁");
            try {
                nativeSetSurface(null);
            } catch (Throwable ignored) {
            }
        }
    }
}
