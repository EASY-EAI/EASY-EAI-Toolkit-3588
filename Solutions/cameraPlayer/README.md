# cameraPlayer

## 模块介绍

基于 V4L2 + DRM/KMS 的双 MIPI 摄像头显示程序，帧以 dma-buf 在
V4L2 / RGA / GPU / DRM 之间零拷贝流转。各源文件职责：

| 源文件 | 职责 |
| --- | --- |
| `src/main.c` | 程序入口，按 `ENABLE_UI` 分发到 `ui_main` 或 `stitch_main` |
| `src/stitch_main.c` | 无 UI 形态主程序：参数解析、DRM 图层初始化、单摄 dma-buf 直显、双摄 RGA/GPU 半宽拼接（输出 2 块 buffer 轮转）与提交、退出清理 |
| `src/ui_main.c` | UI 形态主程序：video + canvas 图层初始化，窗口布局与注册，GPU 画布合成，鼠标 SWAP 交互，双 plane atomic 提交 |
| `src/capture.c` | 采集线程与帧队列：从 V4L2 取帧入队，显示侧消费后释放回驱动；显示侧只取最新帧，积压的旧帧立即归还，延迟不随队列深度累积 |
| `src/camera/mipi_camera.c` | V4L2 MPLANE 采集封装：NV12 格式协商、REQBUFS/MMAP、EXPBUF 导出 dma-buf、STREAMON，以及 poll + DQBUF/QBUF |
| `src/camera/camera_discover.c` | 扫描 media 拓扑找 live ISP 主路径：确认 `rkcif-*` → `rkisp-isp-subdev`:0 已使能，映射到 `rkisp_mainpath` 并识别 sensor，供 `-L/-R` 缺省值使用 |
| `src/rga_util.c` | RGA im2d 封装：两路 NV12 各缩半宽后左右拼接或单摄整幅缩放，支持 dma-buf fd / 虚拟地址输入 |
| `src/gpu_util.c` | EGL/GLES2 封装：dma-buf 导入纹理、双摄拼接与单摄整幅；UI 形态下负责整屏画布与窗口纹理合成 |
| `src/ui_render.c` | CPU 光栅绘制 UI 窗口位图：shell 造型与透明洞、SWAP 按钮、stats 面板、光标 |
| `src/stats.c` | 采样 FPS、CPU / 内存占用与 GPU / RGA 负载，填充 stats 窗口显示数据 |
| `src/mouse.c` | evdev 鼠标线程：维护光标坐标，向 UI 提供点击事件 |

## 数据流

```text
IMX415 → ISP → rkisp_mainpath /dev/videoN（NV12 dma-buf，运行时发现）
  单摄：dma-buf ───────────────────────────────→ DRM video plane
  双摄：dma-buf → RGA / GPU 半宽拼接 → 2 块缓冲轮转 → DRM video plane
  UI  ：CPU 光栅窗口帧 → dma-buf → GPU 画布 ──→ DRM canvas plane（叠加在 video 上）
```

帧始终以 dma-buf fd 在模块间传递，不经过 CPU 拷贝。RGA / GPU 合成失败直接
返回错误，不回退到另一种后端。

## 安装依赖（编译前）

编译前须先部署 GPU 依赖库（libmali / libgraphic_lsf）。链接使用 `-lmali`，
链接器只在交叉 sysroot（`CMAKE_SYSROOT`）中查找
`libmali.so`：install_deps.sh 通过 adb 把库推到板端并建立符号链接，板端根
文件系统作为 sysroot 时即同步生效；若 sysroot 不是板端根文件系统，需把同样
的库和符号链接放进 sysroot。未部署时链接阶段会报 `cannot find -lmali`。

```sh
./install_deps.sh                    # 默认设备 192.168.3.90:5555
./install_deps.sh 192.168.3.143      # 按 IP 部署（自动 adb connect，默认端口 5555）
./install_deps.sh 192.168.3.143:5555 # 指定 IP 与端口
./install_deps.sh ca967bfa2567e6bf   # USB 设备号
```

RGA 路径使用板端系统自带的 librga，无需额外安装。

## 编译

```sh
./build.sh          # 默认无 UI
./build.sh ui       # 启用 UI 绘制（定义 ENABLE_UI）
./build.sh clear    # 清理 build/ 与 Release/
```

产物：`Release/cameraPlayer`。

`ENABLE_UI` 由 CMake 选项控制，默认 OFF；`./build.sh ui` 传入
`-DENABLE_UI=ON`。未定义时入口为 `src/stitch_main.c`，定义后入口为
`src/ui_main.c`，`ui_render/stats/mouse` 的代码由 `#ifdef ENABLE_UI` 屏蔽。

## 目录结构

| 路径 | 说明 |
| --- | --- |
| `src/` | 程序源码（各文件职责见「模块介绍」） |
| `include/` | 对外头文件 |
| `deps/` | 板端 GPU 运行依赖库 |
| `install_deps.sh` | 通过 adb 部署 GPU 依赖到板端 |
| `build.sh` | 构建脚本 |

