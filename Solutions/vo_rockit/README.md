# vo_rockit

## 模块介绍

基于 rockit MPI（RK_MPI_VI / RK_MPI_VO）的 VI 取流直送 VO 显示 demo。取流
与显示拆分为 capturer / player 两个线程，通过深度 2 的零拷贝帧队列交接：
采集线程只负责取帧入队，显示线程出队 SendFrame 后归还 VI buffer，取流不会
被 VO 阻塞。支持一层多通道双摄左右显示、单摄镜像（`-m`）、UI 覆盖层
（`-u`），运行中可用 `kill -USR1 <pid>` 对调左右画面。

## 编译

```sh
./build.sh          # 交叉编译，产物：Release/vo_rockit
./build.sh clear    # 清理 build/ 与 Release/
```

## 执行

程序需要 DRM master，运行前先停止桌面（首次运行先执行 `./install_deps.sh` 安装 rockit 依赖库）：

```sh
adb -s ca967bfa2567e6bf shell systemctl stop lightdm
adb -s ca967bfa2567e6bf push Release/vo_rockit /userdata/Soul/
adb -s ca967bfa2567e6bf shell /userdata/Soul/vo_rockit -w 1920 -h 1080 -I 0 -d 1 -l 1 -t 2
```

退出后恢复桌面：

```sh
adb -s ca967bfa2567e6bf shell systemctl start lightdm
```

### 参数

| 参数 | 说明 |
| --- | --- |
| `-w` / `-h` | VI 采集宽 / 高（默认 1024 / 600） |
| `-I` | VI 通道号（默认 0） |
| `-d` | VO device（即 vp 号，默认 0；本板 HDMI 在 vp1，用 `-d 1`） |
| `-l` | VO layer（默认 -1，自动取与 `-d` 相同；layer 必须属于该 vp，HDMI 用 `-l 1`） |
| `-t` | VO 接口类型（默认 0）：0:auto 1:MIPI 2:HDMI 3:LCD 4:HDMI1 5:MIPI1 |
| `-W` / `-H` | VO 显示宽 / 高（默认 0，与采集分辨率一致） |
| `-m` | 单摄镜像（仅用 VI dev0，同一路画面进左右两窗） |
| `-u` | UI 覆盖层开关（默认 1 开启，`-u 0` 关闭后纯视频显示） |

```sh
vo_rockit -w 1920 -h 1080 -d 1 -l 1 -t 2   # 双摄左右，输出到本板 HDMI
vo_rockit -m                                # 单摄镜像
```

运行中对调左右画面：`kill -USR1 <pid>`。
