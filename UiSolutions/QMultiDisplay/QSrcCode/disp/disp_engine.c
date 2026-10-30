/**
 * disp_engine — 共享显示引擎实现（单 disp_init 管 DRM 设备 + 视频/UI/光标图层）
 *
 * 线程模型：
 *   - init/deinit 由主线程在 Qt 事件循环外串行调用；
 *   - disp_ui_*   由 Qt 渲染线程调用；
 *   - disp_video_* 由视频线程调用；
 *   - disp_cursor_move 由 GUI 线程写待应用位置，视频线程提交时取走；
 *   display_pro 不保证线程安全 → 所有会触碰 DRM 的路径统一经 g_lock。
 *   disp_commit 阻塞至 vsync（约 16.7ms @60Hz）期间持锁，另一路提交最多
 *   等一帧，实测影响可忽略（UI 仅在界面变化时提交）。
 */

#include "disp_engine.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static int g_cursor_plane = -1;     /* 光标图层；-1 表示无空闲图层 */
static int g_cursor_buf = -1;
static DispBufferInfo_t g_cursor_info;
static int g_cursor_size = 0;
static int g_cursor_merge = 0;      /* 1 = 光标位置随视频提交一并上屏 */

/* 待应用光标位置：GUI 线程无锁写入（先 px/py 后 dirty），视频提交时取走 */
static int g_cursor_px = -1;
static int g_cursor_py = -1;
static int g_cursor_dirty = 0;

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

/* 同上，排除两个已占用图层（UI + 视频） */
static uint32_t first_plane_with_format2(DispPixFmt_t fmt, uint32_t ex1,
                                         uint32_t ex2)
{
    for (int i = 0; i < g_dev.connectors[g_conn].planeCount; ++i) {
        const DispPlane_t *p = &g_dev.connectors[g_conn].planes[i];
        if (p->id == ex1 || p->id == ex2)
            continue;
        if (plane_has_format(p, fmt))
            return p->id;
    }
    return 0;
}

/* 阻塞提交，遇 EBUSY 短重试（可能与其他提交争同一 CRTC） */
static int commit_retry(int plane, int buf)
{
    for (int i = 0; i < 40; ++i) {
        int rc = disp_commit_ex(plane, buf, 0);
        if (rc == 0 || errno != EBUSY)
            return rc;
        usleep(1000);
    }
    return -EBUSY;
}

/* 多图层合帧提交（视频 + 光标），遇 EBUSY 短重试 */
static int commit_multi_retry(const int *planes, const int *bufs, int count)
{
    for (int i = 0; i < 40; ++i) {
        int rc = disp_commit_multi(planes, bufs, count, 0);
        if (rc == 0 || errno != EBUSY)
            return rc;
        usleep(1000);
    }
    return -EBUSY;
}

