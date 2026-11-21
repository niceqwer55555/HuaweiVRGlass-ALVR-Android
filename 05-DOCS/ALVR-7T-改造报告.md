# 华为 VR Glass 串流 APK（ALVR 0.5.0）→ OnePlus 7T 改造报告

生成时间：2026-11-20 ｜ 目标机：OnePlus 7T（hotdogb / sm8150 / Adreno 640 / LineageOS 11）
目标镜：华为 VR Glass CV10（VID:PID `12d1:107e`，3DoF）

---

## 一、先说结论

这个包**不是华为的 VR 客户端，而是 ALVR 的 OpenXR 客户端 + 华为定制版 Khronos loader**。它在 7T 上装不上/跑不起来的根因不在权限、不在 ABI、也不在缺 so，而是：

> **它把渲染全部交给 OpenXR runtime，而 7T 上根本没有能驱动 CV10 的 runtime。**

原包只带 loader（`libxr_loader.so`），真正的 runtime `libvrsdk.so` 是华为 EMUI 系统里那套（靠华为系统 VR 服务供应窗口、姿态和显示）。所以"改造成 7T 能用"的本质工作 = **自己写一个 runtime**，不是改 APK。

本次交付把能确定的部分全部落地了：APK 已去华为化并注入自研 runtime（已用官方 `apksigner` 验签通过），runtime 阶段一（instance → system → 视图/格式协商）已可加载；**session 之后的呈现链路仍需实现**——这是下一阶段的活，不是配置能解决的。

---

## 二、这个包到底是什么（实测）

| 项 | 实测值 |
|---|---|
| 包名 / 标签 | `com.lht.huaweivr.alvr` / 「华为 VR Glass 串流」 |
| 编译 SDK | 34，`extractNativeLibs=true` |
| ABI | **仅 `armeabi-v7a`**（20 条目，19 MB） |
| `libalvr_client_openxr.so` | **67,581,504 B**，Rust + wgpu + openxr crate |
| `libxr_loader.so` | 965,360 B（2022-08-22），**华为改过** |
| `classes.dex` | 37,636 B（几乎是空壳，逻辑全在 native） |
| 三个 Activity | `HuaweiVrLauncherActivity`（竖屏 LAUNCHER）、`HuaweiVrActivity`（横屏，`com.huawei.android.vr.action.MAIN`）、`HVRModeActivity`（`com.huawei.android.vr.PROMPT`） |

native 侧对 OpenXR 的直接调用只有 7 个（其余全走 `xrGetInstanceProcAddr`）：
`xrCreateInstance`、`xrCreateSession`、`xrCreateSwapchain`、`xrEnumerateApiLayerProperties`、
`xrEnumerateInstanceExtensionProperties`、`xrEnumerateSwapchainImages`、`xrGetInstanceProcAddr`。

### 关键发现：loader 会在 App 自己的 lib 目录里找 runtime

`libxr_loader.so` 里写死了这些字符串：

```
"library_path": "libvrsdk.so"
android_runtime_manifest.json
XR_API_LAYER_PATH / api_layers/implicit.d / api_layers/explicit.d
Failed to find default runtime with RuntimeInterface::LoadRuntime()
```

也就是说：**只要 `lib/armeabi-v7a/` 下存在 `libvrsdk.so`，loader 就会把它当 runtime 加载**——这和华为 App 把 runtime 打进自己 APK 的做法完全一致。这条路不需要改 native 代码，不需要设 `XR_RUNTIME_JSON`，也不需要系统里有 OpenXR 服务。这是本次改造的技术支点。

### 运行期的硬性要求（从 `[HVR-DIAG]` 日志串还原）

1. `xrEnumerateSwapchainFormats` **必须返回 `GL_RGBA8`**，否则直接报
   `Huawei runtime does not advertise the required GL_RGBA8 swapchain format` 并退出；
2. 必须有 Android native window，否则报 `Huawei VR requires an Android native window`；
3. 用 `XR_KHR_android_create_instance` 建 instance（传 VM + Activity），
   用 `XR_KHR_opengl_es_enable` 查 GLES 要求，再 `xrCreateSession`；
