/*
 * hwvrctl —— 通用安卓上跑华为 VR Glass（CV10 / Halliday）的用户态补全工具。
 *
 * 这个文件替代并扩展了原来的 hwvr-probe：原版只会发一次 HIDIOCGFEATURE(4)
 * 判断谁是控制接口，其余全靠 App 自己。而 libWalleDev 的真实调用链是：
 *
 *   open("/dev/hwvr1") -> GET_FEATURE(vendor/version/optical/offset)
 *   -> SET_FEATURE(report 5 = activate) -> poll/read 收 IMU 与事件
 *
 * 通用安卓上缺的正是后半段：没人发 activate、没人读流、没人把"戴上/低电量"
 * 翻译成系统状态，也没人保证眼镜那 5V 不断。本工具一次补齐：
 *
 *   hwvrctl probe   <dev>              探测是不是控制接口（兼容旧 hwvr-probe）
 *   hwvrctl info    <dev>              读全部厂商/版本/光学/偏移特性报告
 *   hwvrctl activate <dev>             发 SET_FEATURE(report 5)，成功后保持
 *   hwvrctl monitor <dev> [秒]         激活 + 读流 + 按 report id 统计/解码，
 *                                      把"戴上/电量"写进状态文件
 *   hwvrctl power   on|off|status      数据线供电 + 数据角色（host）
 *   hwvrctl dp      status             看 DP 链路：状态/EDID/lane 数
 *   hwvrctl pose    <dev> [hz]         读 IMU 算 3DoF 姿态，写 /data/local/tmp/hwvr_pose
 *
 * 全部走标准 HID，不写死厂商号，不做私有 ABI。所有失败都只降级、不崩。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- HID ioctl */
/* 与 <linux/hidraw.h> 一致：GET 用 _IOC_WRITE|_IOC_READ，SET 同族 opcode 0x06。 */
#define HIDIOC_GFEATURE(len) _IOC(_IOC_WRITE | _IOC_READ, 'H', 0x07, (len))
#define HIDIOC_SFEATURE(len) _IOC(_IOC_WRITE | _IOC_READ, 'H', 0x06, (len))

/* 控制接口上 App 实际用到的报告编号（libWalleDev 符号 + 现场取证） */
#define R_VENDOR     1     /* 特性报告：厂商号（GET，4 字节） */
#define R_IMU        1     /* 输入流里同一个编号是 IMU 帧 */
#define R_VERSION    2
#define R_PROXIMITY  4
#define R_ACTIVATE   5
#define R_DEACTIVATE 6
#define R_BATTERY    8

#define STATE_FILE "/data/local/tmp/hwvr_state"
#define POSE_FILE  "/data/local/tmp/hwvr_pose"
#define POSE_SOCK  "/data/local/tmp/hwvr_pose.sock"

#define PS_USB     "/sys/class/power_supply/usb"
#define PS_MAIN    "/sys/class/power_supply/main"
#define PS_BATTERY "/sys/class/power_supply/battery"

/* 用户设 1.5 A：与 DT 里的 qcom,otg-cl-ua 对齐；不带载时也够眼镜 8 W 以内。 */
#define OTG_BOOST_UA 1500000

static volatile int g_stop;

static void on_sig(int s) { (void)s; g_stop = 1; }

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ------------------------------------------------------------------ sysfs */
static int read_file(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -errno;
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' ')) buf[--r] = 0;
    return (int)r;
}

static int write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -errno;
    ssize_t r = write(fd, val, strlen(val));
    int e = errno;
    close(fd);
    return r < 0 ? -e : 0;
}

static void ps_show(const char *psy, const char *prop) {
    char p[256], v[128];
    snprintf(p, sizeof p, "%s/%s", psy, prop);
    if (read_file(p, v, sizeof v) > 0) printf("  %-28s = %s\n", prop, v);
}

/* --------------------------------------------------------------- 特性报告 */
static int feat_get(int fd, int id, int len, unsigned char *buf) {
    memset(buf, 0, (size_t)len);
    buf[0] = (unsigned char)id;
    int r = ioctl(fd, HIDIOC_GFEATURE(len), buf);
    return r < 0 ? -errno : r;
}

