/**
 * V4L2 MIPI camera capture adapted from cameraToDisplay.
 * Changes vs vendor copy: drop RGA blit; hold DQBUF until release;
 * set MPLANE NV12 format correctly; poll-based acquire timeout.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "app_log.h"
#include "camera.h"

#define CAM_MAX_NUM 4
#define BUF_COUNT   4
#define FMT_NUM_PLANES 1

typedef struct {
    int video_num;
    int fd;
    enum v4l2_buf_type buff_type;
    uint8_t *mptr[BUF_COUNT];
    uint32_t size[BUF_COUNT];
    int dma_fd[BUF_COUNT];
    int width;
    int height;
    int stride;
    bool held[BUF_COUNT];
} mipi_camera_t;

static mipi_camera_t g_cams[CAM_MAX_NUM];

static mipi_camera_t *cam_by_video(int video_num)
{
    for (int i = 0; i < CAM_MAX_NUM; ++i) {
        if (g_cams[i].fd > 0 && g_cams[i].video_num == video_num)
            return &g_cams[i];
    }
    return NULL;
}

static mipi_camera_t *cam_alloc_slot(void)
{
    for (int i = 0; i < CAM_MAX_NUM; ++i) {
        if (g_cams[i].fd <= 0)
            return &g_cams[i];
    }
    return NULL;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static void cam_unmap_all(mipi_camera_t *cam, int count)
{
    for (int i = 0; i < count; ++i) {
        if (cam->dma_fd[i] >= 0) {
            close(cam->dma_fd[i]);
            cam->dma_fd[i] = -1;
        }
        if (cam->mptr[i] && cam->mptr[i] != MAP_FAILED)
            munmap(cam->mptr[i], cam->size[i]);
        cam->mptr[i] = NULL;
        cam->size[i] = 0;
        cam->held[i] = false;
    }
}

static int v4l2_open_stream(mipi_camera_t *cam, int width, int height)
{
    char device[64];
    struct v4l2_capability cap;
    struct v4l2_format vfmt;
    struct v4l2_requestbuffers req;
    int fd;

    snprintf(device, sizeof(device), "/dev/video%d", cam->video_num);
    fd = open(device, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        perror(device);
        return -1;
    }

    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        close(fd);
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) &&
        !(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        LOG_E("%s: need VIDEO_CAPTURE_MPLANE\n", device);
        close(fd);
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING) &&
        !(cap.device_caps & V4L2_CAP_STREAMING)) {
        LOG_E("%s: need STREAMING\n", device);
        close(fd);
        return -1;
    }

    cam->buff_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    memset(&vfmt, 0, sizeof(vfmt));
    vfmt.type = cam->buff_type;
    vfmt.fmt.pix_mp.width = width;
    vfmt.fmt.pix_mp.height = height;
    vfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    vfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    vfmt.fmt.pix_mp.num_planes = 1;
    vfmt.fmt.pix_mp.quantization = V4L2_QUANTIZATION_FULL_RANGE;
    if (xioctl(fd, VIDIOC_S_FMT, &vfmt) < 0) {
        perror("VIDIOC_S_FMT");
        close(fd);
        return -1;
    }
    if (xioctl(fd, VIDIOC_G_FMT, &vfmt) < 0) {
        perror("VIDIOC_G_FMT");
        close(fd);
        return -1;
    }

    cam->width = (int)vfmt.fmt.pix_mp.width;
    cam->height = (int)vfmt.fmt.pix_mp.height;
    cam->stride = (int)vfmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (cam->stride <= 0)
        cam->stride = cam->width;
    if (vfmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12) {
        LOG_E("%s: got pixelformat 0x%x, want NV12\n",
                device, vfmt.fmt.pix_mp.pixelformat);
        close(fd);
        return -1;
    }
    LOG_I("%s: %dx%d NV12 stride=%d (MPLANE)\n",
          device, cam->width, cam->height, cam->stride);

    memset(&req, 0, sizeof(req));
    req.count = BUF_COUNT;
    req.type = cam->buff_type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        perror("VIDIOC_REQBUFS");
        close(fd);
        return -1;
    }

    for (int i = 0; i < BUF_COUNT; ++i)
        cam->dma_fd[i] = -1;

    for (unsigned int i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[FMT_NUM_PLANES];
        struct v4l2_exportbuffer exp;

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = cam->buff_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = FMT_NUM_PLANES;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            cam_unmap_all(cam, (int)i);
            close(fd);
            return -1;
        }
        cam->size[i] = planes[0].length;
        cam->mptr[i] = mmap(NULL, planes[0].length,
                            PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                            planes[0].m.mem_offset);
        if (cam->mptr[i] == MAP_FAILED) {
            perror("mmap");
            cam->mptr[i] = NULL;
            cam_unmap_all(cam, (int)i);
            close(fd);
            return -1;
        }

        memset(&exp, 0, sizeof(exp));
        exp.type = cam->buff_type;
        exp.index = i;
        exp.plane = 0;
        exp.flags = O_CLOEXEC;
        if (xioctl(fd, VIDIOC_EXPBUF, &exp) == 0) {
            cam->dma_fd[i] = exp.fd;
        } else {
            cam->dma_fd[i] = -1;
            if (i == 0)
                perror("VIDIOC_EXPBUF (VIR path still available)");
        }

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            cam_unmap_all(cam, (int)i + 1);
            close(fd);
            return -1;
        }
    }

    if (xioctl(fd, VIDIOC_STREAMON, &cam->buff_type) < 0) {
        perror("VIDIOC_STREAMON");
        cam_unmap_all(cam, BUF_COUNT);
        close(fd);
        return -1;
    }

    cam->fd = fd;
    return 0;
}

int mipicamera_init(int video_num, int width, int height)
{
    mipi_camera_t *cam;

    if (cam_by_video(video_num)) {
        LOG_E("/dev/video%d already open\n", video_num);
        return -1;
    }
    cam = cam_alloc_slot();
    if (!cam) {
        LOG_E("MIPI CAMERA array is full\n");
        return -1;
    }
    memset(cam, 0, sizeof(*cam));
    cam->video_num = video_num;
    cam->fd = -1;
    for (int i = 0; i < BUF_COUNT; ++i)
        cam->dma_fd[i] = -1;
    if (v4l2_open_stream(cam, width, height) != 0) {
        memset(cam, 0, sizeof(*cam));
        return -1;
    }
    return 0;
}

void mipicamera_exit(int video_num)
{
    mipi_camera_t *cam = cam_by_video(video_num);
    if (!cam)
        return;
    if (cam->fd > 0) {
        xioctl(cam->fd, VIDIOC_STREAMOFF, &cam->buff_type);
        cam_unmap_all(cam, BUF_COUNT);
        close(cam->fd);
    }
    memset(cam, 0, sizeof(*cam));
    /* Give driver a moment before reopen (vendor mipi_camera behavior). */
    usleep(200000);
}