4. 扩展清单里它明确要 `XR_HUAWEI_controller_interaction`；
5. 拿到 runtime 的 EGL context 后 "adopting the proven Huawei EGL context in wgpu"，即
   **渲染和会话共用同一个 EGL context**。

---

## 三、原包在 7T 上的四条硬阻塞

| # | 阻塞 | 证据 | 本次处理 |
|---|---|---|---|
| 1 | 系统里没有 OpenXR runtime，也没有 `/vendor/etc/openxr/1/active_runtime.json` | 原包只有 loader；华为 runtime 只在 EMUI 上 | **在 APK 内注入 `libvrsdk.so`**（loader 默认路径） |
| 2 | 声明 `android.hardware.vr.headtracking` required=true、`freeDegree=6dof` | manifest 第 3 / 23 行；7T 无此 feature、无 6DoF（SLAM 绑麒麟 NPU） | `required=false`；6DoF 元数据保留但 runtime 只报 3DoF |
| 3 | 申请 `com.huawei.android.permission.VR`、`com.huawei.vrhandle.permission.DEVICE_MANAGER` | manifest 第 14 / 15 行；LOS 上无定义，纯噪音 | 已删除 |
| 4 | 无 runtime 时首个 `xrCreateInstance` 即失败 | loader 报 `Failed to find default runtime` | 阶段一 runtime 已能应答 instance/system/视图/格式 |

> 注：swapchain 格式那条（`GL_RGBA8`）在阶段一就满足，已写死返回 3 个格式，第一位即 `GL_RGBA8`。

---

## 四、本次实际改动（可复现）

产物：`out/HuaweiVRGlass-ALVR-7T-v0.1.apk`（19,554,204 B）

1. **清单瘦身**（`work/dec/AndroidManifest.xml`）
   - 删 `com.huawei.android.permission.VR`、`com.huawei.vrhandle.permission.DEVICE_MANAGER`
   - `android.hardware.vr.headtracking` → `required="false"`
   - label → 「VR Glass 串流 (7T)」
2. **注入 runtime**：`lib/armeabi-v7a/libvrsdk.so`（30,992 B）
3. **重打包 + 对齐 + 签名**
   ```bash
   apktool b -f work/dec -o work/unsigned.apk
   zipalign -p -f 4 work/unsigned.apk work/aligned.apk
   apksigner sign --key signing/key.der --cert signing/cert.pem --out out/....apk work/aligned.apk
   apksigner verify -v --print-certs out/....apk
   ```
   验证结果：**v2 = true，v3 = true**（v1 未启用，targetSdk≥30 用 v2 即可）
4. **备用方案（不重装 APK）**：`module/` 是一个 KSU/Magisk 模块，把同一个 `libvrsdk.so`
   bind-mount 到 `/data/app/*/com.lht.huaweivr.alvr-*/lib/arm/libvrsdk.so`，
   并把 SELinux 上下文对齐成 `u:object_r:apk_data_file:s0`（否则 App 加载会被拒）。
   日志落 `/data/local/tmp/alvr7t_inject.log`。

### runtime 本体（阶段一）

`runtime/mini_xr.c` → 交叉编译为 ARM32 共享库，**零 `DT_NEEDED`**，唯一外部符号是
`__android_log_print`（进程里 liblog 必在），因此不依赖任何 musl/glibc 运行时：

```bash
python3 -m ziglang cc -target arm-linux-musleabihf -shared -nostdlib \
  -fPIC -O2 -ffreestanding -fno-builtin -fno-unwind-tables \
  -fno-asynchronous-unwind-tables -Wl,-soname,libvrsdk.so -o libvrsdk.so mini_xr.c
```

