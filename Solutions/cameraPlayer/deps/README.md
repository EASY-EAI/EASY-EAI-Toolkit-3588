# deps — 板端 GPU 运行依赖

GPU 路径（无 UI 的 `-C gpu` 以及 UI 画布合成）需要以下用户态库；
RGA 路径使用板端系统自带的 librga，不依赖本目录。

| 文件 | 作用 |
| --- | --- |
| `libgraphic_lsf.so` | Rockchip GPU 合成前端 |
| `libmali.so.1.9.0` | Mali G610 用户态驱动（EGL/GLES），对接 `/dev/mali0` |
| `libmali-hook.so.1.9.0` | libmali hook（随 mali deb 附带） |

## 安装

```bash
./install_deps.sh ca967bfa2567e6bf
```

脚本会把库推到板端 `/usr/lib/aarch64-linux-gnu/`，建立 `libmali.so` /
`libmali.so.1` 符号链接并刷新链接器缓存。

## 说明

- RK3588 GPU 为 Mali **G610**，内核为闭源 mali 驱动（`/dev/mali0`）。
- 合成使用 dma-buf fd 导入 EGLImage，避免 CPU 拷贝。
