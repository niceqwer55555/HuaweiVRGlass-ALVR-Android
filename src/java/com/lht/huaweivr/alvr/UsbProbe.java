package com.lht.huaweivr.alvr;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.util.Log;

import java.util.HashMap;
import java.util.Map;

/**
 * USB 地面真相探针。
 *
 * <p>目的：搞清楚华为 VR 的三个 USB 复合设备各自承载什么数据流。
 * 已知拓扑（dumpsys usb）：
 * <pre>
 *   12d1:4242  HUAWEI GLASS CV10       Audio x3 + HID(class3, interrupt IN 4B)
 *   12d1:3A09  Omnivision 6DoF Kit     仅 class14/1 VideoControl，零端点
 *   12d1:4222  HUAWEI GLASS HID        3x vendor(255/19) bulk512 + 2x HID(class3)
 * </pre>
 *
 * <p>做法：不猜测，直接开设备读原始字节。先 dump 描述符（不需要权限），
 * 再对 4222 / 3A09 申请 USB 权限后逐接口 claim + 读 IN 端点，把收到的报文
 * 以 hex 打到 logcat。有没有数据、什么节奏、什么长度，一看便知。</p>
 *
 * <p>注意：<b>不要</b>去 claim 4242 的 HID 接口——那是 HelmetPose 3DoF 的
 * /dev/hwvr1 来源，强制 claim 会把它从 usbhid 上摘掉，头显姿态会断。</p>
 */
public final class UsbProbe {

    private static final String TAG = "UsbProbe";
    private static final String ACTION_PERM =
            "com.lht.huaweivr.alvr.USB_PERMISSION";

    /** 华为 VID。 */
    private static final int VID_HUAWEI = 0x12D1;
    /**
     * 允许探测的 PID（注意这里是 16 进制常量，dumpsys 里打的是 10 进制）：
     *   0x107E = 4222  HUAWEI GLASS HID
     *   0x3A09 = 14857 6DoF Gaming Kit Par-CG012
     * 排除 0x1092(=4242 HUAWEI GLASS CV10)：它的 HID 接口被 usbhid 占着，
     * 就是 HelmetPose 3DoF 的 /dev/hwvr1 来源，强 claim 会把头显姿态搞断。
     */
    private static final int[] PIDS = {0x107E, 0x3A09};

    private static boolean sRunning;
    private static Activity sActivity;
    private static final Map<Integer, UsbDevice> sPending = new HashMap<Integer, UsbDevice>();
    private static BroadcastReceiver sReceiver;

    private UsbProbe() {
    }

