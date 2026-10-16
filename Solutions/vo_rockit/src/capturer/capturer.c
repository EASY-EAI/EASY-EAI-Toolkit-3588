//=====================   C   =====================
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
//=====================  SDK  =====================
#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_vi.h"
//=====================   PRJ  =====================
#include "system.h"
#include "player/display.h"
#include "capturer.h"

/* V4L2 buffer 数, 需大于 u32Depth, 否则取帧会失败 */
#define VI_BUF_COUNT 4
/* 通道深度: 帧队列深度2 + 1, 保证队列未满时取帧也不会失败 */
#define VI_CHN_DEPTH 3

static int vi_dev_init(Capturer_t *cap)
{
	int ret = 0;
	VI_DEV_ATTR_S stDevAttr;
	VI_DEV_BIND_PIPE_S stBindPipe;

	memset(&stDevAttr, 0, sizeof(stDevAttr));
	memset(&stBindPipe, 0, sizeof(stBindPipe));

	/* 0. get dev config status */
	ret = RK_MPI_VI_GetDevAttr(cap->devId, &stDevAttr);
	if (ret == RK_ERR_VI_NOT_CONFIG) {
		/* 0-1.config dev */
		ret = RK_MPI_VI_SetDevAttr(cap->devId, &stDevAttr);
		if (ret != RK_SUCCESS) {
			RK_LOGE("RK_MPI_VI_SetDevAttr fail %x", ret);
			return -1;
		}
	} else {
		RK_LOGE("RK_MPI_VI_SetDevAttr already");
	}

	/* 1.get dev enable status */
	ret = RK_MPI_VI_GetDevIsEnable(cap->devId);
	if (ret != RK_SUCCESS) {
		/* 1-2.enable dev */
		ret = RK_MPI_VI_EnableDev(cap->devId);
		if (ret != RK_SUCCESS) {
			RK_LOGE("RK_MPI_VI_EnableDev fail %x", ret);
			return -1;
		}
		/* 1-3.bind dev/pipe */
		stBindPipe.u32Num = 1;
		stBindPipe.PipeId[0] = cap->pipeId;
		ret = RK_MPI_VI_SetDevBindPipe(cap->devId, &stBindPipe);
		if (ret != RK_SUCCESS) {
			RK_LOGE("RK_MPI_VI_SetDevBindPipe fail %x", ret);
			return -1;
		}
	} else {
		RK_LOGE("RK_MPI_VI_EnableDev already");
	}

	return 0;
}

static int vi_chn_init(Capturer_t *cap)
{
	int ret;
	VI_CHN_ATTR_S vi_chn_attr;

	memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
	vi_chn_attr.stIspOpt.u32BufCount = VI_BUF_COUNT;
	/* 该板 rkisp 不接受 rockit 自分配的 DMABUF buffer(4 个 buffer 的 VIDIOC_QBUF 全部失败),
	 * 改用驱动侧分配的 MMAP buffer; MMAP 下 buf 类型必须是 EXTERNAL,
	 * 否则 rockit 会告警 "use mmap memory the node_buff_type:0 must be extern(1)" */
	vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_MMAP;
	vi_chn_attr.enAllocBufType = VI_ALLOC_BUF_TYPE_EXTERNAL;
	/* 该字段名为 "NoUse", 置 0 等于启用 libv4l2;
	 * 板上没有 libv4l2.so, 必须置 RK_TRUE 绕开包装层直接走原生 V4L2 */
	vi_chn_attr.stIspOpt.bNoUseLibV4L2 = RK_TRUE;
	/* rkisp mainpath 是 MPLANE 设备(cap 只有 V4L2_CAP_VIDEO_CAPTURE_MPLANE),
	 * 不指定时 rockit 默认用非 MPLANE 采集, 会导致 VIDIOC_QBUF 失败 */
	vi_chn_attr.stIspOpt.enCaptureType = VI_V4L2_CAPTURE_TYPE_VIDEO_CAPTURE_MPLANE;
	vi_chn_attr.stSize.u32Width = cap->width;
	vi_chn_attr.stSize.u32Height = cap->height;
	vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
	vi_chn_attr.u32Depth = VI_CHN_DEPTH;

	ret = RK_MPI_VI_SetChnAttr(cap->pipeId, cap->chnId, &vi_chn_attr);
	ret |= RK_MPI_VI_EnableChn(cap->pipeId, cap->chnId);
	if (ret) {
		RK_LOGE("create VI chn fail! ret=%d", ret);
		return -1;
	}

	return 0;
}

static void *captureThread(void *arg)
{
	Capturer_t *cap = (Capturer_t *)arg;
	RK_S32 s32Ret;
	const RK_S32 waitTime = 1000;
	VIDEO_FRAME_INFO_S stViFrame;
	char tname[16];

	snprintf(tname, sizeof(tname), "viCapture%d", cap->camIdx);
	prctl(PR_SET_NAME, tname);

	while (cap->running) {
		s32Ret = RK_MPI_VI_GetChnFrame(cap->pipeId, cap->chnId, &stViFrame, waitTime);
		if (s32Ret != RK_SUCCESS) {
			RK_LOGE("RK_MPI_VI_GetChnFrame(cam%d) timeout %x", cap->camIdx, s32Ret);
			continue;
		}

		/* 帧所有权移交显示线程, 由其在 SendFrame 后归还;
		 * 入队失败说明显示已停止, 由本线程负责归还 */
		if (0 != display_push_frame(cap->camIdx, cap->pipeId, cap->chnId, &stViFrame)) {
			RK_MPI_VI_ReleaseChnFrame(cap->pipeId, cap->chnId, &stViFrame);
		}
	}

	pthread_exit(NULL);
}

int capturer_init(Capturer_t *cap, int devId, int chnId, int camIdx, int width, int height)
{
	memset(cap, 0, sizeof(*cap));
	cap->devId = devId;
	cap->pipeId = devId;
	cap->chnId = chnId;
	cap->camIdx = camIdx;
	cap->width = width;
	cap->height = height;

	if (0 != vi_dev_init(cap))
		return -1;
	cap->devInited = true;

	if (0 != vi_chn_init(cap))
		return -1;
	cap->chnInited = true;

	printf("VI init success [cam%d: dev %d, pipe %d, chn %d, %dx%d, bufCount %d, depth %d]\n",
			cap->camIdx, cap->devId, cap->pipeId, cap->chnId,
			width, height, VI_BUF_COUNT, VI_CHN_DEPTH);
	return 0;
}

int capturer_start(Capturer_t *cap)
{
	if (!cap || cap->running)
		return 0;

	cap->running = true;
	if (0 != pthread_create(&cap->tid, NULL, captureThread, cap)) {
		RK_LOGE("create capture thread fail!");
		cap->running = false;
		return -1;
	}

	return 0;
}

void capturer_stop(Capturer_t *cap)
{
	if (!cap || (!cap->running && !cap->devInited && !cap->chnInited))
		return;

	cap->running = false;

	if (cap->tid) {
		pthread_join(cap->tid, NULL);
		cap->tid = 0;
	}

	if (cap->chnInited) {
		RK_MPI_VI_DisableChn(cap->pipeId, cap->chnId);
		cap->chnInited = false;
	}

	if (cap->devInited) {
		RK_MPI_VI_DisableDev(cap->devId);
		cap->devInited = false;
	}
}