static void engine_release(void)
{
    g_cursor_merge = 0;
    if (g_cursor_plane >= 0) {
        disp_uninit_planes(g_cursor_plane);
        g_cursor_plane = -1;
    }
    if (g_cursor_buf >= 0) {
        disp_free_buffer(g_cursor_buf);
        g_cursor_buf = -1;
    }
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

/* 光标层初始化：空闲 ARGB 图层 + 32x32 缓冲，全透明先挂上 CRTC。
   无空闲图层时仅告警，不影响视频/UI 通路。 */
static int setup_cursor_plane(void)
{
    const int sz = DISP_CURSOR_SIZE;
    const uint32_t ui_id = (uint32_t)g_ui_info.plane_id;
    uint32_t id = first_plane_with_format2(DISP_PIX_FMT_ARGB8888,
                                           g_video_plane_id, ui_id);

    if (!id) {
        printf("[engine] no spare ARGB8888 plane for cursor, "
               "hardware cursor disabled\n");
        return 0;
    }

    g_cursor_plane = disp_init_planes(g_conn, id);
    if (g_cursor_plane < 0) {
        printf("[engine] disp_init_planes(cursor id=%u) failed\n", id);
        return 0;
    }

    disp_plane_set_src_region(g_cursor_plane, 0, 0, sz, sz);
    disp_plane_set_crtc_region(g_cursor_plane, 0, 0, sz, sz);
    if (disp_plane_set_zpos(g_cursor_plane, DISP_CURSOR_ZPOS) != 0)
        printf("[engine] warn: cursor plane zpos=%d rejected\n",
               DISP_CURSOR_ZPOS);
    disp_plane_set_alpha(g_cursor_plane, 0xFFFF);
    /* 与 UI 层用同一混合模式，Qt 侧按同一 QImage 格式绘制光标 */
    if (disp_plane_set_blend_mode(g_cursor_plane,
                                  (uint64_t)g_ui_info.blend_mode) != 0)
        printf("[engine] warn: cursor blend mode rejected\n");

    g_cursor_buf = disp_alloc_buffer(sz, sz, DISP_PIX_FMT_ARGB8888);
    if (g_cursor_buf < 0 ||
        disp_get_buffer_info(g_cursor_buf, &g_cursor_info) != 0) {
        printf("[engine] cursor buffer alloc failed\n");
        if (g_cursor_buf >= 0) {
            disp_free_buffer(g_cursor_buf);
            g_cursor_buf = -1;
        }
        disp_uninit_planes(g_cursor_plane);
        g_cursor_plane = -1;
        return 0;
    }
    memset(g_cursor_info.pFramebuffer, 0, g_cursor_info.fbSize);
    g_cursor_size = sz;

    /* 首帧全透明提交：图层挂上 CRTC，之后位置随视频提交一并更新 */
    if (disp_commit(g_cursor_plane, g_cursor_buf) != 0)
        printf("[engine] warn: first cursor commit failed\n");
    g_cursor_merge = 1;

    printf("[engine] cursor plane ready: id=%u zpos=%d %dx%d\n", id,
           DISP_CURSOR_ZPOS, sz, sz);
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

    setup_cursor_plane();

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
    rc = commit_retry(g_ui_plane, g_ui_bufs[back]);
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
    if (g_cursor_merge && disp_cursor_available()) {
        int planes[2] = { g_video_plane, g_cursor_plane };
        int bufs[2] = { buf, g_cursor_buf };
        int dirty = __atomic_exchange_n(&g_cursor_dirty, 0, __ATOMIC_ACQ_REL);

        /* 光标位置与视频同一次 atomic commit 生效，不额外占 vsync */
        if (dirty) {
            int px = __atomic_load_n(&g_cursor_px, __ATOMIC_RELAXED);
            int py = __atomic_load_n(&g_cursor_py, __ATOMIC_RELAXED);
            int cx = px - g_cursor_size / 2;
            int cy = py - g_cursor_size / 2;

            if (cx < 0)
                cx = 0;
            if (cy < 0)
                cy = 0;
            if (cx > g_screen_w - g_cursor_size)
                cx = g_screen_w - g_cursor_size;
            if (cy > g_screen_h - g_cursor_size)
                cy = g_screen_h - g_cursor_size;
            if (disp_plane_set_crtc_region(g_cursor_plane, cx, cy,
                                           g_cursor_size, g_cursor_size) != 0)
                __atomic_store_n(&g_cursor_dirty, 1, __ATOMIC_RELEASE);
        }

        rc = commit_multi_retry(planes, bufs, 2);
        if (rc != 0) {
            /* 光标层合帧被拒（资源冲突等）：放弃合帧，先保住视频通路 */
            printf("[engine] warn: merged video+cursor commit failed (%d), "
                   "cursor follows video disabled\n", rc);
            g_cursor_merge = 0;
            rc = commit_retry(g_video_plane, buf);
        }
    } else {
        rc = commit_retry(g_video_plane, buf);
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* ---------------- 光标图层 ---------------- */

int disp_cursor_available(void)
{
    return g_cursor_plane >= 0 && g_cursor_buf >= 0;
}

int disp_cursor_map(void **map, int *stride)
{
    if (!disp_cursor_available())
        return -1;
    if (map)
        *map = g_cursor_info.pFramebuffer;
    if (stride)
        *stride = g_cursor_info.fbStride;
    return g_cursor_info.pFramebuffer ? 0 : -1;
}

int disp_cursor_move(int x, int y)
{
    if (!disp_cursor_available())
        return -1;

    /* 只记录位置，不提交：由视频层的下一次 disp_video_commit 合帧上屏 */
    __atomic_store_n(&g_cursor_px, x, __ATOMIC_RELAXED);
    __atomic_store_n(&g_cursor_py, y, __ATOMIC_RELAXED);
    __atomic_store_n(&g_cursor_dirty, 1, __ATOMIC_RELEASE);
    return 0;
}