已实现并可应答：`xrCreateInstance`（含解析 `XR_KHR_android_create_instance` 的 next 链、打印 VM/Activity）、
`xrGetInstanceProperties`、`xrEnumerateApiLayerProperties`（0 层）、
`xrEnumerateInstanceExtensionProperties`（7 个扩展，含 `XR_HUAWEI_controller_interaction`）、
`xrGetSystem`、`xrGetSystemProperties`（3DoF=有 / 6DoF=无）、
`xrEnumerateViewConfigurations`、`xrGetViewConfigurationProperties`、
`xrEnumerateViewConfigurationViews`（**2 视图 1600×1600**）、`xrEnumerateEnvironmentBlendModes`、
`xrGetOpenGLESGraphicsRequirementsKHR`（3.0–3.2）、`xrEnumerateSwapchainFormats`（RGBA8 优先）、
`xrStringToPath` / `xrPathToString`。

其余入口点（session / swapchain / frame / space / action，共 22 个）当前统一返回
`XR_ERROR_FUNCTION_UNSUPPORTED`，并在 logcat 打 `stub: xxx -> unsupported`，保证不崩、可定位。

---

## 五、还缺什么（这才是"能用"的门槛）

按依赖顺序排：

1. **`xrCreateSession` + 图形绑定**：解析 `XrGraphicsBindingOpenGLESAndroidKHR`
   （display / context / config），用 App 的 EGL context 建纹理；`xrBeginSession/EndSession`。
2. **呈现路径（最大缺口）**：runtime 要拿到一块能画的 `ANativeWindow`。
   华为侧这个窗口由系统 VR 服务给；7T 上只能自己取——可行做法是按 Activity 找
   `SurfaceView.getHolder().getSurface()` → `ANativeWindow_fromSurface`（需手写 JNI 调用表），
   失败则退化为直接 blit 到 `eglGetCurrentSurface(EGL_DRAW)`（ALVR 渲染线程当前的 surface）。
3. **swapchain**：`xrCreateSwapchain` 在 App 的 EGL context 里 `glGenTextures` 出 RGBA8 纹理，
   `xrEnumerateSwapchainImages` 回填 `XrSwapchainImageOpenGLESKHR`；`Acquire/Wait/Release` 做环形队列。
4. **`xrWaitFrame/BeginFrame/EndFrame`**：帧节拍（60/72 Hz），EndFrame 时把两只眼的图层
   用 `glBlitFramebuffer` / 简单着色器合成成 **SBS 1600×3200** 输出到窗口；
   7T 走 USB-C DP AltMode 到眼镜时，系统侧再按 3840×1920 面板映射。
5. **姿态源**：`xrLocateViews` 要真姿态。7T 上眼镜的 IMU 只能从 `/dev/hwvr1` 的 HID 输入报告拿，
   已有的 `hwvrctl` 守护会把四元数写到 `/data/local/tmp/hwvr_pose.sock`；
   runtime 改成读 `AF_UNIX` 或 localhost UDP 即可接上（App 进程受 SELinux 限制，**建议走 localhost UDP**，
   不要直连 `/data/local/tmp`）。
6. **动作/输入**：`XR_HUAWEI_controller_interaction` 的 action 集（至少 head pose 与 3 个按键），
   否则 ALVR 交互初始化会报缺 profile（不影响出画面，但会影响可用性）。
7. 可选：`XR_KHR_android_thread_settings`（设渲染线程优先级）、H.264 硬解分支
   （`[HVR-HISI]` 是华为专用 profile，7T 上会落到软解，能跑但功耗高）。

### 分阶段建议

| 阶段 | 内容 | 验收 |
|---|---|---|
| 一（本次） | 清单瘦身 + 注入 runtime + 阶段一 API | `logcat -s MINIXR` 看到 `createInstance OK`、`getSystemProperties OK`、`enumerateViewConfigurationViews: 2 views 1600x1600` |
| 二 | session + swapchain + frame（无呈现） | 日志过 `xrCreateSession`，swapchain 建出 2×2 张 RGBA8 纹理 |
| 三 | 呈现（窗口获取 + SBS 合成） | 眼镜里出现 ALVR 的 lobby/黑底白字画面 |
| 四 | 姿态 + 输入 | 头部转动画面跟随；手柄/按键有响应 |

---

## 六、真机验证命令

