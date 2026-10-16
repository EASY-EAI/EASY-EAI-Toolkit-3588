//=====================   C   =====================
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <unistd.h>
//=====================  SDK  =====================
#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_vo.h"
//=====================   PRJ  =====================
#include "system.h"
#include "player/display.h"
#include "ui.h"

#define UI_FPS       30
#define UI_BORDER_W  4    /* 绿色边框厚度 */
#define UI_BAR_H     96   /* 底部按键栏高度 */
#define UI_BTN_W     260
#define UI_BTN_H     64
#define UI_MAX_WIN   DISPLAY_CHN_NUM

#define UI_FB_FMT    RK_FMT_BGRA8888
/* 画布轮转: 送显帧以活引用为合成器所持, 消费完成前不能重写.
   送显按 30fps 节流 (见 uiThread), 4 块轮转 ≈133ms 拉开重写间隔;
   门控 (used>1 才停) 仅作积压保险 (used 常驻 1, 见 uiThread) */
#define UI_MB_CNT    4

#define FONT_CW      5    /* 字宽(点) */
#define FONT_CH      7    /* 字高(点) */

/* BGRA8888, 小端内存字节序为 R,G,B,A, 即小端 uint32 = A<<24|B<<16|G<<8|R */
#define ABGR(a, r, g, b)  (((uint32_t)(a) << 24) | ((b) << 16) | ((g) << 8) | (r))

#define COLOR_TRANSPARENT   0x00000000
#define COLOR_BORDER        ABGR(0xFF, 0x00, 0xC8, 0x53)
#define COLOR_BAR_BG        ABGR(0xB0, 0x10, 0x10, 0x10)
#define COLOR_LOGO          ABGR(0xFF, 0xFF, 0xFF, 0xFF)
#define COLOR_BTN_IDLE      ABGR(0xFF, 0x00, 0x96, 0x3C)
#define COLOR_BTN_HOVER     ABGR(0xFF, 0x00, 0xC8, 0x53)
#define COLOR_BTN_DOWN      ABGR(0xFF, 0x6C, 0xFF, 0x92)
#define COLOR_BTN_FRAME     ABGR(0xFF, 0xFF, 0xFF, 0xFF)
#define COLOR_BTN_TEXT      ABGR(0xFF, 0x00, 0x00, 0x00)
#define COLOR_CURSOR_EDGE   ABGR(0xFF, 0x00, 0x00, 0x00)
#define COLOR_CURSOR_FILL   ABGR(0xFF, 0xFF, 0xFF, 0xFF)

typedef struct {
	char ch;
	uint8_t col[FONT_CW];
} Glyph_t;

/* 5x7 点阵字体子集 (列字节, bit0 在上), 只收录 UI 文案用到的字符 */
static const Glyph_t gFont[] = {
	{ ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ '/', { 0x20, 0x10, 0x08, 0x04, 0x02 } },
	{ 'A', { 0x7C, 0x12, 0x11, 0x12, 0x7C } },
	{ 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } },
	{ 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } },
	{ 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
	{ 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } },
	{ 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } },
	{ 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
	{ 'W', { 0x3F, 0x40, 0x38, 0x40, 0x3F } },
	{ 'Y', { 0x07, 0x08, 0x70, 0x08, 0x07 } },
};

/* 自绘鼠标光标: X=黑边, O=白填充, .=透明, 热点在 (0,0) */
#define CURSOR_W 12
#define CURSOR_H 17
static const char *gCursorArt[CURSOR_H] = {
	"X...........",
	"XX..........",
	"XOX.........",
	"XOOX........",
	"XOOOX.......",
	"XOOOOX......",
	"XOOOOOX.....",
	"XOOOOOOX....",
	"XOOOOOOOX...",
	"XOOOOOOOOX..",
	"XOOOOXXXXX..",
	"XOOXOOX.....",
	"XOX.XOOX....",
	"XX..XOOX....",
	"X....XOOX...",
	".....XOOX...",
	"......XX....",
};

