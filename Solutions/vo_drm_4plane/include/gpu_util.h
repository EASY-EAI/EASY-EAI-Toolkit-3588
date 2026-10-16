#ifndef GPU_UTIL_H
#define GPU_UTIL_H

/**
 * GPU（Mali G610）合成工具接口。
 *
 * 数据流转由 GpuBuffer.mem_type 选择：
 *   - GPU_MEM_FD ：dma-buf fd，硬件模块间零拷贝（推荐）
 *   - GPU_MEM_VIR：CPU 虚拟地址（调试 / 无 FD 时）
 * 与 rga_util 对称；选中本路径时不得回退 CPU 拼接。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * 初始化 GPU 合成会话。
 *
 * 内部自行打开 DRM 设备（/dev/dri/card0）供 GBM 使用，并在 deinit 时关闭，
 * 调用方不需要（也不应）传入 fd。
 *
 * @return 0 成功；负值失败
 */
int gpu_composer_init(void);

/** 释放 GPU 合成会话（含内部打开的 DRM fd）。未 init 或已 deinit 时为空操作。 */
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

/* ---------------- UI 绘制（几何 + 文字） ----------------
 * UI 画布按 DRM_FORMAT_ARGB8888 组织，全部由 GPU 绘制：圆角矩形（SDF 覆盖度）、
 * 纯色矩形 / 梯形等多边形、字形贴图、以及用 scissor+glClear 挖出的透明洞。
 */

/**
 * UI 画布顶点：位置为画布像素坐标（原点在左上角），uv 为纹理坐标。
 * 与 gpu_ui_color() 的单色配合即可覆盖本 demo 的全部 2D 图元。
 */
typedef struct {
    float x, y; /* 画布像素坐标 */
    float u, v; /* 纹理坐标；实心几何取内置白纹理中心 */
} GpuUiVertex;

/** gpu_ui_texture() 传入本值时使用内置 1x1 白纹理（实心几何）。 */
#define GPU_UI_TEX_WHITE 0u

/**
 * 开始一次 UI 画布绘制：把 dst 的 dma-buf 导入为 FBO，并设置 2D 混合状态。
 * 之后依次调用 gpu_ui_rounded_rect / gpu_ui_draw 等，最后 gpu_ui_end。
 *
 * @param dst  目标画布，须为 ARGB8888 / XRGB8888 且 mem_type==GPU_MEM_FD
 * @return 0 成功；负值参数非法或 GPU 失败
 */
int gpu_ui_begin(const GpuBuffer *dst);

/** 结束一次 UI 画布绘制：glFinish 后释放 FBO / 纹理 / EGLImage。 */
void gpu_ui_end(void);

/**
 * 等待 GPU 绘制完成（glFinish）；画布随后交由其他引擎读取前须调用。
 */
void gpu_ui_flush(void);

/** 设置后续绘制使用的颜色，字序同 DRM_FORMAT_ARGB8888：A<<24|R<<16|G<<8|B。 */
void gpu_ui_color(uint32_t argb);

/** 绑定后续绘制采样的纹理：纹理 id，或 GPU_UI_TEX_WHITE 表示内置白纹理。 */
void gpu_ui_texture(unsigned tex);

/** 以 GL_TRIANGLES 提交一批顶点（实心几何或纹理四边形）。 */
void gpu_ui_draw(const GpuUiVertex *verts, int count);

/**
 * 绘制抗锯齿圆角矩形。
 *
 * @param radius  圆角半径；允许大于 min(w,h)/2（按 SDF 自然退化）
 * @param border  > 0 时只画宽度为 border 的内描边，0 表示实心填充
 */
void gpu_ui_rounded_rect(float x, float y, float w, float h,
                         float radius, float border, uint32_t argb);

/**
 * 把矩形区域擦成全透明（RGBA=0）：在 ui 画布上挖透明洞，露出下层 video 等 plane。
 *
 * 圆角矩形走 over 混合，无法把已绘像素的 alpha 还原为 0，故此处用 scissor +
 * glClear 直接覆盖。须在 gpu_ui_begin 之后调用，坐标为画布像素坐标系。
 */
void gpu_ui_erase_rect(float x, float y, float w, float h);

/**
 * 上传一张 RGBA8888 纹理（NEAREST 采样 / CLAMP_TO_EDGE），用于字形图集。
 *
 * @return 纹理 id（> 0）；0 表示失败
 */
unsigned gpu_ui_upload_texture(int width, int height, const uint8_t *rgba);

#ifdef __cplusplus
}
#endif

#endif /* GPU_UTIL_H */
