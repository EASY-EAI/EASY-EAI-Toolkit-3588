/**
 *
 * Copyright 2021 by Guangzhou Easy EAI Technologny Co.,Ltd.
 * website: www.easy-eai.com
 *
 * Author: CHM <chenhaiman@easy-eai.com>
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * License file for more details.
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <pthread.h>

#include "clist.h"
#include "vpu_decode.h"
//#include "rga_control.h"
//#include "rkfacial.h"
#include <rga/RgaApi.h>

#define CAMERA_NUM 64
#define BUFFER_COUNT 4
//摄像头不支持输入分辨率时默认设置此分辨率
#define	CAM_INIT_WIDTH	1280
#define	CAM_INIT_HEIGHT	720
#define	DEFAULT_OUTPUT_FMT	RK_FORMAT_BGR_888

#define	BUS1	"usb-xhci-hcd.0.auto-1"	//OTG
#define	BUS2	""
#define	BUS3	"usb-ffe00000.usb-1"	//USB2.0
#define	BUS4	""

struct map_buffer {
    void *start;
    size_t length;
};

typedef struct {
//	int bus;
//	int port;
    int videoIndex;
	int width;
	int height;
	int init_width;
	int init_height;
	RgaSURF_FORMAT fmt;
	int rotation;
	int fd;

	struct map_buffer map_buf[BUFFER_COUNT];
	struct vpu_decode decode;
	bo_t dec_bo;
	int dec_fd;
}usb_camera;

static CList *g_dev_list;
static int g_fps = 0;

int rga_control_buffer_init(bo_t *bo, int *buf_fd, int width, int height, int bpp);
void rga_control_buffer_deinit(bo_t *bo, int buf_fd);

static int qbuf(int fd, struct v4l2_buffer *buf)
{
    int ret;

    ret = ioctl(fd, VIDIOC_QBUF, buf);
    if (ret < 0) {
        perror("VIDIOC_QBUF");
        return -1;
    }
    return 0;
}

static int dqbuf(int fd, struct v4l2_buffer *buf)
{
    int ret;

    ret = ioctl(fd, VIDIOC_DQBUF, buf);
    if (ret < 0) {
        perror("VIDIOC_DQBUF");
        return -1;
    }
    return 0;
}

static int stream_off(int fd)
{
    int ret;
    enum v4l2_buf_type type;

    memset(&type, 0, sizeof(type));
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ret = ioctl(fd, VIDIOC_STREAMOFF, &type);
    if (ret < 0) {
        perror("VIDIOC_STREAMOFF");
        return -1;
    }
    return 0;
}

static int stream_on(int fd)
{
    int ret;
    enum v4l2_buf_type type;

    memset(&type, 0, sizeof(type));
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ret = ioctl(fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        perror("VIDIOC_STREAMON");
        return -1;
    }
    return 0;
}

static void free_bufs(struct map_buffer *map_buf)
{
    int i;

    for (i = 0; i < BUFFER_COUNT; i++) {
        if (map_buf[i].start) {
            munmap(map_buf[i].start, map_buf[i].length);
            map_buf[i].start = NULL;
        }
    }
}

static int req_bufs(int fd, struct map_buffer *map_buf)
{
    int i;
    int ret;
    struct v4l2_requestbuffers reqbuf;

    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = BUFFER_COUNT;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    ret = ioctl(fd, VIDIOC_REQBUFS, &reqbuf);
    if (ret < 0) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }
    for (i = 0; i < reqbuf.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        ret = ioctl(fd, VIDIOC_QUERYBUF, &buf);
        if (ret < 0) {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }
        map_buf[i].length = buf.length;
        map_buf[i].start = (char*)mmap(0, buf.length, PROT_READ | PROT_WRITE,
                MAP_SHARED, fd, buf.m.offset);
        if (map_buf[i].start == MAP_FAILED) {
            perror("mmap");
            return -1;
        }
        ret = ioctl(fd, VIDIOC_QBUF, &buf);
        if (ret < 0) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }
    return 0;
}

static int set_fmt(int fd, int *width, int *height)
{
    int ret;
    struct v4l2_format fmt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = *width;
    fmt.fmt.pix.height = *height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    ret = ioctl(fd, VIDIOC_S_FMT, &fmt);
    if (ret < 0) {
        perror("VIDIOC_S_FMT");
        return -1;
    }

    ret = ioctl(fd, VIDIOC_G_FMT, &fmt);
    if (ret < 0) {
        perror("VIDIOC_G_FMT");
        return -1;
    }
    *width = fmt.fmt.pix.width;
    *height = fmt.fmt.pix.height;
    printf("%s: %dx%d\n", __func__, *width, *height);
    return 0;
}

static int set_parm(int fd, int framerate)
{
	int ret;
	struct v4l2_streamparm parm;

	memset(&parm, 0, sizeof(struct v4l2_streamparm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.denominator = framerate;
    parm.parm.capture.timeperframe.numerator = 1;

	ret = ioctl(fd, VIDIOC_S_PARM, &parm);
    if (ret < 0) {
        perror("VIDIOC_S_PARM");
        return -1;
    }

	memset(&parm, 0, sizeof(struct v4l2_streamparm));
	parm.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(fd, VIDIOC_G_PARM, &parm);
    if (ret < 0) {
        perror("VIDIOC_G_PARM");
        return -1;
    }
	printf("Finally set fps:%f.\n",(double)(parm.parm.capture.timeperframe.denominator) /
								(double)(parm.parm.capture.timeperframe.numerator));
	return 0;
}

static int querycap(int fd, char *type)
{
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        perror("VIDIOC_QUERYCAP");
        return -1;
    }
    if (!strstr((char *)cap.bus_info, type))
        return -1;
    return 0;
}

static int open_usb_camera(int vIndex)
{
    int fd;
    char name[32] = {0};
    struct stat st;
#if 0
	char *cur_bus = NULL;
	char syspath[64] = {0};
	switch(bus) {
	case 1:
		cur_bus = BUS1;
		break;
	case 2:
		cur_bus = BUS2;
		break;
	case 3:
		cur_bus = BUS3;
		break;
	case 4:
		cur_bus = BUS4;
		break;
	default:
		cur_bus = "NOT_SUPPORT";
		break;
	}
	if (port == 0) {
		snprintf(syspath, sizeof(syspath), "%s", cur_bus);
	} else {
		snprintf(syspath, sizeof(syspath), "%s.%d", cur_bus, port);
	}
	printf("syspath:%s\n", syspath);

    for (int i = 0; i < CAMERA_NUM; i++) {
        snprintf(name, sizeof(name), "/dev/video%d", i);
        if (stat(name, &st) == -1)
            continue;
        fd = open(name, O_RDWR, 0);
        if (fd < 0)
            continue;
        if (!querycap(fd, syspath)) {
            printf("%s: %s\n", __func__, name);
            return fd;
        }
        close(fd);
    }
    return -1;
#else
    snprintf(name, sizeof(name), "/dev/video%d", vIndex);
    if (stat(name, &st) == -1){
        return -1;
    }
    fd = open(name, O_RDWR, 0);
    if (fd < 0){
        return -1;
    }
#if 0
    if (!querycap(fd, syspath)) {
        printf("%s: %s\n", __func__, name);
        return fd;
    }else{
        close(fd);
        return -1;
    }
#else
    return fd;
#endif
#endif
}

static int do_usbcamera_getframe(usb_camera* cam, char *pbuf)
{
	struct v4l2_buffer buf;
	RgaSURF_FORMAT dec_fmt;
	rga_info_t src, dst;

	memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

	if (dqbuf(cam->fd, &buf)) {
        printf("%s: %d exit!\n", __func__, __LINE__);
        return -1;
	}

	vpu_decode_jpeg_doing(&cam->decode, cam->map_buf[buf.index].start, buf.bytesused,
                              cam->dec_fd, cam->dec_bo.ptr);

	//rv1126平台解压format一般都是MPP_FMT_YUV422SP
	dec_fmt = (cam->decode.fmt == MPP_FMT_YUV422SP ? RK_FORMAT_YCbCr_422_SP : RK_FORMAT_YCbCr_420_SP);

	memset(&src, 0, sizeof(rga_info_t));
	src.fd = -1;
	src.virAddr = cam->dec_bo.ptr;
	src.mmuFlag = 1;
	src.rotation = cam->rotation;
	rga_set_rect(&src.rect, 0, 0, cam->init_width, cam->init_height, cam->init_width, cam->init_height, dec_fmt);

	memset(&dst, 0, sizeof(rga_info_t));
	dst.fd = -1;
	dst.virAddr = pbuf;
	dst.mmuFlag = 1;
	rga_set_rect(&dst.rect, 0, 0, cam->width, cam->height, cam->width, cam->height, cam->fmt);
	if (c_RkRgaBlit(&src, &dst, NULL)) {
	    printf("%s: rga fail\n", __func__);
	    return -1;
	}

	if (qbuf(cam->fd, &buf)) {
        printf("%s: %d exit!\n", __func__, __LINE__);
        return -1;
	}

	return 0;

}

int usbcamera_getframe(int vIndex, char *pbuf)
{
	int dev_num = 0;
	usb_camera *tmp = NULL;

	if (!g_dev_list || !pbuf) {
		return -1;
	}

	dev_num = g_dev_list->count(g_dev_list);
	for (int i = 0; i < dev_num; i++) {
		tmp = (usb_camera *)g_dev_list->at(g_dev_list, i);
		if (tmp && tmp->videoIndex == vIndex) {
			return do_usbcamera_getframe(tmp, pbuf);
		}
	}
	return -1;
}

void usbcamera_preset_fps(int fps)
{
	g_fps = fps;
}

static usb_camera *do_usbcamera_init(int vIndex, int width, int height, RgaSURF_FORMAT fmt, int rot)
{
	usb_camera *new_camera = NULL;

	new_camera = (usb_camera *)malloc(sizeof(usb_camera));
	if (!new_camera)
		return NULL;

	memset(new_camera, 0, sizeof(usb_camera));
	//new_camera->bus = bus;
	//new_camera->port = port;
	new_camera->videoIndex = vIndex;
	new_camera->width = width;
	new_camera->height = height;
	new_camera->init_width = width;
	new_camera->init_height = height;
	new_camera->fmt = fmt;
	switch(rot) {
	case 90:
		new_camera->rotation = HAL_TRANSFORM_ROT_90;
		break;
	case 180:
		new_camera->rotation = HAL_TRANSFORM_ROT_180;
		break;
	case 270:
		new_camera->rotation = HAL_TRANSFORM_ROT_270;
		break;
	default:
		new_camera->rotation = 0;
		break;
	}
	new_camera->fd = open_usb_camera(vIndex);
	if (new_camera->fd < 0) {
        printf("%s: %d exit!\n", __func__, __LINE__);
        goto error1;
    }

	if(g_fps){
		if (set_parm(new_camera->fd, g_fps)) {
			printf("%s: %d exit!\n", __func__, __LINE__);
			goto error2;
		}
	}

	//这里执行完驱动有可能会更新分辨率
	if (set_fmt(new_camera->fd, &new_camera->init_width, &new_camera->init_height)) {
		printf("%s: %d exit!\n", __func__, __LINE__);
		goto error2;
	}
	if (new_camera->init_width != new_camera->width || new_camera->init_height != new_camera->height) {
		new_camera->init_width = CAM_INIT_WIDTH;
		new_camera->init_height = CAM_INIT_HEIGHT;
		if (set_fmt(new_camera->fd, &new_camera->init_width, &new_camera->init_height)) {
			printf("%s: %d exit!\n", __func__, __LINE__);
			goto error2;
		}
	}

    if (req_bufs(new_camera->fd, new_camera->map_buf)) {
        printf("%s: %d exit!\n", __func__, __LINE__);
        goto error2;
    }
    if (stream_on(new_camera->fd)) {
        printf("%s: %d exit!\n", __func__, __LINE__);
        goto error3;
    }
    if (rga_control_buffer_init(&new_camera->dec_bo, &new_camera->dec_fd, new_camera->width, new_camera->height, 16)) {
        printf("%s: %d exit!\n", __func__, __LINE__);
        goto error4;
    }
	if (vpu_decode_jpeg_init(&new_camera->decode, new_camera->width, new_camera->height)) {
		printf("%s: %d exit!\n", __func__, __LINE__);
		goto error5;
	}

	return new_camera;

	//vpu_decode_jpeg_done(new_camera->decode);
error5:
    rga_control_buffer_deinit(&new_camera->dec_bo, new_camera->dec_fd);
error4:
	stream_off(new_camera->fd);
error3:
    free_bufs(new_camera->map_buf);
error2:
	close(new_camera->fd);
error1:
	free(new_camera);
	return NULL;
}

static void do_camera_exit(usb_camera *cam)
{
	if (!cam) {
		return;
	}

	printf("closing camera(/dev/video%d).\n", cam->videoIndex);

	stream_off(cam->fd);
    free_bufs(cam->map_buf);
    close(cam->fd);

    vpu_decode_jpeg_done(&cam->decode);
    rga_control_buffer_deinit(&cam->dec_bo, cam->dec_fd);
}

int usbcamera_init(int vIndex, int width, int height, int rot)
{
	int dev_num = 0;
	usb_camera *tmp = NULL;

	if (!g_dev_list) {
		g_dev_list = CList_Init(sizeof(usb_camera));
		if (!g_dev_list)
			return -1;
	}

	dev_num = g_dev_list->count(g_dev_list);
	for (int i = 0; i < dev_num; i++) {
		tmp = (usb_camera *)g_dev_list->at(g_dev_list, i);
		if (tmp && tmp->videoIndex == vIndex) {
			printf("camera(/dev/video%d) had init.\n", tmp->videoIndex);
			return -1;
		}
	}

	tmp = do_usbcamera_init(vIndex, width, height, DEFAULT_OUTPUT_FMT, rot);
	if (!tmp) {
		printf("%s: %d exit!\n", __func__, __LINE__);
		return -1;
	}
	g_dev_list->add(g_dev_list, tmp);

    return 0;
}

void usbcamera_exit(int vIndex)
{
	int dev_num = 0;
	usb_camera *tmp = NULL;

	if (!g_dev_list) {
		return;
	}

	dev_num = g_dev_list->count(g_dev_list);
	for (int i = 0; i < dev_num; i++) {
		tmp = (usb_camera *)g_dev_list->at(g_dev_list, i);
		if (tmp && tmp->videoIndex == vIndex) {
			do_camera_exit(tmp);
			g_dev_list->remove(g_dev_list, i);
			break;
		}
	}

	dev_num = g_dev_list->count(g_dev_list);
	if(dev_num == 0) {
		g_dev_list->free(g_dev_list);
		g_dev_list = NULL;
	}
}
