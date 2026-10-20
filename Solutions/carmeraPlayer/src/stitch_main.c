#define _GNU_SOURCE

#ifndef ENABLE_UI

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "app_log.h"
#include "capture.h"
#include "drm_display.h"
#include "gpu_util.h"
#include "rga_util.h"

/**
 * carmeraPlayer（无 UI 模式）
 *
 * -n 1（默认）：只取一路 camera，NV12 dma-buf 直接导入 DRM 图层显示，
 *              不经过 RGA / GPU。
 * -n 2         ：取两路 camera，各自缩到一半宽度后左右拼接为 cam_w x cam_h
 *              （如 1920x1080 + 1920x1080 → 960x1080 + 960x1080 → 1920x1080），
 *              由 -C 选择 RGA / GPU 合成，结果写入同一个 DRM 图层。
 */

typedef enum {
    COMPOSE_RGA = 0,
    COMPOSE_GPU,
} ComposeMode;

volatile sig_atomic_t g_quit;
log_handle_t g_log = LOG_HANDLE_INVALID;

static CaptureState g_capture[2];
static bool g_cap_started[2];
static int g_video_nums[2] = { 44, 53 };
static int g_cam_count = 1;
static int g_cam_w = 1920;
static int g_cam_h = 1080;
static ComposeMode g_compose = COMPOSE_GPU;

static Display_dev_t g_disp;
static bool g_drm_inited;
static int g_conn_index = -1;
static int g_screen_w;
static int g_screen_h;
static int g_video_plane = -1;
static int g_video_buf = -1;
static Drmbuff_t g_video_info;
static int g_video_w;                 /* 拼接输出宽；单摄模式不使用 */
static int g_video_h;
static bool g_gpu_inited;
static bool g_rga_inited;

