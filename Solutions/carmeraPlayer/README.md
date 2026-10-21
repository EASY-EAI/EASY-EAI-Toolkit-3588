# carmeraPlayer

## 模块介绍

基于 V4L2 + DRM/KMS 的双 MIPI 摄像头显示程序，帧以 dma-buf 在
V4L2 / RGA / GPU / DRM 之间零拷贝流转。各源文件职责：

| 源文件 | 职责 |
| --- | --- |
| `src/main.c` | 程序入口，按 `ENABLE_UI` 分发到 `ui_main` 或 `stitch_main` |
| `src/stitch_main.c` | 无 UI 形态主程序：参数解析、DRM 图层初始化、单摄 dma-buf 直显、双摄 RGA/GPU 半宽拼接与提交、退出清理 |
| `src/ui_main.c` | UI 形态主程序：video + canvas 图层初始化，窗口布局与注册，GPU 画布合成，鼠标 SWAP 交互，双 plane atomic 提交 |
| `src/capture.c` | 采集线程与帧队列：从 V4L2 取帧入队，显示侧消费后释放回驱动 |
| `src/camera/mipi_camera.c` | V4L2 MPLANE 采集封装：NV12 格式协商、REQBUFS/MMAP、EXPBUF 导出 dma-buf、STREAMON，以及 poll + DQBUF/QBUF |
| `src/camera/camera_mode.c` | 3840×2160 时停止 3A，配置所选 IMX415 的 RAW12/60 与 ISP NV12 输出，再启动 3A |
| `src/rga_util.c` | RGA im2d 封装：两路 NV12 各缩半宽后左右拼接，支持 dma-buf fd / 虚拟地址输入 |
| `src/gpu_util.c` | EGL/GLES2 封装：dma-buf 导入纹理、双摄拼接；UI 形态下负责整屏画布与窗口纹理合成 |
| `src/ui_render.c` | CPU 光栅绘制 UI 窗口位图：shell 造型与透明洞、SWAP 按钮、stats 面板、光标 |
| `src/stats.c` | 采样 FPS、CPU / 内存占用与 GPU / RGA 负载，填充 stats 窗口显示数据 |
| `src/mouse.c` | evdev 鼠标线程：维护光标坐标，向 UI 提供点击事件 |

## 数据流

```text
IMX415 → ISP → /dev/video44|53（NV12 dma-buf）
  单摄：dma-buf ───────────────────────────────→ DRM video plane
  双摄：dma-buf → RGA / GPU 半宽拼接 ──────────→ DRM video plane
  UI  ：CPU 光栅窗口帧 → dma-buf → GPU 画布 ──→ DRM canvas plane（叠加在 video 上）
```

帧始终以 dma-buf fd 在模块间传递，不经过 CPU 拷贝。RGA / GPU 合成失败直接
返回错误，不回退到另一种后端。

## 安装依赖（编译前）

编译前须先部署 GPU 依赖库（libmali / libgraphic_lsf）。链接使用 `-lmali`，
链接器只在交叉 sysroot（`CMAKE_SYSROOT`，见 `~/configs/cross.cmake`）中查找
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

产物：`Release/carmeraPlayer`。

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
  由 `-C` 选择 RGA / GPU 合成，结果写入同一个 DRM 图层。

### UI（`./build.sh ui`）

使用两个真实 DRM 图层：video plane 承载双摄拼接视频，canvas plane 是整屏
ARGB8888 画布。视频按 `2 × cam_w × cam_h` 拼接（每路保持原分辨率），再等比
适配到画面中的屏幕区域；画布由 GPU 把各窗口帧贴上去。

默认取两路 camera；`-m` 时只取 camera 0 并镜像到左右两半。`-C` 同样可选
RGA / GPU 拼接后端（默认 GPU）。两个 plane 按连接器枚举的格式能力自动选择。

## 摄像头分辨率与帧率

默认 1920×1080 仍使用内核默认的 1944×1097 RAW12/60 sensor 模式。
指定 `-w 3840 -h 2160` 时，程序先停止 3A，再对所选 `/dev/video44`
或 `/dev/video53` 配置 IMX415 4K RAW12/60 和 ISP NV12；配置成功后启动
3A，再开始取流。退出程序后仍保持 4K 模式。其他尺寸仍只设置 ISP 输出。
`VIDIOC_S_PARM` 不能改变这两个节点的 sensor 帧率。

板上 `00.01.0b` 内核的 CIF 和 ISP 主路径均已实测 4K60。新程序单摄使用
`-n 1 -L 53 -w 3840 -h 2160` 时，日志连续显示 `fps=60.0 errors=0`。

## 执行

将 `Release/carmeraPlayer` 放到板端目录（如 `/userdata/Soul`），在板端终端
进入该目录执行。程序需要 DRM master，运行前先停止桌面：

```sh
systemctl stop lightdm
```

运行 4K 时，程序会自动停止 3A、切换模式，并在配置成功后启动 3A。
退出程序后仍保持 4K 模式；再次运行 4K 时会重复这一切换流程。

无 UI 版本：

```sh
./carmeraPlayer -n 1              # 单摄直显
./carmeraPlayer -n 1 -L 53 -w 3840 -h 2160  # 第二路 4K60 单摄直显
./carmeraPlayer -n 2 -C gpu      # 双摄 GPU 拼接
./carmeraPlayer -n 2 -C rga      # 双摄 RGA 拼接
```

UI 版本：

```sh
./carmeraPlayer                  # 双摄 + UI
./carmeraPlayer -m               # 单摄镜像 + UI
./carmeraPlayer -C rga           # RGA 拼接 + UI
```

如需恢复第二路 1080p，退出程序后手动执行：

```sh
systemctl stop rkaiq_3A.service
media-ctl -d /dev/media3 -V '"m01_b_imx415 7-0036":0[fmt:SGBRG12_1X12/1944x1097]'
v4l2-ctl -d /dev/video53 --set-fmt-video=width=1920,height=1080,pixelformat=NV12
systemctl start rkaiq_3A.service
```

退出后恢复桌面：

```sh
systemctl start lightdm
```

### 参数

| 参数 | 适用模式 | 说明 |
| --- | --- | --- |
| `-n 1\|2` | 无 UI | 摄像头路数，默认 `1` |
| `-m` | UI | 只取 camera 0，镜像到左右两半 |
| `-w` / `-h` | 全部 | 单路采集分辨率，默认 `1920` / `1080` |
| `-C rga\|gpu` | 双摄及 UI | 拼接后端，默认 `gpu`，二者互不回退 |
| `-L` / `-R` | 全部 | 左 / 右 `/dev/videoN` 编号，默认 `44` / `53` |

```sh
carmeraPlayer -n 2 -C gpu -w 1920 -h 1080   # 双摄 GPU 拼接
carmeraPlayer -n 2 -C rga                   # 双摄 RGA 拼接
carmeraPlayer -m                            # UI 单摄镜像
carmeraPlayer -L 44 -R 53                   # 指定左右摄像头
```
