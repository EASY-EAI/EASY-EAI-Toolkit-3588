/**
*
* Copyright 2024 by Guangzhou Easy EAI Technologny Co.,Ltd.
* website: www.easy-eai.com
*
* 双摄取流与 VO 左右双窗显示分离的示例:
*   capturer 模块负责 VI 取流 (每路摄像头独立采集线程, 可实例化两路)
*   player   模块负责 VO 显示 (同一 layer 上 chn0/chn1 左右两窗, 各自独立帧队列)
* 两者通过零拷贝帧队列交接, 取流与显示互不阻塞。
*
* 用法:
*   双摄: ./vo_rockit -w 1920 -h 1080 -W 1920 -H 1080 -d 1 -t 2
*   单摄镜像(验证一层多通道): ./vo_rockit -w 1920 -h 1080 -W 1920 -H 1080 -d 1 -t 2 -m
*   运行中对调左右画面: kill -USR1 <pid>
*
*/

//=====================   C   =====================
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
//=====================  SDK  =====================
#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vo.h"
//=====================   PRJ  =====================
#include "system.h"
#include "capturer/capturer.h"
#include "player/display.h"
#include "ui/ui.h"

static bool quit = false;
static int u32Width = 1024;
static int u32Height = 600;
static int s32chnlId = 0;
/* -1 表示自动: 取与 VoDev 相同的值。
 * RK3588 VOP2 上每个 video_port(vp) 有自己的 window, VO layer 号必须与 vp 对应,
 * 否则 DRM core 会报 "Invalid [CRTC] for [PLANE]", 每帧 atomic commit 都 EINVAL。
 * 本板 HDMI 在 vp1, 所以 -d 1 要配 -l 1 */
static int VoLayer = -1;
static int VoDev = 0;
static int IntfType = 0;
static int VoWidth = 0;
static int VoHeight = 0;
/* -m 单摄镜像: 只用 cam0(dev0), 同一路画面轮流进左右两窗,
 * 用于在第二路摄像头就绪前验证 "一层多通道" */
static bool gMirror = false;
/* -u UI 覆盖层: 绿框/logo/对调按键/自绘光标, 失败自动降级为纯视频 */
static bool gUi = true;

static Capturer_t gCapL;
static Capturer_t gCapR;

static volatile sig_atomic_t gSwapReq = 0;

static void sigterm_handler(int sig)
{
	fprintf(stderr, "signal %d\n", sig);
	quit = true;
}

static void sigusr1_handler(int sig)
{
	(void)sig;
	gSwapReq = 1;
}

/* 0 表示未指定接口, 由 display 模块按 VoDev 推断 */
static int vo_get_en_intf_type(int type)
{
	int enIntfType = 0;

	if (type == 1)
		enIntfType = VO_INTF_MIPI;
	else if (type == 2)
		enIntfType = VO_INTF_HDMI;
	else if (type == 3)
		enIntfType = VO_INTF_LCD;
	else if (type == 4)
		enIntfType = VO_INTF_HDMI1;
	else if (type == 5)
		enIntfType = VO_INTF_MIPI1;

	return enIntfType;
}

static RK_CHAR optstr[] = "?::w:h:I:l:d:t:W:H:mu:";
static void print_usage(const RK_CHAR *name)
{
	printf("usage example:\n");
	printf("\t%s  -w 1920 -h 1080 -W 1920 -H 1080 -I 0 -d 1 -l 1 -t 2\n", name);
	printf("\t-w | --width: VI width, Default:1024\n");
	printf("\t-h | --height: VI height, Default:600\n");
	printf("\t-I | --chnid: VI chn id, Default 0\n");
	printf("\t-l | --layer: Vo layer, Default -1(auto: 同 VoDev). \n");
	printf("\t-d | --device: Vo device, Default 0. "
			"注意 layer 需与 device(vp) 对应, 本板 HDMI 在 dev 1 -> 用 -d 1 -l 1\n");
	printf("\t-t | --type: Vo en intf type, Default 0. "
			"0:auto 1:MIPI 2:HDMI 3:LCD 4:HDMI1 5:MIPI1\n");
	printf("\t-W | --width: Vo display width, Default 0(与采集宽一致). \n");
	printf("\t-H | --height: Vo display height, Default 0(与采集高一致). \n");
	printf("\t-m | mirror: 单摄镜像, 只用 VI dev0, 同一路画面进左右两窗"
			"(验证一层多通道, 第二路摄像头未接入时用)\n");
	printf("\t-u | --ui: UI 覆盖层开关, Default 1(开). "
			"0 关闭 (纯视频显示)\n");
	printf("\t运行中对调左右画面: kill -USR1 <pid>\n");
}