/* 单摄：camera dma-buf 导入后的 DRM framebuffer，按 fd 缓存到退出时释放。 */
#define SINGLE_BUF_MAX 8
static int g_single_bufs[SINGLE_BUF_MAX];
static int g_single_fds[SINGLE_BUF_MAX];
static int g_single_buf_count;

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

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void print_usage(const char *name)
{
    printf("Usage: %s [-n 1|2] [-w cam_w] [-h cam_h] [-C rga|gpu]\n"
           "          [-L video_num] [-R video_num]\n"
           "  -n 1  single camera -> one DRM plane (default)\n"
           "  -n 2  dual cameras -> scale each to half width, side-by-side\n"
           "        into cam_w x cam_h on one DRM plane\n"
           "  -C    dual compose backend: rga | gpu (default gpu)\n"
           "  -L/-R /dev/videoN for camera 0/1 (default 44/53)\n"
           "  -w/-h per-camera capture size (default 1920/1080)\n",
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

/* 等比适配到目标区域并居中。 */
static void fit_into(int area_w, int area_h, int src_w, int src_h,
                     int *x, int *y, int *w, int *h)
{
    int64_t dw, dh;

    if (area_w <= 0 || area_h <= 0 || src_w <= 0 || src_h <= 0) {
        *x = *y = *w = *h = 0;
        return;
    }
    dw = area_w;
    dh = (int64_t)area_w * src_h / src_w;
    if (dh > area_h) {
        dh = area_h;
        dw = (int64_t)area_h * src_w / src_h;
    }
    *w = (int)dw;
    *h = (int)dh;
    *x = (area_w - *w) / 2;
    *y = (area_h - *h) / 2;
}

/* ---------------- 缓冲区描述转换 ---------------- */

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

static void video_to_gpu_argb(GpuBuffer *out)
{
    memset(out, 0, sizeof(*out));
    out->fd = g_video_info.dmabuf_fd;
    out->width = g_video_w;
    out->height = g_video_h;
    out->stride = g_video_info.fbStride;
    out->format = GPU_FMT_ARGB8888;
}

/* 按 -C 选定的后端把两路各缩半宽后拼接，失败不回退。 */
static int compose_side_by_side(const V4l2Frame *left, const V4l2Frame *right)
{
    RgaBuffer rga_l, rga_r, rga_d;
    GpuBuffer gpu_l, gpu_r, gpu_d;

    switch (g_compose) {
    case COMPOSE_RGA:
        v4l2_to_rga(left, &rga_l);
        v4l2_to_rga(right, &rga_r);
        video_to_rga(&rga_d);
        return rga_compose_side_by_side(&rga_l, &rga_r, &rga_d, 0);
    case COMPOSE_GPU:
        v4l2_to_gpu(left, &gpu_l);
        v4l2_to_gpu(right, &gpu_r);
        video_to_gpu_argb(&gpu_d);
        return gpu_compose_side_by_side(&gpu_l, &gpu_r, &gpu_d, 0);
    default:
        return -1;
    }
}

/* ---------------- DRM 初始化 ---------------- */

/* 在当前连接器枚举到的图层中挑一个支持 want_fmt 的图层，返回对象 ID。 */
static uint32_t pick_plane(DisplayPixFmt_t want_fmt)
{
    const int conn = g_conn_index;

    for (int i = 0; i < g_disp.connectors[conn].planeCount; ++i) {
        const Display_plane_t *p = &g_disp.connectors[conn].planes[i];
        for (int f = 0; f < p->formatCount; ++f)
            if (p->formats[f] == want_fmt)
                return p->id;
    }
    return 0;
}

/**
 * 初始化唯一的视频图层。
 * alloc_w/alloc_h 为 0 时只绑定图层（单摄模式逐帧导入 camera dma-buf）。
 */
static int setup_video_plane(DisplayPixFmt_t format, int alloc_w, int alloc_h)
{
    uint32_t plane_id = pick_plane(format);

    if (!plane_id) {
        LOG_E("no DRM plane supports format %d\n", format);
        return -1;
    }
    g_video_plane = drm_init_planes(g_conn_index, plane_id);
    if (g_video_plane < 0) {
        LOG_E("drm_init_planes(id=%u) failed\n", plane_id);
        return -1;
    }
    LOG_I("picked video plane id=%u format=%d\n", plane_id, format);
    if (alloc_w <= 0 || alloc_h <= 0)
        return 0;

    g_video_w = alloc_w;
    g_video_h = alloc_h;
    g_video_buf = drm_alloc_buffer(g_video_w, g_video_h, format);
    if (g_video_buf < 0) {
        LOG_E("drm_alloc_buffer(%dx%d, fmt=%d) failed\n", g_video_w, g_video_h, format);
        return -1;
    }
    if (drm_get_buffer_info(g_video_buf, &g_video_info) != 0) {
        LOG_E("drm_get_buffer_info failed\n");
        return -1;
    }
    drm_plane_set_src_region(g_video_plane, 0, 0, g_video_w, g_video_h);

    int x, y, w, h;
    fit_into(g_screen_w, g_screen_h, g_video_w, g_video_h, &x, &y, &w, &h);
    drm_plane_set_crtc_region(g_video_plane, x, y, w, h);
    LOG_I("display area: %dx%d+%d+%d on %dx%d screen\n", w, h, x, y, g_screen_w, g_screen_h);
    return 0;
}

/* ---------------- 运行循环 ---------------- */

static void log_stats(const char *mode, int64_t *frames, unsigned *errors, double *last)
{
    double now = now_sec();
    double elapsed = now - *last;

    if (elapsed < 1.0)
        return;
    LOG_I("%s: fps=%.1f errors=%u\n", mode, (double)*frames / elapsed, *errors);
    *frames = 0;
    *errors = 0;
    *last = now;
}

/* 单摄：按 camera dma-buf fd 缓存 DRM framebuffer，避免每帧导入/销毁。 */
static int import_single_frame(const V4l2Frame *f)
{
    DrmImportDesc_t desc;
    int buf;

    for (int i = 0; i < g_single_buf_count; ++i)
        if (g_single_fds[i] == f->dma_fd)
            return g_single_bufs[i];
    if (g_single_buf_count >= SINGLE_BUF_MAX) {
        LOG_E("single-camera import cache full\n");
        return -1;
    }

    memset(&desc, 0, sizeof(desc));
    desc.dmabuf_fd = f->dma_fd;
    desc.width = f->width;
    desc.height = f->height;
    desc.format = DISPLAY_PIX_FMT_NV12;
    desc.pitches[0] = (uint32_t)f->stride;
    desc.pitches[1] = (uint32_t)f->stride;
    desc.offsets[0] = 0;
    desc.offsets[1] = (uint32_t)(f->stride * f->height);

    buf = drm_import_dmabuf(&desc);
    if (buf < 0)
        return -1;
    g_single_fds[g_single_buf_count] = f->dma_fd;
    g_single_bufs[g_single_buf_count] = buf;
    g_single_buf_count++;
    return buf;
}

/* 单摄：camera NV12 dma-buf 直接导入 DRM 图层，零拷贝显示。 */
static void run_single(void)
{
    int64_t frames = 0;
    unsigned errors = 0;
    bool warned_no_fd = false;
    double last = now_sec();

    while (!g_quit) {
        V4l2Frame f;
        int ret = -1;

        if (frame_queue_pop(&g_capture[0].queue, &f) != 0)
            continue;

        if (f.dma_fd < 0) {
            if (!warned_no_fd) {
                LOG_E("camera %d has no dma-buf FD, cannot display\n",
                        g_capture[0].video_num);
                warned_no_fd = true;
            }
            errors++;
        } else {
            int buf = import_single_frame(&f);

            if (buf < 0) {
                errors++;
            } else {
                int x, y, w, h;

                drm_plane_set_src_region(g_video_plane, 0, 0, f.width, f.height);
                fit_into(g_screen_w, g_screen_h, f.width, f.height, &x, &y, &w, &h);
                drm_plane_set_crtc_region(g_video_plane, x, y, w, h);
                ret = drm_commit(g_video_plane, buf);
                if (ret != 0)
                    errors++;
            }
        }

        capture_release_frame(&g_capture[0], &f);
        if (ret == 0)
            frames++;
        log_stats("single", &frames, &errors, &last);
    }
}

/* 双摄：RGA / GPU 各缩半宽拼接到固定输出缓冲，再提交同一个 DRM 图层。 */
static void run_dual(void)
{
    int64_t frames = 0;
    unsigned errors = 0;
    bool compose_failed = false;
    double last = now_sec();

    while (!g_quit) {
        V4l2Frame left, right;
        int ret;

        if (frame_queue_pop(&g_capture[0].queue, &left) != 0)
            continue;
        if (frame_queue_pop(&g_capture[1].queue, &right) != 0) {
            capture_release_frame(&g_capture[0], &left);
            continue;
        }

        ret = compose_side_by_side(&left, &right);
        capture_release_frame(&g_capture[0], &left);
        capture_release_frame(&g_capture[1], &right);

        if (ret != 0) {
            if (!compose_failed) {
                LOG_E("side-by-side compose (%s) failed\n",
                        g_compose == COMPOSE_GPU ? "gpu" : "rga");
                compose_failed = true;
            }
            errors++;
        } else if (drm_commit(g_video_plane, g_video_buf) != 0) {
            errors++;
        } else {
            frames++;
        }
        log_stats("dual", &frames, &errors, &last);
    }
}

/* ---------------- 启动与清理 ---------------- */

static void stop_capture(void)
{
    /* 置 quit 让阻塞在入队/出队的采集线程立即退出。 */
    g_quit = 1;
    for (int i = 0; i < 2; ++i) {
        g_capture[i].running = false;
        pthread_cond_broadcast(&g_capture[i].queue.not_empty);
        pthread_cond_broadcast(&g_capture[i].queue.not_full);
    }
    for (int i = 0; i < 2; ++i) {
        if (g_cap_started[i]) {
            pthread_join(g_capture[i].thread, NULL);
            g_cap_started[i] = false;
        }
    }
    for (int i = 0; i < 2; ++i) {
        V4l2Frame f;
        while (frame_queue_pop(&g_capture[i].queue, &f) == 0)
            capture_release_frame(&g_capture[i], &f);
        capture_deinit(&g_capture[i]);
    }
}

int stitch_main(int argc, char **argv)
{
    int opt;

    setvbuf(stdout, NULL, _IONBF, 0);

    while ((opt = getopt(argc, argv, "n:w:h:C:L:R:?")) != -1) {
        switch (opt) {
        case 'n': g_cam_count = atoi(optarg); break;
        case 'w': g_cam_w = atoi(optarg); break;
        case 'h': g_cam_h = atoi(optarg); break;
        case 'C':
            if (parse_compose_mode(optarg, &g_compose) != 0) {
                fprintf(stderr, "invalid -C %s (want rga|gpu)\n", optarg);
                print_usage(argv[0]);
                return 1;
            }
            break;
        case 'L': g_video_nums[0] = atoi(optarg); break;
        case 'R': g_video_nums[1] = atoi(optarg); break;
        default: print_usage(argv[0]); return 0;
        }
    }
    if (g_cam_count != 1 && g_cam_count != 2) {
        fprintf(stderr, "invalid -n %d (want 1|2)\n", g_cam_count);
        print_usage(argv[0]);
        return 1;
    }
    if (g_cam_w <= 0 || g_cam_h <= 0 || (g_cam_w & 1) || (g_cam_h & 1)) {
        fprintf(stderr, "camera size must be positive and even, got %dx%d\n",
                g_cam_w, g_cam_h);
        return 1;
    }

    /* 队列必须先于信号处理初始化（信号处理器会广播条件变量）。 */
    for (int i = 0; i < 2; ++i) {
        memset(&g_capture[i], 0, sizeof(g_capture[i]));
        frame_queue_init(&g_capture[i].queue);
        g_capture[i].video_num = g_video_nums[i];
        g_capture[i].width = g_cam_w;
        g_capture[i].height = g_cam_h;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    {
        log_mgr_t mgr = log_manager_init("/userdata/logs/cameraStitchPlayer.ini");
        if (mgr != LOG_MGR_INVALID)
            g_log = log_register(mgr, "cameraStitchPlayer");
        if (g_log == LOG_HANDLE_INVALID)
            fprintf(stderr, "log_manager_pro init failed, logging disabled\n");
    }

    if (drm_init(&g_disp) != 0) {
        LOG_E("drm_init failed\n");
        goto cleanup;
    }
    g_drm_inited = true;
    for (int i = 0; i < g_disp.connectorCount; ++i) {
        if (g_disp.connectors[i].enabled) {
            g_conn_index = g_disp.connectors[i].index;
            break;
        }
    }
    if (g_conn_index < 0) {
        LOG_E("no enabled DRM connector found\n");
        goto cleanup;
    }
    g_screen_w = g_disp.connectors[g_conn_index].screenWidth;
    g_screen_h = g_disp.connectors[g_conn_index].screenHeight;
    LOG_I("display: %dx%d@%dHz (connector %d)\n",
           g_screen_w, g_screen_h,
           g_disp.connectors[g_conn_index].screenRefresh, g_conn_index);

    if (g_cam_count == 1) {
        if (setup_video_plane(DISPLAY_PIX_FMT_NV12, 0, 0) != 0)
            goto cleanup;
        LOG_I("mode: single camera, /dev/video%d\n", g_video_nums[0]);
    } else {
        DisplayPixFmt_t fmt = (g_compose == COMPOSE_GPU)
                                  ? DISPLAY_PIX_FMT_ARGB8888
                                  : DISPLAY_PIX_FMT_NV12;

        /* 每路缩半宽后拼接，输出仍是单路分辨率。 */
        if (setup_video_plane(fmt, g_cam_w, g_cam_h) != 0)
            goto cleanup;

        if (g_compose == COMPOSE_GPU) {
            if (g_video_info.dmabuf_fd < 0) {
                LOG_E("gpu compose requires video buffer dma-buf FD\n");
                goto cleanup;
            }
            if (gpu_composer_init() != 0) {
                LOG_E("gpu_composer_init failed\n");
                goto cleanup;
            }
            g_gpu_inited = true;
        } else {
            if (rga_init() != 0) {
                LOG_E("rga_init failed\n");
                goto cleanup;
            }
            g_rga_inited = true;
        }
        LOG_I("mode: dual camera /dev/video%d + /dev/video%d, %s compose, "
              "each %dx%d -> %dx%d\n",
              g_video_nums[0], g_video_nums[1],
              g_compose == COMPOSE_GPU ? "gpu" : "rga",
              g_cam_w, g_cam_h, g_cam_w / 2, g_cam_h);
    }

    if (capture_init(&g_capture[0]) != 0) {
        LOG_E("V4L2 init /dev/video%d failed\n", g_capture[0].video_num);
        goto cleanup;
    }
    if (g_cam_count == 2 && capture_init(&g_capture[1]) != 0) {
        LOG_E("V4L2 init /dev/video%d failed\n", g_capture[1].video_num);
        goto cleanup;
    }
    for (int i = 0; i < g_cam_count; ++i) {
        if (pthread_create(&g_capture[i].thread, NULL, capture_thread, &g_capture[i]) != 0) {
            LOG_E("pthread_create(camera %d) failed\n", i);
            goto cleanup;
        }
        g_cap_started[i] = true;
    }

    if (g_cam_count == 1)
        run_single();
    else
        run_dual();

cleanup:
    stop_capture();
    if (g_rga_inited)
        rga_deinit();
    if (g_gpu_inited)
        gpu_composer_deinit();
    for (int i = 0; i < g_single_buf_count; ++i)
        drm_free_buffer(g_single_bufs[i]);
    if (g_video_buf >= 0)
        drm_free_buffer(g_video_buf);
    if (g_video_plane >= 0)
        drm_uninit_planes(g_video_plane);
    if (g_drm_inited)
        drm_uninit(&g_disp);
    frame_queue_destroy(&g_capture[0].queue);
    frame_queue_destroy(&g_capture[1].queue);
    return 0;
}

#endif /* !ENABLE_UI */
