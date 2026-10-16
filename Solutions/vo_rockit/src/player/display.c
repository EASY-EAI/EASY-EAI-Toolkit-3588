//=====================   C   =====================
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
//=====================  SDK  =====================
#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_vi.h"
//=====================   PRJ  =====================
#include "system.h"
#include "display.h"

/* 帧队列深度: 一块被采集线程写入, 一块被显示线程消费, 消除串行等待 */
#define FRAME_QUEUE_DEPTH 2

typedef struct {
	VIDEO_FRAME_INFO_S stFrame;
	int s32PipeId;
	int s32ChnId;
} FrameSlot_t;

typedef struct {
	FrameSlot_t slots[FRAME_QUEUE_DEPTH];
	int head;
	int tail;
	int count;
	pthread_mutex_t lock;
	pthread_cond_t notFull;
	pthread_cond_t notEmpty;
} FrameQueue_t;

static FrameQueue_t gQueues[DISPLAY_CHN_NUM];

static int gVoLayer = 0;
static int gVoDev = 0;
static RECT_S gChnRect[DISPLAY_CHN_NUM];
static bool gInited = false;
static bool gRunning = false;
/* 单摄镜像: 两个显示线程共用队列0, 同一路画面轮流进左右两窗 */
static bool gMirror = false;
/* 左右对调开关: 显示线程按 "线程号 ^ gSwapTog" 选目标通道。
 * 对齐整型的单次读写天然原子, 翻转瞬间最坏情况是两线程各有一帧
 * 落到同一通道, 视觉上无影响 */
static volatile int gSwapTog = 0;
static pthread_t gTids[DISPLAY_CHN_NUM];

static void queue_init(FrameQueue_t *q)
{
	memset(q, 0, sizeof(*q));
	pthread_mutex_init(&q->lock, NULL);
	pthread_cond_init(&q->notFull, NULL);
	pthread_cond_init(&q->notEmpty, NULL);
}

static void queue_destroy(FrameQueue_t *q)
{
	pthread_mutex_destroy(&q->lock);
	pthread_cond_destroy(&q->notFull);
	pthread_cond_destroy(&q->notEmpty);
}

static int queue_push(FrameQueue_t *q, int pipeId, int chnId, const VIDEO_FRAME_INFO_S *frame)
{
	pthread_mutex_lock(&q->lock);
	while ((q->count >= FRAME_QUEUE_DEPTH) && gRunning)
		pthread_cond_wait(&q->notFull, &q->lock);

	if (!gRunning) {
		pthread_mutex_unlock(&q->lock);
		return -1;
	}

	q->slots[q->tail].stFrame = *frame;
	q->slots[q->tail].s32PipeId = pipeId;
	q->slots[q->tail].s32ChnId = chnId;
	q->tail = (q->tail + 1) % FRAME_QUEUE_DEPTH;
	q->count++;

	pthread_cond_signal(&q->notEmpty);
	pthread_mutex_unlock(&q->lock);
	return 0;
}

static int queue_pop(FrameQueue_t *q, FrameSlot_t *slot)
{
	pthread_mutex_lock(&q->lock);
	while ((q->count == 0) && gRunning)
		pthread_cond_wait(&q->notEmpty, &q->lock);

	if (q->count == 0) { /* 已停止且队列已排空 */
		pthread_mutex_unlock(&q->lock);
		return -1;
	}

	*slot = q->slots[q->head];
	q->head = (q->head + 1) % FRAME_QUEUE_DEPTH;
	q->count--;

	pthread_cond_signal(&q->notFull);
	pthread_mutex_unlock(&q->lock);
	return 0;
}

/* 未指定接口类型时按 VO 设备取默认值
 * 该板设备树 route: route-hdmi0=disabled, route-hdmi1=okay, route-dsi0/dsi1=okay
 * 故 HDMI 必须用 VO_INTF_HDMI1, VO_INTF_HDMI 对应的 route-hdmi0 被禁用会绑定失败 */
static int vo_default_intf_type(int VoDev)
{
	if (VoDev == 2 || VoDev == 3)
		return VO_INTF_MIPI;

	return VO_INTF_HDMI1;
}

/* 左右两窗布局: 各占一半宽, 保持摄像头宽高比, 顶部对齐
 * (底部留白, 为后续 UI 按键栏预留) */