```bash
# 安装（先卸原包，签名不同）
adb uninstall com.lht.huaweivr.alvr
adb install -r out/HuaweiVRGlass-ALVR-7T-v0.1.apk

# 只看我们 runtime 的日志
adb logcat -c && adb shell am start -n com.lht.huaweivr.alvr/.HuaweiVrActivity
adb logcat -s MINIXR HVR-DIAG AndroidRuntime

# 备用方案（不重装 APK 时）
# 把 module/ 整个推到 /data/adb/modules/alvr7t_runtime/ 后重启，看注入日志
adb shell cat /data/local/tmp/alvr7t_inject.log
```

预期（阶段一）：`createInstance OK (mini_xr stage1)` → `getSystemProperties OK` →
`GLES requirements 3.0 .. 3.2` → `loader asked for unimplemented proc: xrCreateSession` →
ALVR 报 `[HVR-DIAG] ... unsupported`。**这是预期行为，说明链条已通到 session 门口。**

---

## 七、风险与回滚

- 签名换了（`CN=hotdogb-vr`），与原包不兼容，**必须先卸载**；卸载会清数据（这个包本来也没数据）。
- runtime 是阶段一，**必然不会出画面**；若装上后闪退，先看 `logcat -s MINIXR AndroidRuntime` 是否有
  `dlopen` 失败（通常是 ABI 或上下文问题，用模块方案可 `ls -lZ` 对比上下文）。
- 「独立运行」的另一层含义（不依赖 PC 端 ALVR server）**这次没有解决**：ALVR 是串流客户端，
  runtime 补完后仍需 PC 上跑 `alvr_server`。若目标是"眼镜单独当显示器"，那是另一条路（SBS 直显），
  与本包无关。

回滚：`adb uninstall com.lht.huaweivr.alvr`，并删掉 `/data/adb/modules/alvr7t_runtime` 后重启。

---

## 八、v0.2 增量：必需 so 归位 + 3DoF/6DoF 数据通路

### 8.1 APK 内到底需要哪些 so

| so | 来源 | 是否必需 | 说明 |
|---|---|---|---|
| `libxr_loader.so` | 原包自带 965 KB | 必需（已有） | 华为定制 loader，内嵌 `library_path=libvrsdk.so` |
| `libalvr_client_openxr.so` | 原包自带 67 MB | 必需（已有） | Rust+wgpu 客户端本体 |
| **`libvrsdk.so`** | **本次自研 48 KB** | **必需（新增）** | runtime，v0.2 已含姿态/空间/视图/会话/帧/交换链 |
| `libWalleDev.so` / `libipcclient.so` / `libipcserver.so` | 原厂包 | **不加** | 它们是华为的 Java-JNI 体系，宿主是 `com.huawei.hvrsdkserverapp`，塞进来没有 Java 侧注册回调，只会增大体积并误导排查 |
| `libEGL.so` / `libGLESv2.so` | 系统 | 无需打包 | ALVR 的 `DT_NEEDED` 已包含，进程里一定在，runtime 直接借符号 |

> runtime 的 `DT_NEEDED` 为空，未定义符号只有 bionic 必然提供的
> `socket/bind/recvfrom/fcntl/close/clock_gettime/nanosleep`、`libEGL` 的
> `eglMakeCurrent/eglCreatePbufferSurface`、`libGLESv2` 的 5 个 `gl*`、以及 `__android_log_print`。
> 已刻意去掉所有 64 位除法，避免引入 `__aeabi_uldivmod` 这类 ARM32 运行库符号导致 dlopen 失败。

### 8.2 3DoF 数据链（已打通）

```
CV10 IMU ──HID 输入报告(report 1)──> /dev/hwvr1
      └> hwvrctl pose 100Hz ── 陀螺积分 + 加速度计 Mahony 校正 ──> 四元数
            ├─> /data/local/tmp/hwvr_pose            （文件，给 shell/脚本看）
            ├─> AF_UNIX /data/local/tmp/hwvr_pose.sock（root 侧，历史通道）
            └─> UDP 127.0.0.1:47110  "w x y z"        （★ App 侧唯一可达通道）
                     └> libvrsdk.so  xrLocateViews ──> XrView.pose.orientation + XrViewState.ORIENTATION_VALID
                              └> ALVR 取两只眼的视图矩阵
```

