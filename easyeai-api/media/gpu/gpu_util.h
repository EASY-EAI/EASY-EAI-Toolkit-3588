#ifndef GPU_UTIL_H
#define GPU_UTIL_H

/**
 * GPU（Mali G610）合成工具接口。
 *
 * 输入/输出均使用 dma-buf fd，硬件模块间零拷贝；
 * 拼接失败直接返回负值，不回退 CPU / RGA。
 *
 * UI 2D 接口（gpu_ui_*）实现在 gpu_ui.c：把窗口帧的 ARGB8888 dma-buf
 * 导入为纹理，再按窗口矩形贴到全屏画布的 FBO 上。使用方按需把该文件
 * 加入编译，模块内不使用任何编译开关宏。
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "log_manager_pro.h"

/** 像素格式（与 DRM / V4L2 常用格式对齐，数值可自定）。 */
typedef enum {
    GPU_FMT_NV12 = 0,     /* Y/UV 4:2:0 semi-planar，对应 DRM NV12 */
    GPU_FMT_ARGB8888,     /* DRM_FORMAT_ARGB8888 */
    GPU_FMT_XRGB8888,     /* DRM_FORMAT_XRGB8888 */
    GPU_FMT_UNKNOWN = 255,
} GpuFormat;

/**
 * 一帧图像缓冲描述。
 *
 * @param fd        dma-buf 文件描述符，须 >= 0
 * @param width     有效像素宽度，> 0
 * @param height    有效像素高度，> 0
 * @param stride    行跨距（字节）；0 表示按 width 与 format 推算
 * @param format    像素格式
 */
typedef struct {
    int fd;
    int width;
    int height;
    int stride;
    GpuFormat format;
} GpuBuffer;

/**
 * 初始化 GPU 合成会话。
 *
 * 内部自行打开 DRM 设备（/dev/dri/card0）建 GBM + EGL surfaceless 上下文，
 * 与显示链路（display_pro / display_win）互不复用；deinit 时关闭。
 *
 * @param log_mgr 日志管理器：init 时在它下面注册本模块日志句柄（"gpu"），
 *                gpu_util.c / gpu_ui.c 共用；传 LOG_MGR_INVALID 关闭日志
 * @return 0 成功；负值失败
 */
int gpu_composer_init(log_mgr_t log_mgr);

/** 释放 GPU 合成会话。未 init 或已 deinit 时为空操作。 */
void gpu_composer_deinit(void);

/**
 * 将左右两路画面水平拼接到 dst：左路铺满 dst 左半、右路铺满右半，
 * 源帧按各自半边尺寸自动缩放。
 *
 * 常见用法：
 *   - dst 宽 = cam_w    ：每路缩为 cam_w/2，合成后仍占一路分辨率；
 *   - dst 宽 = 2 * cam_w：每路保持原尺寸贴到左右半屏。
 *
 * @param left   左路输入（swap 非 0 时作为右路）
 * @param right  右路输入（swap 非 0 时作为左路）；宽高建议与 left 一致
 * @param dst    拼接输出，由调用方预先分配；宽度须 >= 2
 * @param swap   非 0 时交换左右输入后再拼接
 * @return 0 成功；负值参数非法或 GPU 合成失败
 */
int gpu_compose_side_by_side(const GpuBuffer *left, const GpuBuffer *right,
                             GpuBuffer *dst, int swap);

/**
 * 将单路画面铺满 dst，用于 UI 的 -m 单摄整幅显示（不做左右分屏）。
 *
 * @param src 输入 NV12 缓冲
 * @param dst 输出 ARGB8888 / XRGB8888 缓冲，由调用方预先分配
 * @return 0 成功；负值参数非法或 GPU 合成失败
 */
int gpu_compose_single(const GpuBuffer *src, GpuBuffer *dst);

/* ---------------- UI 2D：窗口帧贴图到画布（gpu_ui.c） ---------------- */

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
 * @param src 源缓冲，必须格式为 ARGB8888 / XRGB8888
 * @return 非 0 纹理 id；0 失败
 */
unsigned gpu_ui_import_texture(const GpuBuffer *src);

/** 释放 gpu_ui_import_texture 返回的纹理；0 或未知 id 为空操作。 */
void gpu_ui_release_texture(unsigned tex);

/**
 * 开始一次画布绘制：把 dst 的 dma-buf 导入为 FBO（首次创建后复用），
 * 以全透明清屏后绑定 UI 程序与混合状态。与 gpu_ui_end 成对使用。
 *
 * @param dst 画布，必须格式为 ARGB8888 / XRGB8888
 * @return 0 成功；负值失败
 */
int gpu_ui_begin(const GpuBuffer *dst);

/** 结束画布绘制：glFinish 后解绑 FBO，画布资源保留供下次复用。 */
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

/** 释放 gpu_ui.c 创建的全部 GL 资源；须在 gpu_composer_deinit 之前调用。 */
void gpu_ui_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* GPU_UTIL_H */