static int feat_set(int fd, int id, int len, const unsigned char *payload) {
    unsigned char buf[64];
    if (len > (int)sizeof buf) return -EINVAL;
    memset(buf, 0, sizeof buf);
    buf[0] = (unsigned char)id;
    if (payload) memcpy(buf, payload, (size_t)len);
    int r = ioctl(fd, HIDIOC_SFEATURE(len), buf);
    return r < 0 ? -errno : r;
}

static void hexdump(const unsigned char *b, int n, int skip) {
    for (int i = skip; i < n; i++) {
        if (b[i] >= 0x20 && b[i] < 0x7f) putchar(b[i]);
        else printf("\\x%02x", b[i]);
    }
}

static int open_dev(const char *dev) { return open(dev, O_RDWR | O_NONBLOCK); }

/* --------------------------------------------------------- 供电 / 数据角色 */
/* dwc3-msm 的 mode 属性：host / peripheral / none。写 host 才有 USB 主机数据。 */
static int find_dwc3_mode(char *out, size_t n) {
    static const char *pats[] = {
        "/sys/devices/platform/soc/*dwc3*/mode",
        "/sys/devices/platform/soc/*.usb*/mode",
        "/sys/devices/platform/soc/*/*dwc3*/mode",
        NULL
    };
    for (int i = 0; pats[i]; i++) {
        glob_t g;
        if (glob(pats[i], 0, NULL, &g) == 0 && g.gl_pathc > 0) {
            snprintf(out, n, "%s", g.gl_pathv[0]);
            globfree(&g);
            return 0;
        }
        globfree(&g);
    }
    return -1;
}

static int power_on(void) {
    int rc = 0;
    char mode[256];

    /* 对外 5V 的开关。qpnp-smb5 把它放在可写白名单里（OTG_SWITCH）。 */
    char p[256];
    snprintf(p, sizeof p, "%s/otg_switch", PS_USB);
    if (write_file(p, "1") != 0) rc = -1;

    /* 限流：有的机型/内核把它暴露成可写，不可写就保持 DT 值，不算失败。 */
    {
        char bp[256];
        snprintf(bp, sizeof bp, "%s/current_max", PS_USB);
        (void)write_file(bp, "1500000");
        snprintf(bp, sizeof bp, "%s/input_current_limit", PS_USB);
        (void)write_file(bp, "1500000");
        snprintf(bp, sizeof bp, "%s/boost_current", PS_USB);
        (void)write_file(bp, "1500000");
    }

    /* 数据线要同时供电 + 传 DP，角色必须是 host。 */
    if (find_dwc3_mode(mode, sizeof mode) == 0) (void)write_file(mode, "host");
    return rc;
}

static int power_off(void) {
    char p[256];
    snprintf(p, sizeof p, "%s/otg_switch", PS_USB);
    return write_file(p, "0") == 0 ? 0 : -1;
}

static int power_status(void) {
    char v[128];
    printf("[power]\n");
    ps_show(PS_USB, "otg_switch");
    ps_show(PS_USB, "boost_current");
    ps_show(PS_USB, "current_max");
    ps_show(PS_USB, "typec_power_role");
    ps_show(PS_USB, "real_type");
    ps_show(PS_USB, "present");
    ps_show(PS_USB, "voltage_now");
    ps_show(PS_USB, "current_now");
    ps_show(PS_MAIN, "force_main_icl");
    ps_show(PS_MAIN, "main_fcc_max");
    ps_show(PS_BATTERY, "capacity");
    char mode[256];
    if (find_dwc3_mode(mode, sizeof mode) == 0) {
        printf("[dwc3]\n  %s = ", mode);
        read_file(mode, v, sizeof v);
        printf("%s\n", v);
    } else {
        printf("[dwc3]\n  未找到 mode 属性\n");
    }
    return 0;
}

