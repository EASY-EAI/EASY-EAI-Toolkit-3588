#ifndef UI_RENDER_H
#define UI_RENDER_H

#include <stddef.h>

#include "stats.h"

#define CURSOR_W 64
#define CURSOR_H 64
#define STATS_W  330
#define STATS_H  170

/* SWAP 按钮本体尺寸；窗口帧四周各留 2px，容纳描边。 */
#define UI_BTN_W   180
#define UI_BTN_H   44
#define UI_BTN_PAD 2

/**
 * 一块可写的 ARGB8888 窗口帧缓冲，直接映射自 DRM dumb 缓冲。
 * 与具体显示后端解耦：只描述内存与几何，绘制原语均在此之上。
 * 窗口帧经 dma-buf 导入 GPU 纹理后贴到画布，因此 fd 必须有效。
 */
typedef struct {
    void *map;   // 帧缓冲基地址
    int width;   // 像素宽度
    int height;  // 像素高度
    int pitch;   // 行跨距，单位：字节
    size_t size; // 总字节数，用于整体清零
    int fd;      // dma-buf fd；< 0 表示 GPU 无法导入
} WinCanvas;

/**
 * 显示器造型布局：各区域供窗口绘制、窗口登记与命中测试使用。
 * 由调用方按连接器分辨率计算后传入。
 */
typedef struct {
    int width, height;
    int body_x, body_y, body_w, body_h;
    int screen_x, screen_y, screen_w, screen_h;
    int stats_x, stats_y;
    int btn_x, btn_y, btn_w, btn_h;
} Layout;

/** 按连接器分辨率算出显示器造型布局（含 stats 面板与 SWAP 按钮位置）。 */
void ui_layout_init(Layout *l, int width, int height);

/** 绘制整屏显示器外壳窗口帧：背景 / 外壳 / 屏幕透明洞 / 底座 / logo。 */
void draw_shell(WinCanvas *c, const Layout *l);

/** 绘制 (UI_BTN_W+2*UI_BTN_PAD) x (UI_BTN_H+2*UI_BTN_PAD) 的 SWAP 按钮窗口帧。 */
void draw_button(WinCanvas *c);

/** 绘制 STATS_W x STATS_H 的状态面板窗口帧。 */
void draw_stats(WinCanvas *c, const DemoStats *s);

/** 绘制 CURSOR_W x CURSOR_H 的鼠标光标窗口帧。 */
void draw_cursor(WinCanvas *c);

#endif /* UI_RENDER_H */