static int gVoDev = 0;
static int gVoLayer = 0;
static int gW = 0;
static int gH = 0;
static int gStridePix = 0;    /* 行距(像素), 256B 对齐 */
static RECT_S gWinRect[UI_MAX_WIN];
static int gWinNum = 0;
static RECT_S gBtnRect;

static MB_POOL gMbPool = MB_INVALID_POOLID;
static MB_BLK gMb[UI_MB_CNT];   /* MB_INVALID_HANDLE 即 NULL, 零初始化即未获取 */
static uint32_t *gCanvasBuf[UI_MB_CNT];
static int gMbIdx = 0;   /* gCanvas 指向正在重画的缓冲, 另一块正被 VO 扫出 */
static uint32_t *gCanvas = NULL;
static VIDEO_FRAME_INFO_S gUiFrame;

/* 每块画布各自的已画状态快照: 多缓冲下各块内容时点不同, 脏区按块跟踪 */
typedef struct {
	int x, y;
	bool hover, pressed;
	bool valid;
} FbState_t;
static FbState_t gFbState[UI_MB_CNT];

static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static int gCurX = 0;
static int gCurY = 0;
static bool gBtnHover = false;
static bool gBtnPressed = false;
static bool gDirty = false;
static bool gInited = false;
static volatile bool gRunning = false;
static pthread_t gUiTid;
static pthread_t gInTid;

/* ---------------- 画布绘制 ---------------- */

static void fill_rect(int x0, int y0, int w, int h, uint32_t color)
{
	int x, y, xe, ye;

	if (x0 < 0) {
		w += x0;
		x0 = 0;
	}
	if (y0 < 0) {
		h += y0;
		y0 = 0;
	}
	xe = (x0 + w > gW) ? gW : x0 + w;
	ye = (y0 + h > gH) ? gH : y0 + h;
	if (w <= 0 || h <= 0)
		return;

	for (y = y0; y < ye; y++) {
		uint32_t *row = &gCanvas[y * gStridePix];
		for (x = x0; x < xe; x++)
			row[x] = color;
	}
}

/* 脏区矩形 (画布坐标系) */
typedef struct {
	int x, y, w, h;
} Clip_t;

/* 与脏区求交后的填充 */
static void fill_rect_clip(const Clip_t *c, int x0, int y0, int w, int h,
		uint32_t color)
{
	int xe = x0 + w, ye = y0 + h;

	if (x0 < c->x)
		x0 = c->x;
	if (y0 < c->y)
		y0 = c->y;
	if (xe > c->x + c->w)
		xe = c->x + c->w;
	if (ye > c->y + c->h)
		ye = c->y + c->h;
	if (xe <= x0 || ye <= y0)
		return;
	fill_rect(x0, y0, xe - x0, ye - y0, color);
}

/* 矩形外框: 厚度 t, 向内收缩, 不超出 (x,y,w,h) */
static void rect_outline_clip(const Clip_t *c, int x, int y, int w, int h,
		int t, uint32_t color)
{
	fill_rect_clip(c, x, y, w, t, color);
	fill_rect_clip(c, x, y + h - t, w, t, color);
	fill_rect_clip(c, x, y + t, t, h - 2 * t, color);
	fill_rect_clip(c, x + w - t, y + t, t, h - 2 * t, color);
}

static const uint8_t *font_glyph(char c)
{
	size_t i;

	for (i = 0; i < sizeof(gFont) / sizeof(gFont[0]); i++) {
		if (gFont[i].ch == c)
			return gFont[i].col;
	}
	return gFont[0].col;   /* 未收录字符按空格处理 */
}

static void draw_text_clip(const Clip_t *c, int x, int y, const char *str,
		uint32_t color, int scale)
{
	int cx = x;

	for (; *str; str++, cx += (FONT_CW + 1) * scale) {
		const uint8_t *g = font_glyph(*str);
		int col, row;

		for (col = 0; col < FONT_CW; col++)
			for (row = 0; row < FONT_CH; row++)
				if ((g[col] >> row) & 1)
					fill_rect_clip(c, cx + col * scale,
							y + row * scale,
							scale, scale, color);
	}
}

