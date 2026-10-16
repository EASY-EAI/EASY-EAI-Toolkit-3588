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

#include "app_log.h"
#include "rga_util.h"

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
    switch (b->mem_type) {
    case RGA_MEM_FD:
        return b->fd >= 0;
    case RGA_MEM_VIR:
        return b->vir != NULL;
    case RGA_MEM_PHY:
        return b->phy != NULL;
    default:
        return 0;
    }
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

    switch (in->mem_type) {
    case RGA_MEM_FD:
        *out = wrapbuffer_fd_t(in->fd, in->width, in->height,
                               wstride, hstride, in->format);
        break;
    case RGA_MEM_VIR:
        *out = wrapbuffer_virtualaddr_t(in->vir, in->width, in->height,
                                        wstride, hstride, in->format);
        break;
    case RGA_MEM_PHY:
        *out = wrapbuffer_physicaladdr_t(in->phy, in->width, in->height,
                                         wstride, hstride, in->format);
        break;
    default:
        return -1;
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

int rga_init(void)
{
    g_rga_ready = 1;
    return 0;
}

void rga_deinit(void)
{
    g_rga_ready = 0;
}

int rga_cvtcolor(const RgaBuffer *src, RgaBuffer *dst)
{
    rga_buffer_t rsrc, rdst;
    im_rect srect, drect;

    if (!g_rga_ready || to_rga_buffer(src, &rsrc) != 0 || to_rga_buffer(dst, &rdst) != 0)
        return -1;
    memset(&srect, 0, sizeof(srect));
    memset(&drect, 0, sizeof(drect));
    srect.width = src->width;
    srect.height = src->height;
    drect.width = dst->width;
    drect.height = dst->height;
    return rga_blit_rect(&rsrc, &srect, &rdst, &drect);
}

int rga_compose_side_by_side(const RgaBuffer *left, const RgaBuffer *right,
                             RgaBuffer *dst, int swap)
{
    const RgaBuffer *frames[2];
    rga_buffer_t rsrc, rdst;
    im_rect srect, drect;
    int width;
    int height;

    if (!g_rga_ready || !buffer_ok(left) || !buffer_ok(right) || !buffer_ok(dst))
        return -1;

    frames[0] = swap ? right : left;
    frames[1] = swap ? left : right;
    width = frames[0]->width;
    height = frames[0]->height;
    if (width <= 0 || height <= 0)
        return -1;
    if (frames[1]->width != width || frames[1]->height != height)
        return -1;
    if (dst->width < width * 2 || dst->height < height)
        return -1;
    if (to_rga_buffer(dst, &rdst) != 0)
        return -1;

    for (int i = 0; i < 2; ++i) {
        if (to_rga_buffer(frames[i], &rsrc) != 0)
            return -1;
        memset(&srect, 0, sizeof(srect));
        memset(&drect, 0, sizeof(drect));
        srect.x = 0;
        srect.y = 0;
        srect.width = width;
        srect.height = height;
        drect.x = i * width;
        drect.y = 0;
        drect.width = width;
        drect.height = height;
        if (rga_blit_rect(&rsrc, &srect, &rdst, &drect) != 0)
            return -1;
    }
    return 0;
}

int rga_blit(const RgaBuffer *src, const RgaRect *src_rect,
             RgaBuffer *dst, const RgaRect *dst_rect)
{
    rga_buffer_t rsrc, rdst;
    im_rect srect, drect;

    if (!g_rga_ready || to_rga_buffer(src, &rsrc) != 0 || to_rga_buffer(dst, &rdst) != 0)
        return -1;

    memset(&srect, 0, sizeof(srect));
    memset(&drect, 0, sizeof(drect));
    if (src_rect) {
        if (src_rect->w <= 0 || src_rect->h <= 0 || src_rect->x < 0 || src_rect->y < 0)
            return -1;
        srect.x = src_rect->x;
        srect.y = src_rect->y;
        srect.width = src_rect->w;
        srect.height = src_rect->h;
    } else {
        srect.width = src->width;
        srect.height = src->height;
    }
    if (dst_rect) {
        if (dst_rect->w <= 0 || dst_rect->h <= 0 || dst_rect->x < 0 || dst_rect->y < 0)
            return -1;
        drect.x = dst_rect->x;
        drect.y = dst_rect->y;
        drect.width = dst_rect->w;
        drect.height = dst_rect->h;
    } else {
        drect.width = dst->width;
        drect.height = dst->height;
    }
    return rga_blit_rect(&rsrc, &srect, &rdst, &drect);
}