## 运行模式

### 无 UI（默认）

- `-n 1`（默认）：只取一路 camera，NV12 dma-buf 直接挂 DRM 图层，零拷贝显示，
  不经过 RGA / GPU。
- `-n 2`：取两路 camera，各缩到一半宽度后左右拼接为 `cam_w × cam_h`
  （如 1920×1080 + 1920×1080 → 960×1080 + 960×1080 → 1920×1080），
  由 `-C` 选择 RGA / GPU 合成，结果写入 DRM 图层。输出用 2 块 buffer 轮转：
  DRM 扫描一块时合成写另一块，避免画面里的运动物体出现撕裂。UI 形态仍是
  单块 video buffer，不参与轮转。

### UI（`./build.sh ui`）

使用两个真实 DRM 图层：video plane 承载双摄拼接视频，canvas plane 是整屏
ARGB8888 画布。视频按 `cam_w × cam_h` 输出：每路先缩到半宽再左右拼接
（与无 UI 双摄一致），再等比适配到画面中的屏幕区域；画布由 GPU 把各窗口
帧贴上去。程序约每秒打印一次 `single` / `dual: fps=...` 日志。

默认取两路 camera；`-m` 时只取 camera 0，整帧铺满视频区，不做左右分屏。
`-C` 同样可选 RGA / GPU 拼接后端（默认 GPU）。两个 plane 按连接器枚举的
格式能力自动选择。

## 摄像头节点发现

程序不使用固定节点号。启动时扫描 `/dev/media0..31`：

1. 找 driver 为 `rkisp*` 的 media 设备；
2. 在该 media 图中找 `rkisp-isp-subdev` 与 `rkisp_mainpath`；
3. 遍历各实体的出边，存在 `rkcif-*` → `rkisp-isp-subdev`:0 的 ENABLED
   链接才视为 live（`MEDIA_IOC_ENUM_LINKS` 只返回实体自身发出的链接，
   因此从 CIF 实体的出边检查）；
4. 由 `rkisp_mainpath` 实体的 dev minor 得到 `/dev/videoN`；
5. 在 model/bus_info 与 `rkcif-*` 同名的 CIF media 设备中找 sensor 实体，
   按 sensor 名（`m00...`、`m01...`）排序后依次分配给 camera 0/1。

排序保证重启后即使节点号变化，camera 0/1 仍对应同一路 sensor。`-L` / `-R`
可覆盖自动结果；只指定其中一个时，另一个仍自动补位。发现不到足够的 live
mainpath 时程序直接报错，不再回退到固定节点号；UI 形态下只发现一路时退回
单摄整幅模式。

换 MIPI 接口不需要改代码，节点号仍由上面的发现流程决定。但双路要跑满 60fps
需要两路分别挂在 ISP0 和 ISP1 上（例如一路走 CSI、另一路走 DC-PHY1）：两路
共用同一个 ISP 时主路径按时间片轮转，双摄拼接帧率到不了 60。

## 执行

将 `Release/cameraPlayer` 放到板端目录（如 `/userdata/Soul`），在板端终端
进入该目录执行。程序需要 DRM master，运行前先停止桌面：

```sh
systemctl stop lightdm
```

1080p 与 4K 使用同一套流程：驱动决定 sensor 模式，应用只设置 V4L2 输出，
不调用 `media-ctl`，也不重启 3A。

无 UI 版本：

```sh
./cameraPlayer -n 1              # 单摄直显
./cameraPlayer -n 1 -w 3840 -h 2160        # 第一路 4K60 单摄直显
./cameraPlayer -n 1 -L 53 -w 3840 -h 2160  # 指定第二路（节点号以发现日志为准）
./cameraPlayer -n 2 -C gpu      # 双摄 GPU 拼接
./cameraPlayer -n 2 -C rga      # 双摄 RGA 拼接
```

UI 版本：

```sh
./cameraPlayer                  # 双摄 + UI
./cameraPlayer -m               # 单摄整幅 + UI
./cameraPlayer -C rga           # RGA 拼接 + UI
```

退出后恢复桌面：

```sh
systemctl start lightdm
```

### 参数

| 参数 | 适用模式 | 说明 |
| --- | --- | --- |
| `-n 1\|2` | 无 UI | 摄像头路数，默认 `1` |
| `-m` | UI | 只取 camera 0，整帧显示（不做左右分屏） |
| `-w` / `-h` | 全部 | 单路采集分辨率，默认 `1920` / `1080` |
| `-C rga\|gpu` | 双摄及 UI | 拼接后端，默认 `gpu`，二者互不回退 |
| `-L` / `-R` | 全部 | 覆盖左 / 右 `/dev/videoN`；缺省自动发现并按 sensor 名排序分配 |

```sh
cameraPlayer -n 2 -C gpu -w 1920 -h 1080   # 双摄 GPU 拼接
cameraPlayer -n 2 -C rga                   # 双摄 RGA 拼接
cameraPlayer -m                            # UI 单摄整幅
cameraPlayer -L 44 -R 53                   # 显式指定左右摄像头
```