static void layout_chn_rects(int dispW, int dispH, int camW, int camH)
{
	int winW = dispW / DISPLAY_CHN_NUM;
	int winH = winW * camH / camW;
	int i;

	if (winH > dispH) {
		winH = dispH;
		winW = dispH * camW / camH;
	}

	gChnRect[0].s32X = 0;
	gChnRect[1].s32X = dispW - winW;
	for (i = 0; i < DISPLAY_CHN_NUM; i++) {
		gChnRect[i].s32Y = 0;
		gChnRect[i].u32Width = winW;
		gChnRect[i].u32Height = winH;
	}

	printf("layout: chn0 (%d,%d,%d,%d) chn1 (%d,%d,%d,%d)\n",
			gChnRect[0].s32X, gChnRect[0].s32Y,
			gChnRect[0].u32Width, gChnRect[0].u32Height,
			gChnRect[1].s32X, gChnRect[1].s32Y,
			gChnRect[1].u32Width, gChnRect[1].u32Height);
}

static int vo_init(int VoLayer, int VoDev, int Width, int Height, int IntfType)
{
	int ret = RK_SUCCESS;
	VO_PUB_ATTR_S stVoPubAttr;
	VO_VIDEO_LAYER_ATTR_S stLayerAttr;
	VO_CHN_ATTR_S stChnAttr;
	int i;

	/* 先清掉可能残留的图层/VO 状态, 否则 BindLayer 会报
	 * "had been set to mode 1, pls unbind first" */
	RK_MPI_VO_DisableLayer(VoLayer);
	RK_MPI_VO_Disable(VoDev);
	for (i = 0; i < DISPLAY_CHN_NUM; i++)
		RK_MPI_VO_DisableChn(VoLayer, i);

	memset(&stVoPubAttr, 0, sizeof(stVoPubAttr));
	memset(&stLayerAttr, 0, sizeof(stLayerAttr));
	memset(&stChnAttr, 0, sizeof(stChnAttr));

	/* layer 用 RGB888 + 不压缩, 由 composer 把 NV12 帧转成 RGB 写入线性 buffer。
	 * 不能用 YUV420SP: rockit 对 YUV420SP 只会分配 AFBC(非线性的) framebuffer
	 * (内核注释 "yuv420_8bit non-Linear mode only"), 实测色度会错乱
	 * (整体偏绿、亮部发紫)。RGB888+COMPRESS_MODE_NONE 得到的是 modifier=0
	 * 的线性 fb, VOP 直接扫描, 颜色正常。 */
	stLayerAttr.enPixFormat = RK_FMT_RGB888;
	stLayerAttr.enCompressMode = COMPRESS_MODE_NONE;
	stLayerAttr.stDispRect.s32X = 0;
	stLayerAttr.stDispRect.s32Y = 0;
	stLayerAttr.stDispRect.u32Width = Width;
	stLayerAttr.stDispRect.u32Height = Height;
	stLayerAttr.stImageSize.u32Width = Width;
	stLayerAttr.stImageSize.u32Height = Height;
	stLayerAttr.u32DispFrmRt = 30;

	ret = RK_MPI_VO_BindLayer(VoLayer, VoDev, VO_LAYER_MODE_VIDEO);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_BindLayer failed, ret = %x", ret);
		return ret;
	}

	/* 先取驱动当前属性, 再显式指定接口类型;
	 * 新建设备的 enIntfType 为 0, 不是合法接口(VO_INTF_CVBS=1), 直接下发会被 DRM 拒绝 */
	ret = RK_MPI_VO_GetPubAttr(VoDev, &stVoPubAttr);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_GetPubAttr failed, ret = %x", ret);
		goto __UNBIND;
	}
	stVoPubAttr.enIntfType = (IntfType != 0) ? IntfType : vo_default_intf_type(VoDev);
	/* HDMI 显式指定 1080P60(原厂 sample 同款), 与 1920x1080 的 layer 尺寸匹配 */
	if ((stVoPubAttr.enIntfType == VO_INTF_HDMI) || (stVoPubAttr.enIntfType == VO_INTF_HDMI1))
		stVoPubAttr.enIntfSync = VO_OUTPUT_1080P60;
	else
		stVoPubAttr.enIntfSync = VO_OUTPUT_DEFAULT;
	printf("VO intf type: 0x%x, sync: %d (dev %d)\n",
			stVoPubAttr.enIntfType, stVoPubAttr.enIntfSync, VoDev);

	ret = RK_MPI_VO_SetPubAttr(VoDev, &stVoPubAttr);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_SetPubAttr failed, ret = %x", ret);
		goto __UNBIND;
	}

	ret = RK_MPI_VO_Enable(VoDev);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_Enable failed, ret = %x", ret);
		goto __UNBIND;
	}

	ret = RK_MPI_VO_SetLayerAttr(VoLayer, &stLayerAttr);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_SetLayerAttr failed, ret = %x", ret);
		goto __UNBIND;
	}

	ret = RK_MPI_VO_EnableLayer(VoLayer);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_EnableLayer failed, ret = %x", ret);
		goto __UNBIND;
	}

	/* 每通道一个独立显示矩形: rockit 的 VO layer 支持一层多通道
	 * (VO_PART_MODE_SINGLE, 软件把多通道画面合成到一个硬件窗口),
	 * 通道矩形内的缩放由 composer 完成 */
	for (i = 0; i < DISPLAY_CHN_NUM; i++) {
		memset(&stChnAttr, 0, sizeof(stChnAttr));
		stChnAttr.stRect = gChnRect[i];
		stChnAttr.u32FgAlpha = 128;
		stChnAttr.u32BgAlpha = 0;
		stChnAttr.enMirror = MIRROR_NONE;
		stChnAttr.enRotation = ROTATION_0;
		stChnAttr.u32Priority = i;

		ret = RK_MPI_VO_SetChnAttr(VoLayer, i, &stChnAttr);
		if (ret != RK_SUCCESS) {
			RK_LOGE("RK_MPI_VO_SetChnAttr(chn %d) failed, ret = %x", i, ret);
			goto __UNBIND;
		}

		ret = RK_MPI_VO_EnableChn(VoLayer, i);
		if (ret != RK_SUCCESS) {
			RK_LOGE("RK_MPI_VO_EnableChn(chn %d) failed, ret = %x", i, ret);
			goto __UNBIND;
		}
	}

	RK_LOGE("Create vo [dev: %d, layer: %d, chn: 0~%d] success!",
			VoDev, VoLayer, DISPLAY_CHN_NUM - 1);
	return ret;

