//=====================  C++  =====================
#include <string>
//=====================   C   =====================
#include "system.h"
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
//=====================  PRJ  =====================
#include "display.h"
#include "display_pro.h"
#include "log_manager_pro.h"

#define DISP_LOG_CONF "/userdata/logs/rtspMultiPlayer.ini"

static DispDev_t gDev;
static int gDevInited = 0;
static int gConnIdx = -1;
static int gPlane = -1;
static int gBuf = -1;
static DispBufferInfo_t gBufInfo;
static char *gFrameBuffer = NULL;
static int gInited = 0;
static int gLog = LOG_HANDLE_INVALID;
static volatile sig_atomic_t gRunning = 1;
static pthread_mutex_t gCommitLock = PTHREAD_MUTEX_INITIALIZER;

/* 等比缩放到屏幕内并居中 */
static void fit_into(int sw, int sh, int w, int h, int *x, int *y, int *dw, int *dh)
{
    int nw = w;
    int nh = h;

    if ((int64_t)w * sh > (int64_t)h * sw)
        nh = (int)((int64_t)h * sw / w);
    else
        nw = (int)((int64_t)w * sh / h);

    *dw = nw;
    *dh = nh;
    *x = (sw - nw) / 2;
    *y = (sh - nh) / 2;
}

static void display_log_init(void)
{
    log_mgr_t mgr = log_manager_init(DISP_LOG_CONF);

    if (mgr != LOG_MGR_INVALID)
        gLog = log_register(mgr, "rtspMultiPlayer");
}

static void drm_fini(void)
{
    /* 先摘掉对外暴露的缓冲指针，避免采集线程在释放后继续写帧 */
    gFrameBuffer = NULL;
    gInited = 0;

    if (gBuf >= 0) {
        disp_free_buffer(gBuf);
        gBuf = -1;
    }
    if (gPlane >= 0) {
        disp_uninit_planes(gPlane);
        gPlane = -1;
    }
    if (gDevInited) {
        /* 交还 DRM master，避免程序退出后桌面无法接管显示 */
        disp_drop_master();
        disp_uninit(&gDev);
        gDevInited = 0;
    }
    gConnIdx = -1;
}

static int drm_init(const Display_t *disp)
{
    if (disp_init(&gDev) != 0) {
        printf("disp_init failed\n");
        return -1;
    }
    gDevInited = 1;

    for (int i = 0; i < gDev.connectorCount; i++) {
        if (gDev.connectors[i].enabled) {
            gConnIdx = gDev.connectors[i].index;
            break;
        }
    }
    if (gConnIdx < 0) {
        printf("no enabled DRM connector found\n");
        return -1;
    }

    uint32_t planeId = 0;
    int planeType = -1;
    for (int i = 0; i < gDev.connectors[gConnIdx].planeCount && !planeId; i++) {
        for (int f = 0; f < gDev.connectors[gConnIdx].planes[i].formatCount; f++) {
            if (gDev.connectors[gConnIdx].planes[i].formats[f] == DISP_PIX_FMT_ARGB8888) {
                planeId = gDev.connectors[gConnIdx].planes[i].id;
                planeType = gDev.connectors[gConnIdx].planes[i].type;
                break;
            }
        }
    }
    if (!planeId) {
        printf("no DRM plane supports ARGB8888\n");
        return -1;
    }

    gPlane = disp_init_planes(gConnIdx, planeId);
    if (gPlane < 0) {
        printf("disp_init_planes(id=%u) failed: %d\n", planeId, gPlane);
        return -1;
    }

    gBuf = disp_alloc_buffer(disp->width, disp->height, DISP_PIX_FMT_ARGB8888);
    if (gBuf < 0) {
        printf("disp_alloc_buffer(%dx%d) failed: %d\n", disp->width, disp->height, gBuf);
        return -1;
    }
    if (disp_get_buffer_info(gBuf, &gBufInfo) != 0 || NULL == gBufInfo.pFramebuffer) {
        printf("disp_get_buffer_info failed\n");
        return -1;
    }

    if (disp_plane_set_src_region(gPlane, 0, 0, disp->width, disp->height) != 0) {
        printf("disp_plane_set_src_region failed\n");
        return -1;
    }

    int sw = gDev.connectors[gConnIdx].screenWidth;
    int sh = gDev.connectors[gConnIdx].screenHeight;
    int x, y, w, h;
    fit_into(sw, sh, disp->width, disp->height, &x, &y, &w, &h);
    if (disp_plane_set_crtc_region(gPlane, x, y, w, h) != 0) {
        printf("disp_plane_set_crtc_region failed\n");
        return -1;
    }

    printf("display: screen %dx%d, video %dx%d+%d+%d, fd=%d stride=%d, plane=%u type=%d\n",
           sw, sh, w, h, x, y, gBufInfo.dmabuf_fd, gBufInfo.fbStride, planeId, planeType);

    gInited = 1;
    return 0;
}

char **dispBufferMap(Display_t *dispDesc)
{
    if (!gInited) {
        display_log_init();
        if (drm_init(dispDesc) != 0) {
            drm_fini();
            return &gFrameBuffer;
        }
        gFrameBuffer = gBufInfo.pFramebuffer;
    }

    return &gFrameBuffer;
}

int display_frame_fd(void)
{
    return gInited ? gBufInfo.dmabuf_fd : -1;
}

int display_frame_stride(void)
{
    return gInited ? gBufInfo.fbStride : 0;
}

static int gCommitOk = 0;
static int gCommitFail = 0;

int display_frame_commit(void)
{
    int ret;

    if (!gInited)
        return -1;

    pthread_mutex_lock(&gCommitLock);
    ret = disp_commit(gPlane, gBuf);
    pthread_mutex_unlock(&gCommitLock);

    if (ret != 0) {
        if (gCommitFail++ < 5)
            printf("disp_commit failed: ret=%d errno=%d (%s)\n", ret, errno, strerror(errno));
    } else if (gCommitOk++ == 0) {
        DispConnectorStatus_t cs;
        DispPlaneState_t ps;
        int master = -1;

        disp_is_master(&master);
        printf("first commit ok: master=%d\n", master);
        if (disp_get_connector_status(gConnIdx, &cs) == 0)
            printf("connector: connected=%d crtcBound=%d crtcEnabled=%d dpms=%d "
                   "scanout=%d %dx%d@%d\n",
                   cs.connected, cs.crtcBound, cs.crtcEnabled, (int)cs.dpms,
                   cs.scanoutActive, cs.width, cs.height, cs.vrefresh);
        if (disp_get_plane_state(gPlane, &ps) == 0)
            printf("plane: attached=%d crtcId=%u fbId=%u\n",
                   ps.attached, ps.crtcId, ps.fbId);
    }

    return ret;
}

static void display_on_signal(int sig)
{
    (void)sig;
    gRunning = 0;
}

int display(Display_t *disp)
{
    (void)disp;

    if (!gInited)
        return -1;

    signal(SIGINT, display_on_signal);
    signal(SIGTERM, display_on_signal);

    while (gRunning)
        pause();

    return 0;
}

void display_deinit(void)
{
    pthread_mutex_lock(&gCommitLock);
    drm_fini();
    pthread_mutex_unlock(&gCommitLock);
}
