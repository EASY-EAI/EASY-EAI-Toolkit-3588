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

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include <rga/RgaApi.h>

#define PTRINT uint64_t
#define CAM_MAX_NUM     4
/* 宏定义 */
#define BUF_COUNT       3

typedef struct {
    int cam_chn_num;
    // ============= v4l2 =============
    int video_num;
    int fd;
    enum v4l2_buf_type buff_Type;
    uint8_t *mptr[BUF_COUNT];
    uint32_t size[BUF_COUNT];
    int in_width;
    int in_height;
    uint32_t in_format;
    // ================================
    int rotation;
    int out_width;
    int out_height;
    int out_format;
} mipi_camera_t;

/* 全局变量 */

static mipi_camera_t mipiCam[CAM_MAX_NUM] = {0};

static char *v4l2_fmt(uint32_t fmt)
{
    static char strFormat[8];
    memset(strFormat, 0, sizeof(strFormat));
    uint32_t offset = 0;
    for(int i = 0; i < 4; i++){
        offset = i<<3;
        strFormat[i] = (fmt>>offset)&0xff;
    }

    return strFormat;
}
static RgaSURF_FORMAT rga_fmt(uint32_t v4l2_fmt)
{
    RgaSURF_FORMAT retFmt = RK_FORMAT_UNKNOWN;
    switch(v4l2_fmt)
    {
        //case V4L2_PIX_FMT_H264: retFmt = RK_FORMAT_UNKNOWN; break;
        case V4L2_PIX_FMT_YUYV: retFmt = RK_FORMAT_YUYV_422; break;
        case V4L2_PIX_FMT_YVYU: retFmt = RK_FORMAT_YVYU_422; break;
        //case V4L2_PIX_FMT_YUYV: retFmt = RK_FORMAT_YCbCr_422_SP; break;
        //case V4L2_PIX_FMT_YVYU: retFmt = RK_FORMAT_YCrCb_422_SP; break;
        case V4L2_PIX_FMT_NV12: retFmt = RK_FORMAT_YCbCr_420_SP; break;
        case V4L2_PIX_FMT_NV21: retFmt = RK_FORMAT_YCrCb_420_SP; break;
        case V4L2_PIX_FMT_BGR24: retFmt = RK_FORMAT_BGR_888; break;
        case V4L2_PIX_FMT_RGB24: retFmt = RK_FORMAT_RGB_888; break;
        default : retFmt = RK_FORMAT_UNKNOWN;break;
    }
    return retFmt;
}

