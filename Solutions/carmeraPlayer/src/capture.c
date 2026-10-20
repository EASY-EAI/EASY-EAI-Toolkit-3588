#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "capture.h"

void frame_queue_init(FrameQueue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

void frame_queue_destroy(FrameQueue *q)
{
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

int frame_queue_push(FrameQueue *q, const V4l2Frame *frame)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == QUEUE_CAP && !g_quit)
        pthread_cond_wait(&q->not_full, &q->lock);
    if (g_quit) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    q->slots[q->tail].frame = *frame;
    q->slots[q->tail].valid = true;
    q->tail = (q->tail + 1) % QUEUE_CAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int frame_queue_pop(FrameQueue *q, V4l2Frame *frame)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !g_quit) {
        if (pthread_cond_timedwait(&q->not_empty, &q->lock, &deadline) == ETIMEDOUT)
            break;
    }
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    *frame = q->slots[q->head].frame;
    q->slots[q->head].valid = false;
    q->head = (q->head + 1) % QUEUE_CAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int capture_init(CaptureState *c)
{
    if (!c)
        return -1;
    if (mipicamera_init(c->video_num, c->width, c->height) != 0)
        return -1;
    c->running = true;
    return 0;
}

void capture_deinit(CaptureState *c)
{
    if (!c)
        return;
    c->running = false;
    mipicamera_exit(c->video_num);
}

void capture_release_frame(CaptureState *c, const V4l2Frame *frame)
{
    if (!c || !frame)
        return;
    mipicamera_release(c->video_num, frame);
}

void *capture_thread(void *arg)
{
    CaptureState *c = arg;
    while (c->running && !g_quit) {
        V4l2Frame frame;
        if (mipicamera_acquire(c->video_num, &frame, 1000) != 0)
            continue;
        if (frame_queue_push(&c->queue, &frame) != 0)
            mipicamera_release(c->video_num, &frame);
    }
    return NULL;
}
