/**
 * disp_engine — 共享显示引擎
 *
 * 全进程唯一一次 disp_init：枚举连接器与图层，一次性选好视频层与 UI 层。
 *
 * display_pro 为进程内单实例且不保证线程安全：视频线程（camera 上屏）与
 * Qt 渲染线程（UI 上屏）都会提交图层，所有配置/提交路径经引擎内互斥锁
 * 串行化，调用方无需再加锁。
 *
 * 图层选择（与 cameraPlayer 单独运行时的 pick_plane 结果一致）：
 *   视频层 = 枚举中首个支持所需格式的图层
 *            （单摄/双摄 RGA → NV12；双摄 GPU → ARGB8888）
 *   UI 层  = 首个支持 ARGB8888 且 ≠ 视频层的图层（可用 --plane 强制指定）
 *   视频层 zpos=0，UI 层 zpos=10，同在 VOP2 一个 VP 下硬件像素级混合。
 *
 * 使用流程：
 *   disp_engine_init()      → disp_init + 选双图层 + UI 双缓冲 + 首帧全透明提交
 *   video_stitch / Qt 各自  → 经 disp_video_* / disp_ui_* 访问两个图层
 *   disp_engine_deinit()    → 释放两个图层、UI 缓冲与 DRM 设备
 */

#ifndef DISP_ENGINE_H
#define DISP_ENGINE_H

#include "display_pro.h"

/* DRM "pixel blend mode" 属性枚举值（对应内核 drm_mode.h） */
#define DISP_BLEND_PREMULTI   0   /* 预乘 Alpha：配 QImage::Format_ARGB32_Premultiplied */
#define DISP_BLEND_COVERAGE   1   /* 非预乘 Alpha：配 QImage::Format_ARGB32 */

/* 两图层的叠放顺序：视频在下、UI 在上 */
#define DISP_VIDEO_ZPOS       0
#define DISP_UI_ZPOS          10

/* UI 图层只读信息（Qt 侧选 QImage 格式、构建界面用） */
typedef struct {
    int plane_id;      /* DRM 图层对象 ID（诊断显示用） */
    int width;         /* 屏幕宽（= UI framebuffer 宽），像素 */
    int height;
    int blend_mode;    /* DISP_BLEND_*，Qt 侧据此选 QImage 格式 */
    int alpha8;       /* UI 整层不透明度 0~255 */
} DispUiInfo;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 初始化显示引擎：唯一的 disp_init，选视频层与 UI 层，配置 UI 图层
 * （全屏区域、zpos、整层透明度、混合模式、双缓冲、首帧全透明提交）。
 * video_fmt 为视频层所需像素格式（由 video_plane_format() 依运行模式推出）。
 * explicit_ui_plane_id >= 0 时强制指定 UI 图层，否则自动挑选。
 * alpha8 为 UI 整层不透明度（0~255，255 不透明）。
 * 返回 0 成功，< 0 失败。
 */
int disp_engine_init(DispPixFmt_t video_fmt, int explicit_ui_plane_id,
                     int alpha8);

/* 释放两个图层、UI 缓冲与 DRM 设备（须先 video_stop 释放视频缓冲） */
void disp_engine_deinit(void);

/* UI 图层只读信息（plane_id / 屏幕尺寸 / blend_mode / alpha8） */
const DispUiInfo *disp_ui_info(void);

/* 取 UI 后台缓冲的 mmap 映射与行跨度，调用方直接把下一帧画进去 */
int disp_ui_begin(void **map, int *stride);

/* 提交 UI 后台缓冲上屏（阻塞至 vsync）并翻转双缓冲 */
int disp_ui_commit(void);

/* 当前屏幕分辨率（视频层 fit_into 等比缩放计算用） */
void disp_screen_size(int *w, int *h);

/* ---------------- 视频图层（video_stitch 线程调用，内部加锁） ---------------- */

/* 按实际图像尺寸申请视频显示缓冲（单摄模式不用，双摄模式分配 2 块轮转） */
int disp_video_alloc_buffer(int width, int height, DispPixFmt_t format,
                            int *buf, DispBufferInfo_t *info);

/* 导入外部 DMA-BUF（单摄模式逐帧导入 camera 缓冲，按 fd 缓存） */
int disp_video_import_dmabuf(const DispImportDesc_t *desc);

/* 释放视频显示缓冲 */
void disp_video_free_buffer(int buf);

/* 一次性设置视频图层的源区域与目标显示区域（内部加锁） */
int disp_video_set_region(int src_x, int src_y, int src_w, int src_h,
                          int crtc_x, int crtc_y, int crtc_w, int crtc_h);

/* 提交视频缓冲上屏（阻塞至 vsync，内部加锁） */
int disp_video_commit(int buf);

#ifdef __cplusplus
}
#endif

#endif /* DISP_ENGINE_H */