/* -------------------------------------------------------------------- DP */
static void show_file(const char *title, const char *path, int grep_lane) {
    char buf[4096];
    int r = read_file(path, buf, sizeof buf);
    if (r <= 0) {
        printf("  %-24s (不可读, %d)\n", title, r);
        return;
    }
    if (!grep_lane) {
        printf("  %-24s = %s\n", title, buf);
        return;
    }
    printf("  %s:\n", title);
    char *save = NULL;
    for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        if (strstr(l, "lane") || strstr(l, "Lane") || strstr(l, "rate") ||
            strstr(l, "Rate") || strstr(l, "link") || strstr(l, "Link") ||
            strstr(l, "bw") || strstr(l, "err"))
            printf("      %s\n", l);
    }
}

static int dp_status(void) {
    printf("[dp]\n");
    show_file("card0-DP-1/status", "/sys/class/drm/card0-DP-1/status", 0);
    if (access("/sys/class/drm/card0-DP-1/edid", F_OK) == 0) {
        char buf[64];
        if (read_file("/sys/class/drm/card0-DP-1/edid", buf, sizeof buf) > 8)
            printf("  %-24s = 有 EDID\n", "edid");
        else
            printf("  %-24s = 空（链路还没起来）\n", "edid");
    }
    show_file("dp_debug", "/sys/kernel/debug/dp/dp_debug", 1);
    show_file("edid_modes", "/sys/kernel/debug/dp/edid_modes", 0);
    printf("  %-24s = %s\n", "hint", "若 lane=2 且带宽贴顶，就是闪烁根因，降分辨率或换线");
    return 0;
}

/* ------------------------------------------------------------------ 激活 */
/* report 5 的长度是 5，载荷字段未知；按候选逐个试，谁返回 5 就用谁。 */
static int do_activate(int fd) {
    static const unsigned char cand[][5] = {
        {5, 0, 0, 0, 0},
        {5, 1, 0, 0, 0},
        {5, 2, 0, 0, 0},
        {5, 0, 1, 0, 0},
    };
    for (size_t i = 0; i < sizeof cand / sizeof cand[0]; i++) {
        int r = feat_set(fd, R_ACTIVATE, 5, cand[i]);
        if (r == 5) {
            if (i) printf("activate: 载荷候选 #%zu 生效\n", i);
            return 0;
        }
    }
    return -1;
}

static void do_deactivate(int fd) {
    unsigned char p[5] = {6, 0, 0, 0, 0};
    (void)feat_set(fd, R_DEACTIVATE, 5, p);
}

/* --------------------------------------------------------------- 状态文件 */
/* 用 key=value 落到 /data/local/tmp，service.sh 读到变化再 setprop。
 * 避免 native 直接 fork setprop，也避免高频写属性。 */
