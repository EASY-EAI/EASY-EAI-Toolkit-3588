#define _GNU_SOURCE

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "app_log.h"
#include "drm_display.h"

#include "capture.h"
#include "gpu_util.h"
#include "mouse.h"
#include "rga_util.h"
#include "stats.h"
#include "ui_render.h"

typedef enum {
    COMPOSE_RGA = 0,
    COMPOSE_GPU,
} ComposeMode;

volatile sig_atomic_t g_quit;
log_handle_t g_log = LOG_HANDLE_INVALID;
static CaptureState g_capture[2];
static bool g_mirror;
static ComposeMode g_compose = COMPOSE_GPU;

/* Default: cam0=/dev/video44 (rkisp0-vir0), cam1=/dev/video53 (rkisp0-vir1). */
static int g_video_nums[2] = { 44, 53 };

/*
 * 一个真实 DRM 图层：drm_init_planes() 得到的图层句柄 +
 * drm_alloc_buffer()/drm_get_buffer_info() 得到的缓冲区句柄与信息。
 * 四个图层 video/ui/stats/cursor 分别对应一个独立 plane。
 */
typedef struct {
    int plane;      /* 图层句柄；< 0 表示未启用 */
    int buffer;     /* 缓冲区句柄；< 0 表示未分配 */
    Drmbuff_t info; /* 缓冲区信息（mmap 地址 / 跨距 / dma-buf fd） */
} Layer;

static Display_dev_t g_display;
static int g_conn_index = -1;
static DemoLayout g_layout;
static Layer g_video = { -1, -1, { 0 } };
static Layer g_ui = { -1, -1, { 0 } };
static Layer g_stats = { -1, -1, { 0 } };
static Layer g_cursor = { -1, -1, { 0 } };

/* DRM plane 叠放顺序：video 在最底，ui 全屏叠其上（透明洞露出 video），
 * stats 面板位于 ui 之上，cursor 置于最顶层。本板 zpos 属性有效范围为
 * 0..11（>=12 返回 EINVAL），故三者分别取 9 / 10 / 11。 */
#define UI_ZPOS      9
#define STATS_ZPOS   10
#define CURSOR_ZPOS  11

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
    for (int i = 0; i < 2; ++i) {
        g_capture[i].running = false;
        pthread_cond_broadcast(&g_capture[i].queue.not_empty);
        pthread_cond_broadcast(&g_capture[i].queue.not_full);
    }
}

static void print_usage(const char *name)
{
    printf("%s [-w cam_w] [-h cam_h] [-m] [-C rga|gpu]\n"
           "  [-L left_video_num] [-R right_video_num]\n"
           "  -C: video side-by-side compose backend (default gpu, no CPU fallback).\n"
           "      ui layer 恒用 GPU 绘制几何/文字/纯色矩形。\n"
           "  video/ui/stats/cursor 图层按连接器枚举的格式能力自动选择，无需指定。\n",
           name);
}

static int parse_compose_mode(const char *s, ComposeMode *out)
{
    if (!s || !out)
        return -1;
    if (!strcmp(s, "rga")) {
        *out = COMPOSE_RGA;
        return 0;
    }
    if (!strcmp(s, "gpu")) {
        *out = COMPOSE_GPU;
        return 0;
    }
    return -1;
}

/* 将一块 DRM 缓冲区信息包装成可绘制的 ARGB 画布。 */
static Canvas canvas_of(const Drmbuff_t *info, int width, int height)
{
    Canvas c;
    c.map = info->pFramebuffer;
    c.width = width;
    c.height = height;
    c.pitch = info->fbStride;
    c.size = info->fbSize;
    c.fd = info->dmabuf_fd;
    return c;
}

/* RGA wstride 按像素；本 demo NV12 字节跨距通常等于像素宽。 */
static int nv12_wstride_px(int stride_bytes, int width)
{
    if (stride_bytes > 0)
        return stride_bytes;
    return width;
}

