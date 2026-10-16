# deps — 板端运行依赖库

vo_rockit 的"一层多通道"(同一 VO layer 上 chn0/chn1 左右双窗)运行时,
rockit 会走 **GPU composer**(`vo_gpu_composer`)做软件多画面合成,
由此引入三个板上默认缺失的库。缺库时的典型现象:
`Create vo success` 后约 1 秒进程 `Aborted`(SIGABRT),
日志里有 `can't not open libgraphic_lsf.so` 和
`RTMPIGpuComposer:41 Assertion gpu_pgl != RT_NULL failed`。

| 文件 | 大小 | 来源(SDK: /home/chenjianguo/sdk/rk3588_linux6.1) | 作用 |
| --- | --- | --- | --- |
| `librockit.so` | 2.4MB | `external/rockit/lib/lib64/librockit.so` | rockit MPI 主库 |
| `libgraphic_lsf.so` | 176KB | `external/rockit/lib/lib64/libgraphic_lsf.so` | rockit GPU 合成前端(RKGFX_LSF_*) |
| `libmali.so.1.9.0` | 57MB | `debian/packages/arm64/libmali/libmali-valhall-g610-g24p0-x11-wayland-gbm_1.9-1_arm64.deb` 解包 | Mali G610 用户态驱动 blob,提供 EGL/GLES 3.2,对接内核 `/dev/mali0` |
| `libmali-hook.so.1.9.0` | 14KB | 同上 deb 解包 | libmali hook 库(随包附带) |

## 安装

```bash
./install_deps.sh                # 默认设备 192.168.3.90
./install_deps.sh <adb-serial>   # 指定设备
```

脚本行为:push 到板上 `/usr/lib/aarch64-linux-gnu/` → 建 libmali 符号链接 →
chmod 644 → `ldconfig` → 打印校验结果。

## 为什么选 x11-wayland-gbm 变体的 libmali

- RK3588 GPU = Mali **G610 (Valhall)**,内核跑的是 ARM 闭源 mali 驱动
  (`/dev/mali0`,`DRIVER=mali`);
- `x11-wayland-gbm` 变体同时支持三种平台后端,rockit 的 GPU composer
  走的是 **GBM**(`rk-debug init version=gbm...`、`/dev/dri/card0 open`),
  lightdm 停掉(X11 不在)时也能正常创建 EGL 上下文。

## 背景:Mesa 是什么,为什么这块板必须用 libmali

**Mesa 是 Linux 上的开源图形驱动栈**,实现的是应用程序看到的标准图形
API(OpenGL / OpenGL ES / EGL / Vulkan),属于"用户态 GPU 驱动"。整条链路:

```
应用程序 (调 OpenGL/EGL API)
    ↓
Mesa  ←—— 把标准 API 翻译成具体 GPU 的指令(用户态驱动)
    ↓
内核 DRM 驱动 (panthor / panfrost / amdgpu / i915 ...)
    ↓
GPU 硬件
```

要点:

- Mesa **不负责"点亮屏幕"**。显示输出是另一条链路
  (应用提交 buffer → 内核 VOP2/DRM 扫描 → HDMI);Mesa 管的是**渲染**
  ——桌面特效、浏览器合成、播放器 UI、游戏等 GL 应用的像素计算。
- Mesa 的硬件后端只对接**开源内核驱动**(panfrost/panthor 等)。
- 当 Mesa 没有可用的硬件后端时,会退到 **llvmpipe**
  (纯 CPU 软件渲染,功能齐全但慢)。

**本板的实际情况:**

| | Mesa | libmali (ARM) |
| --- | --- | --- |
| 性质 | 开源实现 | ARM 官方**闭源 blob** |
| 对接的内核驱动 | 仅开源驱动(panfrost/panthor 等) | ARM 闭源 mali 驱动 |
| 本板内核驱动 | — | `/dev/mali0`(`DRIVER=mali`)✅ 在跑 |
| 本板实际效果 | 不认识闭源 mali 内核驱动 → **GPU 用不了**,只有 llvmpipe 软渲染 | **硬件加速,G610 真实可用** |

