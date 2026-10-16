# vo_drm_4plane

## 模块介绍

双 MIPI 摄像头左右拼接显示 demo，使用 4 个真实 DRM 图层叠放：video 在最底
（双摄拼接视频），其上是 ui（zpos 9，含透明洞的整屏界面）/ stats（10，
CPU/RGA/GPU 占用状态栏）/ cursor（11，鼠标光标），后三层全部由 GPU 绘制
几何、文字与纯色矩形。拼接后端可选 RGA / GPU（默认 GPU），图层按连接器
枚举的格式能力自动选择。

## 编译

```sh
./build.sh          # 交叉编译，产物：Release/vo_drm_4plane
./build.sh clear    # 清理 build/ 与 Release/
```

## 安装依赖（首次运行）

GPU 渲染依赖板端 libmali / libgraphic_lsf，用 install_deps.sh 推送并建好符号链接：

```sh
./install_deps.sh                    # 默认设备 192.168.3.90:5555
./install_deps.sh 192.168.3.143      # 按 IP 部署（自动 adb connect，默认端口 5555）
./install_deps.sh 192.168.3.143:5555 # 指定 IP 与端口
./install_deps.sh ca967bfa2567e6bf   # USB 设备号
```

## 执行

程序需要 DRM master，运行前先停止桌面：

```sh
adb -s ca967bfa2567e6bf shell systemctl stop lightdm
adb -s ca967bfa2567e6bf push Release/vo_drm_4plane /userdata/Soul/
adb -s ca967bfa2567e6bf shell /userdata/Soul/vo_drm_4plane -w 1920 -h 1080
```

退出后恢复桌面：

```sh
adb -s ca967bfa2567e6bf shell systemctl start lightdm
```

### 参数

| 参数 | 说明 |
| --- | --- |
| `-w` / `-h` | 单路摄像头分辨率（默认 1920 / 1080） |
| `-m` | 单摄镜像（只取 camera 0，镜像到左右两窗） |
| `-C rga\|gpu` | 双摄拼接后端，默认 `gpu`，二者互不回退 |
| `-L` / `-R` | 左 / 右 `/dev/videoN` 号（默认 44 / 53） |

```sh
vo_drm_4plane -w 1920 -h 1080          # 双摄左右，默认 GPU 拼接
vo_drm_4plane -m                       # 单摄镜像
vo_drm_4plane -C rga -w 1920 -h 1080   # RGA 拼接视频
```

四个 DRM 图层（video / ui / stats / cursor）由程序按连接器枚举的格式能力自动选择，无需指定。