#define FMT_NUM_PLANES 1
static int v4l2_camera_init(mipi_camera_t *cam)
{
    int ret = -1;
    
    char device[32] = {0};
    sprintf(device, "/dev/video%d", cam->video_num);
    cam->buff_Type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // 打开摄像头
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd == -1) {
        printf("Error opening camera device[%s]\n", device);
        return -1;
    }
    // 查询设备属性
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    ret = ioctl(fd, VIDIOC_QUERYCAP, &cap);
    if(ret < 0) {
        perror("1. ioctl: VIDIOC_QUERYCAP fail");
        close(fd);
        return -1;
    }
    printf("Driver Name:%s\nCard Name:%s\nBus info:%s\nDriver Version:%u.%u.%u\n",
    cap.driver, cap.card, cap.bus_info, (cap.version>>16)&0XFF, (cap.version>>8)&0XFF, cap.version&0XFF);
	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) &&
        !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
		printf("The Current device is not a video capture device, capabilities: %x\n", cap.capabilities);
        close(fd);
        return -1;
	}
    //judge whether or not to supply the form of video stream
    if(!(cap.capabilities & V4L2_CAP_STREAMING)){
        printf("The Current device does not support streaming i/o\n");
        close(fd);
        return -1;
    }
    
    printf("Capabilities:\n");
	if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE){
		cam->buff_Type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        printf("\tNORMAL\n");
	}else if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE){
		cam->buff_Type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        printf("\tMPLANE\n");
    }
    
    printf("Support format:\n");
    struct v4l2_fmtdesc fmtdesc; 
    fmtdesc.type  = cam->buff_Type;
    fmtdesc.index = 0;
    while(ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) != -1) {
        // fmt
        printf("\t%d.%s[", fmtdesc.index+1, fmtdesc.description);
        // resolution
        struct v4l2_frmsizeenum frmsize;
        memset(&frmsize, 0, sizeof(frmsize));
        frmsize.pixel_format = fmtdesc.pixelformat;
        frmsize.index = 0;
        while(ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) != -1) {
            printf("%d-(%ux%u);", frmsize.index+1, frmsize.stepwise.max_width, frmsize.stepwise.max_height);
            frmsize.index++;
        }
        printf("]\n");
        fmtdesc.index++;
    }
    
    // 设置摄像头参数
    struct v4l2_format vfmt;
    memset(&vfmt, 0, sizeof(vfmt));
    vfmt.type = cam->buff_Type;   //设置类型摄像头采集
    if(ioctl(fd, VIDIOC_G_FMT, &vfmt) < 0){
        perror("2. ioctl: VIDIOC_G_FMT fail");
        close(fd);
        return -1;
    }
    //vfmt.fmt.pix.width = 3840;//u32Width;
    //vfmt.fmt.pix.height = 2160;//u32Height;
    cam->in_width = vfmt.fmt.pix.width;
    cam->in_height = vfmt.fmt.pix.height;
    cam->in_format = vfmt.fmt.pix.pixelformat;
    //vfmt.fmt.pix.pixelformat = cam->in_format; //根据实际情况设置格式
    vfmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
    //vfmt.fmt.pix.bytesperline = IMAGE_RATIO*u32Width;
    //vfmt.fmt.pix.sizeimage = IMAGE_RATIO*u32Width*u32Height;
    //vfmt.fmt.pix_mp.flags = V4L2_PIX_FMT_FLAG_SET_CSC;
    //vfmt.fmt.pix_mp.flags = V4L2_PIX_FMT_FLAG_PREMUL_ALPHA;
    vfmt.fmt.pix_mp.quantization = V4L2_QUANTIZATION_FULL_RANGE;
    ret = ioctl(fd, VIDIOC_S_FMT, &vfmt);
    if(ret < 0) {
        perror("2. ioctl: VIDIOC_S_FMT fail");
        close(fd);
        return -1;
    }
    if(ioctl(fd, VIDIOC_G_FMT, &vfmt) < 0){
        perror("2. ioctl: VIDIOC_G_FMT fail");
        close(fd);
        return -1;
    }
    if(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == cam->buff_Type){
        int plane_cnt = vfmt.fmt.pix_mp.num_planes;
        printf("plane_cnt = %d\n", plane_cnt);
    }
    
#if 1
    printf("Current data format information:\n\twidth:%d\n\theight:%d\n\tfmt:%s\n", vfmt.fmt.pix.width, vfmt.fmt.pix.height, v4l2_fmt(vfmt.fmt.pix.pixelformat));
#else
    // Print Stream Format
    printf("Stream Format Informations:\n");
    printf(" type: %d\n", tv_fmt.type);
    printf(" width: %d\n", tv_fmt.fmt.pix.width);
    printf(" height: %d\n", tv_fmt.fmt.pix.height);
    char fmtstr[8];
    memset(fmtstr, 0, 8);
    memcpy(fmtstr, &tv_fmt.fmt.pix.pixelformat, 4);
    printf(" pixelformat: %s\n", fmtstr);
    printf(" field: %d\n", tv_fmt.fmt.pix.field);
    printf(" bytesperline: %d\n", tv_fmt.fmt.pix.bytesperline);
    printf(" sizeimage: %d\n", tv_fmt.fmt.pix.sizeimage);
    printf(" colorspace: %d\n", tv_fmt.fmt.pix.colorspace);
    printf(" priv: %d\n", tv_fmt.fmt.pix.priv);
    printf(" raw_date: %s\n", tv_fmt.fmt.raw_data);
#endif


#if 0
    while(ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) != -1) {
        if(fmtdesc.pixelformat & vfmt.fmt.pix.pixelformat) {
            printf("\tformat:%s\n", fmtdesc.description);
            break;
        }
        fmtdesc.index++;
    }
