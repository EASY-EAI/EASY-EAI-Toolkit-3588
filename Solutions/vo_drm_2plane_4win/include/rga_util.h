#ifndef RGA_UTIL_H
#define RGA_UTIL_H

/**
 * RGA 2D 加速工具接口（画面拼接、格式转换等）。
 *
 * 数据流转支持两种方式（由 RgaBuffer.mem_type 选择）：
 *   - RGA_MEM_FD ：dma-buf fd（V4L2 EXPBUF / DRM PRIME），硬件模块间零拷贝
 *   - RGA_MEM_VIR：CPU 虚拟地址，用户态可访问缓冲（兼容调试 / 无 FD 时）
 * 实现按调用方填好的 mem_type 执行，不做隐式回退。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 像素格式，数值与 RK_FORMAT_* 一致。 */
typedef enum {
    RGA_FMT_RGBA_8888        = 0x0 << 8,
    RGA_FMT_RGBX_8888        = 0x1 << 8,
    RGA_FMT_RGB_888          = 0x2 << 8,
    RGA_FMT_BGRA_8888        = 0x3 << 8,
    RGA_FMT_RGB_565          = 0x4 << 8,
    RGA_FMT_BGR_888          = 0x7 << 8,

    RGA_FMT_YCbCr_422_SP     = 0x8 << 8,
    RGA_FMT_YCbCr_420_SP     = 0xa << 8, /* NV12 */
    RGA_FMT_YCrCb_422_SP     = 0xc << 8,
    RGA_FMT_YCrCb_420_SP     = 0xe << 8, /* NV21 */

    RGA_FMT_BGRX_8888        = 0x16 << 8,

    RGA_FMT_YVYU_422         = 0x18 << 8,
    RGA_FMT_VYUY_422         = 0x1a << 8,
    RGA_FMT_YUYV_422         = 0x1c << 8,
    RGA_FMT_UYVY_422         = 0x1e << 8,

    RGA_FMT_YCbCr_420_SP_10B = 0x20 << 8,
    RGA_FMT_YCrCb_420_SP_10B = 0x21 << 8,
    RGA_FMT_YCbCr_422_SP_10B = 0x22 << 8,
    RGA_FMT_YCrCb_422_SP_10B = 0x23 << 8,

    RGA_FMT_BGR_565          = 0x24 << 8,

    RGA_FMT_ARGB_8888        = 0x28 << 8, /* RGA3 仅作输入 */
    RGA_FMT_XRGB_8888        = 0x29 << 8,
    RGA_FMT_ABGR_8888        = 0x2c << 8, /* RGA3 仅作输入 */
    RGA_FMT_XBGR_8888        = 0x2d << 8,

    RGA_FMT_UNKNOWN          = 0x100 << 8,
} RgaFormat;

/** 缓冲区内存类型：一次只应填充对应字段。 */
typedef enum {
    RGA_MEM_FD = 0,  /* 使用 fd（dma-buf） */
    RGA_MEM_VIR,     /* 使用 vir 虚拟地址 */
    RGA_MEM_PHY,     /* 使用 phy 物理地址 */
} RgaMemType;

/**
 * 一帧图像缓冲描述。
 *
 * @param mem_type  内存类型，决定 fd / vir / phy 哪个有效
 * @param fd        dma-buf 文件描述符；mem_type==RGA_MEM_FD 时有效，且 >= 0
 * @param vir       CPU 虚拟地址；mem_type==RGA_MEM_VIR 时有效，不可为 NULL
 * @param phy       物理地址；mem_type==RGA_MEM_PHY 时有效，不可为 NULL
 * @param width     有效像素宽度，> 0；YUV 格式通常须满足对齐约束
 * @param height    有效像素高度，> 0
 * @param wstride   宽度方向跨距（像素）；0 表示与 width 相同
 * @param hstride   高度方向跨距（像素）；0 表示与 height 相同
 * @param format    像素格式，取 RgaFormat 中本平台支持的值
 */
typedef struct {
    RgaMemType mem_type;
    int fd;
    void *vir;
    void *phy;
    int width;
    int height;
    int wstride;
    int hstride;
    int format;
} RgaBuffer;

/**
 * 矩形区域，坐标系原点在图像左上角，单位为像素。
 *
 * @param x  左上角 X，>= 0
 * @param y  左上角 Y，>= 0
 * @param w  宽度，> 0
 * @param h  高度，> 0
 */
typedef struct {
    int x;
    int y;
    int w;
    int h;
} RgaRect;

/**
 * 初始化 RGA 会话。使用其它 rga_* 接口前须先成功调用。
 *
 * @return 0 成功；负值初始化失败
 */
int rga_init(void);

/**
 * 释放 RGA 会话资源。未 init 或已 deinit 时为空操作。
 */
void rga_deinit(void);

/**
 * 将 src 转换为 dst 指定的像素格式（可同时缩放至 dst 尺寸）。
 *
 * @param src  源缓冲，格式须为当前平台支持的输入格式
 * @param dst  目标缓冲，格式须为当前平台支持的输出格式；
 *             宽高为输出尺寸，由调用方预先分配好内存
 * @return 0 成功；负值参数非法或硬件操作失败
 */
int rga_cvtcolor(const RgaBuffer *src, RgaBuffer *dst);

/**
 * 将左右两路画面水平拼接到 dst。
 * dst 宽度应能容纳两路输出（通常为单路宽的 2 倍），高度与单路一致或按
 * dst 自身尺寸缩放；左右输入格式须一致且为支持的输入格式，dst 格式须为
 * 支持的输出格式。
 *
 * @param left   左路输入（swap 非 0 时作为右路）
 * @param right  右路输入（swap 非 0 时作为左路）；宽高/格式建议与 left 一致
 * @param dst    拼接输出缓冲，由调用方预先分配
 * @param swap   非 0 时交换左右输入后再拼接
 * @return 0 成功；负值参数非法或硬件操作失败
 */
int rga_compose_side_by_side(const RgaBuffer *left, const RgaBuffer *right,
                             RgaBuffer *dst, int swap);

/**
 * 将 src 的 src_rect 区域拷贝/缩放到 dst 的 dst_rect 区域。
 * src_rect / dst_rect 传 NULL 表示使用整幅图像。
 *
 * @param src       源缓冲
 * @param src_rect  源裁剪区，可为 NULL
 * @param dst       目标缓冲
 * @param dst_rect  目标写入区，可为 NULL
 * @return 0 成功；负值参数非法或硬件操作失败
 */
int rga_blit(const RgaBuffer *src, const RgaRect *src_rect,
             RgaBuffer *dst, const RgaRect *dst_rect);

#ifdef __cplusplus
}
#endif

#endif /* RGA_UTIL_H */