int main(int argc, char *argv[])
{
	int c;
	int ret = -1;
	int enIntfType = 0;
	int dispWidth = 0;
	int dispHeight = 0;

	while ((c = getopt(argc, argv, optstr)) != -1) {
		switch (c) {
		case 'w':
			u32Width = atoi(optarg);
			break;
		case 'h':
			u32Height = atoi(optarg);
			break;
		case 'I':
			s32chnlId = atoi(optarg);
			break;
		case 'l':
			VoLayer = atoi(optarg);
			break;
		case 'd':
			VoDev = atoi(optarg);
			break;
		case 't':
			IntfType = atoi(optarg);
			break;
		case 'W':
			VoWidth = atoi(optarg);
			break;
		case 'H':
			VoHeight = atoi(optarg);
			break;
		case 'm':
			gMirror = true;
			break;
		case 'u':
			gUi = (atoi(optarg) != 0);
			break;
		case '?':
		default:
			print_usage(argv[0]);
			return -1;
		}
	}

	/* 未指定 layer 时跟随 VoDev: RK3588 每个 vp 有自己的 window, 层号需与 vp 对应 */
	if (VoLayer < 0)
		VoLayer = VoDev;
	/* UI 覆盖层用同一 vp 的 Esmart 窗口: Cluster1(vp1)=layer1 -> Esmart1=layer5 */
	int uiLayer = VoLayer + 4;

	printf("#Resolution: %dx%d\n", u32Width, u32Height);
	printf("#ChannelIdx: %d\n", s32chnlId);
	printf("#Vo Layer: %d\n", VoLayer);
	printf("#Vo Devices: %d\n", VoDev);
	printf("#Mode: %s\n", gMirror ? "mirror(单摄双窗)" : "dual camera(左右两窗)");

	signal(SIGINT, sigterm_handler);
	signal(SIGUSR1, sigusr1_handler);

	if (RK_MPI_SYS_Init() != RK_SUCCESS) {
		RK_LOGE("rk mpi sys init fail!");
		return -1;
	}

	/* 1.初始化 VI 取流: cam0=dev0 -> 左窗, cam1=dev1 -> 右窗; -m 时只用 cam0 */
	if (0 != capturer_init(&gCapL, 0, s32chnlId, 0, u32Width, u32Height)) {
		RK_LOGE("capturer_init cam0 fail!");
		goto __FAILED;
	}
	if (!gMirror && 0 != capturer_init(&gCapR, 1, s32chnlId, 1, u32Width, u32Height)) {
		RK_LOGE("capturer_init cam1 (VI dev 1) fail!");
		RK_LOGE("第二路摄像头不可用: 检查 sensor 接线与 DTS, 或加 -m 用单摄镜像模式验证");
		goto __FAILED;
	}

	/* 2.初始化 VO: 同一 layer 上 chn0/chn1 两个显示矩形(左右两窗) */
	enIntfType = vo_get_en_intf_type(IntfType);
	dispWidth = (VoWidth != 0) ? VoWidth : u32Width;
	dispHeight = (VoHeight != 0) ? VoHeight : u32Height;
	printf("VoWidth %d, VoHeight %d\n", dispWidth, dispHeight);
	if (gMirror)
		display_set_mirror(true);
	if (0 != display_init(VoLayer, VoDev, dispWidth, dispHeight, enIntfType,
				u32Width, u32Height)) {
		RK_LOGE("display_init fail!");
		goto __FAILED;
	}

	/* 3.先启动显示线程消费帧队列, 再启动采集线程生产帧 */
	if (0 != display_start()) {
		RK_LOGE("display_start fail!");
		display_stop();
		goto __FAILED;
	}
	if (0 != capturer_start(&gCapL)) {
		RK_LOGE("capturer_start cam0 fail!");
		display_stop();
		goto __FAILED;
	}
	if (!gMirror && 0 != capturer_start(&gCapR)) {
		RK_LOGE("capturer_start cam1 fail!");
		display_stop();
		goto __FAILED;
	}

	/* UI 覆盖层: 拿到左右两窗矩形后画边框/按键/光标, 失败只降级不影响视频 */
	if (gUi) {
		RECT_S winRects[DISPLAY_CHN_NUM];
		if (0 == display_get_chn_rects(winRects) &&
		    0 == ui_init(VoDev, uiLayer, dispWidth, dispHeight,
				winRects, DISPLAY_CHN_NUM)) {
			if (0 != ui_start()) {
				RK_LOGE("ui_start fail, 继续无 UI 显示");
				ui_stop();
			}
		} else {
			RK_LOGE("ui init fail, 降级为纯视频显示");
		}
	}

	printf("dual camera display running... Ctrl+C to exit, kill -USR1 %d to swap L/R.\n",
			getpid());
	while (!quit) {
		if (gSwapReq) {
			gSwapReq = 0;
			display_swap_sources();
			printf("swapped L/R sources\n");
		}
		usleep(50000);
	}

	/* 4.先停采集(不再产帧), 再停 UI, 最后停显示 (ui 的 SendFrame 依赖 VO 未关闭) */
	capturer_stop(&gCapR);
	capturer_stop(&gCapL);
	ui_stop();
	display_stop();
	ret = 0;

__FAILED:
	capturer_stop(&gCapR);
	capturer_stop(&gCapL);
	ui_stop();
	RK_LOGE("test running exit:%d", ret);
	RK_MPI_SYS_Exit();

	return ret;
}
