#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *winTitle;
    int x;
    int y;
    int width;
    int height;
}Display_t;

/* 初始化 DRM 显示并分配一帧缓冲，返回缓冲首地址的指针；失败时 *ppBuf 为 NULL */
extern char **dispBufferMap(Display_t *dispDesc);

/* 显示缓冲的 DMA-BUF fd，供 RGA 零拷贝写帧；未初始化时返回 -1 */
extern int display_frame_fd(void);

/* 显示缓冲每行字节数；未初始化时返回 0 */
extern int display_frame_stride(void);

/* RGA 写完一帧后调用，把当前缓冲提交到 DRM 图层（默认阻塞到下一个 vsync） */
extern int display_frame_commit(void);

/* 进入等待循环，收到 SIGINT/SIGTERM 后返回 */
extern int display(Display_t *dispDesc);

/* 停止采集后调用，释放显示缓冲、图层与 DRM 设备 */
extern void display_deinit(void);

#endif
