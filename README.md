# HuaweiVRGlass-ALVR-Android

一加 7T + 华为 VR Glass CV10 的 ALVR 串流改造工程。

让手机运行 ALVR OpenXR 客户端，把 PC 上的 SteamVR 画面串流到华为 VR Glass。原厂方案依赖 EMUI/HarmonyOS 的华为 VR 服务，本工程将其改造为可不依赖华为系统的自包含客户端 + 自研 OpenXR shim。

## 目录

| 目录 | 说明 |
|---|---|
| `01-APP/` | 改造后的 APK（v0.3 ~ v0.6） |
| `02-MODULE/` | Magisk 模块：注入 runtime、姿态守护、`/dev/hwvr` 节点 |
| `04-SRC/` | 自研 mini_xr runtime 源码、构建脚本、验签脚本 |
| `05-DOCS/` | 改造报告、快速上手 |
| `client_openxr/` | ALVR OpenXR 客户端（Rust）及华为平台适配 |
| `src/xr/` | 自研 OpenXR loader/shim（C）：姿态、swapchain、双目合成 |
| `server/` | ALVR 服务端配置与启动脚本（不含二进制工具链） |

## 关键修复记录

- **SteamVR 默认场景不显示 / 眼镜纯色待机**：3DoF 下 OpenXR shim 未上报 `POSITION_VALID/TRACKED`，SteamVR 判定头显未佩戴，约每 10 秒进入 standby 使 `layers=0`。修复位于 `src/xr/jni/xr_core.c` 的 `space_pose_in_stage()` 与 `xrLocateViews()`，用稳定坐姿眼高代替真实位置并置位位置追踪。
- **双目显示方向**：`xr.cfg` 的 `rotate=90`、`mirror_eye` 控制每眼旋转/镜像；`eye_shift_mm`、`eye_shift_y_mm` 控制逐眼光心偏移，`gain_r/g/b` 控制逐通道增益。
- 其余改造细节见 `05-DOCS/ALVR-7T-改造报告.md`。

## 构建

- shim：`ndk-build NDK_PROJECT_PATH=src/xr`，产物 `src/xr/libs/armeabi-v7a/libxr_loader.so`
- APK 重打包见 `04-SRC/build_all.sh` 与 `05-DOCS/快速上手.md`

> 签名私钥未纳入版本库，`04-SRC/signing/` 只保留公钥证书。
