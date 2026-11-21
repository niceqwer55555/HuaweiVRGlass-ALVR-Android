# alvr-7t —— 华为 VR Glass 串流 APK 的 7T 改造工程（v0.2 含 3DoF/6DoF 通路）

## 目录
- `out/HuaweiVRGlass-ALVR-7T-v0.1.apk`  成品（已去华为化 + 注入自研 runtime，v2/v3 已验签）
- `runtime/mini_xr_v3.c`                 runtime v0.3：+窗口获取 + SBS 合成 + present
- `runtime/mini_xr_v2.c`                 runtime v0.2：姿态(UDP 47110)/空间/视图/会话/帧/交换链
- `runtime/libvrsdk.so`                 ARM32 产物（48 KB，零 DT_NEEDED）
- `module/bin/hwvrctl`                  姿态守护（aarch64，已加 UDP 出口）
- `module/bin/pose_probe`               姿态探针：现场看 3DoF/6DoF 报文
- `runtime/libvrsdk.so`                 ARM32 产物，零 DT_NEEDED，只依赖 __android_log_print
- `module/`                             KSU/Magisk 模块：bind-mount runtime 进 App lib 目录（免重装）
- `tools/verify_apk.py`                 离线校验 APK Signing Block v2 是否自洽
- `signing/`                            测试用密钥与证书（CN=hotdogb-vr）
- `docs/ALVR-7T-改造报告.md`            ← 先看这个
- `build_all.sh`                        一键复现

## 关键结论（短版）
1. 这个包是 ALVR 的 OpenXR 客户端 + 华为定制 loader，**不含 runtime**；7T 上缺的就是 runtime。
2. 华为 loader 里写死 `"library_path": "libvrsdk.so"` 且在 **App 自己的 lib 目录**里找，
   所以把自研 runtime 以同名 so 塞进 APK 即可被加载，不用改 native、不用设环境变量。
3. v0.2 已实现 session/空间/视图/帧/交换链，并接入姿态：3DoF 走 hwvrctl→UDP 47110→runtime，
   6DoF 在报文第 8 值 bit0=1 时自动生效（位置源需外部提供）。
4. v0.3 已做合成/呈现：优先复用当前 EGL surface，否则 JNI 挖 Activity 的 SurfaceView → ANativeWindow，
   再用 glBlitFramebuffer 把两只眼铺成 SBS + eglSwapBuffers。仍未做输入 action。

## 编译 runtime
```bash
python3 -m ziglang cc -target arm-linux-musleabihf -shared -nostdlib -fPIC -O2 \
  -ffreestanding -fno-builtin -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -Wl,-soname,libvrsdk.so -o runtime/libvrsdk.so runtime/mini_xr.c
```

## 验证
```bash
adb install -r out/HuaweiVRGlass-ALVR-7T-v0.1.apk
adb logcat -s MINIXR HVR-DIAG
```
