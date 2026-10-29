/**
 * disp_engine — 共享显示引擎实现（单 disp_init 管 DRM 设备 + 视频/UI 两个图层）
 *
 * 线程模型：
 *   - init/deinit 由主线程在 Qt 事件循环外串行调用；
 *   - disp_ui_*   由 Qt 渲染线程调用；
 *   - disp_video_* 由视频线程调用；
 *   display_pro 不保证线程安全 → 所有会触碰 DRM 的路径统一经 g_lock。
 *   disp_commit 阻塞至 vsync（约 16.7ms @60Hz）期间持锁，另一路提交最多
 *   等一帧，实测影响可忽略（UI 仅在界面变化时提交）。
 */

#include "disp_engine.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* ---------------- 模块状态 ---------------- */

static DispDev_t g_dev;
static int g_valid = 0;
static int g_conn = -1;
static int g_screen_w;
static int g_screen_h;

static int g_video_plane = -1;      /* disp_init_planes 句柄 */
static int g_video_plane_id = 0;
static DispPixFmt_t g_video_fmt = DISP_PIX_FMT_NV12;

static int g_ui_plane = -1;         /* disp_init_planes 句柄 */
static int g_ui_bufs[2] = { -1, -1 };
static DispBufferInfo_t g_ui_infos[2];
static int g_ui_front = 0;
static DispUiInfo g_ui_info;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- 内部工具 ---------------- */

static int plane_has_format(const DispPlane_t *p, DispPixFmt_t fmt)
{
    for (int i = 0; i < p->formatCount; ++i)
        if (p->formats[i] == fmt)
            return 1;
    return 0;
}

/* 枚举打印当前连接器全部图层与格式能力（板端诊断用） */
static void log_planes(void)
{
    const DispDev_t *d = &g_dev;

    printf("[engine] connector #%d planes (%d):\n", g_conn,
           d->connectors[g_conn].planeCount);
    for (int i = 0; i < d->connectors[g_conn].planeCount; ++i) {
        const DispPlane_t *p = &d->connectors[g_conn].planes[i];
        printf("[engine]   plane id=%u type=%d formats=[", p->id, p->type);
        for (int f = 0; f < p->formatCount; ++f)
            printf("%s%d", f ? "," : "", p->formats[f]);
        printf("]\n");
    }
}

/* 在连接器枚举顺序里找首个支持 fmt 的图层对象 ID，0 表示没有 */
static uint32_t first_plane_with_format(DispPixFmt_t fmt, uint32_t exclude_id)
{
    for (int i = 0; i < g_dev.connectors[g_conn].planeCount; ++i) {
        const DispPlane_t *p = &g_dev.connectors[g_conn].planes[i];
        if (p->id == exclude_id)
            continue;
        if (plane_has_format(p, fmt))
            return p->id;
    }
    return 0;
}

static void engine_release(void)
{
    if (g_ui_plane >= 0) {
        disp_uninit_planes(g_ui_plane);
        g_ui_plane = -1;
    }
    if (g_video_plane >= 0) {
        disp_uninit_planes(g_video_plane);
        g_video_plane = -1;
    }
    for (int i = 0; i < 2; ++i) {
        if (g_ui_bufs[i] >= 0) {
            disp_free_buffer(g_ui_bufs[i]);
            g_ui_bufs[i] = -1;
        }
    }
    if (g_valid) {
        disp_uninit(&g_dev);
        g_valid = 0;
    }
}