static void v4l2_to_rga(const V4l2Frame *in, RgaBuffer *out)
{
    memset(out, 0, sizeof(*out));
    out->width = in->width;
    out->height = in->height;
    out->wstride = nv12_wstride_px(in->stride, in->width);
    out->hstride = in->height;
    out->format = RGA_FMT_YCbCr_420_SP;
    if (in->dma_fd >= 0) {
        out->mem_type = RGA_MEM_FD;
        out->fd = in->dma_fd;
    } else {
        out->mem_type = RGA_MEM_VIR;
        out->vir = (void *)in->y;
    }
}

static void info_to_rga(const Drmbuff_t *in, int width, int height, RgaBuffer *out)
{
    memset(out, 0, sizeof(*out));
    out->width = width;
    out->height = height;
    out->wstride = nv12_wstride_px(in->fbStride, width);
    out->hstride = height;
    out->format = RGA_FMT_YCbCr_420_SP;
    if (in->dmabuf_fd >= 0) {
        out->mem_type = RGA_MEM_FD;
        out->fd = in->dmabuf_fd;
    } else {
        out->mem_type = RGA_MEM_VIR;
        out->vir = in->pFramebuffer;
    }
}

static void v4l2_to_gpu(const V4l2Frame *in, GpuBuffer *out)
{
    memset(out, 0, sizeof(*out));
    out->width = in->width;
    out->height = in->height;
    out->stride = in->stride;
    out->format = GPU_FMT_NV12;
    if (in->dma_fd >= 0) {
        out->mem_type = GPU_MEM_FD;
        out->fd = in->dma_fd;
    } else {
        out->mem_type = GPU_MEM_VIR;
        out->vir = (void *)in->y;
    }
}

static void info_to_gpu_argb(const Drmbuff_t *in, int width, int height, GpuBuffer *out)
{
    memset(out, 0, sizeof(*out));
    out->width = width;
    out->height = height;
    out->stride = in->fbStride;
    out->format = GPU_FMT_ARGB8888;
    if (in->dmabuf_fd >= 0) {
        out->mem_type = GPU_MEM_FD;
        out->fd = in->dmabuf_fd;
    } else {
        out->mem_type = GPU_MEM_VIR;
        out->vir = in->pFramebuffer;
    }
}

/**
 * 按 -C 选择的后端做左右拼接，写入视频图层缓冲区。
 * COMPOSE_RGA / COMPOSE_GPU 失败时直接返回负值，禁止回退。
 */
static int compose_side_by_side(const Drmbuff_t *dst, int dst_w, int dst_h,
                                const V4l2Frame *left, const V4l2Frame *right,
                                int swap)
{
    RgaBuffer rga_l, rga_r, rga_d;
    GpuBuffer gpu_l, gpu_r, gpu_d;

    switch (g_compose) {
    case COMPOSE_RGA:
        v4l2_to_rga(left, &rga_l);
        v4l2_to_rga(right, &rga_r);
        info_to_rga(dst, dst_w, dst_h, &rga_d);
        return rga_compose_side_by_side(&rga_l, &rga_r, &rga_d, swap);
    case COMPOSE_GPU:
        v4l2_to_gpu(left, &gpu_l);
        v4l2_to_gpu(right, &gpu_r);
        info_to_gpu_argb(dst, dst_w, dst_h, &gpu_d);
        return gpu_compose_side_by_side(&gpu_l, &gpu_r, &gpu_d, swap);
    default:
        return -1;
    }
}