    /** 触发一次完整探测：先 dump 描述符，再对目标设备申请权限后读数据。 */
    public static void trigger(final Activity activity) {
        if (activity == null) {
            return;
        }
        // 注意：EMUI 上 registerReceiver / requestPermission 有可能长时间阻塞，
        // 所以整体丢到独立线程里，别把调用方（触发文件监视线程）卡死。
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                doTrigger(activity);
            }
        }, "usb-probe");
        t.setDaemon(true);
        t.start();
    }

    private static void doTrigger(Activity activity) {
        if (activity == null) {
            return;
        }
        synchronized (UsbProbe.class) {
            if (sRunning) {
                Log.w(TAG, "探针已在运行，忽略本次触发");
                return;
            }
            sRunning = true;
        }
        sActivity = activity;
        try {
            UsbManager um = (UsbManager) activity.getSystemService(Context.USB_SERVICE);
            if (um == null) {
                Log.e(TAG, "没有 UsbManager");
                sRunning = false;
                return;
            }
            dumpAll(um);

            // 已授权的直接开读，未授权的走一次系统授权对话框
            boolean needDialog = false;
            boolean anyStarted = false;
            for (UsbDevice d : um.getDeviceList().values()) {
                if (!isTarget(d)) {
                    continue;
                }
                if (um.hasPermission(d)) {
                    startRead(um, d);
                    anyStarted = true;
                } else {
                    needDialog = true;
                    Log.i(TAG, "未授权: " + describe(d));
                }
            }
            if (!needDialog) {
                Log.i(TAG, "全部目标设备已授权，直接开始读");
                if (!anyStarted) {
                    synchronized (UsbProbe.class) {
                        sRunning = false;
                    }
                }
                return;
            }
            // 【已禁用】EMUI 上 registerReceiver()+requestPermission 会永久阻塞，
            // 触发线程直接挂死（实测第二次触发报"探针已在运行"）。这里不再走授权，
            // 只提示结论：这两台设备的 vendor 接口在没有 root / 系统签名时打不开。
            Log.w(TAG, "跳过 USB 授权：EMUI 上 requestPermission 会阻塞，"
                    + "未授权设备无法通过 UsbManager 打开（需要 root 或系统签名）");
            synchronized (UsbProbe.class) {
                sRunning = false;
            }
        } catch (Throwable t) {
            Log.e(TAG, "探针异常", t);
            sRunning = false;
        }
    }

    private static boolean isTarget(UsbDevice d) {
        if (d == null || d.getVendorId() != VID_HUAWEI) {
            return false;
        }
        int pid = d.getProductId();
        for (int p : PIDS) {
            if (p == pid) {
                return true;
            }
        }
        return false;
    }

    private static String describe(UsbDevice d) {
        return String.format("VID=%04X PID=%04X name=%s cls=%d/%d/%d ifCount=%d cfgCount=%d",
                d.getVendorId(), d.getProductId(), d.getProductName(),
                d.getDeviceClass(), d.getDeviceSubclass(), d.getDeviceProtocol(),
                d.getInterfaceCount(), d.getConfigurationCount());
    }

    /** 把 UsbManager 看到的完整拓扑打出来（这一步不需要权限）。 */
    private static void dumpAll(UsbManager um) {
        Log.i(TAG, "======== USB 拓扑 ========");
        for (UsbDevice d : um.getDeviceList().values()) {
            Log.i(TAG, describe(d));
            for (int ci = 0; ci < d.getConfigurationCount(); ci++) {
                Log.i(TAG, "  cfg[" + ci + "] id=" + d.getConfiguration(ci).getId()
                        + " ifCount=" + d.getConfiguration(ci).getInterfaceCount());
            }
            int ifCount = d.getInterfaceCount();
            for (int i = 0; i < ifCount; i++) {
                UsbInterface ui = d.getInterface(i);
                StringBuilder sb = new StringBuilder();
                sb.append("  if[").append(ui.getId()).append("] alt=").append(ui.getAlternateSetting())
                        .append(" cls=").append(ui.getInterfaceClass())
                        .append('/').append(ui.getInterfaceSubclass())
                        .append('/').append(ui.getInterfaceProtocol())
                        .append(" name=").append(ui.getName())
                        .append(" eps=").append(ui.getEndpointCount());
                Log.i(TAG, sb.toString());
                for (int e = 0; e < ui.getEndpointCount(); e++) {
                    UsbEndpoint ep = ui.getEndpoint(e);
                    Log.i(TAG, "    ep addr=0x" + Integer.toHexString(ep.getAddress())
                            + " dir=" + (ep.getDirection() == UsbConstants.USB_DIR_IN ? "IN" : "OUT")
                            + " type=" + epTypeName(ep.getType())
                            + " maxPkt=" + ep.getMaxPacketSize()
                            + " interval=" + ep.getInterval());
                }
            }
        }
        Log.i(TAG, "======== 拓扑结束 ========");
    }

    private static String epTypeName(int type) {
        switch (type) {
            case UsbConstants.USB_ENDPOINT_XFER_CONTROL:
                return "CONTROL";
            case UsbConstants.USB_ENDPOINT_XFER_ISOC:
                return "ISOC";
            case UsbConstants.USB_ENDPOINT_XFER_BULK:
                return "BULK";
            case UsbConstants.USB_ENDPOINT_XFER_INT:
                return "INT";
            default:
                return "?" + type;
        }
    }

    private static void registerReceiver() {
        if (sReceiver != null) {
            return;
        }
        sReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                if (!ACTION_PERM.equals(intent.getAction())) {
                    return;
                }
                int code = intent.getIntExtra("code", -1);
                // 兼容不同实现：先看 extra，再按请求码反查
                UsbDevice dev = sPending.remove(code);
                if (dev == null) {
                    for (Map.Entry<Integer, UsbDevice> en : sPending.entrySet()) {
                        dev = en.getValue();
                        break;
                    }
                }
                boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                Log.i(TAG, "权限回调 granted=" + granted + " dev=" + (dev == null ? "null" : describe(dev)));
                if (granted && dev != null) {
                    UsbManager um = (UsbManager) context.getSystemService(Context.USB_SERVICE);
                    if (um != null) {
                        startRead(um, dev);
                    }
                }
            }
        };
        IntentFilter f = new IntentFilter(ACTION_PERM);
        sActivity.registerReceiver(sReceiver, f);
    }

    /** 对指定设备开线程，逐接口读 IN 端点。 */
    private static void startRead(final UsbManager um, final UsbDevice dev) {
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                readDevice(um, dev);
            }
        }, "usb-read-" + Integer.toHexString(dev.getProductId()));
        t.setDaemon(true);
        t.start();
    }

    private static void readDevice(UsbManager um, UsbDevice dev) {
        String tag = String.format("PID=%04X", dev.getProductId());
        UsbDeviceConnection conn = null;
        try {
            conn = um.openDevice(dev);
            if (conn == null) {
                Log.e(TAG, tag + " openDevice 返回 null");
                return;
            }
            Log.i(TAG, tag + " 已打开，fd=" + conn.getFileDescriptor());

            int ifCount = dev.getInterfaceCount();
            for (int i = 0; i < ifCount; i++) {
                UsbInterface ui = dev.getInterface(i);
                // HID 接口先读报告描述符：能直接看出是键盘/摇杆/厂商自定义数据
                if (ui.getInterfaceClass() == UsbConstants.USB_CLASS_HID) {
                    dumpHidReportDescriptor(tag, ui, conn);
                }
                // 只挑带 IN 端点的接口
                UsbEndpoint in = null;
                for (int e = 0; e < ui.getEndpointCount(); e++) {
                    UsbEndpoint ep = ui.getEndpoint(e);
                    if (ep.getDirection() == UsbConstants.USB_DIR_IN) {
                        in = ep;
                        break;
                    }
                }
                if (in == null) {
                    Log.i(TAG, tag + " if[" + ui.getId() + "] 无 IN 端点，跳过");
                    continue;
                }
                boolean claimed = conn.claimInterface(ui, true);
                Log.i(TAG, tag + " if[" + ui.getId() + "] claim=" + claimed
                        + " ep=0x" + Integer.toHexString(in.getAddress())
                        + " type=" + epTypeName(in.getType()));
                if (!claimed) {
                    continue;
                }
                drain(tag, ui, conn, in);
                conn.releaseInterface(ui);
                Log.i(TAG, tag + " if[" + ui.getId() + "] released");
            }
        } catch (Throwable t) {
            Log.e(TAG, tag + " 读取异常", t);
        } finally {
            if (conn != null) {
                conn.close();
            }
            Log.i(TAG, tag + " 探测线程结束");
            synchronized (UsbProbe.class) {
                sRunning = false;
            }
        }
    }

    /** 读 HID 报告描述符（GET_DESCRIPTOR type=0x22），不需要 claim 接口。 */
    private static void dumpHidReportDescriptor(String tag, UsbInterface ui, UsbDeviceConnection conn) {
        byte[] buf = new byte[256];
        // 0x81 = device-to-host | standard | interface
        // 0x06 = GET_DESCRIPTOR, 0x2200 = HID Report, wIndex = 接口号
        int n = conn.controlTransfer(0x81, 0x06, 0x2200, ui.getId(), buf, buf.length, 500);
        if (n > 0) {
            Log.i(TAG, tag + " if[" + ui.getId() + "] HID report desc len=" + n + " " + hex(buf, n));
        } else {
            Log.i(TAG, tag + " if[" + ui.getId() + "] HID report desc 读取失败 n=" + n);
        }
    }

    /** 读一个端点 4 秒，最多打印 40 个报文。 */
    private static void drain(String tag, UsbInterface ui, UsbDeviceConnection conn, UsbEndpoint ep) {
        byte[] buf = new byte[Math.max(64, ep.getMaxPacketSize())];
        long end = System.currentTimeMillis() + 4000;
        int ok = 0;
        int timeouts = 0;
        while (System.currentTimeMillis() < end && ok < 40) {
            int n = conn.bulkTransfer(ep, buf, buf.length, 250);
            if (n > 0) {
                ok++;
                Log.i(TAG, tag + " if[" + ui.getId() + "] #" + ok + " len=" + n
                        + " " + hex(buf, n));
            } else {
                timeouts++;
            }
        }
        Log.i(TAG, tag + " if[" + ui.getId() + "] 结束: 收到 " + ok
                + " 包, 超时 " + timeouts + " 次");
    }

    private static String hex(byte[] b, int n) {
        StringBuilder sb = new StringBuilder(n * 2);
        for (int i = 0; i < n; i++) {
            int v = b[i] & 0xFF;
            if (v < 16) {
                sb.append('0');
            }
            sb.append(Integer.toHexString(v));
            if (i != n - 1) {
                sb.append(' ');
            }
        }
        return sb.toString();
    }
}