__UNBIND:
	/* 失败时必须解绑, 否则下次启动会报 "had been set to mode, pls unbind first" */
	RK_MPI_VO_UnBindLayer(VoLayer, VoDev);
	RK_MPI_VO_CloseFd();
	return ret;
}

static int vo_deinit(int VoLayer, int VoDev)
{
	int ret = 0;
	int i;

	for (i = 0; i < DISPLAY_CHN_NUM; i++) {
		ret = RK_MPI_VO_DisableChn(VoLayer, i);
		if (ret != RK_SUCCESS) {
			RK_LOGE("RK_MPI_VO_DisableChn(%d) failed, ret = %x", i, ret);
			return ret;
		}
	}

	ret = RK_MPI_VO_DisableLayer(VoLayer);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_DisableLayer failed, ret = %x", ret);
		return ret;
	}

	ret = RK_MPI_VO_Disable(VoDev);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_Disable failed, ret = %x", ret);
		return ret;
	}

	ret = RK_MPI_VO_UnBindLayer(VoLayer, VoDev);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VO_UnBindLayer failed, ret = %x", ret);
		return ret;
	}

	RK_MPI_VO_CloseFd();

	RK_LOGE("Destroy vo [dev: %d, layer: %d, chn: 0~%d] success!",
			VoDev, VoLayer, DISPLAY_CHN_NUM - 1);
	return ret;
}