static void state_write(const char *key, const char *val) {
    char old[512], line[160];
    int have_old = read_file(STATE_FILE, old, sizeof old) > 0;
    char tmp[2048];
    size_t off = 0;
    tmp[0] = 0;

    snprintf(line, sizeof line, "%s=%s\n", key, val);

    if (have_old) {
        char *save = NULL, *nl = strdup(old);
        for (char *l = strtok_r(nl, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
            if (strncmp(l, key, strlen(key)) == 0 && l[strlen(key)] == '=') continue;
            off += (size_t)snprintf(tmp + off, sizeof tmp - off, "%s\n", l);
            if (off >= sizeof tmp - 64) break;
        }
        free(nl);
    }
    off += (size_t)snprintf(tmp + off, sizeof tmp - off, "%s", line);
    write_file(STATE_FILE, tmp);
}

/* ------------------------------------------------------------------ 监视 */
static void monitor(const char *dev, int seconds, int verbose) {
    int fd = open_dev(dev);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
        exit(5);
    }
    if (do_activate(fd) != 0)
        fprintf(stderr, "activate 未确认（继续读流，设备可能已由别人激活）\n");

    /* 只有戴上/摘下时才可能有 proximity；把初始态先标成 unknown。 */
    state_write("worn", "-1");
    state_write("monitor", "1");

    long t0 = now_ms();
    long last_report = t0;
    long last_poll = 0;
    unsigned long cnt[32];
    memset(cnt, 0, sizeof cnt);
    unsigned char buf[512];

    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    while (!g_stop) {
        /* 事件流之外再补一路轮询：原厂 VRServiceX 就是 GET(1) id4 / GET(8) id8 轮询，
         * 静置不戴时事件流可能一帧都没有，轮询能立刻给出状态。 */
        long np = now_ms();
        if (np - last_poll >= 1000) {
            last_poll = np;
            unsigned char pb[16];
            int r4 = feat_get(fd, R_PROXIMITY, 1, pb);
            if (r4 == 1) {
                state_write("worn", pb[1] ? "1" : "0");
                state_write("prox_src", "poll");
                if (verbose) printf("[poll] proximity id4/1 = 0x%02x\n", pb[1]);
            }
            int r8 = feat_get(fd, R_BATTERY, 8, pb);
            if (r8 > 0) {
                int pct = -1;
                for (int k = 1; k < r8 && k < 16; k++)
                    if (pb[k] >= 1 && pb[k] <= 100) { pct = pb[k]; break; }
                if (pct > 0) {
                    char v[8];
                    snprintf(v, sizeof v, "%d", pct);
                    state_write("battery", v);
                    if (verbose) printf("[poll] battery id8/%d = %d%%\n", r8, pct);
                }
            }
        }

        struct pollfd pfd = {fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 300);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            if (seconds > 0 && now_ms() - t0 > (long)seconds * 1000) break;
            /* 10 秒没有任何报文：眼镜可能没被真正戴上，或链路没激活成功 */
            if (now_ms() - last_report > 10000) {
                state_write("worn", "0");
                last_report = now_ms();
            }
            continue;
        }
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            fprintf(stderr, "read: %s\n", n < 0 ? strerror(errno) : "0（设备消失）");
            break;
        }
        last_report = now_ms();
        int id = buf[0];
        if (id < 0 || id >= 32) id = 31;
        cnt[id]++;

        if (verbose)
            printf("[%6ldms] id=%-2d len=%-3zd ", now_ms() - t0, buf[0], n);

        switch (buf[0]) {
        case R_PROXIMITY:
            state_write("prox_src", "stream");
            /* 载荷首字节语义按现场标定：非 0 = 戴上。先按这个约定实现，
             * 第一次抓到真帧后用注释里的原样字节校正。 */
            if (n > 1) state_write("worn", buf[1] ? "1" : "0");
            if (verbose) printf("proximity -> worn=%d", n > 1 ? (buf[1] ? 1 : 0) : -1);
            break;
        case R_BATTERY:
            if (n > 1) {
                char v[16];
                snprintf(v, sizeof v, "%d", buf[1]);
                state_write("battery", v);
            }
            if (verbose) printf("battery -> %d%%", n > 1 ? buf[1] : -1);
            break;
        case R_IMU:
            /* 输入侧的 report 1 是 IMU；只在 -v 时打前 12 字节，免得刷屏 */
            if (verbose) { printf("imu: "); hexdump(buf, (int)(n < 13 ? n : 13), 1); }
            break;
        default:
            if (verbose) { printf("id%d: ", buf[0]); hexdump(buf, (int)n, 1); }
            break;
        }
        if (verbose) putchar('\n');

        if (seconds > 0 && now_ms() - t0 > (long)seconds * 1000) break;
    }

    do_deactivate(fd);
    close(fd);
    state_write("monitor", "0");
    printf("统计:");
    for (int i = 0; i < 32; i++)
        if (cnt[i]) printf(" id%d=%lu", i, cnt[i]);
    printf("\n");
}

/* ------------------------------------------------------------------ 姿态 */
/* 姿态既落文件也给 socket：runtime 想低延迟就 bind 这个 socket 收 100 Hz 数据报，
 * 不想写 runtime 就先读文件。socket 没人监听时 sendto 返回 ECONNREFUSED，
 * 自动退回只写文件，不会拖慢主循环。 */
static int pose_sock = -1;
static long pose_sock_retry;