#endif

    // 申请一个拥有四个缓冲帧的缓冲区
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = BUF_COUNT; //RK的SDK至少需要申请3个缓冲区
    req.type   = cam->buff_Type; // 缓冲帧数据格式
    req.memory = V4L2_MEMORY_MMAP; //内存映射
    if(ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("3. ioctl: VIDIOC_REQBUFS fail");
        close(fd);
        return -1;
    }
    
    for (unsigned int n_buffers = 0; n_buffers < req.count; ++n_buffers) {        
        struct v4l2_buffer mapbuffer;
		struct v4l2_plane planes[FMT_NUM_PLANES] = {0};
		memset(&mapbuffer, 0, sizeof(mapbuffer));
        mapbuffer.index = n_buffers;                //buffer 序号
        mapbuffer.type = cam->buff_Type;            //设置类型摄像头采集
        mapbuffer.memory = V4L2_MEMORY_MMAP;        //内存映射  IO 方式，被应用程序设置
        //mapbuffer.memory = V4L2_MEMORY_USERPTR;
	    if(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == mapbuffer.type){
	        mapbuffer.m.planes = planes;
	        mapbuffer.length = FMT_NUM_PLANES;
        }
        // 查询序号为n_buffers 的缓冲区，得到其起始物理地址和大小
        if (-1 == ioctl (fd, VIDIOC_QUERYBUF, &mapbuffer)){
            printf("4. ioctl: VIDIOC_QUERYBUF(mmap %d buff from kernal) fail\n", n_buffers);
            close(fd);
            return -1;
        }
        // 把通过ioctrl申请的内存地址映射到用户空间
		if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ==  mapbuffer.type) {
            cam->size[n_buffers] = mapbuffer.m.planes[0].length;
            cam->mptr[n_buffers] = (uint8_t *)mmap(NULL, mapbuffer.m.planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mapbuffer.m.planes[0].m.mem_offset);
	    } else {
            cam->size[n_buffers] = mapbuffer.length;
            cam->mptr[n_buffers] = (uint8_t *)mmap(NULL, mapbuffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mapbuffer.m.offset);
	    }
        // 把映射失败的内存断开映射
        if(MAP_FAILED == cam->mptr[n_buffers]) {
            printf("Fail to mmap[%d]\n", n_buffers);
            for(int i = n_buffers-1; i >= 0; i--){
                printf("need to unmap [%d] buffer\n", i);
    	        munmap(cam->mptr[i], cam->size[i]); // 断开映射
            }
            close(fd);
            return -1;
        }

        // 将缓冲帧放入队列
        if(ioctl(fd, VIDIOC_QBUF, &mapbuffer) < 0) {   //【VIDIOC_QBUF】把帧放入队列；【VIDIOC_DQBUF】从队列中取出帧。
            printf("5. ioctl: VIDIOC_QBUF(put %d buff to Queue) faild\n", n_buffers);
            for(int i = 0; i <= n_buffers; i++){
                printf("need to unmap [%d] buffer\n", i);
    	        munmap(cam->mptr[i], cam->size[i]); // 断开映射
            }
            close(fd);
            return -1;
        }
        printf("usr_buf start= 0x%lx\n", (PTRINT)cam->mptr[n_buffers]); 
    }

    //启动 或 停止数据流 VIDIOC_STREAMON， VIDIOC_STREAMOFF
    //例：把四个缓冲帧放入队列，并启动数据流
    int type = cam->buff_Type;
    if(ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        printf("6. ioctl: VIDIOC_STREAMON fail\n");
        for(int i = 0; i<req.count; i++) {
            printf("need to unmap [%d] buffer\n", i);
    	    munmap(cam->mptr[i], cam->size[i]); // 断开映射
        }
        close(fd);
        return -1;
    }
    printf("\033[36m>>>>--start to capture video stream ...--<<<<\033[0m \n");

    return fd;
}

static void v4l2_camera_exit(mipi_camera_t *cam)
{
    if(cam->fd < 0){
		printf("error func:%s, line:%d\n", __func__, __LINE__);
        perror("cam->fd invalid !");
    }
    printf("\033[33m>>>>--stop capture video stream ...--<<<<\033[0m \n");
    // stop video capturer & close v4l2 fd
    int type = (int) cam->buff_Type;
    if(ioctl(cam->fd, VIDIOC_STREAMOFF, &type) < 0) {
        printf("ioctl: VIDIOC_STREAMOFF fail\n");
    }
    
    // unmap buff
    for(int i=0; i<BUF_COUNT; i++) {
        munmap(cam->mptr[i], cam->size[i]); // 断开映射
    }
    close(cam->fd);
    cam->fd = 0;

    //close完，要给点时间等待资源释放完毕，不然再次打开会概率出现buff读取卡死的情况。
    usleep(200000);
}

