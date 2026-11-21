#!/bin/bash
# 一键复现：解包 → 改清单 → 注入 runtime → 重打包 → 对齐 → 签名 → 校验
# 依赖：java(apktool)、build-tools34(zipalign/apksigner)、python3 + ziglang
set -e
APKTOOL=${APKTOOL:-/path/to/apktool_2.10.0.jar}
BT=${BT:-/path/to/build-tools}          # 含 zipalign + apksigner
SRC=${1:-work/orig.apk}

java -jar "$APKTOOL" d -f -o work/dec "$SRC"
python3 - <<'PY'
p='work/dec/AndroidManifest.xml'; s=open(p).read()
s=s.replace('<uses-permission android:name="com.huawei.android.permission.VR"/>\n','')
s=s.replace('<uses-permission android:name="com.huawei.vrhandle.permission.DEVICE_MANAGER"/>\n','')
s=s.replace('android:name="android.hardware.vr.headtracking" android:required="true"','android:name="android.hardware.vr.headtracking" android:required="false"')
open(p,'w').write(s)
PY

python3 -m ziglang cc -target arm-linux-musleabihf -shared -nostdlib -fPIC -O2 \
  -ffreestanding -fno-builtin -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -Wl,-soname,libvrsdk.so -o runtime/libvrsdk.so runtime/mini_xr.c
cp runtime/libvrsdk.so work/dec/lib/armeabi-v7a/libvrsdk.so

java -jar "$APKTOOL" b -f work/dec -o work/unsigned.apk
"$BT/zipalign" -p -f 4 work/unsigned.apk work/aligned.apk
"$BT/apksigner" sign --key signing/key.der --cert signing/cert.pem \
  --out out/HuaweiVRGlass-ALVR-7T-v0.1.apk work/aligned.apk
"$BT/apksigner" verify -v --print-certs out/HuaweiVRGlass-ALVR-7T-v0.1.apk
