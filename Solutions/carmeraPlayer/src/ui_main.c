#define _GNU_SOURCE

#ifdef ENABLE_UI

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* DRM_MODE_ATOMIC_NONBLOCK；SDK 的 display 头已解耦 libdrm，需显式包含。 */
#include <drm_mode.h>

#include "app_log.h"
#include "capture.h"
#include "display_win.h"
#include "drm_display.h"
#include "gpu_util.h"
#include "mouse.h"
#include "rga_util.h"
#include "stats.h"
#include "ui_render.h"

/**
 * 本 demo 只用两个真实 DRM 图层：
 *   - video plane：双摄左右拼接后的视频（-C gpu 时 ARGB8888，-C rga 时 NV12）
 *   - canvas plane：整屏 ARGB8888 画布，由 GPU 把下面的窗口帧贴上去
 * 其余内容不再各占一个硬件图层，而是登记为 display_win 窗口：
 *   shell（整屏显示器造型，含屏幕透明洞）/ stats / button / cursor。
 * 窗口内容仍由 ui_render 用 CPU 光栅到各自的 dumb 缓冲，再经 dma-buf
 * 导入 GPU 纹理合成进画布；屏幕洞处画布 alpha=0，由 plane 混合透出视频。
 */

typedef enum {
    COMPOSE_RGA = 0,
    COMPOSE_GPU,
} ComposeMode;

/* display_win 窗口角色；zpos 越大越靠前。 */
typedef enum {
    WIN_SHELL = 0,
    WIN_STATS,
    WIN_BUTTON,
    WIN_CURSOR,
    WIN_COUNT,
} WinId;

static const int g_win_zpos[WIN_COUNT] = { 1, 2, 3, 4 };

/* 一个窗口帧：DRM dumb 缓冲 + CPU 光栅视图 + GPU 纹理 + display_win 槽位。 */
typedef struct {
    int buffer;        /* drm_alloc_buffer 缓冲区句柄 */
    int width;
    int height;
    Drmbuff_t info;
    WinCanvas canvas;
    unsigned tex;      /* GPU 纹理；0 表示尚未导入 */
    int chn;           /* display_win 窗口索引 */
} WinFrame;

volatile sig_atomic_t g_quit;
log_handle_t g_log = LOG_HANDLE_INVALID;
static CaptureState g_capture[2];
static bool g_mirror;
static ComposeMode g_compose = COMPOSE_GPU;

/* Default: cam0=/dev/video44 (rkisp0-vir0), cam1=/dev/video53 (rkisp0-vir1). */
static int g_video_nums[2] = { 44, 53 };

static Display_dev_t g_disp;
static int g_conn_index = -1;
static Layout g_layout;
static int g_width;                 /* 连接器分辨率（画布尺寸） */
static int g_height;
static int g_video_w;               /* 拼接视频尺寸 = cam_w * 2 */
static int g_video_h;
static int g_video_plane = -1;
static int g_canvas_plane = -1;
static int g_video_buf = -1;
static Drmbuff_t g_video_info;
static int g_canvas_buf = -1;
static Drmbuff_t g_canvas_info;
static WinFrame g_win[WIN_COUNT];

/* canvas plane 位于 video plane 之上（画布透明洞露出 video）。 */
#define CANVAS_ZPOS 10

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
           "  -C: side-by-side compose backend (default gpu, no CPU fallback).\n"
           "      rga/gpu 互不回退。\n"
           "  UI layer: 2 real DRM planes (video + full-screen ARGB canvas);\n"
           "      shell/stats/button/cursor are display_win windows, GPU-composited.\n"
           "  video/canvas 图层按连接器枚举的格式能力自动选择，无需指定。\n",
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

/* ---------------- 缓冲区描述转换 ---------------- */