int mipicamera_acquire(int video_num, V4l2Frame *frame, int timeout_ms)
{
    mipi_camera_t *cam;
    struct pollfd pfd;
    struct v4l2_buffer buf;
    struct v4l2_plane planes[FMT_NUM_PLANES];
    size_t y_bytes;

    if (!frame)
        return -1;
    cam = cam_by_video(video_num);
    if (!cam || cam->fd <= 0)
        return -1;

    pfd.fd = cam->fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, timeout_ms) <= 0)
        return -1;

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = cam->buff_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = FMT_NUM_PLANES;
    if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0)
        return -1;
    if (buf.index >= BUF_COUNT || !cam->mptr[buf.index]) {
        xioctl(cam->fd, VIDIOC_QBUF, &buf);
        return -1;
    }

    y_bytes = (size_t)cam->stride * (size_t)cam->height;
    if (y_bytes >= cam->size[buf.index]) {
        xioctl(cam->fd, VIDIOC_QBUF, &buf);
        return -1;
    }

    cam->held[buf.index] = true;
    memset(frame, 0, sizeof(*frame));
    frame->index = (int)buf.index;
    frame->y = cam->mptr[buf.index];
    frame->uv = cam->mptr[buf.index] + y_bytes;
    frame->width = cam->width;
    frame->height = cam->height;
    frame->stride = cam->stride;
    frame->dma_fd = cam->dma_fd[buf.index];
    return 0;
}

int mipicamera_release(int video_num, const V4l2Frame *frame)
{
    mipi_camera_t *cam;
    struct v4l2_buffer buf;
    struct v4l2_plane planes[FMT_NUM_PLANES];

    if (!frame || frame->index < 0 || frame->index >= BUF_COUNT)
        return -1;
    cam = cam_by_video(video_num);
    if (!cam || cam->fd <= 0)
        return -1;
    if (!cam->held[frame->index])
        return 0;

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = cam->buff_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = (unsigned int)frame->index;
    buf.m.planes = planes;
    buf.length = FMT_NUM_PLANES;
    if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
        perror("VIDIOC_QBUF release");
        return -1;
    }
    cam->held[frame->index] = false;
    return 0;
}