为什么必须走 UDP：App 是 `untrusted_app`，SELinux 不允许它访问 `/data/local/tmp`，也不允许直读
`/dev/hidraw*`；localhost UDP 是它在 SELinux 下唯一稳定的跨进程通道。runtime 里已经做了非阻塞收包
（每次 `xrWaitFrame` / `xrLocateViews` 前各 poll 一次，最多 8 个数据报，只保留最新姿态）。

### 8.3 6DoF：能接，但要认清楚源

- **眼镜本体只有 3DoF**（CV10 无摄像头，`freeDegree=6dof` 是给 6DoF 手机用的元数据）。
- 7T 上华为那套 6DoF 是 **手机端 SLAM**（`libhcl`/`libhiai` 绑麒麟 NPU + vcom `AT^SET6DOFSTATUS`），
  sm8150 上没有对应硬件与闭源库，**这条路不可复现**。
- 因此 6DoF 在 v0.2 里做成 **"数据就绪即生效"**：报文第 8 个数 bit0=1 表示位置有效，
  runtime 立刻把 `positionTracking` 置 1、`viewStateFlags` 置 3、`XrSpaceLocation.positionValid` 置位，
  并按 IPD 把位置分到左右眼。可用的位置源有两类：
  1. **外部 SLAM 桥**（PC / 另一台手机的 ARCore、或任何 6DoF tracker）往 `47110` 发 8 值报文；
  2. 眼镜 `class=0xff/subclass=0x13` 接口若固件直接吐 6DoF 位姿（当前无驱动、未验证）。
- 想静态喂位置做联调：模块目录放 `pos.conf` 写 `x y z`，service.sh 会以 `HWVR_POS` 传给 hwvrctl。

### 8.4 现场验证（三条命令）

```bash
# 1) 姿态源是否有数据（在模块跑起来后）
adb shell "cat /data/local/tmp/hwvr_pose.log; cat /data/local/tmp/hwvr_pose"
# 2) 广播是否在发（模块里带了探针）
adb shell "/data/adb/modules/alvr7t_runtime/bin/pose_probe"
#    3DoF 会打 [3DoF] q=...（每 10 帧一行）；若位置有效会打 [6DoF] q=... pos=...
# 3) runtime 是否吃到了
adb logcat -s MINIXR HVR-DIAG | grep -E "pose:|LocateViews|position=|6DoF"
#    期望：pose: 监听 udp 127.0.0.1:47110 → pose: 收到首个姿态包 → 每 100 帧一行 pose: q=(...) pos=3DoF
```

### 8.5 v0.2 已完成 / 未完成

已完成：instance、system（positionTracking 随数据源动态变）、双视图 1600×1600、GLES 3.2、
swapchain 格式（RGBA8 优先）、**reference space / locate space / locate views**、session（解析 GLES 绑定）、
begin/end session、wait/begin/end frame（60 Hz 节拍）、**swapchain 真纹理**（在 App 的 EGL context 里
`eglMakeCurrent(pbuffer)` 后 `glGenTextures`+`glTexImage2D`，3 张环形）、acquire/wait/release、姿态接入。

未完成（v0.3）：EndFrame 的**合成与呈现**（要拿 `ANativeWindow` 做 SBS 1600×3200 输出）、
action/输入（`XR_HUAWEI_controller_interaction` 的 action 集）、H.264 硬解分支。

> 也就是说：**现在 3DoF 数据已经进到 ALVR 的视图矩阵里了**，但因为还没有把两只眼的纹理合到
> 眼镜的显示面上，画面仍然不会亮——这是 v0.3 的唯一阻塞点（窗口获取 + 合成）。

---

## 九、v0.3：合成与呈现（画面链路）

### 9.1 怎么拿到"能画的那块面"