static void pose_sock_try(void) {
    long now = now_ms();
    if (now - pose_sock_retry < 2000) return;
    pose_sock_retry = now;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, POSE_SOCK, sizeof sa.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return; }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    pose_sock = fd;
}

/* UDP 出口：App 进程受 SELinux 限制摸不到 /data/local/tmp，但 localhost UDP 可以。
 * 报文格式（一行 ASCII，兼容旧 4 值）：
 *   "w x y z [px py pz flags]"  flags bit0=位置有效(6DoF)，bit1..=预留
 * 外部 6DoF 源（PC 端 SLAM / 手机 ARCore 桥）也可以往同一端口发同样格式，runtime 不挑食。 */
#define POSE_UDP_PORT 47110
static int pose_udp = -1;
static struct sockaddr_in pose_udp_addr;

static void pose_udp_init(void) {
    if (pose_udp >= 0) return;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(POSE_UDP_PORT);
    sa.sin_addr.s_addr = htonl(0x7f000001);   /* 127.0.0.1 */
    pose_udp_addr = sa;
    pose_udp = fd;
}

static void pose_udp_send(const char *line, int len) {
    pose_udp_init();
    if (pose_udp < 0) return;
    sendto(pose_udp, line, (size_t)len, 0, (struct sockaddr *)&pose_udp_addr, sizeof pose_udp_addr);
}

/* 位置源：默认无（7T 无 SLAM）。由 --pos "x y z" 或环境 HWVR_POS 提供则视为 6DoF 有效。 */
static double g_pos[3];
static int g_pos_valid;

static void pose_emit(const char *line, int len) {
    if (pose_sock < 0) pose_sock_try();
    if (pose_sock >= 0) {
        if (send(pose_sock, line, (size_t)len, 0) < 0) {
            close(pose_sock);
            pose_sock = -1;
        }
    }
    pose_udp_send(line, len);
}

/* 把 4 元组扩成 8 值（带位置与标志）后再发一次，供 runtime 区分 3DoF/6DoF */
static void pose_emit_full(double w, double x, double y, double z) {
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%.4f %.4f %.4f %.4f %.4f %.4f %.4f %d\n",
             w, x, y, z, g_pos[0], g_pos[1], g_pos[2], g_pos_valid ? 1 : 0);
    pose_udp_send(tmp, (int)strlen(tmp));
}
/* 最小 3DoF：陀螺积分 + 加速度计重力校正（Mahony 互补滤波）。
 * 报告 1 的字段偏移在 CV10 上还没标定过，默认按「u32 时间戳 + 3×int16 陀螺
 * + 3×int16 加速度」解释，可用 --gyro-off/--acc-off 现场校正。 */
static double q[4] = {1, 0, 0, 0}; /* w,x,y,z */

