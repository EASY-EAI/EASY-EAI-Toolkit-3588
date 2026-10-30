/**
 * video_stitch — cameraPlayer 显示通路的移植实现
 *
 * 与原 stitch_main.c 的差异：
 *   - disp_init/选层/区域与缓冲申请改走 disp_engine 共享引擎（全进程唯一
 *     disp_init，视频层与 UI 层互斥提交）；
 *   - display_watch 健康监测未移植（统计行仍保留 fps/errors）；
 *   - getopt/信号处理移至 Qt main.cpp（经 video_request_stop 通知本模块）；
 *   - 每秒统计额外发布快照，供 UI 底栏显示视频 fps。
 *
 * 线程模型：video_start/video_stop 仅由主线程调用；采集线程与视频线程由
 * 本模块创建/回收；视频线程里所有 DRM 访问经 disp_engine 内互斥锁。
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_log.h"
#include "camera_discover.h"
#include "capture.h"
#include "disp_engine.h"
#include "display_watch.h"
#include "display_pro.h"
#include "gpu_util.h"
#include "rga_util.h"
#include "video_stitch.h"

/* capture.c 等模块共用的进程退出标志 */
volatile sig_atomic_t g_quit;

/* 视频模块自身的日志句柄（gpu / rga / camera 各自持有句柄，见其 init） */
log_handle_t g_log = LOG_HANDLE_INVALID;
/* 日志管理器：video_log_init 保存，video_start 时传给各模块 init 注册句柄 */
static log_mgr_t g_log_mgr = LOG_MGR_INVALID;

typedef enum {
    COMPOSE_RGA = 0,
    COMPOSE_GPU,
} ComposeMode;

static CaptureState g_capture[2];
static bool g_cap_started[2];
static bool g_cap_inited[2];
/* 负值表示运行时自动发现：按 sensor 名排序后的 live ISP mainpath。 */
static int g_video_nums[2] = { -1, -1 };
static int g_cam_count = 1;
static int g_cam_w = 1920;
static int g_cam_h = 1080;
static ComposeMode g_compose = COMPOSE_GPU;

/* 双摄输出缓冲：双缓冲轮转，DRM 扫描一块时合成线程写另一块，避免撕裂。 */
#define VIDEO_BUF_COUNT 2
static int g_video_bufs[VIDEO_BUF_COUNT] = { -1, -1 };
static DispBufferInfo_t g_video_infos[VIDEO_BUF_COUNT];
static int g_video_buf_idx;
static int g_video_w;                 /* 拼接输出宽；单摄模式不使用 */
static int g_video_h;
static bool g_gpu_inited;
static bool g_rga_inited;

/* 单摄：camera dma-buf 导入后的 DRM framebuffer，按 fd 缓存到退出时释放。 */
#define SINGLE_BUF_MAX 8
static int g_single_bufs[SINGLE_BUF_MAX];
static int g_single_fds[SINGLE_BUF_MAX];
static int g_single_buf_count;

static pthread_t g_video_thread;
static bool g_video_started;

/* 统计快照：视频线程写、主线程读 */
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static VideoStats g_pub;

/* ---------------- 基础工具 ---------------- */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
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

