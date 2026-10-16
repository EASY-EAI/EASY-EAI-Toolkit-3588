#ifndef CAPTURE_H
#define CAPTURE_H

#include <pthread.h>
#include <stdbool.h>
#include <signal.h>

#include "camera.h"

#define QUEUE_CAP 3

/* Global quit flag owned by main; shared with capture/mouse threads. */
extern volatile sig_atomic_t g_quit;

/** 队列中的一帧；frame 的 V4L2 buffer 由消费者负责 release。 */
typedef struct {
    V4l2Frame frame;
    bool valid;
} FrameSlot;

/** 线程安全的有界视频帧队列；满时生产者等待，空时消费者等待。 */
typedef struct {
    FrameSlot slots[QUEUE_CAP];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} FrameQueue;

/** 一路 V4L2 采集配置及其后台线程。 */
typedef struct {
    int video_num;
    int width;
    int height;
    bool running;
    pthread_t thread;
    FrameQueue queue;
} CaptureState;

/** 初始化队列的 mutex、条件变量和索引。 */
void frame_queue_init(FrameQueue *q);

/** 销毁队列同步对象；调用前应确保队列已停止使用。 */
void frame_queue_destroy(FrameQueue *q);

/** 入队一帧并转移其 buffer 所有权；成功返回 0。 */
int frame_queue_push(FrameQueue *q, const V4l2Frame *frame);

/** 出队一帧并转移给调用者；队列停止或超时返回负值。 */
int frame_queue_pop(FrameQueue *q, V4l2Frame *frame);

/** 打开并启动一路 V4L2 设备；成功返回 0。 */
int capture_init(CaptureState *c);

/** 停止采集并关闭设备。 */
void capture_deinit(CaptureState *c);

/** 归还一帧到驱动队列。 */
void capture_release_frame(CaptureState *c, const V4l2Frame *frame);

/** 采集线程入口，持续取帧并放入 c->queue。 */
void *capture_thread(void *arg);

#endif
