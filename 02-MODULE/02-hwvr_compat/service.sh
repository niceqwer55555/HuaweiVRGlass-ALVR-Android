#!/system/bin/sh
# hwvr_compat v3: 把「通用安卓不认识 VR 眼镜」缺的四件事一次补上。
#
#   1) 供电 —— 数据线要对外输出 5V 才能点亮眼镜；不锁住就会随 PD 协商掉电，
#      表现就是画面一闪一闪。
#   2) 节点 —— /dev/hwvr1 指向真正的 HID 控制接口（按协议应答挑选，不看厂商号）。
#   3) 事件 —— 启动 hwvrctl monitor 读输入流：先发 SET_FEATURE(report 5) 激活，
#      再按 report id 把「戴上/摘下」「电量」翻译成 sys.hwvr.*。这是原厂 App 的
#      做法（libWalleDev 的 ProcessDataProxyimity / ProcessDataLowBattery），
#      通用安卓上没人做，所以以前永远拿不到接近与电量。
#   4) 显示 —— 手机逻辑尺寸对齐面板 2:1，镜像 1:1，每只眼占一半。
#
# 姿态链不由本模块负责（那是 OpenXR runtime 的事），但 IMU 流在这里是通的，
# 需要时用 hwvrctl pose 直接验证。
MOD=/data/adb/modules/hwvr_compat
[ -d "$MOD" ] || MOD=/data/adb/modules_update/hwvr_compat

CTL="$MOD/hwvrctl"
LOG=/data/local/tmp/hwvr.log
STATE=/data/local/tmp/hwvr_state
ROT_STATE=/data/local/tmp/hwvr_rotation_saved
PIDF=/data/local/tmp/hwvr_monitor.pid
LOWRES=/data/local/tmp/hwvr_lowres

chmod 755 "$MOD/service.sh" 2>/dev/null
chmod 755 "$MOD/hwvrctl" "$MOD/hwvr-probe" 2>/dev/null

log() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$LOG"; }

while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 2; done
log "service.sh v3 启动，控制工具: $CTL"

[ -x "$CTL" ] || log "警告: $CTL 不存在或不可执行，本模块只剩节点/尺寸功能"

kv() { [ -f "$STATE" ] && sed -n "s/^$1=//p" "$STATE" 2>/dev/null | head -1; }
setprop_if_changed() { [ "$2" = "$3" ] || setprop "$1" "$2" 2>/dev/null; }

# --------------------------------------------------------------- 1. 供电锁
# qpnp-smb5 把 otg_switch 放在可写白名单里：1 = 对外 5V。拔插瞬间 PD 会重协商，
# 若没人守着，otg 被关掉 -> 眼镜重新握手 -> 画面闪。
ensure_power() {
    v=$(cat /sys/class/power_supply/usb/otg_switch 2>/dev/null)
    if [ "$v" != "1" ]; then
        if [ -x "$CTL" ]; then
            "$CTL" power on >/dev/null 2>&1
        else
            # 没编出 hwvrctl 也能撑住供电：直接写白名单里的 sysfs 节点
            echo 1 > /sys/class/power_supply/usb/otg_switch 2>/dev/null
            echo 1500000 > /sys/class/power_supply/usb/boost_current 2>/dev/null
        fi
        log "补供电: otg_switch 原值='$v' 已置 1"
    fi
}

# --------------------------------------------------- 2. 探测控制接口
probe() {
    if [ -x "$CTL" ]; then "$CTL" probe "/dev/$1" >/dev/null 2>&1
    elif [ -x "$MOD/hwvr-probe" ]; then "$MOD/hwvr-probe" "/dev/$1" >/dev/null 2>&1
    else return 1; fi
}

find_control_iface() {
    for d in /sys/class/hidraw/hidraw*; do
        [ -e "$d/device/uevent" ] || continue
        if probe "${d##*/}"; then echo "${d##*/}"; return 0; fi
    done
    for d in /sys/class/hidraw/hidraw*; do
        [ -e "$d/device/uevent" ] || continue
        if grep -q "GLASS HID" "$d/device/uevent" 2>/dev/null; then
            echo "${d##*/}"; return 0
        fi
    done
    return 1
}

# ------------------------------------------------------------ 3. 显示尺寸
# 默认 1600x3200（与面板 1:1）。若 /data/local/tmp/hwvr_lowres 存在，
# 改成 1440x2880：像素少 19%，DP 链路压力小，是「闪」的应急降档手段。
set_size() {
    if [ -f "$LOWRES" ]; then SIZE="1440x2880"; EXPECT="Override size: 1440x2880"
    else SIZE="1600x3200"; EXPECT="Override size: 1600x3200"; fi
    if wm size 2>/dev/null | grep -q "$EXPECT"; then return 0; fi
    if [ ! -f "$ROT_STATE" ]; then
        echo "$(settings get system accelerometer_rotation) $(settings get system user_rotation)" > "$ROT_STATE"
    fi
    wm size reset 2>/dev/null
    wm size "$SIZE" 2>/dev/null
    settings put system accelerometer_rotation 0 2>/dev/null
    settings put system user_rotation 1 2>/dev/null
    log "显示尺寸 -> $SIZE"
}

