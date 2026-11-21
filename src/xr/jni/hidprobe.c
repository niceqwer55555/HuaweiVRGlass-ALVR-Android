/*
 * hidprobe.c —— /dev/hwvr* 原始 HID 报文探针
 *
 * 背景：
 *   USB Manager 那条路在 EMUI 上走不通（requestPermission 会卡住），
 *   但 HID 字符设备本身是 0644 root:system，普通 app 能 open。
 *   原厂 libvrpose/libWalleDev 就是从这里拿眼镜 IMU 的（HelmetPose，1000Hz）。
 *
 *   而 libWalleDev 的 WalleCommon 里同时有
 *       ProcessDataHeadPos(DeviceData*)   头显位姿
 *       ProcessDataContrPos(DeviceData*)  手柄位姿
 *   说明手柄数据就复用同一条 HID 数据流，靠报文里的 tag 区分。
 *   本探针的作用就是把原始报文都打出来，看清 tag 分布。
 *
 * 做四件事：
 *   1. 逐个 open /dev/hwvr0..3，报 errno（顺便确认权限边界）
 *   2. HIDIOCGRAWINFO / HIDIOCGRAWNAME 识别设备身份
 *   3. 按 HELMET_HID_PROTOCOL.md 读 feature report 1（厂商号）、
 *      再 HIDIOCSFEATURE 发 report 5（激活设备）
 *   4. poll + read 若干秒，把每个报文按 "长度 + 前 48 字节 hex + tag" 打出来
 *
 * 注意：必须先让 libvrpose 停下来（pose_mode=3dof），否则同一个 hidraw
 * 已经被 HelmetPose 的读线程占着，我们这边收不到数据。
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "xr_shim.h"

/* ---- 内核 hidraw ioctl（NDK 里不一定有 linux/hidraw.h，这里自己定义） ---- */
struct hidraw_devinfo_local {
    unsigned int bustype;
    short vendor;
    short product;
};

#define HIDIOCGRAWINFO_LOCAL _IOR('H', 0x03, struct hidraw_devinfo_local)
#define HIDIOCGRAWNAME_LOCAL(len) _IOC(_IOC_READ, 'H', 0x04, len)
#define HIDIOCSFEATURE_LOCAL(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x06, len)
#define HIDIOCGFEATURE_LOCAL(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x07, len)

#define PROBE_BUF 512
#define PROBE_MS 6000       /* 每个节点监听时长 */
#define PROBE_MAX_DUMP 80   /* 最多打印多少个报文 */

static void hexdump(const char *tag, const unsigned char *b, int n, int limit) {
    char line[3 * 64 + 8];
    int cap = n < limit ? n : limit;
    int p = 0;
    for (int i = 0; i < cap && p < (int)sizeof(line) - 4; i++) {
        p += snprintf(line + p, sizeof(line) - p, "%02x ", b[i]);
    }
    line[p > 0 ? p : 0] = '\0';
    XLOG("%s len=%d [%s]", tag, n, line);
}

/* 打开一个节点并报告身份 */
static int open_node(const char *path) {
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        /* 退一步：只读打开（有些固件不需要写） */
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            XWARN("HIDPROBE: %s 打开失败 errno=%d (%s)", path, errno, strerror(errno));
            return -1;
        }
        XLOG("HIDPROBE: %s 只读打开 fd=%d", path, fd);
    } else {
        XLOG("HIDPROBE: %s 读写打开 fd=%d", path, fd);
    }

    char name[128];
    memset(name, 0, sizeof(name));
    if (ioctl(fd, HIDIOCGRAWNAME_LOCAL(sizeof(name) - 1), name) >= 0) {
        XLOG("HIDPROBE: %s name=\"%s\"", path, name);
    } else {
        XWARN("HIDPROBE: %s GRAWNAME 失败 errno=%d", path, errno);
    }

    struct hidraw_devinfo_local info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, HIDIOCGRAWINFO_LOCAL, &info) >= 0) {
        XLOG("HIDPROBE: %s bustype=%u vendor=%04x product=%04x", path,
             info.bustype, (unsigned short) info.vendor, (unsigned short) info.product);
    } else {
        XWARN("HIDPROBE: %s GRAWINFO 失败 errno=%d", path, errno);
    }
    return fd;
}