/* 按连接器实际分辨率推导显示器造型布点。 */
static void layout_init(DemoLayout *L, int width, int height)
{
    int margin_x = width * 3 / 100;
    int body_x = margin_x;
    int body_w = width - 2 * margin_x;
    int body_y = height * 2 / 100;
    int body_h = height * 66 / 100;
    int bezel = body_w * 22 / 1000;
    int top_extra;

    if (bezel < 24)
        bezel = 24;
    top_extra = bezel / 2;

    L->width = width;
    L->height = height;
    L->body_x = body_x;
    L->body_y = body_y;
    L->body_w = body_w;
    L->body_h = body_h;
    L->screen_x = body_x + bezel;
    L->screen_y = body_y + bezel + top_extra;
    L->screen_w = body_w - 2 * bezel;
    L->screen_h = body_h - 2 * bezel - top_extra;

    L->stats_x = width - STATS_W - 18;
    L->stats_y = height - STATS_H - 18;
    if (L->stats_x < 0)
        L->stats_x = 0;
    if (L->stats_y < 0)
        L->stats_y = 0;

    L->btn_x = L->btn_y = L->btn_w = L->btn_h = 0;
}

/* 申请一个图层：初始化 plane 与缓冲区并读取缓冲区信息。 */
static int init_layer(Layer *l, uint32_t plane_id, int width, int height, uint32_t format)
{
    l->plane = drm_init_planes(g_conn_index, plane_id);
    if (l->plane < 0) {
        LOG_E("drm_init_planes(id=%u) failed\n", plane_id);
        return -1;
    }
    l->buffer = drm_alloc_buffer(width, height, format);
    if (l->buffer < 0) {
        LOG_E("drm_alloc_buffer(%dx%d, fmt=0x%08x) failed\n",
                width, height, format);
        return -1;
    }
    if (drm_get_buffer_info(l->buffer, &l->info) != 0) {
        LOG_E("drm_get_buffer_info failed\n");
        return -1;
    }
    return 0;
}

/* 在当前连接器枚举到的图层中，按像素格式能力挑选一个尚未被占用的图层，
 * 返回其 DRM 图层对象 ID；找不到返回 0。used 为已选中的图层 ID 列表。 */