static void qmul(const double a[4], const double b[4], double out[4]) {
    out[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    out[1] = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    out[2] = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    out[3] = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
}

static void qnorm(double a[4]) {
    double n = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
    if (n < 1e-9) { a[0] = 1; a[1] = a[2] = a[3] = 0; return; }
    for (int i = 0; i < 4; i++) a[i] /= n;
}

/* 用重力方向把 roll/pitch 拉回来（yaw 不可观，靠陀螺保持） */
static void acc_correct(double ax, double ay, double az, double dt) {
    double an = sqrt(ax * ax + ay * ay + az * az);
    if (an < 0.5 || an > 30) return; /* 单位 g，异常丢弃 */
    double gx = ax / an, gy = ay / an, gz = az / an;

    /* 当前姿态下「上」方向 */
    double vx = 2 * (q[1] * q[3] + q[0] * q[2]);
    double vy = 2 * (q[2] * q[3] - q[0] * q[1]);
    double vz = q[0] * q[0] - q[1] * q[1] - q[2] * q[2] + q[3] * q[3];

    /* 用误差轴做小角度修正 */
    double ex = gy * vz - gz * vy;
    double ey = gz * vx - gx * vz;
    double ez = gx * vy - gy * vx;
    double k = 0.02 * (dt / 0.01);
    if (k > 0.2) k = 0.2;

    double dq[4] = {1, ex * k * 0.5, ey * k * 0.5, ez * k * 0.5};
    double out[4];
    qmul(dq, q, out);
    memcpy(q, out, sizeof out);
    qnorm(q);
}

static int16_t s16(const unsigned char *p) { return (int16_t)(p[0] | (p[1] << 8)); }

static void pose(const char *dev, int hz, int gyro_off, int acc_off, double scale) {
    /* 6DoF 位置源：7T 自身没有 SLAM，位置只能外部给（HWVR_POS="x y z" 或 --pos） */
    const char *pos_env = getenv("HWVR_POS");
    if (pos_env) {
        if (sscanf(pos_env, "%lf %lf %lf", &g_pos[0], &g_pos[1], &g_pos[2]) == 3) g_pos_valid = 1;
    }
    int fd = open_dev(dev);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", dev, strerror(errno)); exit(5); }
    if (do_activate(fd) != 0) fprintf(stderr, "activate 未确认，仍尝试读流\n");

    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    unsigned char buf[512];
    double dt = hz > 0 ? 1.0 / hz : 0.01;
    long last = now_ms();
    long last_out = 0;

    while (!g_stop) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 100) <= 0) continue;
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) continue;
        if (buf[0] != R_IMU) continue;
        if (n < acc_off + 6 || n < gyro_off + 6) continue;

        double gx = s16(buf + gyro_off) * scale * M_PI / 180.0;
        double gy = s16(buf + gyro_off + 2) * scale * M_PI / 180.0;
        double gz = s16(buf + gyro_off + 4) * scale * M_PI / 180.0;
        double ax = s16(buf + acc_off) / 32768.0 * 2.0;
        double ay = s16(buf + acc_off + 2) / 32768.0 * 2.0;
        double az = s16(buf + acc_off + 4) / 32768.0 * 2.0;

        long now = now_ms();
        double real_dt = (now - last) / 1000.0;
        last = now;
        if (real_dt < 0.0005 || real_dt > 0.1) real_dt = dt;

        /* 角速度积分：q' = q ⊗ [1, ω dt/2] */
        double w = sqrt(gx * gx + gy * gy + gz * gz);
        double dq[4];
        if (w > 1e-6) {
            double half = w * real_dt / 2.0;
            double s = sin(half) / w;
            dq[0] = cos(half);
            dq[1] = gx * s;
            dq[2] = gy * s;
            dq[3] = gz * s;
        } else {
            dq[0] = 1; dq[1] = dq[2] = dq[3] = 0;
        }
        double out[4];
        qmul(q, dq, out);
        memcpy(q, out, sizeof out);
        qnorm(q);
        acc_correct(ax, ay, az, real_dt);

        if (now - last_out >= 10) { /* 100 Hz 输出上限 */
            last_out = now;
            char tmp[128];
            snprintf(tmp, sizeof tmp, "%.4f %.4f %.4f %.4f\n", q[0], q[1], q[2], q[3]);
            /* 原地覆盖写，运行时要读就 msync 级别的原子性这里做不到，
             * 但一行 30 字节的常规 write 在 tmpfs 上足够，读取侧按行校验即可。 */
            write_file(POSE_FILE, tmp);
            pose_emit(tmp, (int)strlen(tmp));
            pose_emit_full(q[0], q[1], q[2], q[3]);
            if (getenv("HWVR_POSE_ECHO")) fputs(tmp, stdout);
        }
    }
    {
        char zero[64] = "1.0000 0.0000 0.0000 0.0000\n";
        write_file(POSE_FILE, zero);
    }
    do_deactivate(fd);
    close(fd);
}

