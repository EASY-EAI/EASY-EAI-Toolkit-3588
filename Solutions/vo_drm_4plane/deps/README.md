# deps — 板端可选运行依赖库

`vo_drm_4plane` **已不再依赖 rockit / `RK_MPI_*`**（采集为裸 V4L2，
显示为 DRM atomic）。默认 `-C rga` 只需板端系统库：

- `libdrm`
- `librga`（`-C rga`，板端通常已有 `/usr/lib/.../librga.so`）

本目录仅保留后续 **`-C gpu`（Mali GBM/EGL）** 可能用到的用户态库；
当前 GPU 路径仍为 API 桩，未链接这些库。

| 文件 | 作用 |
| --- | --- |
| `libgraphic_lsf.so` | Rockchip GPU 合成前端（`RKGFX_LSF_*`），供未来 GPU 路径参考 |
| `libmali.so.1.9.0` | Mali G610 用户态驱动（EGL/GLES），对接 `/dev/mali0` |
| `libmali-hook.so.1.9.0` | libmali hook（随 mali deb 附带） |

> `librockit.so` 已删除：本 demo 不再链接 / 部署 rockit。

## 安装（仅 GPU 实验需要）

```bash
./install_deps.sh ca967bfa2567e6bf
```

脚本会把 mali / graphic_lsf 推到板上 `/usr/lib/aarch64-linux-gnu/`，
并建立 `libmali.so` 符号链接。**不影响** 当前 rga 路径。

## 说明

- RK3588 GPU = Mali **G610**，内核闭源 mali 驱动（`/dev/mali0`）。
- 板端系统 `librga` 已足够支撑 `-C rga` 的 dma-buf / VIR 双路径。
- 若实现真实 GPU 合成，优先用 dma-buf fd（V4L2 `VIDIOC_EXPBUF`、
  DRM `drmPrimeHandleToFD`）导入 GLES 纹理，避免 CPU 拷贝。