/* 等比适配到当前屏幕并居中（屏幕尺寸来自共享引擎）。 */
static void fit_into_screen(int src_w, int src_h, int *x, int *y, int *w, int *h)
{
    int sw, sh;

    disp_screen_size(&sw, &sh);
    fit_into(sw, sh, src_w, src_h, x, y, w, h);
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

static void video_to_rga(int idx, RgaBuffer *out)
{
    const DispBufferInfo_t *info = &g_video_infos[idx];

    memset(out, 0, sizeof(*out));
    out->width = g_video_w;
    out->height = g_video_h;
    out->wstride = nv12_wstride_px(info->fbStride, g_video_w);
    out->hstride = g_video_h;
    out->format = RGA_FMT_YCbCr_420_SP;
    if (info->dmabuf_fd >= 0) {
        out->mem_type = RGA_MEM_FD;
        out->fd = info->dmabuf_fd;
    } else {
        out->mem_type = RGA_MEM_VIR;
        out->vir = info->pFramebuffer;
    }
}

static void video_to_gpu_argb(int idx, GpuBuffer *out)
{
    const DispBufferInfo_t *info = &g_video_infos[idx];

    memset(out, 0, sizeof(*out));
    out->fd = info->dmabuf_fd;
    out->width = g_video_w;
    out->height = g_video_h;
    out->stride = info->fbStride;
    out->format = GPU_FMT_ARGB8888;
}

/* 按选定后端把两路各缩半宽后拼接到第 idx 块输出缓冲，失败不回退。 */
static int compose_side_by_side(const V4l2Frame *left, const V4l2Frame *right,
                                int idx)
{
    RgaBuffer rga_l, rga_r, rga_d;
    GpuBuffer gpu_l, gpu_r, gpu_d;

    switch (g_compose) {
    case COMPOSE_RGA:
        v4l2_to_rga(left, &rga_l);
        v4l2_to_rga(right, &rga_r);
        video_to_rga(idx, &rga_d);
        return rga_compose_side_by_side(&rga_l, &rga_r, &rga_d, 0);
    case COMPOSE_GPU:
        v4l2_to_gpu(left, &gpu_l);
        v4l2_to_gpu(right, &gpu_r);
        video_to_gpu_argb(idx, &gpu_d);
        return gpu_compose_side_by_side(&gpu_l, &gpu_r, &gpu_d, 0);
    default:
        return -1;
    }
}

/* ---------------- 统计 ---------------- */

typedef struct {
    int64_t frames;           /* 本统计窗口内成功提交的帧数 */
    unsigned errors;          /* 本统计窗口内的失败次数 */
    unsigned long frames_total;
    unsigned long errors_total;
    double last;
} RunStats;

static void stats_ok(RunStats *s)
{
    s->frames++;
    s->frames_total++;
}

static void stats_err(RunStats *s)
{
    s->errors++;
    s->errors_total++;
}

/* 每秒打印窗口值与累计值，并发布快照给 UI 底栏。 */
static void log_stats(const char *mode, RunStats *s)
{
    double now = now_sec();
    double elapsed = now - s->last;

    if (elapsed < 1.0)
        return;
    LOG_I("%s: fps=%.1f errors=%u total_frames=%lu total_errors=%lu\n",
          mode, (double)s->frames / elapsed, s->errors,
          s->frames_total, s->errors_total);
    pthread_mutex_lock(&g_stats_lock);
    g_pub.fps = (double)s->frames / elapsed;
    g_pub.frames_total = s->frames_total;
    g_pub.errors_total = s->errors_total;
    pthread_mutex_unlock(&g_stats_lock);
    s->frames = 0;
    s->errors = 0;
    s->last = now;
}

/* ---------------- 运行循环 ---------------- */

/* 单摄：按 camera dma-buf fd 缓存 DRM framebuffer，避免每帧导入/销毁。 */
static int import_single_frame(const V4l2Frame *f)
{
    DispImportDesc_t desc;
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
    desc.format = DISP_PIX_FMT_NV12;
    desc.pitches[0] = (uint32_t)f->stride;
    desc.pitches[1] = (uint32_t)f->stride;
    desc.offsets[0] = 0;
    desc.offsets[1] = (uint32_t)(f->stride * f->height);

    buf = disp_video_import_dmabuf(&desc);
    if (buf < 0)
        return -1;
    g_single_fds[g_single_buf_count] = f->dma_fd;
    g_single_bufs[g_single_buf_count] = buf;
    g_single_buf_count++;
    return buf;
}

/* 取当前最新帧；积压的旧帧直接归还驱动，避免延迟随队列深度累积。 */
static int pop_latest_frame(CaptureState *c, V4l2Frame *frame)
{
    V4l2Frame newer;

    if (frame_queue_pop(&c->queue, frame) != 0)
        return -1;
    while (frame_queue_try_pop(&c->queue, &newer) == 0) {
        capture_release_frame(c, frame);
        *frame = newer;
    }
    return 0;
}

/* 单摄：camera NV12 dma-buf 直接导入 DRM 图层，零拷贝显示。 */
static void run_single(void)
{
    RunStats st;
    bool warned_no_fd = false;
    bool warned_commit = false;

    memset(&st, 0, sizeof(st));
    st.last = now_sec();

    while (!g_quit) {
        V4l2Frame f;
        int ret = -1;

        if (pop_latest_frame(&g_capture[0], &f) != 0)
            continue;

        if (f.dma_fd < 0) {
            if (!warned_no_fd) {
                LOG_E("camera %d has no dma-buf FD, cannot display\n",
                        g_capture[0].video_num);
                warned_no_fd = true;
            }
            stats_err(&st);
        } else {
            int buf = import_single_frame(&f);

            if (buf < 0) {
                stats_err(&st);
            } else {
                int x, y, w, h;

                fit_into_screen(f.width, f.height, &x, &y, &w, &h);
                disp_video_set_region(0, 0, f.width, f.height, x, y, w, h);
                ret = disp_video_commit(buf);
                if (ret != 0) {
                    /* 输出被关掉时提交失败会长期存在，只报第一条 */
                    if (!warned_commit) {
                        LOG_E("disp_commit failed: %s (同类错误后续只计数)\n",
                              drm_errstr(ret));
                        warned_commit = true;
                    }
                    stats_err(&st);
                } else {
                    warned_commit = false;
                }
            }
        }

        capture_release_frame(&g_capture[0], &f);
        if (ret == 0)
            stats_ok(&st);
        log_stats("single", &st);
    }
}

/* 双摄：RGA / GPU 各缩半宽拼接到当前输出缓冲，提交后切到另一块。 */
static void run_dual(void)
{
    RunStats st;
    bool compose_failed = false;
    bool commit_failed = false;

    memset(&st, 0, sizeof(st));
    st.last = now_sec();

    while (!g_quit) {
        V4l2Frame left, right;
        int idx = g_video_buf_idx;
        int ret;

        if (pop_latest_frame(&g_capture[0], &left) != 0)
            continue;
        if (pop_latest_frame(&g_capture[1], &right) != 0) {
            capture_release_frame(&g_capture[0], &left);
            continue;
        }

        ret = compose_side_by_side(&left, &right, idx);
        capture_release_frame(&g_capture[0], &left);
        capture_release_frame(&g_capture[1], &right);

        if (ret != 0) {
            if (!compose_failed) {
                LOG_E("side-by-side compose (%s) failed\n",
                        g_compose == COMPOSE_GPU ? "gpu" : "rga");
                compose_failed = true;
            }
            stats_err(&st);
        } else {
            ret = disp_video_commit(g_video_bufs[idx]);
            if (ret != 0) {
                if (!commit_failed) {
                    LOG_E("disp_commit failed: %s (同类错误后续只计数)\n",
                            drm_errstr(ret));
                    commit_failed = true;
                }
                stats_err(&st);
            } else {
                commit_failed = false;
                /* 提交成功才换缓冲：上一块已上屏，下一帧安全写另一块。 */
                g_video_buf_idx = idx ^ 1;
                stats_ok(&st);
            }
        }
        log_stats("dual", &st);
    }
}

static void *video_thread(void *unused)
{
    (void)unused;
    if (g_cam_count == 1)
        run_single();
    else
        run_dual();
    return NULL;
}

/* ---------------- 启动与清理 ---------------- */

/* 采集线程收尾：唤醒退出 → join → 排空队列归还驱动 → 关设备。 */
static void stop_capture_threads(void)
{
    for (int i = 0; i < 2; ++i) {
        g_capture[i].running = false;
        pthread_cond_broadcast(&g_capture[i].queue.not_empty);
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
        if (g_cap_inited[i]) {
            capture_deinit(&g_capture[i]);
            g_cap_inited[i] = false;
        }
    }
}

/* 释放合成后端与视频图层缓冲（视频线程已停止后调用）。 */
static void free_video_buffers(void)
{
    for (int i = 0; i < g_single_buf_count; ++i)
        disp_video_free_buffer(g_single_bufs[i]);
    g_single_buf_count = 0;
    for (int i = 0; i < VIDEO_BUF_COUNT; ++i) {
        if (g_video_bufs[i] >= 0)
            disp_video_free_buffer(g_video_bufs[i]);
        g_video_bufs[i] = -1;
    }
}

/* 双摄：申请两块输出缓冲并配置源/显示区域。 */
static int setup_video_buffers(DispPixFmt_t fmt)
{
    int sw, sh;
    int x, y, w, h;

    g_video_w = g_cam_w;
    g_video_h = g_cam_h;
    for (int i = 0; i < VIDEO_BUF_COUNT; ++i) {
        if (disp_video_alloc_buffer(g_video_w, g_video_h, fmt,
                                    &g_video_bufs[i], &g_video_infos[i]) != 0) {
            LOG_E("alloc video buffer(%dx%d fmt=%d) failed\n",
                  g_video_w, g_video_h, fmt);
            return -1;
        }
    }
    g_video_buf_idx = 0;

    disp_screen_size(&sw, &sh);
    fit_into(sw, sh, g_video_w, g_video_h, &x, &y, &w, &h);
    if (disp_video_set_region(0, 0, g_video_w, g_video_h, x, y, w, h) != 0) {
        LOG_E("set video plane region failed\n");
        return -1;
    }
    LOG_I("display area: %dx%d+%d+%d on %dx%d screen\n",
          w, h, x, y, sw, sh);
    return 0;
}

/* 按 media 拓扑选路：-L/-R 为显式覆盖，未指定的槽位由 live mainpath 补齐。 */
static int resolve_cameras(int need)
{
    CameraPath paths[CAMERA_PATH_MAX];
    int count = camera_paths_discover(paths, CAMERA_PATH_MAX);

    if (count < 0) {
        LOG_E("camera discovery failed\n");
        return -1;
    }
    for (int i = 0; i < count; ++i)
        LOG_I("live camera[%d]: /dev/video%d sensor=%s cif=%s (media%d)\n",
              i, paths[i].video_num, paths[i].sensor, paths[i].cif,
              paths[i].media_num);

    if (camera_paths_assign(paths, count, g_video_nums, need) != 0) {
        LOG_E("found %d live ISP mainpath(s), need %d; check links or use -L/-R\n",
              count, need);
        return -1;
    }
    for (int i = 0; i < need; ++i) {
        bool live = false;

        for (int j = 0; j < count; ++j)
            if (paths[j].video_num == g_video_nums[i])
                live = true;
        LOG_I("camera%d: /dev/video%d%s\n", i, g_video_nums[i],
              live ? "" : " (explicit override, not live)");
    }
    return 0;
}

/* ---------------- 对外接口 ---------------- */

int video_log_init(const char *config_file, const char *module)
{
    g_log_mgr = log_manager_init(config_file);

    if (g_log_mgr == LOG_MGR_INVALID)
        return -1;
    g_log = log_register(g_log_mgr, module);
    return (g_log == LOG_HANDLE_INVALID) ? -1 : 0;
}

void video_request_stop(void)
{
    g_quit = 1;
    if (!g_video_started)
        return; /* 队列可能尚未初始化（信号先于 video_start 到达） */
    for (int i = 0; i < 2; ++i) {
        g_capture[i].running = false;
        pthread_cond_broadcast(&g_capture[i].queue.not_empty);
    }
}

int video_start(const VideoConfig *cfg)
{
    DispPixFmt_t fmt;

    if (g_video_started) {
        LOG_E("video already started\n");
        return -1;
    }
    if (cfg->cam_count != 1 && cfg->cam_count != 2) {
        fprintf(stderr, "invalid cam_count %d (want 1|2)\n", cfg->cam_count);
        return -1;
    }
    if (cfg->cam_w <= 0 || cfg->cam_h <= 0 ||
        (cfg->cam_w & 1) || (cfg->cam_h & 1)) {
        fprintf(stderr, "camera size must be positive and even, got %dx%d\n",
                cfg->cam_w, cfg->cam_h);
        return -1;
    }
    g_cam_count = cfg->cam_count;
    g_cam_w = cfg->cam_w;
    g_cam_h = cfg->cam_h;
    g_compose = cfg->compose_gpu ? COMPOSE_GPU : COMPOSE_RGA;
    g_video_nums[0] = cfg->video_nums[0];
    g_video_nums[1] = cfg->video_nums[1];

    if (resolve_cameras(g_cam_count) != 0)
        return -1;
    if (g_quit)
        return -1; /* 信号先于启动到达 */

    /* 队列必须先于 video_request_stop 可达路径初始化（会广播条件变量）。 */
    for (int i = 0; i < 2; ++i) {
        memset(&g_capture[i], 0, sizeof(g_capture[i]));
        frame_queue_init(&g_capture[i].queue);
        g_capture[i].video_num = g_video_nums[i];
        g_capture[i].width = g_cam_w;
        g_capture[i].height = g_cam_h;
    }

    if (g_cam_count == 1) {
        LOG_I("mode: single camera, /dev/video%d\n", g_video_nums[0]);
    } else {
        fmt = video_plane_format(cfg);
        if (setup_video_buffers(fmt) != 0)
            goto fail;

        if (g_compose == COMPOSE_GPU) {
            for (int i = 0; i < VIDEO_BUF_COUNT; ++i) {
                if (g_video_infos[i].dmabuf_fd < 0) {
                    LOG_E("gpu compose requires video buffer dma-buf FD\n");
                    goto fail;
                }
            }
            if (gpu_composer_init(g_log_mgr) != 0) {
                LOG_E("gpu_composer_init failed\n");
                goto fail;
            }
            g_gpu_inited = true;
        } else {
            if (rga_init(g_log_mgr) != 0) {
                LOG_E("rga_init failed\n");
                goto fail;
            }
            g_rga_inited = true;
        }
        LOG_I("mode: dual camera /dev/video%d + /dev/video%d, %s compose, "
              "each %dx%d -> %dx%d\n",
              g_video_nums[0], g_video_nums[1],
              g_compose == COMPOSE_GPU ? "gpu" : "rga",
              g_cam_w, g_cam_h, g_cam_w / 2, g_cam_h);
    }

    if (capture_init(&g_capture[0], g_log_mgr) != 0) {
        LOG_E("V4L2 init /dev/video%d failed\n", g_capture[0].video_num);
        goto fail;
    }
    g_cap_inited[0] = true;
    if (g_cam_count == 2 && capture_init(&g_capture[1], g_log_mgr) != 0) {
        LOG_E("V4L2 init /dev/video%d failed\n", g_capture[1].video_num);
        goto fail;
    }
    g_cap_inited[1] = (g_cam_count == 2);
    for (int i = 0; i < g_cam_count; ++i) {
        if (pthread_create(&g_capture[i].thread, NULL,
                           capture_thread, &g_capture[i]) != 0) {
            LOG_E("pthread_create(camera %d) failed\n", i);
            goto fail;
        }
        g_cap_started[i] = true;
    }

    pthread_mutex_lock(&g_stats_lock);
    g_pub.fps = -1;
    g_pub.frames_total = 0;
    g_pub.errors_total = 0;
    pthread_mutex_unlock(&g_stats_lock);

    if (pthread_create(&g_video_thread, NULL, video_thread, NULL) != 0) {
        LOG_E("pthread_create(video thread) failed\n");
        goto fail;
    }
    g_video_started = true;
    return 0;

fail:
    stop_capture_threads();
    if (g_rga_inited) {
        rga_deinit();
        g_rga_inited = false;
    }
    if (g_gpu_inited) {
        gpu_composer_deinit();
        g_gpu_inited = false;
    }
    free_video_buffers();
    frame_queue_destroy(&g_capture[0].queue);
    frame_queue_destroy(&g_capture[1].queue);
    return -1;
}

void video_stop(void)
{
    if (!g_video_started)
        return;
    video_request_stop();
    pthread_join(g_video_thread, NULL);
    g_video_started = false;
    stop_capture_threads();
    if (g_rga_inited) {
        rga_deinit();
        g_rga_inited = false;
    }
    if (g_gpu_inited) {
        gpu_composer_deinit();
        g_gpu_inited = false;
    }
    free_video_buffers();
    frame_queue_destroy(&g_capture[0].queue);
    frame_queue_destroy(&g_capture[1].queue);
}

void video_get_stats(VideoStats *out)
{
    if (!out)
        return;
    pthread_mutex_lock(&g_stats_lock);
    *out = g_pub;
    pthread_mutex_unlock(&g_stats_lock);
}

DispPixFmt_t video_plane_format(const VideoConfig *cfg)
{
    if (cfg->cam_count == 2 && cfg->compose_gpu)
        return DISP_PIX_FMT_ARGB8888;
    return DISP_PIX_FMT_NV12;
}