- 本板内核用的是 ARM 闭源 mali 驱动,Mesa 根本驱动不了这块 G610,
  所以 Xorg 桌面的 GL 目前其实是 CPU 在算(桌面 GL 性能弱的原因);
- 想用上真 GPU,本板上**只有 libmali 一条路**;
- rockit 的 GPU composer(`libgraphic_lsf.so`)正是基于 libmali 的
  EGL/GLES 写的——运行日志里 `GL Vendor = ARM`、
  `GL Renderer = Mali-G610`、`init version=gbm` 就是它在通过
  libmali + GBM 建立 EGL 上下文。

## 图形栈全景:两套用户态驱动、后端自动选择与渲染/扫描三段论

本节回答四个问题:rockit 调 GPU 和 Mesa 什么关联?deps 里这几个库是什么关系?
Xorg 桌面的 GL 是什么?Mesa 怎么"自动选后端",渲染到底是 CPU 还是 GPU 在跑?

### rockit 与 Mesa 零关联:一块 GPU,两套用户态驱动

libmali 只对接 ARM **闭源** mali 内核驱动(mali_kbase,uevent 里 `DRIVER=mali`),
Mesa 的硬件后端只对接**开源**内核驱动(panthor/panfrost)。本板内核选了前者,
所以"真 GPU"只有 libmali 一条路;哪天内核换 panthor,局面反过来:
桌面 GL 有硬件加速,libmali 反而用不了。

```
 应用A: Xorg/GTK/浏览器               应用B: vo_rockit (rockit GPU合成)
   │ libEGL.so.1 / libGL.so.1           │
   ▼ (glvnd 调度器)                      ▼
 Mesa 用户态驱动 (开源)               libgraphic_lsf.so
   │ loader 自动选后端:                  │ NEEDED
   │ 读内核绑定驱动名 "mali",            ▼
   │ 查映射表 {amdgpu→radeonsi,        libmali.so.1 (ARM闭源blob)
   │            i915→iris,              │ ioctl
   │            panthor→panfrost,...}   ▼
   │ "mali" ∉ 表 → 无硬件后端         /dev/mali0 (ARM mali内核驱动)
   ▼                                   │  + mali_csffw.bin 固件
 llvmpipe → 纯 CPU 算像素              ▼
   │ (GPU 全程闲着)                  Mali G610 硬件
   ▼
 GBM/dma-buf 缓冲 ────────────────── GBM/dma-buf 缓冲
   └──────────────┬────────────────────┘
                  ▼
     DRM/KMS atomic commit (各自以 DRM master 提交到 plane)
                  ▼
     VOP2 硬件混合各 plane → 扫描 → HDMI
```

### deps 四个库:谁加载谁

```
 vo_rockit
   └ NEEDED → librockit.so        MPI 实现, RK_MPI_* 都在这
        │ 检测到一层多通道 → RTLibraryLoader dlopen
        │ (日志里 "failed to load libgraphic_lsf.so" 就是这个 dlopen 失败)
        ▼
      libgraphic_lsf.so           GPU合成前端: 经GBM建EGL上下文,
                                  每帧把 chn0/1 画进 layer fb
        └ NEEDED → libmali.so.1   EGL/GLES 真正实现 → ioctl /dev/mali0
                    └ 内部加载 mali_csffw.bin (G610 CSF 固件)

 libmali-hook.so.1.9.0            不在这条链上!
                                  它是 deb 里的"转发机构": 供 shim 库/静态注入
                                  (libmali-hook-injector.a) 把 dlopen 进来的
                                  标准库名转发到 libmali。rockit 直连 libmali,
                                  用不到它; push 它只是随包顺手。
```

### Mesa 自动选后端的机制(两级,全自动)

1. **glvnd 层**:Debian 的 `libEGL.so.1`/`libGL.so.1` 本身不是驱动,是调度器,
   按 `/usr/share/glvnd/egl_vendor.d/*.json` 分发给各 vendor 实现。
   本板只有 `50_mesa.json` → 所有 EGL/GL 都进 Mesa。