/* UI 图层完整初始化：区域/zpos/透明度/混合模式/双缓冲/首帧全透明提交 */
static int setup_ui_plane(int explicit_plane_id, int alpha8)
{
    uint32_t ui_id = 0;

    if (explicit_plane_id >= 0) {
        if (explicit_plane_id > 0xFFFF) {
            printf("[engine] invalid explicit UI plane id %d\n", explicit_plane_id);
            return -1;
        }
        ui_id = (uint32_t)explicit_plane_id;
        const DispPlane_t *p = NULL;
        for (int i = 0; i < g_dev.connectors[g_conn].planeCount; ++i)
            if (g_dev.connectors[g_conn].planes[i].id == ui_id)
                p = &g_dev.connectors[g_conn].planes[i];
        if (!p) {
            printf("[engine] explicit UI plane %u not found\n", ui_id);
            return -1;
        }
        if (!plane_has_format(p, DISP_PIX_FMT_ARGB8888)) {
            printf("[engine] explicit UI plane %u has no ARGB8888\n", ui_id);
            return -1;
        }
    } else {
        /* 首个支持 ARGB 且不与视频层重叠的图层 */
        ui_id = first_plane_with_format(DISP_PIX_FMT_ARGB8888,
                                        g_video_plane_id);
        if (!ui_id) {
            printf("[engine] no spare ARGB8888 plane for UI\n");
            return -1;
        }
    }

    g_ui_plane = disp_init_planes(g_conn, ui_id);
    if (g_ui_plane < 0) {
        printf("[engine] disp_init_planes(UI id=%u) failed\n", ui_id);
        return -1;
    }

    /* 全屏源区域 + 全屏目标区域 */
    disp_plane_set_src_region(g_ui_plane, 0, 0, g_screen_w, g_screen_h);
    disp_plane_set_crtc_region(g_ui_plane, 0, 0, g_screen_w, g_screen_h);

    if (disp_plane_set_zpos(g_ui_plane, DISP_UI_ZPOS) != 0)
        printf("[engine] warn: UI plane zpos=%d rejected, driver default applies\n",
               DISP_UI_ZPOS);

    if (alpha8 < 0)
        alpha8 = 0;
    if (alpha8 > 255)
        alpha8 = 255;
    uint32_t a16 = ((uint32_t)alpha8 << 8) | (uint32_t)alpha8;
    disp_plane_set_alpha(g_ui_plane, (uint16_t)a16);

    /* Qt 光栅引擎输出预乘 alpha → 优先 PREMULTI；被拒绝再退回 COVERAGE */
    int blend = DISP_BLEND_PREMULTI;
    if (disp_plane_set_blend_mode(g_ui_plane, blend) != 0) {
        blend = DISP_BLEND_COVERAGE;
        if (disp_plane_set_blend_mode(g_ui_plane, blend) != 0)
            printf("[engine] warn: blend mode rejected, driver default applies\n");
    }

    for (int i = 0; i < 2; ++i) {
        g_ui_bufs[i] = disp_alloc_buffer(g_screen_w, g_screen_h,
                                        DISP_PIX_FMT_ARGB8888);
        if (g_ui_bufs[i] < 0) {
            printf("[engine] disp_alloc_buffer(UI %dx%d) failed\n",
                   g_screen_w, g_screen_h);
            return -1;
        }
        if (disp_get_buffer_info(g_ui_bufs[i], &g_ui_infos[i]) != 0) {
            printf("[engine] disp_get_buffer_info(UI) failed\n");
            return -1;
        }
        /* 清成全透明：首帧前不遮挡视频层 */
        memset(g_ui_infos[i].pFramebuffer, 0, g_ui_infos[i].fbSize);
    }
    g_ui_front = 0;

    g_ui_info.plane_id = (int)ui_id;
    g_ui_info.width = g_screen_w;
    g_ui_info.height = g_screen_h;
    g_ui_info.blend_mode = blend;
    g_ui_info.alpha8 = alpha8;

    /* 首帧全透明提交：图层挂上 CRTC（内部加锁，保持路径一致） */
    if (disp_ui_commit() != 0) {
        printf("[engine] first UI commit failed\n");
        return -1;
    }

    printf("[engine] UI plane ready: id=%u zpos=%d alpha=%d/255 blend=%s "
           "bufs={%d,%d} stride=%d\n",
           ui_id, DISP_UI_ZPOS, alpha8,
           blend == DISP_BLEND_PREMULTI ? "PREMULTI" : "COVERAGE",
           g_ui_bufs[0], g_ui_bufs[1], g_ui_infos[0].fbStride);
    return 0;
}

/* ---------------- 生命周期 ---------------- */

