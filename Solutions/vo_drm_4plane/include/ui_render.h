#ifndef UI_RENDER_H
#define UI_RENDER_H

#include <stddef.h>

#include "stats.h"

#define CURSOR_W 64
#define CURSOR_H 64
#define STATS_W  330
#define STATS_H  170

/**
 * 一块可写的 ARGB8888 画布，直接映射自 DRM dumb 缓冲。
 * 与具体显示后端解耦：只描述内存与几何，绘制原语均在此之上。
 * 绘制由 GPU 完成（圆角 / 多边形 / 文字 / 纯色矩形 / 透明洞），因此 fd 必须有效。
 */
typedef struct {
    void *map;   // 画布基地址（drm 缓冲 mmap 后的地址）；仅 CPU 光栅的光标使用
    int width;   // 像素宽度
    int height;  // 像素高度
    int pitch;   // 行跨距，单位：字节
    size_t size; // 总字节数，用于整体清零
    int fd;      // dma-buf fd；< 0 表示 GPU 不可用
} Canvas;

/**
 * 显示器造型布局：各区域供 UI 绘制与命中测试使用。
 * 由调用方按连接器分辨率计算后传入。
 */
typedef struct {
    int width, height;
    int body_x, body_y, body_w, body_h;
    int screen_x, screen_y, screen_w, screen_h;
    int stats_x, stats_y;
    int btn_x, btn_y, btn_w, btn_h;
} DemoLayout;

/**
 * 绘制显示器外壳、透明视频区域和 SWAP 按钮到 UI 画布，并回填按键区域到 layout。
 * 纯色矩形、透明洞与圆角/文字全部由 GPU 绘制；调用前 GPU 会话须已初始化
 * （gpu_composer_init）。
 */
void draw_ui(Canvas *ui, DemoLayout *layout);

/** 将当前统计值绘制到独立 status 画布（GPU 擦除 + 面板与文字）。 */
void draw_stats(Canvas *stats, const DemoStats *s);

/** 绘制 CURSOR_W x CURSOR_H 的 ARGB 鼠标光标画布（一次性 CPU 光栅）。 */
void draw_cursor(Canvas *cursor);

#endif