static int text_width(const char *str, int scale)
{
	return strlen(str) * (FONT_CW + 1) * scale - scale;
}

static void draw_cursor_clip(const Clip_t *c, int x, int y)
{
	int row, col;

	for (row = 0; row < CURSOR_H; row++) {
		const char *line = gCursorArt[row];
		for (col = 0; col < CURSOR_W; col++) {
			if (line[col] == 'X')
				fill_rect_clip(c, x + col, y + row, 1, 1,
						COLOR_CURSOR_EDGE);
			else if (line[col] == 'O')
				fill_rect_clip(c, x + col, y + row, 1, 1,
						COLOR_CURSOR_FILL);
		}
	}
}

/* 重画一个脏区: 先清为透明, 再按交集补画绿框/底栏/按键/光标.
   单次调用自洽, 不触碰脏区外像素 —— 合成器滞留采样时最多损失本区域一帧,
   不再出现整屏 memset 造成的条状黑块 */
static void render_region(Clip_t c, int cx, int cy, bool hover, bool pressed)
{
	uint32_t btnColor;
	int i, barY, tx;

	if (c.x < 0) {
		c.w += c.x;
		c.x = 0;
	}
	if (c.y < 0) {
		c.h += c.y;
		c.y = 0;
	}
	if (c.x + c.w > gW)
		c.w = gW - c.x;
	if (c.y + c.h > gH)
		c.h = gH - c.y;
	if (c.w <= 0 || c.h <= 0)
		return;

	for (i = c.y; i < c.y + c.h; i++)
		memset(&gCanvas[i * gStridePix + c.x], 0, (size_t)c.w * 4);

	/* 绿框画在视频矩形外侧: 不依赖 UI 层与视频层的 z 顺序都可见 */
	for (i = 0; i < gWinNum; i++) {
		rect_outline_clip(&c,
				gWinRect[i].s32X - UI_BORDER_W,
				gWinRect[i].s32Y - UI_BORDER_W,
				gWinRect[i].u32Width + 2 * UI_BORDER_W,
				gWinRect[i].u32Height + 2 * UI_BORDER_W,
				UI_BORDER_W, COLOR_BORDER);
	}

	/* 底部按键栏 (视频窗下方留白区) */
	barY = gH - UI_BAR_H;
	fill_rect_clip(&c, 0, barY, gW, UI_BAR_H, COLOR_BAR_BG);

	draw_text_clip(&c, 24, barY + (UI_BAR_H - FONT_CH * 3) / 2, "EASY EAI",
			COLOR_LOGO, 3);

	btnColor = pressed ? COLOR_BTN_DOWN :
			(hover ? COLOR_BTN_HOVER : COLOR_BTN_IDLE);
	fill_rect_clip(&c, gBtnRect.s32X, gBtnRect.s32Y,
			gBtnRect.u32Width, gBtnRect.u32Height, btnColor);
	rect_outline_clip(&c, gBtnRect.s32X - 2, gBtnRect.s32Y - 2,
			gBtnRect.u32Width + 4, gBtnRect.u32Height + 4,
			2, COLOR_BTN_FRAME);

	tx = gBtnRect.s32X + ((int)gBtnRect.u32Width -
			text_width("SWAP L/R", 2)) / 2;
	draw_text_clip(&c, tx, gBtnRect.s32Y +
			((int)gBtnRect.u32Height - FONT_CH * 2) / 2,
			"SWAP L/R", COLOR_BTN_TEXT, 2);

	/* 光标最后画, 是唯一覆盖视频区域的元素 */
	draw_cursor_clip(&c, cx, cy);
}

/* ---------------- VO 图层 ---------------- */