static void *displayThread(void *arg)
{
	int chnIdx = (int)(intptr_t)arg;
	char tname[16];
	FrameQueue_t *q;
	FrameSlot_t slot;

	snprintf(tname, sizeof(tname), "voDisplay%d", chnIdx);
	prctl(PR_SET_NAME, tname);

	/* 镜像模式下两个线程共用队列0; 正常模式各消费自己的队列 */
	q = &gQueues[gMirror ? 0 : chnIdx];

	while (0 == queue_pop(q, &slot)) {
		/* 对调只换数据落点: 队列归属不变, 通道映射翻转 */
		int voChn = chnIdx ^ gSwapTog;

		RK_S32 s32Ret = RK_MPI_VO_SendFrame(gVoLayer, voChn, &slot.stFrame, -1);
		if (s32Ret != RK_SUCCESS)
			RK_LOGE("RK_MPI_VO_SendFrame fail %x", s32Ret);

		/* 显示线程是帧的消费者, 发送完成后负责归还 VI buffer */
		RK_MPI_VI_ReleaseChnFrame(slot.s32PipeId, slot.s32ChnId, &slot.stFrame);
	}

	pthread_exit(NULL);
}

int display_init(int voLayer, int voDev, int width, int height, int intfType,
		int camW, int camH)
{
	int i;

	gVoLayer = voLayer;
	gVoDev = voDev;

	layout_chn_rects(width, height, camW, camH);

	for (i = 0; i < DISPLAY_CHN_NUM; i++)
		queue_init(&gQueues[i]);

	if (RK_SUCCESS != vo_init(voLayer, voDev, width, height, intfType)) {
		for (i = 0; i < DISPLAY_CHN_NUM; i++)
			queue_destroy(&gQueues[i]);
		return -1;
	}

	gInited = true;
	return 0;
}

int display_start(void)
{
	int i;
	int created = 0;

	if (!gInited)
		return -1;

	if (gRunning)
		return 0;

	gRunning = true;
	for (i = 0; i < DISPLAY_CHN_NUM; i++) {
		if (0 == pthread_create(&gTids[i], NULL, displayThread, (void *)(intptr_t)i)) {
			created++;
		} else {
			RK_LOGE("create display thread %d fail!", i);
			break;
		}
	}

	if (created != DISPLAY_CHN_NUM) {
		gRunning = false;
		for (i = 0; i < DISPLAY_CHN_NUM; i++) {
			pthread_mutex_lock(&gQueues[i].lock);
			pthread_cond_broadcast(&gQueues[i].notFull);
			pthread_cond_broadcast(&gQueues[i].notEmpty);
			pthread_mutex_unlock(&gQueues[i].lock);
		}
		for (i = 0; i < created; i++)
			pthread_join(gTids[i], NULL);
		return -1;
	}

	return 0;
}

int display_push_frame(int camIdx, int pipeId, int chnId, VIDEO_FRAME_INFO_S *frame)
{
	int qIdx;

	if (!gRunning)
		return -1;

	/* 镜像模式统一进队列0, 两个显示线程轮流取帧送左右窗 */
	qIdx = gMirror ? 0 : (camIdx % DISPLAY_CHN_NUM);

	return queue_push(&gQueues[qIdx], pipeId, chnId, frame);
}

void display_set_mirror(bool on)
{
	gMirror = on;
}

void display_swap_sources(void)
{
	gSwapTog ^= 1;
}

int display_get_chn_rects(RECT_S rects[DISPLAY_CHN_NUM])
{
	if (!gInited)
		return -1;
	memcpy(rects, gChnRect, sizeof(gChnRect));
	return 0;
}

void display_stop(void)
{
	FrameSlot_t slot;
	int i;

	if (!gRunning && !gInited)
		return;

	gRunning = false;
	for (i = 0; i < DISPLAY_CHN_NUM; i++) {
		pthread_mutex_lock(&gQueues[i].lock);
		pthread_cond_broadcast(&gQueues[i].notFull);
		pthread_cond_broadcast(&gQueues[i].notEmpty);
		pthread_mutex_unlock(&gQueues[i].lock);
	}

	for (i = 0; i < DISPLAY_CHN_NUM; i++) {
		if (gTids[i]) {
			pthread_join(gTids[i], NULL);
			gTids[i] = 0;
		}
	}

	/* 兜底归还队列中残留的帧, 避免 VI buffer 泄漏 */
	for (i = 0; i < DISPLAY_CHN_NUM; i++) {
		while (0 == queue_pop(&gQueues[i], &slot))
			RK_MPI_VI_ReleaseChnFrame(slot.s32PipeId, slot.s32ChnId, &slot.stFrame);
		queue_destroy(&gQueues[i]);
	}

	if (gInited) {
		vo_deinit(gVoLayer, gVoDev);
		gInited = false;
	}
}
