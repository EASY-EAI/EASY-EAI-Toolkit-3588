/**
 * display — display_pro 的极简封装，提供 overlay / primary 两层最常用的送显流程。
 *
 * 与 display_pro.h 互斥：同一编译单元只能选择其中一套接口，不能混用。
 *
 * 每层各自独立初始化：
 *   - init 选取第一个可用连接器（已连接、有有效模式且已分配 CRTC），
 *     再在该连接器中找到对应类型的图层；
 *   - commit 把 DMA-BUF 提交到该图层，源区域为整帧，目标区域为整屏（按需缩放）；
 *   - uninit 释放该层，两层都释放后自动关闭显示设备。
 *
 * 进程内单实例，不保证线程安全。
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#if defined(DISPLAY_PRO_H_INCLUDED) && !defined(DISPLAY_SIMPLE_IMPL)
#error "display.h 与 display_pro.h 二选一，不能在同一编译单元混用"
#endif

#define DISPLAY_SIMPLE_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_FMT_ARGB8888 = 0, // 32bpp，每像素 A/R/G/B 各 8 位
    DISPLAY_FMT_XRGB8888,     // 32bpp，每像素 X/R/G/B 各 8 位
    DISPLAY_FMT_NV12          // 8 位 Y 平面 + UV 交织平面（4:2:0 半平面）
} DisplayFmt_t;

/**
 * @brief 初始化 overlay 图层
 * @return 0 成功，< 0 失败（-EBUSY 已初始化，-ENODEV 无可用连接器或 overlay 图层，
 *         其余为负的 errno）
 */
int display_overlay_init(void);

/**
 * @brief 提交一帧到 overlay 图层
 * @param dmabuf_fd DMA-BUF 文件描述符，调用后仍由调用方持有
 * @param width     图像宽度，单位：像素（NV12 要求为偶数）
 * @param height    图像高度，单位：像素（NV12 要求为偶数）
 * @param format    像素格式
 * @param pitch     每行字节数；NV12 的 Y/UV 平面共用，UV 平面紧随 Y 平面
 * @return 0 成功，< 0 失败（-ENODEV 未初始化，-EINVAL 参数或格式无效，其余为负的 errno）
 * @note 每帧导入并释放一次 DMA-BUF，返回后调用方即可关闭 fd
 */
int display_overlay_commit(int dmabuf_fd, int width, int height, DisplayFmt_t format, int pitch);

/** @brief 释放 overlay 图层；未初始化时为空操作 */
void display_overlay_uninit(void);

/**
 * @brief 初始化 primary 图层
 * @return 0 成功，< 0 失败（-EBUSY 已初始化，-ENODEV 无可用连接器或 primary 图层，
 *         其余为负的 errno）
 */
int display_primary_init(void);

/**
 * @brief 提交一帧到 primary 图层
 * @param dmabuf_fd DMA-BUF 文件描述符，调用后仍由调用方持有
 * @param width     图像宽度，单位：像素（NV12 要求为偶数）
 * @param height    图像高度，单位：像素（NV12 要求为偶数）
 * @param format    像素格式
 * @param pitch     每行字节数；NV12 的 Y/UV 平面共用，UV 平面紧随 Y 平面
 * @return 0 成功，< 0 失败（-ENODEV 未初始化，-EINVAL 参数或格式无效，其余为负的 errno）
 * @note 每帧导入并释放一次 DMA-BUF，返回后调用方即可关闭 fd
 */
int display_primary_commit(int dmabuf_fd, int width, int height, DisplayFmt_t format, int pitch);

/** @brief 释放 primary 图层；未初始化时为空操作 */
void display_primary_uninit(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */
