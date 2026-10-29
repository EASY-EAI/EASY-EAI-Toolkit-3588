#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_log.h"
#include "display_watch.h"
#include "display_pro.h"

typedef struct {
    double next_poll;                          /* 下一次允许查询的时刻 */
    bool polled;                               /* 是否已完成基线查询 */
    bool scanout;                              /* 上一次判定的上屏状态 */
    bool attached;                             /* 上一次判定的图层绑定状态 */
    DispDpms_t dpms;                        /* 上一次读到的 DPMS 档位，仅用于统计行 */
    int conn_index;
    int planes[DISPLAY_WATCH_MAX_PLANES];
    int plane_count;
} DisplayWatch;

static DisplayWatch g_watch;

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *dpms_name(DispDpms_t dpms)
{
    switch (dpms) {
    case DISP_DPMS_ON:
        return "ON";
    case DISP_DPMS_STANDBY:
        return "STANDBY";
    case DISP_DPMS_SUSPEND:
        return "SUSPEND";
    case DISP_DPMS_OFF:
        return "OFF";
    default:
        return "UNKNOWN";
    }
}

int drm_errcode(int ret)
{
    int err;

    if (ret == 0)
        return 0;
    err = (ret == -1) ? errno : -ret;
    return err > 0 ? err : 0;
}

const char *drm_errstr(int ret)
{
    int err = drm_errcode(ret);

    return err ? strerror(err) : "unknown";
}

void display_watch_start(int conn_index, const int *planes, int plane_count)
{
    memset(&g_watch, 0, sizeof(g_watch));
    g_watch.scanout = true;
    g_watch.attached = true;
    g_watch.dpms = DISP_DPMS_ON;
    g_watch.conn_index = conn_index;
    if (plane_count > DISPLAY_WATCH_MAX_PLANES)
        plane_count = DISPLAY_WATCH_MAX_PLANES;
    g_watch.plane_count = plane_count;
    for (int i = 0; i < plane_count; ++i)
        g_watch.planes[i] = planes ? planes[i] : -1;
}

/*
 * 任一图层脱离 CRTC 即视为未绑定。
 * @return 1 绑定正常，0 有图层被摘下，-1 查询失败（由调用方保留上次判定）
 */
static int planes_attached(void)
{
    for (int i = 0; i < g_watch.plane_count; ++i) {
        DispPlaneState_t ps;

        if (g_watch.planes[i] < 0)
            continue;
        if (disp_get_plane_state(g_watch.planes[i], &ps) != 0)
            return -1;
        if (!ps.attached)
            return 0;
    }
    return 1;
}

/*
 * 把"看不见"的原因一次说全。这种场景下 disp_commit 的返回码永远是 0，
 * 只看返回码拿不到任何线索，因此连接器/CRTC/DPMS/图层都要落进日志。
 */
static void log_scanout_lost(const DispConnectorStatus_t *st, bool attached)
{
    LOG_E("显示输出中断: connector=%u connected=%d crtc=%u crtc_enabled=%d dpms=%s "
          "plane_attached=%d -> disp_commit 仍返回 0 但屏幕无画面；"
          "输出被桌面息屏时，用键盘鼠标唤醒或 xset dpms force on 后自动恢复\n",
          st->connectorId, st->connected, st->crtcId, st->crtcEnabled,
          dpms_name(st->dpms), attached);
}

bool display_watch_tick(void)
{
    DispConnectorStatus_t st;
    double now = now_sec();
    int attached;
    int ret;
    bool scanout;

    if (now < g_watch.next_poll)
        return g_watch.scanout;
    g_watch.next_poll = now + DISPLAY_WATCH_POLL_SEC;

    ret = disp_get_connector_status(g_watch.conn_index, &st);
    if (ret != 0) {
        /* 保持上一次判定：查询本身失败不代表输出断了，误报会让调用方白折腾一次重建 */
        LOG_W("disp_get_connector_status(connector %d) failed: %s\n",
              g_watch.conn_index, drm_errstr(ret));
        return g_watch.scanout;
    }
    attached = planes_attached();
    if (attached < 0)
        attached = g_watch.attached;

    scanout = st.scanoutActive && attached;
    if (!g_watch.polled) {
        /* 基线常驻打印：启动时输出是什么样，后续每条告警才有参照 */
        g_watch.polled = true;
        LOG_I("输出基线: connector=%u connected=%d crtc=%u crtc_enabled=%d dpms=%s "
              "%dx%d@%dHz plane_attached=%d scanout=%d\n",
              st.connectorId, st.connected, st.crtcId, st.crtcEnabled,
              dpms_name(st.dpms), st.width, st.height, st.vrefresh,
              attached, scanout);
        if (!scanout)
            log_scanout_lost(&st, attached);
    } else if (scanout != g_watch.scanout) {
        if (scanout)
            LOG_I("显示输出恢复: connector=%u %dx%d@%dHz dpms=%s plane_attached=%d\n",
                  st.connectorId, st.width, st.height, st.vrefresh,
                  dpms_name(st.dpms), attached);
        else
            log_scanout_lost(&st, attached);
    }
    g_watch.scanout = scanout;
    g_watch.attached = attached != 0;
    g_watch.dpms = st.dpms;
    return scanout;
}

const char *display_watch_state_str(void)
{
    static char buf[64];

    snprintf(buf, sizeof(buf), "scanout=%s dpms=%s",
             g_watch.scanout ? "on" : "OFF", dpms_name(g_watch.dpms));
    return buf;
}
