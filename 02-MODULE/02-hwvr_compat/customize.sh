#!/system/bin/sh
# zip 打包不保留 Unix 可执行位，安装时补上。
ui_print "- setting exec permissions"
chmod 755 "$MODPATH/service.sh" 2>/dev/null

# hwvrctl 优先用包里已经编好的 arm64 二进制；没有就现场编（设备上有 clang 的话）。
if [ -f "$MODPATH/hwvrctl" ]; then
    chmod 755 "$MODPATH/hwvrctl"
    ui_print "- hwvrctl: 使用包内预编译二进制"
elif [ -f "$MODPATH/hwvrctl.c" ] && command -v clang >/dev/null 2>&1; then
    ui_print "- hwvrctl: 现场编译"
    if clang -O2 -std=gnu99 -o "$MODPATH/hwvrctl" "$MODPATH/hwvrctl.c" -lm 2>/dev/null; then
        chmod 755 "$MODPATH/hwvrctl"
        ui_print "- hwvrctl: 编译完成"
    else
        ui_print "! hwvrctl 编译失败，模块将退回 v2 行为（只有节点与尺寸）"
    fi
else
    ui_print "! 没有 hwvrctl 也没有 clang：本模块只剩节点与尺寸功能"
    ui_print "  先跑 ksu/hwvr_compat/build.sh 再打包"
fi

chmod 755 "$MODPATH/hwvr-probe" 2>/dev/null

# 清掉上一版留下的状态，避免 sys.hwvr.* 挂着旧值。
rm -f /data/local/tmp/hwvr_state /data/local/tmp/hwvr_monitor.pid
setprop sys.hwvr.worn "" 2>/dev/null