华为侧窗口由系统 VR 服务供给，7T 上没有这条通道，所以分两级兜底：

1. **先看当前线程有没有现成的 EGL surface**
   `eglGetCurrentSurface(EGL_DRAW)`。ALVR 在自己的渲染线程调 `xrEndFrame`，如果那时它的
   EGL context 已经绑了窗口 surface，直接复用，零成本。
2. **没有就自己挖窗口**（JNI，不引 `jni.h`，用 `(void**)env[idx]` 直接索引函数表）：
   ```
   Activity.getWindow() → Window.getDecorView()
     → decorView.getChildCount()/getChildAt(i)
       → 第一个能 getHolder() 的 View（NativeActivity 的内容视图是 SurfaceView）
         → SurfaceHolder.getSurface() → ANativeWindow_fromSurface()
   ```
   拿到 `ANativeWindow` 后 `eglCreateWindowSurface(dpy, cfg, anw, 0)`。
   函数表下标用的是规范顺序：`GetObjectClass=31 / GetMethodID=33 / CallObjectMethod=34 /
   CallIntMethod=49`，JavaVM 的 `GetEnv=7`。
   每一步都打日志（`jni: decorView childCount=N`、`jni: 拿到窗口 WxH`、
   `jni: child i 的 Surface 还是 null`），拿不到时能立刻定位是哪一层断了。

### 9.2 合成方式：GL blit 到 SBS，不做着色器

不用写 shader，全部走 GLES3 核心的 `glBlitFramebuffer`：

```
每个眼：把交换链纹理挂到 FBO 的 COLOR_ATTACHMENT0
      → glBindFramebuffer(DRAW, 0)（默认帧缓冲 = 窗口）
      → glBlitFramebuffer(0,0,1600,1600,  eye? w/2..w : 0..w/2, 0..h, COLOR, LINEAR)
最后 eglSwapBuffers(dpy, surf)
```

一次 `xrEndFrame` 里：解析投影层 → `views[0]`/`views[1]` 各自的 `subImage.swapchain` →
用 `xrReleaseSwapchainImage` 时记下的 `last` 下标取到刚提交的那张纹理 → 两张都 blit。
首帧成功会打 `present: 首帧已合成到 3200x1600（左右各 1600）`。

### 9.3 真机验收（按日志判断卡在哪一层）

```bash
adb logcat -s MINIXR HVR-DIAG | grep -E "jni:|present:|swapchain|LocateViews"
```

| 日志 | 含义 | 下一步 |
|---|---|---|
| `swapchain 1600x1600 建出 3 张 RGBA8 纹理` | 交换链 OK | — |
| `present: 首帧已合成到 WxH` | **画面链路通了** | 看眼镜是否亮 |
| `present: 没有可画的 surface` | 线程无 surface 且 JNI 没拿到窗口 | 看下面两行 |
| `jni: decorView childCount=0` | 内容视图还没创建/被移除 | 等 surfaceCreated 后再试（runtime 每帧会重试） |
| `jni: 没找到可用窗口` | NativeActivity 内容视图不是 SurfaceView | 需要改成从 `ANativeActivity` 结构取（下一版） |
| `present: eglMakeCurrent 失败 0x3001` | surface 无效 | 换窗口重试 |

### 9.4 已知风险

- 同一个 EGL context 只能在一个线程 current。`xrEndFrame` 若不在 ALVR 的渲染线程调用，
  我们 `eglMakeCurrent` 会把 context 抢过来，可能让 ALVR 后续 GL 调用报 `EGL_BAD_ACCESS`。
  目前的假设是 EndFrame 就在渲染线程，若真机上出现 GL 报错，把 present 改成独立线程 + 独立 context 共享纹理。
- 窗口尺寸若被系统按 1600×3200 竖屏给，SBS 的左右切分就是上下切分，需要按 `win_w/win_h` 自适应
  （当前已按宽度二分，竖屏时需改成高度二分）。
- 眼镜的 DP 输出由系统合成器管，App 只画到手机窗口；若眼镜侧没画面，问题在 DP/显示配置，不在本 runtime。