/* 将一块 DRM 缓冲区信息包装成可绘制的 ARGB 窗口帧缓冲。 */
static WinCanvas canvas_of(const Drmbuff_t *info, int width, int height)
{
    WinCanvas c;
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

static void v4l2_to_gpu(const V4l2Frame *in, GpuBuffer *out)
{
    memset(out, 0, sizeof(*out));
    out->fd = in->dma_fd;
    out->width = in->width;
    out->height = in->height;
    out->stride = in->stride;
    out->format = GPU_FMT_NV12;
}

/* 视频 dumb 缓冲（NV12）→ RGA / GPU 描述。 */
static void video_to_rga(RgaBuffer *out)
{
    memset(out, 0, sizeof(*out));
    out->width = g_video_w;
    out->height = g_video_h;
    out->wstride = nv12_wstride_px(g_video_info.fbStride, g_video_w);
    out->hstride = g_video_h;
    out->format = RGA_FMT_YCbCr_420_SP;
    if (g_video_info.dmabuf_fd >= 0) {
        out->mem_type = RGA_MEM_FD;
        out->fd = g_video_info.dmabuf_fd;
    } else {
        out->mem_type = RGA_MEM_VIR;
        out->vir = g_video_info.pFramebuffer;
    }
}

/* ARGB dumb 缓冲 → GPU 描述（画布与窗口帧贴图）。 */
static void info_to_gpu_argb(const Drmbuff_t *in, int width, int height, GpuBuffer *out)
{
    memset(out, 0, sizeof(*out));
    out->fd = in->dmabuf_fd;
    out->width = width;
    out->height = height;
    out->stride = in->fbStride;
    out->format = GPU_FMT_ARGB8888;
}

/**
 * 按 -C 选择的后端做左右拼接，写入视频图层缓冲区。
 * COMPOSE_RGA / COMPOSE_GPU 失败时直接返回负值，禁止回退。
 */
static int compose_side_by_side(const V4l2Frame *left, const V4l2Frame *right, int swap)
{
    RgaBuffer rga_l, rga_r, rga_d;
    GpuBuffer gpu_l, gpu_r, gpu_d;

    switch (g_compose) {
    case COMPOSE_RGA:
        v4l2_to_rga(left, &rga_l);
        v4l2_to_rga(right, &rga_r);
        video_to_rga(&rga_d);
        return rga_compose_side_by_side(&rga_l, &rga_r, &rga_d, swap);
    case COMPOSE_GPU:
        v4l2_to_gpu(left, &gpu_l);
        v4l2_to_gpu(right, &gpu_r);
        info_to_gpu_argb(&g_video_info, g_video_w, g_video_h, &gpu_d);
        return gpu_compose_side_by_side(&gpu_l, &gpu_r, &gpu_d, swap);
    default:
        return -1;
    }
}

/* 申请一个图层：初始化 plane 与缓冲区并读取缓冲区信息。 */
static int init_plane(int *plane, int *buffer, Drmbuff_t *info, uint32_t plane_id,
                      int width, int height, DisplayPixFmt_t format)
{
    *plane = drm_init_planes(g_conn_index, plane_id);
    if (*plane < 0) {
        LOG_E("drm_init_planes(id=%u) failed\n", plane_id);
        return -1;
    }
    *buffer = drm_alloc_buffer(width, height, format);
    if (*buffer < 0) {
        LOG_E("drm_alloc_buffer(%dx%d, fmt=%d) failed\n",
                width, height, format);
        return -1;
    }
    if (drm_get_buffer_info(*buffer, info) != 0) {
        LOG_E("drm_get_buffer_info failed\n");
        return -1;
    }
    return 0;
}

/* 在当前连接器枚举到的图层中，按像素格式能力挑选一个尚未被占用的图层，
 * 返回其 DRM 图层对象 ID；找不到返回 0。used 为已选中的图层 ID 列表。 */
static uint32_t pick_plane(DisplayPixFmt_t want_fmt, const uint32_t *used, int nused)
{
    const int conn = g_conn_index;
    int i, f, u;

    for (i = 0; i < g_disp.connectors[conn].planeCount; ++i) {
        const Display_plane_t *p = &g_disp.connectors[conn].planes[i];
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

/* 申请一个窗口帧缓冲（ARGB8888）并建立 CPU 光栅视图。 */
static int win_alloc(WinFrame *f, int width, int height)
{
    f->buffer = drm_alloc_buffer(width, height, DISPLAY_PIX_FMT_ARGB8888);
    if (f->buffer < 0) {
        LOG_E("window buffer alloc %dx%d failed\n", width, height);
        return -1;
    }
    if (drm_get_buffer_info(f->buffer, &f->info) != 0) {
        LOG_E("window buffer info failed\n");
        return -1;
    }
    f->width = width;
    f->height = height;
    f->canvas = canvas_of(&f->info, width, height);
    f->tex = 0;
    f->chn = -1;
    return 0;
}

/* 登记一个窗口并绑定其帧。 */
static int win_register(WinFrame *f, int zpos, int x, int y, int w, int h)
{
    window_t win;
    display_frame_t fr;

    win.zpos = zpos;
    win.win_x = x;
    win.win_y = y;
    win.win_w = w;
    win.win_h = h;
    f->chn = dp_win_add(&win);
    if (f->chn < 0) {
        LOG_E("dp_win_add(zpos=%d) failed\n", zpos);
        return -1;
    }

    memset(&fr, 0, sizeof(fr));
    fr.dmabuf_fd = f->info.dmabuf_fd;
    fr.vir_addr = f->info.pFramebuffer;
    fr.width = f->width;
    fr.height = f->height;
    fr.pitch_bytes = f->info.fbStride;
    fr.rotation = 0;
    fr.format = DISPLAY_PIX_FMT_ARGB8888;
    fr.src_x = 0;
    fr.src_y = 0;
    fr.src_w = f->width;
    fr.src_h = f->height;
    if (dp_win_set_frame(f->chn, &fr) != 0) {
        LOG_E("dp_win_set_frame(chn=%d) failed\n", f->chn);
        return -1;
    }
    return 0;
}

int ui_main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int cam_w = 1920, cam_h = 1080;
    MouseState *mouse = NULL;
    StatsContext stats_ctx;
    bool rga_inited = false;
    bool gpu_inited = false;
    int opt;

    for (int i = 0; i < WIN_COUNT; ++i) {
        g_win[i].buffer = -1;
        g_win[i].tex = 0;
        g_win[i].chn = -1;
    }

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
        log_mgr_t mgr = log_manager_init("/userdata/logs/vo_drm_2plane_4win.ini");
        if (mgr != LOG_MGR_INVALID)
            g_log = log_register(mgr, "vo_drm_2plane_4win");
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

    g_video_w = cam_w * 2;
    g_video_h = cam_h;

    /* 初始化 DRM 设备并挑选已启用的连接器。 */
    if (drm_init(&g_disp) != 0) {
        LOG_E("drm_init failed\n");
        goto fail_sys;
    }
    for (int i = 0; i < g_disp.connectorCount; ++i) {
        if (g_disp.connectors[i].enabled) {
            g_conn_index = g_disp.connectors[i].index;
            break;
        }
    }
    if (g_conn_index < 0) {
        LOG_E("no enabled DRM connector found\n");
        goto fail_drm;
    }
    g_width = g_disp.connectors[g_conn_index].screenWidth;
    g_height = g_disp.connectors[g_conn_index].screenHeight;
    ui_layout_init(&g_layout, g_width, g_height);
    LOG_I("display: %dx%d@%dHz (connector %d)\n",
           g_width, g_height,
           g_disp.connectors[g_conn_index].screenRefresh, g_conn_index);

    /*
     * 两个真实图层，按连接器枚举出的图层格式能力自动挑选，不再依赖命令行入参：
     *   video ：rga 用 NV12，gpu 用 GLES 输出的 ARGB8888；
     *   canvas：整屏 ARGB8888，由 GPU 贴 4 个窗口帧合成。
     */
    DisplayPixFmt_t video_fmt =
        (g_compose == COMPOSE_GPU) ? DISPLAY_PIX_FMT_ARGB8888 : DISPLAY_PIX_FMT_NV12;
    uint32_t used[2];
    uint32_t video_id, ui_id;

    for (int i = 0; i < g_disp.connectors[g_conn_index].planeCount; ++i) {
        const Display_plane_t *p = &g_disp.connectors[g_conn_index].planes[i];
        LOG_I("plane[%d]: id=%u type=%d formats=%d\n", i, p->id, p->type, p->formatCount);
    }
    used[0] = video_id = pick_plane(video_fmt, used, 0);
    used[1] = ui_id    = pick_plane(DISPLAY_PIX_FMT_ARGB8888, used, 1);
    if (!video_id || !ui_id) {
        LOG_E("no suitable DRM plane (video=%u canvas=%u)\n", video_id, ui_id);
        goto fail_drm;
    }
    LOG_I("picked planes: video=%u canvas=%u\n", video_id, ui_id);

    if (init_plane(&g_video_plane, &g_video_buf, &g_video_info, video_id, g_video_w, g_video_h,
                   video_fmt) != 0)
        goto fail_drm;
    if (init_plane(&g_canvas_plane, &g_canvas_buf, &g_canvas_info, ui_id, g_width, g_height,
                   DISPLAY_PIX_FMT_ARGB8888) != 0)
        goto fail_drm;

    /* 四个窗口帧缓冲：shell / stats / button / cursor。 */
    if (win_alloc(&g_win[WIN_SHELL], g_width, g_height) != 0)
        goto fail_drm;
    if (win_alloc(&g_win[WIN_STATS], STATS_W, STATS_H) != 0)
        goto fail_drm;
    if (win_alloc(&g_win[WIN_BUTTON], UI_BTN_W + 2 * UI_BTN_PAD, UI_BTN_H + 2 * UI_BTN_PAD) != 0)
        goto fail_drm;
    if (win_alloc(&g_win[WIN_CURSOR], CURSOR_W, CURSOR_H) != 0)
        goto fail_drm;

    /* 图层属性：canvas 叠在 video 之上、不透明、按像素 alpha 混合。 */
    drm_plane_set_zpos(g_video_plane, 0);
    drm_plane_set_zpos(g_canvas_plane, CANVAS_ZPOS);
    drm_plane_set_alpha(g_canvas_plane, 0xFFFF);
    drm_plane_set_blend_mode(g_canvas_plane, 1 /* DRM_MODE_BLEND_COVERAGE */);

    /* canvas 区域固定为全屏；video 源区域固定，目标区域每帧按 letterbox 更新。 */
    drm_plane_set_src_region(g_canvas_plane, 0, 0, g_width, g_height);
    drm_plane_set_crtc_region(g_canvas_plane, 0, 0, g_width, g_height);
    drm_plane_set_src_region(g_video_plane, 0, 0, g_video_w, g_video_h);

    /* 画布合成恒走 GPU，故无条件初始化。 */
    if (gpu_composer_init() != 0) {
        LOG_E("gpu_composer_init failed\n");
        goto fail_drm;
    }
    gpu_inited = true;

    if (g_compose == COMPOSE_RGA) {
        if (rga_init() != 0) {
            LOG_E("rga_init failed (compose backend=rga)\n");
            goto fail_drm;
        }
        rga_inited = true;
    } else if (g_video_info.dmabuf_fd < 0) {
        LOG_E("gpu compose backend requires video buffer dma-buf FD\n");
        goto fail_drm;
    }
    LOG_I("compose backend: %s\n", g_compose == COMPOSE_GPU ? "gpu" : "rga");

    /* 画布与窗口帧都要用 dma-buf 导入 GPU 纹理，不做 CPU 合成回退。 */
    if (g_canvas_info.dmabuf_fd < 0) {
        LOG_E("canvas requires a dma-buf FD for GPU composite\n");
        goto fail_drm;
    }

    /* CPU 光栅各窗口帧；stats 先留全透明，待首次刷新再画。 */
    draw_shell(&g_win[WIN_SHELL].canvas, &g_layout);
    draw_button(&g_win[WIN_BUTTON].canvas);
    draw_cursor(&g_win[WIN_CURSOR].canvas);
    memset(g_win[WIN_STATS].canvas.map, 0, g_win[WIN_STATS].canvas.size);
    gpu_ui_flush();

    /* 各窗口帧导入为常驻纹理；纹理与 dma-buf 内容实时联动。 */
    for (int i = 0; i < WIN_COUNT; ++i) {
        GpuBuffer b;
        if (g_win[i].info.dmabuf_fd < 0) {
            LOG_E("window %d requires a dma-buf FD for GPU texture\n", i);
            goto fail_drm;
        }
        info_to_gpu_argb(&g_win[i].info, g_win[i].width, g_win[i].height, &b);
        g_win[i].tex = gpu_ui_import_texture(&b);
        if (g_win[i].tex == 0) {
            LOG_E("gpu_ui_import_texture(window %d) failed\n", i);
            goto fail_drm;
        }
    }

    /* display_win：登记 4 个窗口（shell 底层 → cursor 顶层）。 */
    if (dp_win_init(g_width, g_height) != 0) {
        LOG_E("dp_win_init failed\n");
        goto fail_drm;
    }
    if (win_register(&g_win[WIN_SHELL], g_win_zpos[WIN_SHELL], 0, 0, g_width, g_height) != 0)
        goto fail_drm;
    if (win_register(&g_win[WIN_STATS], g_win_zpos[WIN_STATS],
                     g_layout.stats_x, g_layout.stats_y, STATS_W, STATS_H) != 0)
        goto fail_drm;
    if (win_register(&g_win[WIN_BUTTON], g_win_zpos[WIN_BUTTON],
                     g_layout.btn_x - UI_BTN_PAD, g_layout.btn_y - UI_BTN_PAD,
                     UI_BTN_W + 2 * UI_BTN_PAD, UI_BTN_H + 2 * UI_BTN_PAD) != 0)
        goto fail_drm;
    if (win_register(&g_win[WIN_CURSOR], g_win_zpos[WIN_CURSOR],
                     g_width / 2, g_height / 2, CURSOR_W, CURSOR_H) != 0)
        goto fail_drm;

    mouse = mouse_create();
    if (!mouse) {
        LOG_E("mouse_create failed\n");
        goto fail_drm;
    }
    mouse_set_pos(mouse, g_width / 2, g_height / 2);
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

    DemoStats stats = { .display_w = g_width, .display_h = g_height,
                        .camera_w = cam_w, .camera_h = cam_h };
    stats_init(&stats_ctx);
    unsigned long frames = 0;
    bool swap = false;
    struct timespec ts_t0, ts_t1, ts_t2, ts_t3;
    double t_pop0=0, t_pop1=0, t_commit=0, t_total=0;

    while (!g_quit) {
        V4l2Frame left;
        V4l2Frame right;
        int cx_click, cy_click;
        GpuBuffer canvas_gpu;

        clock_gettime(CLOCK_MONOTONIC, &ts_t0);

        if (mouse_consume_click(mouse, &cx_click, &cy_click)) {
            if (cx_click >= g_layout.btn_x && cx_click < g_layout.btn_x + g_layout.btn_w &&
                cy_click >= g_layout.btn_y && cy_click < g_layout.btn_y + g_layout.btn_h)
                swap = !swap;
        }
        if (frame_queue_pop(&g_capture[0].queue, &left) != 0)
            continue;
        clock_gettime(CLOCK_MONOTONIC, &ts_t1);
        bool have_right = g_mirror;
        if (g_mirror) {
            right = left;
        } else if (frame_queue_pop(&g_capture[1].queue, &right) != 0) {
            capture_release_frame(&g_capture[0], &left);
            continue;
        }
        clock_gettime(CLOCK_MONOTONIC, &ts_t2);

        /* rga/gpu: compose_side_by_side 失败不回退。 */
        if (compose_side_by_side(&left, &right, swap ? 1 : 0) == 0) {
            int vw, vh, vx, vy;
            int cx, cy;
            bool ok;
            int chns[DISPLAY_WIN_REG_MAX];
            int nwin, i;

            vw = g_layout.screen_w;
            vh = vw * g_video_h / g_video_w;
            if (vh > g_layout.screen_h) {
                vh = g_layout.screen_h;
                vw = vh * g_video_w / g_video_h;
            }
            vx = g_layout.screen_x + (g_layout.screen_w - vw) / 2;
            vy = g_layout.screen_y + (g_layout.screen_h - vh) / 2;
            drm_plane_set_crtc_region(g_video_plane, vx, vy, vw, vh);

            /* stats 约一秒刷新一次 */
            if (stats_update(&stats_ctx, &stats, frames)) {
                draw_stats(&g_win[WIN_STATS].canvas, &stats);
                gpu_ui_flush();
            }

            /* cursor 帧不重登，仅按鼠标位置更新窗口矩形。 */
            mouse_get_pos(mouse, &cx, &cy);
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (cx > g_width - CURSOR_W) cx = g_width - CURSOR_W;
            if (cy > g_height - CURSOR_H) cy = g_height - CURSOR_H;
            dp_win_set_rect(g_win[WIN_CURSOR].chn, cx, cy, CURSOR_W, CURSOR_H);

            /*
             * 全量重贴画布（含 shell）：清屏后按 zpos 升序把各窗口纹理贴回去，
             * cursor 每帧移动才不留残影；不依赖 dirty 增量。
             */
            info_to_gpu_argb(&g_canvas_info, g_width, g_height, &canvas_gpu);
            if (gpu_ui_begin(&canvas_gpu) == 0) {
                nwin = dp_win_list_by_zpos(chns);
                for (i = 0; i < nwin; ++i) {
                    dp_win_view_t view;
                    int id;
                    if (!dp_win_get_view(chns[i], &view))
                        continue;
                    if (!view.visible || !view.has_frame)
                        continue;
                    for (id = 0; id < WIN_COUNT; ++id)
                        if (g_win[id].chn == chns[i])
                            break;
                    if (id >= WIN_COUNT || g_win[id].tex == 0)
                        continue;
                    gpu_ui_blit(g_win[id].tex, (float)view.win.win_x, (float)view.win.win_y,
                                (float)view.win.win_w, (float)view.win.win_h);
                }
                gpu_ui_end();
                dp_win_clear_dirty();
            }

            /* 合并 video + canvas 到一次 atomic commit */
            {
                static int nonblock_ok = 0, nonblock_ebusy = 0, blocking_ok = 0, blocking_fail = 0;
                int planes[2] = { g_video_plane, g_canvas_plane };
                int bufs[2]   = { g_video_buf,   g_canvas_buf   };
                int commit_ret = drm_commit_multi(planes, bufs, 2, DRM_MODE_ATOMIC_NONBLOCK);
                clock_gettime(CLOCK_MONOTONIC, &ts_t3);
                if (commit_ret == 0) {
                    nonblock_ok++;
                } else if (errno == EBUSY) {
                    nonblock_ebusy++;
                    commit_ret = drm_commit_multi(planes, bufs, 2, 0);
                    if (commit_ret == 0) blocking_ok++;
                    else blocking_fail++;
                } else {
                    LOG_E("commit nonblock errno=%d\n", errno);
                }
                t_pop0 = (ts_t1.tv_sec-ts_t0.tv_sec)*1e3 + (ts_t1.tv_nsec-ts_t0.tv_nsec)/1e6;
                t_pop1 = (ts_t2.tv_sec-ts_t1.tv_sec)*1e3 + (ts_t2.tv_nsec-ts_t1.tv_nsec)/1e6;
                t_commit = (ts_t3.tv_sec-ts_t2.tv_sec)*1e3 + (ts_t3.tv_nsec-ts_t2.tv_nsec)/1e6;
                if (stats_update(&stats_ctx, &stats, frames)) {
                    double now = ts_t3.tv_sec + ts_t3.tv_nsec/1e9;
                    static double last_t = 0;
                    if (last_t > 0) t_total = (now - last_t) * 1000.0;
                    last_t = now;
                    LOG_I("fps=%.1f pop0=%.1fms pop1=%.1fms commit=%.1fms total=%.1fms frames=%lu\n",
                          stats.fps, t_pop0, t_pop1, t_commit, t_total, frames);
                }
                ok = (commit_ret == 0);
            }

            /* GPU 完成后立即释放 V4L2 buffer，不等 commit 完成 —— dma-buf 已导入 DRM plane */
            capture_release_frame(&g_capture[0], &left);
            if (!have_right)
                capture_release_frame(&g_capture[1], &right);
            if (ok)
                frames++;
        }
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
    if (mouse) {
        mouse_destroy(mouse);
        mouse = NULL;
    }
fail_drm:
    /* 先释放窗口与纹理，再销毁 GPU 会话，最后回收 DRM 资源。 */
    dp_win_uninit();
    for (int i = 0; i < WIN_COUNT; ++i) {
        if (g_win[i].tex)
            gpu_ui_release_texture(g_win[i].tex);
    }
    if (gpu_inited)
        gpu_composer_deinit();
    if (rga_inited)
        rga_deinit();
    for (int i = 0; i < WIN_COUNT; ++i) {
        if (g_win[i].buffer >= 0)
            drm_free_buffer(g_win[i].buffer);
    }
    if (g_canvas_buf >= 0)
        drm_free_buffer(g_canvas_buf);
    if (g_video_buf >= 0)
        drm_free_buffer(g_video_buf);
    if (g_canvas_plane >= 0)
        drm_uninit_planes(g_canvas_plane);
    if (g_video_plane >= 0)
        drm_uninit_planes(g_video_plane);
    drm_uninit(&g_disp);
fail_sys:
    frame_queue_destroy(&g_capture[0].queue);
    frame_queue_destroy(&g_capture[1].queue);
    return 0;
}

#endif /* ENABLE_UI */