static int v4l2_camera_getframe(mipi_camera_t *cam, char *pbuf)
{
    int ret = -1;
    if(cam->fd < 0){
		printf("error func:%s, line:%d\n", __func__, __LINE__);
        perror("cam->fd invalid !");
        return -1;
    }
    
    //从队列中提取一帧数据
    struct v4l2_buffer readbuffer;
    memset(&readbuffer, 0, sizeof(readbuffer));
    readbuffer.type = cam->buff_Type;
    readbuffer.memory = V4L2_MEMORY_MMAP;
	if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ==  readbuffer.type) {
		struct v4l2_plane planes[FMT_NUM_PLANES];
        memset(planes, 0, FMT_NUM_PLANES*sizeof(struct v4l2_plane));
		readbuffer.m.planes = planes;
		readbuffer.length = FMT_NUM_PLANES;
	}

    ret = ioctl(cam->fd, VIDIOC_DQBUF, &readbuffer); // 从队列中取出一个缓冲帧
    if(ret < 0){
		printf("error func:%s, line:%d\n", __func__, __LINE__);
        perror("get buffer form queue fail !");
        return ret;
    }
#if 0
    uint32_t buffSize;
	if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == readbuffer.type)
		buffSize = readbuffer.m.planes[0].bytesused;
	else
		buffSize = readbuffer.bytesused;
#endif
    // 把一帧数据送出去
//===================================================== [rga转换] =====================================================
	rga_info_t src;
	memset(&src, 0, sizeof(rga_info_t));
	src.fd = -1;
	src.virAddr = cam->mptr[readbuffer.index];
	src.mmuFlag = 1;
	src.rotation = cam->rotation;
	rga_set_rect(&src.rect, 0, 0, cam->in_width, cam->in_height, cam->in_width, cam->in_height, rga_fmt(cam->in_format));

	rga_info_t dst;
	memset(&dst, 0, sizeof(rga_info_t));
	dst.fd = -1;
	dst.virAddr = pbuf;
	dst.mmuFlag = 1;
	rga_set_rect(&dst.rect, 0, 0, cam->out_width, cam->out_height, cam->out_width, cam->out_height, cam->out_format);

	if (c_RkRgaBlit(&src, &dst, NULL)) {
        printf("%s: rga fail\n", __func__);
	}
//=====================================================================================================================
    
    ret = ioctl(cam->fd, VIDIOC_QBUF, &readbuffer); // 用完以后把缓存帧放回队列中
    if(ret < 0) {
		printf("error func:%s, line:%d\n", __func__, __LINE__);
        perror("put buffer back to queue fail !");
        return ret;
    }

    return 0;
}

static mipi_camera_t *ptrCam(int camIndex)
{
    for(int i = 0; i < CAM_MAX_NUM; i++){
        if(mipiCam[i].fd <= 0)
            continue;
        if(mipiCam[i].video_num == camIndex)
            return &mipiCam[i];
    }

    return NULL;
}

void mipicamera_set_format(int camIndex, int format)
{
    mipi_camera_t *pCam = ptrCam(camIndex);
    if(pCam){
        pCam->out_format = format;
    }
}

int mipicamera_init(int camIndex, int outWidth, int outHeight, int rot)
{
    int i;
    for(i = 0; i < CAM_MAX_NUM; i++){
        if(mipiCam[i].fd <= 0)
            break;
    }
    if(CAM_MAX_NUM <= i){
		printf("MIPI CAMERA array is full!\n");        
        return -1;
    }
    
	memset(&mipiCam[i], 0, sizeof(mipi_camera_t));
	mipiCam[i].out_width = outWidth;
	mipiCam[i].out_height = outHeight;
	mipiCam[i].out_format = RK_FORMAT_BGR_888;

	switch(rot) {
	case 90:
			mipiCam[i].rotation = HAL_TRANSFORM_ROT_90;
			break;
	case 180:
			mipiCam[i].rotation = HAL_TRANSFORM_ROT_180;
			break;
	case 270:
			mipiCam[i].rotation = HAL_TRANSFORM_ROT_270;
			break;
	case 0:
	default:
			mipiCam[i].rotation = 0;
			break;
	}

	if (c_RkRgaInit()) {
		printf("%s: rga init fail!\n", __func__);
		return -1;
	}
    
	mipiCam[i].video_num = camIndex;
    mipiCam[i].fd = v4l2_camera_init(&mipiCam[i]);
    if(mipiCam[i].fd < 0){
		printf("MIPI CAMERA init failed!\n");
        return -1;
    }
    
	return 0;
}

void mipicamera_exit(int camIndex)
{
    mipi_camera_t *pCam = ptrCam(camIndex);
    if(pCam){
        v4l2_camera_exit(pCam);
    }
}

int mipicamera_getframe(int camIndex, char *pbuf)
{
    mipi_camera_t *pCam = ptrCam(camIndex);
    if(pCam){
	    return v4l2_camera_getframe(pCam, pbuf);
    }else{
        return -1;
    }
}


