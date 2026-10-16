#ifndef GPU_UTIL_H
#define GPU_UTIL_H

/**
 * GPU（Mali G610）合成工具接口。
 *
 * 数据流转由 GpuBuffer.mem_type 选择：
 *   - GPU_MEM_FD ：dma-buf fd，硬件模块间零拷贝（推荐）
 *   - GPU_MEM_VIR：CPU 虚拟地址（调试 / 无 FD 时）
 * 与 rga_util 对称；选中本路径时不得回退 CPU 拼接。
 *
 * 另提供 gpu_ui_* 一组 2D 接口：把窗口帧的 ARGB8888 dma-buf 导入为纹理，
 * 再按窗口矩形贴到全屏画布的 FBO 上。
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** 像素格式（与本 demo DRM / V4L2 常用格式对齐，数值可自定）。 */
typedef enum {
    GPU_FMT_NV12 = 0,     /* Y/UV 4:2:0 semi-planar，对应 DRM NV12 */
    GPU_FMT_NV21,         /* Y/VU 4:2:0 */
    GPU_FMT_ARGB8888,     /* DRM_FORMAT_ARGB8888 */
    GPU_FMT_XRGB8888,     /* DRM_FORMAT_XRGB8888 */
    GPU_FMT_UNKNOWN = 255,
} GpuFormat;

/** 缓冲区内存类型：一次只应填充对应字段。 */
typedef enum {
    GPU_MEM_FD = 0,  /* dma-buf fd；零拷贝导入 GLES 纹理时优先 */
    GPU_MEM_VIR,     /* CPU 虚拟地址；桩/调试可用，量产路径不推荐 */
} GpuMemType;

/**
 * 一帧图像缓冲描述。
 *
 * @param mem_type  内存类型，决定 fd / vir 哪个有效
 * @param fd        dma-buf 文件描述符；mem_type==GPU_MEM_FD 时有效，且 >= 0
 * @param vir       CPU 虚拟地址；mem_type==GPU_MEM_VIR 时有效，不可为 NULL
 * @param width     有效像素宽度，> 0
 * @param height    有效像素高度，> 0
 * @param stride    行跨距（字节）；0 表示按 width 与 format 推算
 * @param format    像素格式
 */
typedef struct {
    GpuMemType mem_type;
    int fd;
    void *vir;
    int width;
    int height;
    int stride;
    GpuFormat format;
} GpuBuffer;

/**
 * 合成层：一路输入及其在输出画布上的目标矩形。
 *
 * @param src       输入缓冲，不可为 NULL
 * @param dst_x     输出画布上的目标左上角 X，>= 0
 * @param dst_y     输出画布上的目标左上角 Y，>= 0
 * @param dst_w     目标宽度，> 0
 * @param dst_h     目标高度，> 0
 */
typedef struct {
    const GpuBuffer *src;
    int dst_x;
    int dst_y;
    int dst_w;
    int dst_h;
} GpuLayer;

/**
 * 初始化 GPU 合成会话。
 *
 * 内部自行打开 DRM 设备（/dev/dri/card0）建 GBM + EGL surfaceless 上下文，
 * 与显示链路（drm_display / display_win）互不复用；deinit 时关闭。
 *
 * @return 0 成功；负值失败
 */
int gpu_composer_init(void);

/** 释放 GPU 合成会话。未 init 或已 deinit 时为空操作。 */
void gpu_composer_deinit(void);

/**
 * 将左右两路画面水平拼接到 dst。
 *
 * @param left   左路输入（swap 非 0 时作为右路）
 * @param right  右路输入（swap 非 0 时作为左路）；宽高建议与 left 一致
 * @param dst    拼接输出；宽度通常为单路宽的 2 倍，由调用方预先分配
 * @param swap   非 0 时交换左右输入后再拼接
 * @return 0 成功；负值参数非法或 GPU 合成失败
 */
int gpu_compose_side_by_side(const GpuBuffer *left, const GpuBuffer *right,
                             GpuBuffer *dst, int swap);

/**
 * 将多层按各自目标矩形合成到 dst。
 * layers 按数组顺序从底到顶绘制；超出 dst 的区域由实现裁剪。
 *
 * @param layers      层数组，不可为 NULL
 * @param layer_count 层数，> 0
 * @param dst         输出缓冲，由调用方预先分配
 * @return 0 成功；负值参数非法或 GPU 合成失败
 */
int gpu_compose_layers(const GpuLayer *layers, int layer_count, GpuBuffer *dst);

/* ---------------- UI 2D：窗口帧贴图到画布 ---------------- */

/** UI 顶点：位置为画布像素坐标（原点左上、y 轴向下），uv 归一化到 0..1。 */
typedef struct {
    float x, y;
    float u, v;
} GpuUiVertex;

/**
 * 把一个 ARGB8888 dma-buf 导入为 GL_TEXTURE_2D 并返回纹理 id。
 *
 * 纹理常驻，内容随该 dma-buf 内存变化；不再使用时须 gpu_ui_release_texture。
 * 需先 gpu_composer_init 成功。
 *
 * @param src 源缓冲，必须 GPU_MEM_FD 且格式为 ARGB8888 / XRGB8888
 * @return 非 0 纹理 id；0 失败
 */
unsigned gpu_ui_import_texture(const GpuBuffer *src);

/** 释放 gpu_ui_import_texture 返回的纹理；0 或未知 id 为空操作。 */
void gpu_ui_release_texture(unsigned tex);

/**
 * 开始一次画布绘制：把 dst 的 dma-buf 导入为 FBO，以全透明清屏后绑定 UI
 * 程序与混合状态。与 gpu_ui_end 成对使用。
 *
 * @param dst 画布，必须 GPU_MEM_FD 且格式为 ARGB8888 / XRGB8888
 * @return 0 成功；负值失败
 */
int gpu_ui_begin(const GpuBuffer *dst);

/** 结束画布绘制：glFinish 后释放本次导入的 FBO / 纹理 / EGLImage。 */
void gpu_ui_end(void);

/** 等待 GPU 完成已提交的绘制（CPU 改写过窗口帧后、RGA 覆盖画布前调用）。 */
void gpu_ui_flush(void);

/** 设置当前绘制颜色（0xAARRGGBB），供 gpu_ui_draw 使用。 */
void gpu_ui_color(uint32_t argb);

/** 绑定 gpu_ui_draw 采样用的纹理；0 表示不绑定。 */
void gpu_ui_texture(unsigned tex);

/** 按 GpuUiVertex 数组绘制三角形，颜色与纹理取当前 gpu_ui_color/texture 设置。 */
void gpu_ui_draw(const GpuUiVertex *verts, int count);

/** 把纹理按窗口矩形贴到画布（内部按 (x,y,w,h) 生成 uv 0..1 的四边形）。 */
void gpu_ui_blit(unsigned tex, float x, float y, float w, float h);

#ifdef __cplusplus
}
#endif

#endif /* GPU_UTIL_H */