static int ui_layer_init(int voDev, int layer)
{
	VO_VIDEO_LAYER_ATTR_S layerAttr;
	VO_CHN_ATTR_S chnAttr;
	int ret;

	/* 清掉可能残留的图层状态, 与 display.c 同款处理 */
	RK_MPI_VO_DisableChn(layer, 0);
	RK_MPI_VO_DisableLayer(layer);

	memset(&layerAttr, 0, sizeof(layerAttr));
	layerAttr.enPixFormat = UI_FB_FMT;
	layerAttr.enCompressMode = COMPRESS_MODE_NONE;
	layerAttr.stDispRect.s32X = 0;
	layerAttr.stDispRect.s32Y = 0;
	layerAttr.stDispRect.u32Width = gW;
	layerAttr.stDispRect.u32Height = gH;
	layerAttr.stImageSize.u32Width = gW;
	layerAttr.stImageSize.u32Height = gH;
	layerAttr.u32DispFrmRt = UI_FPS;

	ret = RK_MPI_VO_BindLayer(layer, voDev, VO_LAYER_MODE_GRAPHIC);
	if (ret != RK_SUCCESS) {
		RK_LOGE("ui BindLayer(%d) failed, ret = %x", layer, ret);
		return ret;
	}

	ret = RK_MPI_VO_SetLayerAttr(layer, &layerAttr);
	if (ret != RK_SUCCESS) {
		RK_LOGE("ui SetLayerAttr(%d) failed, ret = %x", layer, ret);
		goto __UNBIND;
	}

	ret = RK_MPI_VO_EnableLayer(layer);
	if (ret != RK_SUCCESS) {
		RK_LOGE("ui EnableLayer(%d) failed, ret = %x", layer, ret);
		goto __UNBIND;
	}

	/* 单通道全屏画布; BGRA8888 的透明度由像素 A 通道决定 */
	memset(&chnAttr, 0, sizeof(chnAttr));
	chnAttr.stRect.s32X = 0;
	chnAttr.stRect.s32Y = 0;
	chnAttr.stRect.u32Width = gW;
	chnAttr.stRect.u32Height = gH;
	chnAttr.u32FgAlpha = 255;
	chnAttr.u32BgAlpha = 0;
	chnAttr.enMirror = MIRROR_NONE;
	chnAttr.enRotation = ROTATION_0;
	chnAttr.u32Priority = 0;

	ret = RK_MPI_VO_SetChnAttr(layer, 0, &chnAttr);
	if (ret != RK_SUCCESS) {
		RK_LOGE("ui SetChnAttr failed, ret = %x", ret);
		goto __DISABLE;
	}

	ret = RK_MPI_VO_EnableChn(layer, 0);
	if (ret != RK_SUCCESS) {
		RK_LOGE("ui EnableChn failed, ret = %x", ret);
		goto __DISABLE;
	}

	printf("[ui] graphic layer %d on vo dev %d (%dx%d BGRA8888)\n",
			layer, voDev, gW, gH);
	return 0;

__DISABLE:
	RK_MPI_VO_DisableChn(layer, 0);
	RK_MPI_VO_DisableLayer(layer);
__UNBIND:
	RK_MPI_VO_UnBindLayer(layer, voDev);
	return -1;
}

static void ui_layer_deinit(int voDev, int layer)
{
	RK_MPI_VO_DisableChn(layer, 0);
	RK_MPI_VO_DisableLayer(layer);
	RK_MPI_VO_UnBindLayer(layer, voDev);
}

/* ---------------- 鼠标输入 ---------------- */

/* 探测第一个鼠标设备 (EV_REL+REL_X 且带按键), 返回 fd 或 -1 */
static int mouse_probe(void)
{
	unsigned char types[(EV_MAX + 7) / 8];
	unsigned char rel[(REL_MAX + 7) / 8];
	char path[32];
	int fd, i;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		memset(types, 0, sizeof(types));
		memset(rel, 0, sizeof(rel));
		if (ioctl(fd, EVIOCGBIT(0, sizeof(types)), types) < 0 ||
		    !(types[EV_REL / 8] & (1 << (EV_REL % 8))) ||
		    !(types[EV_KEY / 8] & (1 << (EV_KEY % 8)))) {
			close(fd);
			continue;
		}
		if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) < 0 ||
		    !(rel[REL_X / 8] & (1 << (REL_X % 8)))) {
			close(fd);
			continue;
		}

		printf("[ui] mouse: %s\n", path);
		return fd;
	}
	return -1;
}

