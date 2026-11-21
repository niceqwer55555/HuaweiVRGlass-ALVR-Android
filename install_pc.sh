#!/bin/bash
# 在 PC 上跑：装 APK + 推模块 + 起日志
set -e
APK=$(ls 01-APP/*.apk | head -1)
adb uninstall com.lht.huaweivr.alvr 2>/dev/null || true
adb install -r "$APK"
adb shell su -c 'mkdir -p /data/adb/modules' 2>/dev/null || true
for m in 02-MODULE/*; do
  n=$(basename "$m")
  adb push "$m" "/data/local/tmp/$n"
  adb shell su -c "rm -rf /data/adb/modules/$n && cp -rf /data/local/tmp/$n /data/adb/modules/$n && chmod 755 /data/adb/modules/$n"
done
echo ">> 请重启设备，然后：adb logcat -s MINIXR HVR-DIAG"