reset_size() {
    need=0
    [ -f "$ROT_STATE" ] && need=1
    wm size 2>/dev/null | grep -q "Override size:" && need=1
    wm get-ignore-orientation-request 2>/dev/null | grep -q "true" && need=1
    [ "$need" = "0" ] && return 0

    wm size reset 2>/dev/null
    if [ -f "$ROT_STATE" ]; then
        read accel user < "$ROT_STATE"
        rm -f "$ROT_STATE"
    fi
    [ -n "$accel" ] || accel=1
    [ -n "$user" ] || user=0
    settings put system accelerometer_rotation "$accel" 2>/dev/null
    settings put system user_rotation "$user" 2>/dev/null
    wm set-ignore-orientation-request false 2>/dev/null
    log "显示尺寸已复位"
}

# ------------------------------------------------------- 4. 事件流守护
start_monitor() {
    if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF" 2>/dev/null)" 2>/dev/null; then return 0; fi
    [ -x "$CTL" ] || return 0
    "$CTL" monitor /dev/hwvr1 >> "$LOG" 2>&1 &
    echo $! > "$PIDF"
    log "monitor 启动 pid=$!"
}

stop_monitor() {
    if [ -f "$PIDF" ]; then
        p=$(cat "$PIDF" 2>/dev/null)
        [ -n "$p" ] && kill "$p" 2>/dev/null
        rm -f "$PIDF"
        log "monitor 停止"
    fi
}

# DP 一连上就把链路情况记一笔，闪烁时这份日志就是证据。
dp_note() {
    [ -f /data/local/tmp/hwvr_dp_noted ] && return 0
    st=$(cat /sys/class/drm/card0-DP-1/status 2>/dev/null)
    [ "$st" = "connected" ] || return 0
    touch /data/local/tmp/hwvr_dp_noted
    log "DP connected:"
    "$CTL" dp status 2>/dev/null | while read -r l; do log "  $l"; done
}

# ------------------------------------------------------------------ 主循环
attached=0
last_worn=""

while true; do
    node=$(find_control_iface)

    if [ -n "$node" ]; then
        major_minor=$(cat "/sys/class/hidraw/$node/dev" 2>/dev/null)
        maj=${major_minor%%:*}
        min=${major_minor##*:}
        if [ -n "$maj" ] && [ -n "$min" ]; then
            want=$(printf "%x:%x" "$maj" "$min")
            cur=$(stat -c "%t:%T" /dev/hwvr1 2>/dev/null)
            if [ "$cur" != "$want" ]; then
                rm -f /dev/hwvr1
                mknod /dev/hwvr1 c "$maj" "$min"
                chcon u:object_r:null_device:s0 /dev/hwvr1 2>/dev/null
                chmod 666 /dev/hwvr1
                log "节点 /dev/hwvr1 -> hidraw/$node ($maj:$min)"
            fi
        fi

        if [ "$attached" = "0" ]; then
            attached=1
            rm -f /data/local/tmp/hwvr_dp_noted
            ensure_power
            set_size
            start_monitor
            log "眼镜接入，供电+节点+事件流已就绪"
        else
            ensure_power          # 在位期间一直守着，别让 PD 把 5V 收回去
            start_monitor         # monitor 意外退出时自动拉起
            dp_note
        fi
    else
        if [ "$attached" = "1" ]; then
            attached=0
            stop_monitor
            if [ -x "$CTL" ]; then "$CTL" power off >/dev/null 2>&1
            else echo 0 > /sys/class/power_supply/usb/otg_switch 2>/dev/null; fi
            rm -f /dev/hwvr1
            reset_size
            rm -f /data/local/tmp/hwvr_dp_noted
            log "眼镜拔出，已断电复位"
        fi
    fi

    # 事件状态 -> 系统属性（供 App / runtime 读，不必自己开节点）
    worn=$(kv worn)
    battery=$(kv battery)
    if [ -n "$worn" ]; then
        setprop_if_changed sys.hwvr.worn "$worn" "$last_worn"
        last_worn="$worn"
    fi
    [ -n "$battery" ] && setprop sys.hwvr.battery "$battery" 2>/dev/null
    if [ "$attached" = "1" ]; then setprop sys.hwvr.present 1 2>/dev/null
    else setprop sys.hwvr.present 0 2>/dev/null; fi

    sleep 2
done