/* 读 feature report：report id = id，长度 len */
static void read_feature(const char *path, int fd, unsigned char id, int len) {
    unsigned char buf[256];
    if (len > (int)sizeof(buf)) {
        return;
    }
    memset(buf, 0, sizeof(buf));
    buf[0] = id;
    int r = ioctl(fd, HIDIOCGFEATURE_LOCAL(len), buf);
    if (r >= 0) {
        XLOG("HIDPROBE: %s GET_FEATURE id=%d 返回 %d 字节", path, id, r);
        hexdump("  feature", buf, len > 0 ? len : r, 48);
    } else {
        XWARN("HIDPROBE: %s GET_FEATURE id=%d 失败 errno=%d (%s)", path, id, errno,
              strerror(errno));
    }
}

/* 发 feature report 激活设备（协议：report id 5，长度 5） */
static void send_activate(const char *path, int fd) {
    unsigned char buf[8];
    memset(buf, 0, sizeof(buf));
    buf[0] = 5;   /* report id */
    buf[1] = 1;   /* 1 = 激活（协议里 6 = 去激活） */
    int r = ioctl(fd, HIDIOCSFEATURE_LOCAL(5), buf);
    if (r >= 0) {
        XLOG("HIDPROBE: %s SET_FEATURE id=5 激活成功", path);
    } else {
        XWARN("HIDPROBE: %s SET_FEATURE id=5 失败 errno=%d (%s)", path, errno,
              strerror(errno));
    }
}

/* 监听并 dump 报文 */
static void drain_node(const char *path, int fd) {
    unsigned char buf[PROBE_BUF];
    long long deadline_ms = -1;
    int total = 0;
    int dumps = 0;
    int by_tag[16];
    memset(by_tag, 0, sizeof(by_tag));

    int64_t end = xr_now_ns() + (int64_t) PROBE_MS * 1000000LL;
    while (xr_now_ns() < end) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) {
            continue;
        }
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            continue;
        }
        total++;
        int tag = n > 0 ? (buf[0] & 0x0F) : 0;
        by_tag[tag]++;
        if (dumps < PROBE_MAX_DUMP) {
            char label[64];
            snprintf(label, sizeof(label), "HIDPROBE: %s #%d tag=%d", path, total, tag);
            hexdump(label, buf, (int) n, 48);
            dumps++;
        }
        (void) deadline_ms;
    }

    char stat[256];
    int p = 0;
    for (int i = 0; i < 16; i++) {
        if (by_tag[i] > 0) {
            p += snprintf(stat + p, sizeof(stat) - p, " tag%d=%d", i, by_tag[i]);
        }
    }
    XLOG("HIDPROBE: %s 结束，共 %d 报文，tag 分布:%s", path, total, p > 0 ? stat : " 无");
}

void xr_hid_probe_run(void) {
    XLOG("======== HID 探针开始 ========");
    static const char *nodes[] = {
            "/dev/hwvr0", "/dev/hwvr1", "/dev/hwvr2", "/dev/hwvr3", NULL
    };
    for (int i = 0; nodes[i]; i++) {
        XLOG("---- %s ----", nodes[i]);
        int fd = open_node(nodes[i]);
        if (fd < 0) {
            continue;
        }
        /* 先看身份：协议里 report 1 = 厂商号(4B)，report 2 = 版本(62B)，
         * report 9 = 光学信息(48B)。读一遍就能确认是眼镜还是手柄。 */
        read_feature(nodes[i], fd, 1, 4);
        read_feature(nodes[i], fd, 2, 62);
        read_feature(nodes[i], fd, 9, 48);
        send_activate(nodes[i], fd);
        drain_node(nodes[i], fd);
        close(fd);
    }
    XLOG("======== HID 探针结束 ========");
}