static uint32_t pick_plane(uint32_t want_fmt, const uint32_t *used, int nused)
{
    const int conn = g_conn_index;
    int i, f, u;

    for (i = 0; i < g_display.connectors[conn].planeCount; ++i) {
        const Display_plane_t *p = &g_display.connectors[conn].planes[i];
        bool taken = false;

        for (u = 0; u < nused; ++u)
            if (used[u] == p->id) { taken = true; break; }
        if (taken)
            continue;
        for (f = 0; f < p->formatCount; ++f)
            if (p->formats[f] == want_fmt)
                return p->id;
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int cam_w = 1920, cam_h = 1080;
    const int vid_w = cam_w * 2;
    const int vid_h = cam_h;
    MouseState *mouse = NULL;
    StatsContext stats_ctx;
    bool rga_inited = false;
    bool gpu_inited = false;
    int opt;

    while ((opt = getopt(argc, argv, "w:h:mC:L:R:?")) != -1) {
        switch (opt) {
        case 'w': cam_w = atoi(optarg); break;
        case 'h': cam_h = atoi(optarg); break;
        case 'm': g_mirror = true; break;
        case 'C':
            if (parse_compose_mode(optarg, &g_compose) != 0) {
                LOG_E("invalid -C %s (want rga|gpu)\n", optarg);
                print_usage(argv[0]);
                return 1;
            }
            break;
        case 'L': g_video_nums[0] = atoi(optarg); break;
        case 'R': g_video_nums[1] = atoi(optarg); break;
        default: print_usage(argv[0]); return 0;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 接入 log_manager_pro：注册本进程模块句柄，供 LOG_* 宏输出。 */
    {
        log_mgr_t mgr = log_manager_init("/userdata/logs/vo_drm_4plane.ini");
        if (mgr != LOG_MGR_INVALID)
            g_log = log_register(mgr, "vo_drm_4plane");
        if (g_log == LOG_HANDLE_INVALID)
            fprintf(stderr, "log_manager_pro init failed, logging disabled\n");
    }

    for (int i = 0; i < 2; ++i) {
        memset(&g_capture[i], 0, sizeof(g_capture[i]));
        frame_queue_init(&g_capture[i].queue);
        g_capture[i].video_num = g_video_nums[i];
        g_capture[i].width = cam_w;
        g_capture[i].height = cam_h;
    }

    /* 初始化 DRM 设备并挑选已启用的连接器。 */
    if (drm_init(&g_display) != 0) {
        LOG_E("drm_init failed\n");
        goto fail_sys;
    }
    for (int i = 0; i < g_display.connectorCount; ++i) {
        if (g_display.connectors[i].enabled) {
            g_conn_index = g_display.connectors[i].index;
            break;
        }
    }
    if (g_conn_index < 0) {
        LOG_E("no enabled DRM connector found\n");
        goto fail_drm;
    }
    layout_init(&g_layout,
                g_display.connectors[g_conn_index].screenWidth,
                g_display.connectors[g_conn_index].screenHeight);
    LOG_I("display: %dx%d@%dHz (connector %d)\n",
           g_layout.width, g_layout.height,
           g_display.connectors[g_conn_index].screenRefresh, g_conn_index);

    /*
     * 四个 plane 对应四个真实图层，按连接器枚举出的图层格式能力自动挑选，
     * 不再依赖命令行入参：
     *   video：rga 用 NV12，gpu 用 GLES 输出的 ARGB8888；
     *   ui / stats / cursor：全 ARGB8888。
     */
    uint32_t video_fmt = (g_compose == COMPOSE_GPU) ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_NV12;
    uint32_t used[4];
    uint32_t video_id, ui_id, stats_id, cursor_id;

    for (int i = 0; i < g_display.connectors[g_conn_index].planeCount; ++i) {
        const Display_plane_t *p = &g_display.connectors[g_conn_index].planes[i];
        LOG_I("plane[%d]: id=%u type=%d formats=%d\n", i, p->id, p->type, p->formatCount);
    }
    used[0] = video_id  = pick_plane(video_fmt, used, 0);
    used[1] = ui_id     = pick_plane(DRM_FORMAT_ARGB8888, used, 1);
    used[2] = stats_id  = pick_plane(DRM_FORMAT_ARGB8888, used, 2);
    used[3] = cursor_id = pick_plane(DRM_FORMAT_ARGB8888, used, 3);
    if (!video_id || !ui_id || !stats_id) {
        LOG_E("no suitable DRM plane (video=%u ui=%u stats=%u)\n",
              video_id, ui_id, stats_id);
        goto fail_drm;
    }
    LOG_I("picked planes: video=%u ui=%u stats=%u cursor=%u\n",
          video_id, ui_id, stats_id, cursor_id);

    if (init_layer(&g_video, video_id, vid_w, vid_h, video_fmt) != 0)
        goto fail_drm;
    if (init_layer(&g_ui, ui_id, g_layout.width, g_layout.height,
                   DRM_FORMAT_ARGB8888) != 0)
        goto fail_drm;
    if (init_layer(&g_stats, stats_id, STATS_W, STATS_H, DRM_FORMAT_ARGB8888) != 0)
        goto fail_drm;
    if (cursor_id != 0 &&
        init_layer(&g_cursor, cursor_id, CURSOR_W, CURSOR_H, DRM_FORMAT_ARGB8888) != 0)
        goto fail_drm;

    /* 逐图层设置叠放顺序（立即通过 atomic commit 生效）。 */
    if (drm_plane_set_zpos(g_ui.plane, UI_ZPOS) != 0)
        LOG_E("set ui zpos=%d failed\n", UI_ZPOS);
    if (drm_plane_set_zpos(g_stats.plane, STATS_ZPOS) != 0)
        LOG_E("set stats zpos=%d failed\n", STATS_ZPOS);
    if (g_cursor.plane >= 0 && drm_plane_set_zpos(g_cursor.plane, CURSOR_ZPOS) != 0)
        LOG_E("set cursor zpos=%d failed\n", CURSOR_ZPOS);

    /*
     * 合成后端初始化：ui / stats 图层固定由 GPU 绘制（几何/文字/纯色矩形），
     * GPU 始终需要初始化；rga 仅在 -C rga 的视频左右拼接路径用到。
     * GPU 所需的 DRM fd 由 gpu_composer_init 内部自行打开与关闭。
     */
    if (rga_init() != 0) {
        LOG_E("rga_init failed\n");
        goto fail_drm;
    }
    rga_inited = true;
    if (gpu_composer_init() != 0) {
        LOG_E("gpu_composer_init failed\n");
        goto fail_drm;
    }
    gpu_inited = true;
    /* UI/stats 由 GPU 绘制，都需要 dma-buf FD；不做 CPU 回退。 */
    if (g_ui.info.dmabuf_fd < 0 || g_stats.info.dmabuf_fd < 0) {
        LOG_E("ui/stats buffer requires a dma-buf FD for GPU render\n");
        goto fail_drm;
    }
    if (g_compose == COMPOSE_GPU && g_video.info.dmabuf_fd < 0) {
        LOG_E("gpu compose backend requires video buffer dma-buf FD\n");
        goto fail_drm;
    }
    LOG_I("compose backend: %s (ui: gpu geometry/text/rect)\n",
           g_compose == COMPOSE_GPU ? "gpu" : "rga");

    /*
     * 各图层区域只在初始化时配置一次，之后仅按需 drm_commit：
     *   ui    全屏、只提交一次；
     *   stats 固定在右下角、内容刷新时提交；
     *   cursor 源区域固定、目标区域随鼠标位置每帧更新；
     *   video  源区域固定、目标区域每帧按 letterbox 计算。
     */
    Canvas ui_canvas = canvas_of(&g_ui.info, g_layout.width, g_layout.height);
    draw_ui(&ui_canvas, &g_layout);
    drm_plane_set_src_region(g_ui.plane, 0, 0, g_layout.width, g_layout.height);
    drm_plane_set_crtc_region(g_ui.plane, 0, 0, g_layout.width, g_layout.height);
    if (drm_commit(g_ui.plane, g_ui.buffer) != 0)
        LOG_E("commit ui layer failed\n");

    drm_plane_set_src_region(g_stats.plane, 0, 0, STATS_W, STATS_H);
    drm_plane_set_crtc_region(g_stats.plane, g_layout.stats_x, g_layout.stats_y,
                              STATS_W, STATS_H);

    drm_plane_set_src_region(g_video.plane, 0, 0, vid_w, vid_h);

    if (g_cursor.plane >= 0) {
        Canvas cursor_canvas = canvas_of(&g_cursor.info, CURSOR_W, CURSOR_H);
        draw_cursor(&cursor_canvas);
        drm_plane_set_src_region(g_cursor.plane, 0, 0, CURSOR_W, CURSOR_H);
    }

    mouse = mouse_create();
    if (!mouse) {
        LOG_E("mouse_create failed\n");
        goto fail_drm;
    }
    mouse_set_pos(mouse, g_layout.width / 2, g_layout.height / 2);
    if (mouse_start(mouse) != 0) {
        LOG_E("mouse_start failed\n");
        goto fail_drm;
    }
    if (capture_init(&g_capture[0]) != 0) {
        LOG_E("V4L2 init /dev/video%d failed\n", g_capture[0].video_num);
        goto fail_drm;
    }
    if (!g_mirror && capture_init(&g_capture[1]) != 0) {
        LOG_E("V4L2 camera1 /dev/video%d unavailable, use camera0 mirror mode\n",
                g_capture[1].video_num);
        g_mirror = true;
    }
    if (pthread_create(&g_capture[0].thread, NULL, capture_thread, &g_capture[0]) != 0)
        goto fail_cap;
    if (!g_mirror && pthread_create(&g_capture[1].thread, NULL, capture_thread, &g_capture[1]) != 0)
        goto fail_cap;

    DemoStats stats = { .display_w = g_layout.width, .display_h = g_layout.height };
    stats_init(&stats_ctx);
    unsigned long frames = 0;
    bool swap = false;

    while (!g_quit) {
        V4l2Frame left;
        V4l2Frame right;
        int cx_click, cy_click;
        if (mouse_consume_click(mouse, &cx_click, &cy_click)) {
            if (cx_click >= g_layout.btn_x && cx_click < g_layout.btn_x + g_layout.btn_w &&
                cy_click >= g_layout.btn_y && cy_click < g_layout.btn_y + g_layout.btn_h)
                swap = !swap;
        }
        if (frame_queue_pop(&g_capture[0].queue, &left) != 0)
            continue;
        bool have_right = g_mirror;
        if (g_mirror) {
            right = left;
        } else if (frame_queue_pop(&g_capture[1].queue, &right) != 0) {
            capture_release_frame(&g_capture[0], &left);
            continue;
        }
        /* rga/gpu: compose_side_by_side 失败不回退。 */
        if (compose_side_by_side(&g_video.info, vid_w, vid_h, &left, &right,
                                 swap ? 1 : 0) == 0) {
            int vw, vh, vx, vy;
            int cx, cy;
            bool ok;
            vw = g_layout.screen_w;
            vh = vw * cam_h / (cam_w * 2);
            if (vh > g_layout.screen_h) {
                vh = g_layout.screen_h;
                vw = vh * (cam_w * 2) / cam_h;
            }
            vx = g_layout.screen_x + (g_layout.screen_w - vw) / 2;
            vy = g_layout.screen_y + (g_layout.screen_h - vh) / 2;
            drm_plane_set_crtc_region(g_video.plane, vx, vy, vw, vh);
            ok = (drm_commit(g_video.plane, g_video.buffer) == 0);

            if (stats_update(&stats_ctx, &stats, frames)) {
                Canvas stats_canvas = canvas_of(&g_stats.info, STATS_W, STATS_H);
                draw_stats(&stats_canvas, &stats);
                if (drm_commit(g_stats.plane, g_stats.buffer) != 0)
                    LOG_E("commit stats layer failed\n");
            }

            if (g_cursor.plane >= 0) {
                mouse_get_pos(mouse, &cx, &cy);
                if (cx < 0) cx = 0;
                if (cy < 0) cy = 0;
                if (cx > g_layout.width - CURSOR_W) cx = g_layout.width - CURSOR_W;
                if (cy > g_layout.height - CURSOR_H) cy = g_layout.height - CURSOR_H;
                drm_plane_set_crtc_region(g_cursor.plane, cx, cy, CURSOR_W, CURSOR_H);
                drm_commit(g_cursor.plane, g_cursor.buffer);
            }
            if (ok)
                frames++;
        }
        capture_release_frame(&g_capture[0], &left);
        if (!have_right)
            capture_release_frame(&g_capture[1], &right);
    }

fail_cap:
    for (int i = 0; i < 2; ++i)
        g_capture[i].running = false;
    pthread_cond_broadcast(&g_capture[0].queue.not_empty);
    pthread_cond_broadcast(&g_capture[0].queue.not_full);
    pthread_cond_broadcast(&g_capture[1].queue.not_empty);
    pthread_cond_broadcast(&g_capture[1].queue.not_full);
    if (g_capture[0].thread)
        pthread_join(g_capture[0].thread, NULL);
    if (g_capture[1].thread)
        pthread_join(g_capture[1].thread, NULL);
    for (int i = 0; i < 2; ++i) {
        V4l2Frame leftover;
        while (frame_queue_pop(&g_capture[i].queue, &leftover) == 0)
            capture_release_frame(&g_capture[i], &leftover);
        capture_deinit(&g_capture[i]);
    }
    mouse_destroy(mouse);
    mouse = NULL;
fail_drm:
    if (gpu_inited)
        gpu_composer_deinit();
    if (rga_inited)
        rga_deinit();
    /* drm_uninit 兜底释放仍登记的图层与缓冲区。 */
    drm_uninit(&g_display);
fail_sys:
    frame_queue_destroy(&g_capture[0].queue);
    frame_queue_destroy(&g_capture[1].queue);
    return 0;
}
