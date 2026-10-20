/**
 * display_pixfmt — display 模块（drm_display / display_win）对外的像素格式定义。
 *
 * 调用方只使用本枚举描述格式，具体后端取值（DRM fourcc、RGA 格式、GPU 内部格式）
 * 由各模块内部自行映射，避免把 libdrm 头文件透传到应用侧。
 */

#ifndef DISPLAY_PIXFMT_H
#define DISPLAY_PIXFMT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_PIX_FMT_ARGB8888 = 0, // 32bpp，每像素 A/R/G/B 各 8 位
    DISPLAY_PIX_FMT_XRGB8888,     // 32bpp，每像素 X/R/G/B 各 8 位
    DISPLAY_PIX_FMT_NV12,         // 8 位 Y 平面 + UV 交织平面（4:2:0 半平面）
    DISPLAY_PIX_FMT_BUTT          // 枚举边界，非有效格式
} DisplayPixFmt_t;

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_PIXFMT_H */
