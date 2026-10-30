#define _GNU_SOURCE

/**
 * RGA 2D 加速实现（librga / im2d）。
 *
 * 格式枚举数值与 Rockchip librga 的 RK_FORMAT_* 对齐，便于直接对接。
 * 下列「输入/输出」能力以 RK3588 上的 RGA3 为准（SDK
 * Rockchip_Developer_Guide_RGA_CN.md）。同 SoC 另有 RGA2 core，多核调度时
 * 部分任务可能落到 RGA2；运行时请以 querystring / 实机校验为准。
 *
 * RGA3 输入格式：
 *   RGA_FMT_RGBA_8888, RGA_FMT_BGRA_8888, RGA_FMT_ARGB_8888, RGA_FMT_ABGR_8888
 *   RGA_FMT_RGBX_8888, RGA_FMT_BGRX_8888, RGA_FMT_XRGB_8888, RGA_FMT_XBGR_8888
 *   RGA_FMT_RGB_888,   RGA_FMT_BGR_888
 *   RGA_FMT_RGB_565,   RGA_FMT_BGR_565
 *   RGA_FMT_YCbCr_420_SP (NV12), RGA_FMT_YCrCb_420_SP (NV21)
 *   RGA_FMT_YCbCr_422_SP,        RGA_FMT_YCrCb_422_SP
 *   RGA_FMT_YUYV_422, RGA_FMT_YVYU_422, RGA_FMT_UYVY_422, RGA_FMT_VYUY_422
 *   RGA_FMT_YCbCr_420_SP_10B, RGA_FMT_YCrCb_420_SP_10B
 *   RGA_FMT_YCbCr_422_SP_10B, RGA_FMT_YCrCb_422_SP_10B
 *
 * RGA3 输出格式（注意：不含 ARGB/ABGR_8888）：
 *   RGA_FMT_RGBA_8888, RGA_FMT_BGRA_8888
 *   RGA_FMT_RGBX_8888, RGA_FMT_BGRX_8888
 *   RGA_FMT_RGB_888,   RGA_FMT_BGR_888
 *   RGA_FMT_RGB_565,   RGA_FMT_BGR_565
 *   RGA_FMT_YCbCr_420_SP, RGA_FMT_YCrCb_420_SP
 *   RGA_FMT_YCbCr_422_SP, RGA_FMT_YCrCb_422_SP
 *   RGA_FMT_YUYV_422, RGA_FMT_YVYU_422, RGA_FMT_UYVY_422, RGA_FMT_VYUY_422
 *   RGA_FMT_YCbCr_420_SP_10B, RGA_FMT_YCrCb_420_SP_10B
 *   RGA_FMT_YCbCr_422_SP_10B, RGA_FMT_YCrCb_422_SP_10B
 */

#include <stdio.h>
#include <string.h>

#include <rga/im2d.h>
#include <rga/rga.h>

#include "rga_util.h"

/* RGA 模块日志句柄：rga_init 注册。 */
static log_handle_t s_log = LOG_HANDLE_INVALID;

#define APP_LOG_HANDLE s_log
#include "app_log.h"

static int g_rga_ready;

static int buffer_ok(const RgaBuffer *b)
{
    int wstride;
    int hstride;

    if (!b || b->width <= 0 || b->height <= 0)
        return 0;
    if (b->format == RGA_FMT_UNKNOWN)
        return 0;
    wstride = b->wstride > 0 ? b->wstride : b->width;
    hstride = b->hstride > 0 ? b->hstride : b->height;
    if (wstride < b->width || hstride < b->height)
        return 0;
    if (b->mem_type == RGA_MEM_FD)
        return b->fd >= 0;
    if (b->mem_type == RGA_MEM_VIR)
        return b->vir != NULL;
    return 0;
}

/**
 * 将 RgaBuffer 转为 im2d rga_buffer_t。
 * FD → wrapbuffer_fd（dma-buf 零拷贝）；VIR → wrapbuffer_virtualaddr。
 * wstride/hstride 按像素计。
 */
static int to_rga_buffer(const RgaBuffer *in, rga_buffer_t *out)
{
    int wstride;
    int hstride;

    if (!buffer_ok(in) || !out)
        return -1;
    wstride = in->wstride > 0 ? in->wstride : in->width;
    hstride = in->hstride > 0 ? in->hstride : in->height;
    memset(out, 0, sizeof(*out));

    if (in->mem_type == RGA_MEM_FD) {
        *out = wrapbuffer_fd_t(in->fd, in->width, in->height,
                               wstride, hstride, in->format);
    } else {
        *out = wrapbuffer_virtualaddr_t(in->vir, in->width, in->height,
                                        wstride, hstride, in->format);
    }
    if (out->width <= 0 || out->height <= 0)
        return -1;
    return 0;
}

static int rga_blit_rect(const rga_buffer_t *src, const im_rect *srect,
                         rga_buffer_t *dst, const im_rect *drect)
{
    rga_buffer_t pat;
    im_rect prect;
    IM_STATUS st;

    memset(&pat, 0, sizeof(pat));
    memset(&prect, 0, sizeof(prect));
    st = improcess(*src, *dst, pat, *srect, *drect, prect, IM_SYNC);
    if (st != IM_STATUS_SUCCESS) {
        LOG_E("rga improcess failed: %s\n", imStrError(st));
        return -1;
    }
    return 0;
}

int rga_init(log_mgr_t log_mgr)
{
    s_log = app_log_register_module(log_mgr, "rga");
    g_rga_ready = 1;
    return 0;
}

void rga_deinit(void)
{
    g_rga_ready = 0;
}

int rga_compose_side_by_side(const RgaBuffer *left, const RgaBuffer *right,
                             RgaBuffer *dst, int swap)
{
    const RgaBuffer *frames[2];
    rga_buffer_t rsrc, rdst;
    im_rect srect, drect;
    int half;

    if (!g_rga_ready || !buffer_ok(left) || !buffer_ok(right) || !buffer_ok(dst))
        return -1;
    if (dst->width < 2 || dst->width % 2 != 0 || dst->height <= 0)
        return -1;

    frames[0] = swap ? right : left;
    frames[1] = swap ? left : right;
    half = dst->width / 2;
    if (to_rga_buffer(dst, &rdst) != 0)
        return -1;

    /* 左右各占输出一半：源整帧缩放到 (half × dst->height)。 */
    for (int i = 0; i < 2; ++i) {
        if (to_rga_buffer(frames[i], &rsrc) != 0)
            return -1;
        memset(&srect, 0, sizeof(srect));
        memset(&drect, 0, sizeof(drect));
        srect.width = frames[i]->width;
        srect.height = frames[i]->height;
        drect.x = i * half;
        drect.y = 0;
        drect.width = half;
        drect.height = dst->height;
        if (rga_blit_rect(&rsrc, &srect, &rdst, &drect) != 0)
            return -1;
    }
    return 0;
}

int rga_compose_single(const RgaBuffer *src, RgaBuffer *dst)
{
    rga_buffer_t rsrc, rdst;
    im_rect srect, drect;

    if (!g_rga_ready || !buffer_ok(src) || !buffer_ok(dst))
        return -1;
    if (to_rga_buffer(src, &rsrc) != 0 || to_rga_buffer(dst, &rdst) != 0)
        return -1;

    memset(&srect, 0, sizeof(srect));
    memset(&drect, 0, sizeof(drect));
    srect.width = src->width;
    srect.height = src->height;
    drect.width = dst->width;
    drect.height = dst->height;
    return rga_blit_rect(&rsrc, &srect, &rdst, &drect);
}