static bool cursor_in_btn(int x, int y)
{
	return x >= gBtnRect.s32X &&
	       x < gBtnRect.s32X + (int)gBtnRect.u32Width &&
	       y >= gBtnRect.s32Y &&
	       y < gBtnRect.s32Y + (int)gBtnRect.u32Height;
}

static void mouse_handle_event(const struct input_event *ev)
{
	int x, y;
	bool inBtn;

	if (ev->type == EV_REL) {
		if (ev->code != REL_X && ev->code != REL_Y)
			return;   /* 滚轮等忽略 */

		pthread_mutex_lock(&gLock);
		if (ev->code == REL_X) {
			gCurX += ev->value;
			if (gCurX < 0)
				gCurX = 0;
			if (gCurX > gW - 1)
				gCurX = gW - 1;
		} else {
			gCurY += ev->value;
			if (gCurY < 0)
				gCurY = 0;
			if (gCurY > gH - 1)
				gCurY = gH - 1;
		}
		x = gCurX;
		y = gCurY;
		gBtnHover = cursor_in_btn(x, y);
		gDirty = true;
		pthread_mutex_unlock(&gLock);
	} else if (ev->type == EV_KEY && ev->code == BTN_LEFT) {
		pthread_mutex_lock(&gLock);
		x = gCurX;
		y = gCurY;
		inBtn = cursor_in_btn(x, y);
		if (ev->value == 1 && inBtn) {
			gBtnPressed = true;
			gDirty = true;
		} else if (ev->value == 0 && gBtnPressed) {
			gBtnPressed = false;
			gDirty = true;
			if (inBtn)
				display_swap_sources();   /* 点按对调左右 */
		}
		pthread_mutex_unlock(&gLock);
	}
}

