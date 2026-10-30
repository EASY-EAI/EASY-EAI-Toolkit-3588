/**
 * V4L2 MIPI camera capture (based on cameraToDisplay mipi_camera).
 * Holds DQBUF'd buffers until mipicamera_release().
 *
 * 每帧同时提供：
 *   - y/uv：mmap 虚拟地址（用户态拷贝路径）
 *   - dma_fd：VIDIOC_EXPBUF 导出的 dma-buf（硬件模块间零拷贝路径）
 * 两种流转可并存；消费者按后端能力二选一或优先 FD。
 */
#ifndef CAMERA_H
#define CAMERA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "log_manager_pro.h"

/** One held NV12 capture buffer; release with mipicamera_release(). */
typedef struct {
    int index;
    const uint8_t *y;
    const uint8_t *uv;
    int width;
    int height;
    int stride;   /* bytes per line */
    int dma_fd;   /* dma-buf fd from EXPBUF; -1 if unavailable */
} V4l2Frame;

/**
 * Open /dev/video<video_num>, set NV12 MPLANE, mmap REQBUFS, STREAMON.
 * 成功后对每个 buffer 尝试 VIDIOC_EXPBUF；失败时 dma_fd 为 -1，VIR 仍可用。
 * @param log_mgr 日志管理器：init 时在它下面注册本模块日志句柄（"camera"）；
 *                传 LOG_MGR_INVALID 关闭日志
 * @return 0 on success.
 */
int mipicamera_init(int video_num, int width, int height, log_mgr_t log_mgr);

/** STREAMOFF, munmap, close EXPBUF fds, close device. */
void mipicamera_exit(int video_num);

/**
 * DQBUF one frame and keep it mapped until release.
 * @param timeout_ms  poll timeout; <=0 waits forever.
 * @return 0 on success, -1 on timeout/error.
 */
int mipicamera_acquire(int video_num, V4l2Frame *frame, int timeout_ms);

/** QBUF the buffer from a previous acquire. */
int mipicamera_release(int video_num, const V4l2Frame *frame);

#ifdef __cplusplus
}
#endif
#endif