int disp_engine_init(DispPixFmt_t video_fmt, int explicit_ui_plane_id,
                     int alpha8)
{
    if (disp_init(&g_dev) != 0) {
        printf("[engine] disp_init failed\n");
        return -1;
    }
    g_valid = 1;

    /* 找已启用的连接器（HDMI-A-2 → video_port1 1080p60） */
    g_conn = -1;
    for (int i = 0; i < g_dev.connectorCount; ++i) {
        if (g_dev.connectors[i].enabled) {
            g_conn = i;
            break;
        }
    }
    if (g_conn < 0) {
        printf("[engine] no enabled connector\n");
        goto fail;
    }
    g_screen_w = g_dev.connectors[g_conn].screenWidth;
    g_screen_h = g_dev.connectors[g_conn].screenHeight;
    printf("[engine] connector #%d: %dx%d@%dHz\n", g_conn, g_screen_w,
           g_screen_h, g_dev.connectors[g_conn].screenRefresh);

    log_planes();

    /* 视频层：首个支持所需格式的图层（与 cameraPlayer pick_plane 一致） */
    g_video_fmt = video_fmt;
    uint32_t video_id = first_plane_with_format(video_fmt, 0);
    if (!video_id) {
        printf("[engine] no DRM plane supports format %d\n", video_fmt);
        goto fail;
    }
    g_video_plane_id = video_id;
    g_video_plane = disp_init_planes(g_conn, video_id);
    if (g_video_plane < 0) {
        printf("[engine] disp_init_planes(video id=%u) failed\n", video_id);
        goto fail;
    }
    printf("[engine] video plane: id=%u fmt=%d zpos=%d\n", video_id,
           video_fmt, DISP_VIDEO_ZPOS);
    /* 视频层压底；失败仅告警（ui_main 在板上验证过 zpos=0/10 组合） */
    if (disp_plane_set_zpos(g_video_plane, DISP_VIDEO_ZPOS) != 0)
        printf("[engine] warn: video plane zpos=%d rejected, driver default applies\n",
               DISP_VIDEO_ZPOS);

    if (setup_ui_plane(explicit_ui_plane_id, alpha8) != 0)
        goto fail;

    return 0;

fail:
    engine_release();
    return -1;
}

void disp_engine_deinit(void)
{
    if (!g_valid && g_video_plane < 0 && g_ui_plane < 0)
        return;
    engine_release();
    memset(&g_ui_info, 0, sizeof(g_ui_info));
}

/* ---------------- UI 图层（Qt 渲染线程） ---------------- */

const DispUiInfo *disp_ui_info(void)
{
    return &g_ui_info;
}

int disp_ui_begin(void **map, int *stride)
{
    int back = !g_ui_front;
    if (map)
        *map = g_ui_infos[back].pFramebuffer;
    if (stride)
        *stride = g_ui_infos[back].fbStride;
    return 0;
}

int disp_ui_commit(void)
{
    int back = !g_ui_front;
    int rc;

    pthread_mutex_lock(&g_lock);
    rc = disp_commit(g_ui_plane, g_ui_bufs[back]);
    pthread_mutex_unlock(&g_lock);
    if (rc == 0)
        g_ui_front = back;
    return rc;
}

void disp_screen_size(int *w, int *h)
{
    if (w)
        *w = g_screen_w;
    if (h)
        *h = g_screen_h;
}

/* ---------------- 视频图层（视频线程） ---------------- */

int disp_video_alloc_buffer(int width, int height, DispPixFmt_t format,
                            int *buf, DispBufferInfo_t *info)
{
    int handle;
    int rc = 0;

    pthread_mutex_lock(&g_lock);
    handle = disp_alloc_buffer(width, height, format);
    if (handle < 0) {
        rc = -1;
    } else if (disp_get_buffer_info(handle, info) != 0) {
        disp_free_buffer(handle);
        rc = -1;
    }
    pthread_mutex_unlock(&g_lock);
    if (rc == 0 && buf)
        *buf = handle;
    return rc;
}

int disp_video_import_dmabuf(const DispImportDesc_t *desc)
{
    int buf;
    pthread_mutex_lock(&g_lock);
    buf = disp_import_dmabuf(desc);
    pthread_mutex_unlock(&g_lock);
    return buf;
}

void disp_video_free_buffer(int buf)
{
    if (buf < 0)
        return;
    pthread_mutex_lock(&g_lock);
    disp_free_buffer(buf);
    pthread_mutex_unlock(&g_lock);
}

int disp_video_set_region(int src_x, int src_y, int src_w, int src_h,
                          int crtc_x, int crtc_y, int crtc_w, int crtc_h)
{
    int rc;
    pthread_mutex_lock(&g_lock);
    rc = disp_plane_set_src_region(g_video_plane, src_x, src_y, src_w, src_h);
    if (rc == 0)
        rc = disp_plane_set_crtc_region(g_video_plane, crtc_x, crtc_y,
                                       crtc_w, crtc_h);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int disp_video_commit(int buf)
{
    int rc;
    pthread_mutex_lock(&g_lock);
    rc = disp_commit(g_video_plane, buf);
    pthread_mutex_unlock(&g_lock);
    return rc;
}
