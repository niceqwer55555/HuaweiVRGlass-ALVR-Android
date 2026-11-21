#!/system/bin/sh
# ALVR 7T Runtime Injector v0.2
#   1) 把自研 OpenXR runtime(libvrsdk.so) bind-mount 进目标 App 的 lib 目录（免重装 APK）
#   2) 起姿态守护：hwvrctl pose 读 /dev/hwvr1 的 IMU，算 3DoF，并按 UDP 47110 广播
#      若设置了 6DoF 位置源（pos.conf / HWVR_POS），同一报文带位置，runtime 自动切 6DoF
MODDIR=${0%/*}
PKG=com.lht.huaweivr.alvr
SRC=$MODDIR/lib/libvrsdk.so
LOG=/data/local/tmp/alvr7t_inject.log

log() { echo "[$(date '+%H:%M:%S')] $*" >> $LOG; }

for i in $(seq 1 60); do
  [ "$(getprop sys.boot_completed)" = "1" ] && break
  sleep 1
done
sleep 5
: > $LOG
log "start v0.2, pkg=$PKG"

# ---------------------------------------------------------------- 1) 注入 runtime
APKDIR=""
for d in /data/app/*/$PKG-*/lib/arm /data/app/*/$PKG-*/lib/armeabi-v7a; do
  [ -d "$d" ] && APKDIR="$d" && break
done
if [ -z "$APKDIR" ]; then
  log "未找到 $PKG 的 lib 目录（App 没装？），跳过注入"
else
  log "apkdir=$APKDIR"
  TARGET=$APKDIR/libvrsdk.so
  [ -f "$TARGET" ] || : > "$TARGET" 2>/dev/null
  chcon u:object_r:apk_data_file:s0 "$SRC" 2>/dev/null
  chmod 0644 "$SRC"
  mount -o bind "$SRC" "$TARGET" 2>>$LOG && log "bind OK"
  ls -lZ "$TARGET" >> $LOG 2>&1
fi

# ---------------------------------------------------------------- 2) 姿态守护
# 设备节点：优先 /dev/hwvr1（hwvr_compat 建好的），否则扫 hidraw 里 VID12d1 的接口
DEV=""
[ -e /dev/hwvr1 ] && DEV=/dev/hwvr1
if [ -z "$DEV" ]; then
  for h in /dev/hidraw*; do
    [ -e "$h" ] || continue
    UEV=$(cat /sys/class/hidraw/$(basename "$h")/device/uevent 2>/dev/null)
    echo "$UEV" | grep -qi "12D1" && DEV=$h && break
  done
fi
log "imu dev=$DEV"

# 6DoF 位置源（可选）：pos.conf 里写 "x y z"，或运行时由外部程序往 47110 发 8 值报文
[ -f $MODDIR/pos.conf ] && export HWVR_POS="$(cat $MODDIR/pos.conf)"

if [ -n "$DEV" ]; then
  chmod 666 "$DEV" 2>/dev/null
  chcon u:object_r:null_device:s0 "$DEV" 2>/dev/null
  # 100 Hz 输出，静默后台跑
  ( $MODDIR/bin/hwvrctl pose "$DEV" 100 >> /data/local/tmp/hwvr_pose.log 2>&1 ) &
  log "pose 守护已启动 (pid=$!)"
else
  log "没找到 IMU 设备，姿态只能由外部源喂 47110"
fi

log "done"