2. **Mesa loader 层**:建 context 时读 DRM 设备绑定的内核驱动名(udev `DRIVER=`),
   查内置映射表选 gallium 驱动(amdgpu→radeonsi、i915→iris、msm→freedreno、
   panthor→panfrost…)。本板是 `mali`,表里没有 → 回落 swrast = **llvmpipe**。

"自动"指这套探测全自动、无需配置;需要时可干预:
`MESA_LOADER_DRIVER_OVERRIDE`、`LIBGL_ALWAYS_SOFTWARE=1` 等。

**Xorg 桌面的 GL 具体是什么**:Xorg 走 modesetting + **glamor**
(把 X 的 2D 绘图翻译成 GL 调用)→ 上面这套 Mesa → llvmpipe;
GTK/Qt/浏览器的 GL 同样落到 llvmpipe。整个桌面的像素都是 CPU 算的。

本方案只 push 裸 blob、**不装 shim 目录和 ld.so.conf.d 优先级**,
系统的 `libEGL.so.1` 仍解析到 Mesa——桌面不可能"意外"用上 GPU。
想给桌面挂真 GPU,需完整安装 libmali deb(shim + 优先级,让 EGL 解析到 mali shim),
那是全局行为变更,本方案刻意不做。

### 渲染 → 提交 → 扫描:三段论

"CPU/GPU 渲染然后输出给图层"其实是三段,只有第一段有 CPU/GPU 之分:

```
 第①段 渲染(谁算像素)       第②段 提交(挂到plane)      第③段 扫描输出
─────────────────────────  ──────────────────────────  ────────────────
桌面:    llvmpipe (CPU)  →  Xorg(DRM master) 挂plane  →  VOP2 硬件混合扫描
vo_rockit: G610 (libmali)  →  rockit(DRM master) 挂plane →  VOP2 硬件混合扫描
```

- 第①段:两路 1080p 缩放合成,CPU 算会卡、GPU 算轻松——这就是一层多通道
  必须要 libmali 的原因;
- 第②段:只是把算好的缓冲 fd 挂到某个 plane 上(atomic commit);
- 第③段:永远是 VOP2 硬件——多 plane 逐像素混合、扫描到 HDMI,
  不消耗任何 CPU/GPU。

名字上的巧合,别被绕进去:系统里的 `libgbm.so.1`(GBM 缓冲分配器)也是
Mesa 项目维护的,rockit 合成链上经 GBM 分配缓冲可能经过它——但那只是
"分配器助手"角色,与渲染后端选择完全无关。
"链上有 Mesa 家族的库" ≠ "Mesa 驱动在渲染"。

### 上板自检

```bash
adb -s 192.168.3.90 shell 'grep DRIVER /sys/class/drm/card0/device/uevent'   # → DRIVER=mali
adb -s 192.168.3.90 shell 'ls /usr/share/glvnd/egl_vendor.d/'                # → 只有 50_mesa.json
# 桌面会话里(需装 mesa-utils): glxinfo -B | grep -i renderer                  # → llvmpipe (LLVM ...)
```

第一条输出 `DRIVER=mali` 就等于宣判:Mesa 的映射表里查无此人,桌面 GL 必然是 llvmpipe。

## 与系统图形栈的关系

- 只**新增**这 4 个文件,不替换任何系统库;Mesa 的 `libEGL.so.1`、
  Xorg 的渲染路径完全不受影响;
- `libgraphic_lsf.so` 按 NEEDED 直接加载 `libmali.so.1`(不经过 Mesa),
  两者互不干扰:rockit/GPU 加速走 libmali,Xorg 桌面继续走 Mesa(软渲染);
- libmali 自身依赖 libdrm/libwayland/libX11/libxcb 等,Debian 桌面
  rootfs 上已齐备,无需额外安装。

## 换板/换 SDK 时注意

- libmali 变体要和 GPU 型号、内核驱动匹配:G610+闭源 mali 内核驱动
  → `libmali-valhall-g610-g24p0-*`;若内核换用 panthor 开源驱动,
  则应改用 Mesa(panthor),不需要 libmali;
- librockit.so / libgraphic_lsf.so 必须与 rockit 版本配套
  (本套来自 rockit git-d8c637244, built 2025-03-06)。