/* -------------------------------------------------------------------- 入口 */
static void usage(void) {
    fprintf(stderr,
        "用法:\n"
        "  hwvrctl probe <dev>\n"
        "  hwvrctl info <dev>\n"
        "  hwvrctl activate <dev>\n"
        "  hwvrctl monitor <dev> [秒] [--verbose]\n"
        "  hwvrctl power on|off|status\n"
        "  hwvrctl dp status\n"
        "  hwvrctl pose <dev> [hz] [--gyro-off N] [--acc-off N] [--scale F]\n");
    exit(2);
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    const char *cmd = argv[1];

    if (!strcmp(cmd, "probe")) {
        if (argc < 3) usage();
        int fd = open(argv[2], O_RDWR | O_NONBLOCK);
        if (fd < 0) return 3;
        unsigned char b[8];
        int r = feat_get(fd, R_VENDOR, 4, b);
        close(fd);
        return r == 4 ? 0 : 4;
    }

    if (!strcmp(cmd, "info")) {
        if (argc < 3) usage();
        int fd = open_dev(argv[2]);
        if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return 5; }
        struct { int id, len; const char *name; } q[] = {
            {R_VENDOR, 4, "vendor(1,4)"},
            {R_VERSION, 62, "version(2,62)"},     /* CV10 */
            {R_VERSION, 80, "version(2,80)"},     /* Halliday */
            {9, 48, "optical(9,48)"},
            {9, 16, "offset(9,16)"},
        };
        unsigned char b[128];
        for (size_t i = 0; i < sizeof q / sizeof q[0]; i++) {
            int r = feat_get(fd, q[i].id, q[i].len, b);
            printf("%-16s ", q[i].name);
            if (r > 0) { printf("ret=%-3d [", r); hexdump(b, r, 1); printf("]\n"); }
            else printf("失败 errno=%d (%s)\n", -r, strerror(-r));
        }
        /* VRServiceX 走的是小块查询（接近/电量/亮度/温度），与 Walle 的大块不同 */
        printf("\n[小块查询]\n");
        struct { int id, len; const char *name; } sq[] = {
            {4, 1, "proximity(4,1)"}, {4, 2, "proximity(4,2)"},
            {8, 8, "battery(8,8)"}, {8, 1, "battery(8,1)"},
            {10, 1, "thermal(10,1)"}, {11, 4, "brightness(11,4)"},
        };
        for (size_t i = 0; i < sizeof sq / sizeof sq[0]; i++) {
            int r = feat_get(fd, sq[i].id, sq[i].len, b);
            printf("%-16s ", sq[i].name);
            if (r > 0) { printf("ret=%-3d [", r); hexdump(b, r, 1); printf("]\n"); }
            else printf("失败 errno=%d (%s)\n", -r, strerror(-r));
        }
        close(fd);
        return 0;
    }

    if (!strcmp(cmd, "activate")) {
        if (argc < 3) usage();
        int fd = open_dev(argv[2]);
        if (fd < 0) return 5;
        int r = do_activate(fd) == 0 ? 0 : 6;
        printf("activate %s\n", r == 0 ? "ok" : "失败");
        close(fd);
        return r;
    }

    if (!strcmp(cmd, "monitor")) {
        if (argc < 3) usage();
        int sec = 0, verbose = 0;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) verbose = 1;
            else if (!sec) sec = atoi(argv[i]);
        }
        monitor(argv[2], sec, verbose);
        return 0;
    }

    if (!strcmp(cmd, "power")) {
        const char *op = argc > 2 ? argv[2] : "status";
        if (!strcmp(op, "on")) {
            int r = power_on();
            printf("power on %s\n", r == 0 ? "ok" : "部分失败（见 status）");
            if (argc > 3 && !strcmp(argv[3], "--verbose")) power_status();
            return 0;
        }
        if (!strcmp(op, "off")) { printf("power off\n"); return power_off() == 0 ? 0 : 0; }
        power_status();
        return 0;
    }

    if (!strcmp(cmd, "dp")) {
        dp_status();
        return 0;
    }

    if (!strcmp(cmd, "pose")) {
        if (argc < 3) usage();
        int hz = 100, goff = 8, aoff = 16;
        double scale = 1.0;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--gyro-off") && i + 1 < argc) goff = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--acc-off") && i + 1 < argc) aoff = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atof(argv[++i]);
            else if (isdigit((unsigned char)argv[i][0])) hz = atoi(argv[i]);
        }
        pose(argv[2], hz, goff, aoff, scale);
        return 0;
    }

    usage();
    return 2;
}