static void *inputThread(void *arg)
{
	(void)arg;
	prctl(PR_SET_NAME, "uiMouse", 0, 0, 0);

	while (gRunning) {
		struct input_event ev;
		struct pollfd pfd;
		int fd = mouse_probe();
		int n;

		if (fd < 0) {
			usleep(1000000);
			continue;
		}

		pfd.fd = fd;
		pfd.events = POLLIN;
		while (gRunning) {
			n = poll(&pfd, 1, 200);
			if (!gRunning || n < 0)
				break;
			if (n == 0)
				continue;

			/* 事件读光为止, 设备移除时 read 返回错误跳出重探 */
			while (gRunning &&
			       read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
				mouse_handle_event(&ev);

			if (errno != EAGAIN && errno != EWOULDBLOCK)
				break;
		}
		close(fd);
		if (gRunning)
			printf("[ui] mouse device lost, re-probing...\n");
	}

	pthread_exit(NULL);
}

/* ---------------- 渲染线程 ---------------- */

static void *uiThread(void *arg)
{
	(void)arg;
	prctl(PR_SET_NAME, "uiRender", 0, 0, 0);

	while (gRunning) {
		bool need, hover, pressed, queued;
		Clip_t dmg[3];
		FbState_t *st;
		VO_QUERY_STATUS_S stat;
		int cx, cy, nDmg = 0, i;

		/* 送显帧在合成器队列里是画布的活引用: 未消化就重写画布,
		   消化时会重放出旧内容. rockit 常驻保留最后一帧供视频帧
		   重合成 (used 恒 ≥1, ==0 门控会死锁), 故仅在积压 (used>1)
		   时暂停重画; 查询失败按空闲处理, 退化为原节奏 */
		queued = false;
		if (RK_SUCCESS == RK_MPI_VO_QueryChnStat(gVoLayer, 0, &stat))
			queued = (stat.u32ChnBufUsed > 1);

		pthread_mutex_lock(&gLock);
		need = gDirty;
		if (need && !queued)
			gDirty = false;
		cx = gCurX;
		cy = gCurY;
		hover = gBtnHover;
		pressed = gBtnPressed;
		pthread_mutex_unlock(&gLock);

		if (need && !queued) {
			st = &gFbState[gMbIdx];
			if (!st->valid) {
				dmg[nDmg].x = 0;
				dmg[nDmg].y = 0;
				dmg[nDmg].w = gW;
				dmg[nDmg].h = gH;
				nDmg++;
			} else {
				if (st->hover != hover ||
				    st->pressed != pressed) {
					dmg[nDmg].x = gBtnRect.s32X - 4;
					dmg[nDmg].y = gBtnRect.s32Y - 4;
					dmg[nDmg].w = gBtnRect.u32Width + 8;
					dmg[nDmg].h = gBtnRect.u32Height + 8;
					nDmg++;
				}
				if (cx != st->x || cy != st->y) {
					dmg[nDmg].x = st->x;
					dmg[nDmg].y = st->y;
					dmg[nDmg].w = CURSOR_W;
					dmg[nDmg].h = CURSOR_H;
					nDmg++;
					dmg[nDmg].x = cx;
					dmg[nDmg].y = cy;
					dmg[nDmg].w = CURSOR_W;
					dmg[nDmg].h = CURSOR_H;
					nDmg++;
				}
			}

			for (i = 0; i < nDmg; i++)
				render_region(dmg[i], cx, cy, hover, pressed);

			gUiFrame.stVFrame.pMbBlk = gMb[gMbIdx];
			gUiFrame.stVFrame.pVirAddr[0] = gCanvas;
			gUiFrame.stVFrame.u64PTS += 33333;   /* 30fps */
			if (RK_SUCCESS == RK_MPI_VO_SendFrame(gVoLayer, 0,
						&gUiFrame, -1)) {
				/* 送显成功才推进状态快照并翻到另一块重画:
				   失败时不翻转, 下轮按同一脏区幂等重画 */
				st->x = cx;
				st->y = cy;
				st->hover = hover;
				st->pressed = pressed;
				st->valid = true;
				gMbIdx = (gMbIdx + 1) % UI_MB_CNT;
				gCanvas = gCanvasBuf[gMbIdx];
			} else {
				RK_LOGE("ui SendFrame fail");
			}
		}
		/* 30fps 节流: 与图层 DispFrmRt 一致, 同时 4 块画布轮转
		   周期 ≈133ms, 拉开对同一画布的重写间隔 */
		usleep(33333);
	}

	pthread_exit(NULL);
}

/* ---------------- 对外接口 ---------------- */

int ui_init(int voDev, int uiLayer, int width, int height,
		const RECT_S *winRects, int winNum)
{
	MB_POOL_CONFIG_S stMbCfg;
	int ret, i;

	if (gInited)
		return 0;

	gVoDev = voDev;
	gVoLayer = uiLayer;
	gW = width;
	gH = height;
	gWinNum = (winNum < UI_MAX_WIN) ? winNum : UI_MAX_WIN;
	if (gWinNum > 0 && winRects)
		memcpy(gWinRect, winRects, gWinNum * sizeof(RECT_S));

	gBtnRect.s32X = (gW - UI_BTN_W) / 2;
	gBtnRect.s32Y = gH - UI_BAR_H + (UI_BAR_H - UI_BTN_H) / 2;
	gBtnRect.u32Width = UI_BTN_W;
	gBtnRect.u32Height = UI_BTN_H;

	gCurX = gW / 2;
	gCurY = gH * 3 / 4;

	/* 行距 256B 对齐 (1920*4 天然对齐; 其余宽度留 padding, 画布整块清零) */
	gStridePix = (gW + 63) & ~63;

	memset(&stMbCfg, 0, sizeof(stMbCfg));
	stMbCfg.u64MBSize = (RK_U64)gStridePix * gH * 4;
	stMbCfg.u32MBCnt = UI_MB_CNT;
	stMbCfg.enRemapMode = MB_REMAP_MODE_NONE;
	stMbCfg.enAllocType = MB_ALLOC_TYPE_DMA;
	stMbCfg.enDmaType = MB_DMA_TYPE_CMA;
	stMbCfg.bPreAlloc = RK_TRUE;
	gMbPool = RK_MPI_MB_CreatePool(&stMbCfg);
	if (gMbPool == MB_INVALID_POOLID) {
		RK_LOGE("ui MB CreatePool fail");
		return -1;
	}

	for (i = 0; i < UI_MB_CNT; i++) {
		gMb[i] = RK_MPI_MB_GetMB(gMbPool, stMbCfg.u64MBSize, RK_TRUE);
		if (gMb[i] == MB_INVALID_HANDLE) {
			RK_LOGE("ui MB GetMB(%d) fail", i);
			goto __ERR_MB;
		}
		gCanvasBuf[i] = (uint32_t *)RK_MPI_MB_Handle2VirAddr(gMb[i]);
		if (!gCanvasBuf[i]) {
			RK_LOGE("ui MB Handle2VirAddr(%d) fail", i);
			goto __ERR_MB;
		}
		/* 清整块画布: 覆盖 stride padding 区, 避免边缘扫出花屏 */
		memset(gCanvasBuf[i], 0, (size_t)gStridePix * gH * 4);
	}
	gCanvas = gCanvasBuf[gMbIdx];

	memset(&gUiFrame, 0, sizeof(gUiFrame));
	gUiFrame.stVFrame.pMbBlk = gMb[gMbIdx];
	gUiFrame.stVFrame.u32Width = gW;
	gUiFrame.stVFrame.u32Height = gH;
	gUiFrame.stVFrame.u32VirWidth = gStridePix;
	gUiFrame.stVFrame.u32VirHeight = gH;
	gUiFrame.stVFrame.enPixelFormat = UI_FB_FMT;
	gUiFrame.stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
	gUiFrame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
	gUiFrame.stVFrame.enField = VIDEO_FIELD_FRAME;
	gUiFrame.stVFrame.pVirAddr[0] = gCanvas;

	ret = ui_layer_init(voDev, uiLayer);
	if (ret != 0)
		goto __ERR_MB;

	gDirty = true;
	gInited = true;
	printf("[ui] init ok, button (%d,%d) %dx%d\n",
			gBtnRect.s32X, gBtnRect.s32Y,
			gBtnRect.u32Width, gBtnRect.u32Height);
	return 0;

__ERR_MB:
	for (i = 0; i < UI_MB_CNT; i++) {
		if (gMb[i] != MB_INVALID_HANDLE)
			RK_MPI_MB_ReleaseMB(gMb[i]);
		gMb[i] = MB_INVALID_HANDLE;
		gCanvasBuf[i] = NULL;
	}
	gCanvas = NULL;
	RK_MPI_MB_DestroyPool(gMbPool);
	gMbPool = MB_INVALID_POOLID;
	return -1;
}

int ui_start(void)
{
	if (!gInited || gRunning)
		return -1;

	gRunning = true;
	if (0 != pthread_create(&gUiTid, NULL, uiThread, NULL)) {
		gRunning = false;
		return -1;
	}
	if (0 != pthread_create(&gInTid, NULL, inputThread, NULL)) {
		gRunning = false;
		pthread_join(gUiTid, NULL);
		return -1;
	}
	return 0;
}

void ui_stop(void)
{
	int i;

	if (!gInited)
		return;

	if (gRunning) {
		gRunning = false;
		pthread_join(gUiTid, NULL);
		pthread_join(gInTid, NULL);
	}

	ui_layer_deinit(gVoDev, gVoLayer);

	for (i = 0; i < UI_MB_CNT; i++) {
		if (gMb[i] != MB_INVALID_HANDLE)
			RK_MPI_MB_ReleaseMB(gMb[i]);
		gMb[i] = MB_INVALID_HANDLE;
		gCanvasBuf[i] = NULL;
	}
	RK_MPI_MB_DestroyPool(gMbPool);
	gMbPool = MB_INVALID_POOLID;
	gCanvas = NULL;
	gMbIdx = 0;
	memset(gFbState, 0, sizeof(gFbState));
	gInited = false;
}
