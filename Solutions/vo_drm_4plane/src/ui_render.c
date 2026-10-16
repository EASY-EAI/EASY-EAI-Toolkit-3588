#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_log.h"
#include "gpu_util.h"
#include "ui_render.h"

/* DRM_FORMAT_ARGB8888, 小端 uint32 = A<<24 | R<<16 | G<<8 | B */
#define ARGB(a, r, g, b)  (((uint32_t)(a) << 24) | ((r) << 16) | ((g) << 8) | (b))

/* ---- 显示器造型配色 (深色科技风, 与 vo_rockit_cursor 一致) ---- */
#define COLOR_BG            ARGB(0xFF, 0x0A, 0x0C, 0x12)
#define COLOR_BODY          ARGB(0xFF, 0x23, 0x27, 0x30)
#define COLOR_BODY_HI       ARGB(0xFF, 0x3A, 0x40, 0x4C)
#define COLOR_BODY_LO       ARGB(0xFF, 0x16, 0x19, 0x1F)
#define COLOR_STAND         ARGB(0xFF, 0x1A, 0x1D, 0x24)
#define COLOR_STAND_HI      ARGB(0xFF, 0x2C, 0x31, 0x3C)

/* ---- 文字 / 信息面板 ---- */
#define COLOR_LOGO          ARGB(0xFF, 0xE6, 0xEA, 0xF2)
#define COLOR_PANEL_BG      ARGB(0xC8, 0x0D, 0x11, 0x18)
#define COLOR_PANEL_FRAME   ARGB(0xFF, 0x3A, 0xC8, 0x53)
#define COLOR_TEXT          ARGB(0xFF, 0xD8, 0xDE, 0xE8)
#define COLOR_ACCENT        ARGB(0xFF, 0x3A, 0xC8, 0x53)

/* ---- 按键 ---- */
#define COLOR_BTN_IDLE      ARGB(0xFF, 0x1E, 0x6F, 0x43)
#define COLOR_BTN_FRAME     ARGB(0xFF, 0xD8, 0xDE, 0xE8)
#define COLOR_BTN_TEXT      ARGB(0xFF, 0x04, 0x10, 0x08)

/* ---- 光标 ---- */
#define COLOR_CURSOR_EDGE   ARGB(0xFF, 0x00, 0x00, 0x00)
#define COLOR_CURSOR_FILL   ARGB(0xFF, 0xFF, 0xFF, 0xFF)

#define UI_BTN_W     180
#define UI_BTN_H     44
#define STATS_R      12

#define FONT_CW      5
#define FONT_CH      7

/* 图集每格多留 1 像素间隔，NEAREST 采样放大时不会串到相邻字形。 */
#define GLYPH_CELL_W (FONT_CW + 1)
#define GLYPH_CELL_H (FONT_CH + 1)
#define GLYPH_COLS   8

typedef struct {
    char ch;
    uint8_t col[FONT_CW];
} Glyph_t;

/* 5x7 点阵字体 (列字节, bit0 在上)。收录数字/大写字母/常用符号 */
static const Glyph_t gFont[] = {
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
    { '/', { 0x20, 0x10, 0x08, 0x04, 0x02 } },
    { ':', { 0x00, 0x36, 0x36, 0x00, 0x00 } },
    { '%', { 0x23, 0x13, 0x08, 0x64, 0x62 } },
    { '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } },
    { '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } },
    { '2', { 0x62, 0x51, 0x49, 0x49, 0x46 } },
    { '3', { 0x22, 0x41, 0x49, 0x49, 0x36 } },
    { '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } },
    { '5', { 0x2F, 0x49, 0x49, 0x49, 0x31 } },
    { '6', { 0x3E, 0x49, 0x49, 0x49, 0x32 } },
    { '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
    { '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
    { '9', { 0x26, 0x49, 0x49, 0x49, 0x3E } },
    { 'A', { 0x7C, 0x12, 0x11, 0x12, 0x7C } },
    { 'B', { 0x7F, 0x49, 0x49, 0x49, 0x36 } },
    { 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } },
    { 'D', { 0x7F, 0x41, 0x41, 0x22, 0x1C } },
    { 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } },
    { 'F', { 0x7F, 0x09, 0x09, 0x01, 0x01 } },
    { 'G', { 0x3E, 0x41, 0x49, 0x49, 0x7A } },
    { 'H', { 0x7F, 0x08, 0x08, 0x08, 0x7F } },
    { 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } },
    { 'J', { 0x20, 0x40, 0x41, 0x3F, 0x01 } },
    { 'K', { 0x7F, 0x08, 0x14, 0x22, 0x41 } },
    { 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
    { 'M', { 0x7F, 0x02, 0x04, 0x02, 0x7F } },
    { 'N', { 0x7F, 0x04, 0x08, 0x10, 0x7F } },
    { 'O', { 0x3E, 0x41, 0x41, 0x41, 0x3E } },
    { 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } },
    { 'Q', { 0x3E, 0x41, 0x51, 0x21, 0x5E } },
    { 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } },
    { 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
    { 'T', { 0x01, 0x01, 0x7F, 0x01, 0x01 } },
    { 'U', { 0x3F, 0x40, 0x40, 0x40, 0x3F } },
    { 'V', { 0x1F, 0x20, 0x40, 0x20, 0x1F } },
    { 'W', { 0x3F, 0x40, 0x38, 0x40, 0x3F } },
    { 'X', { 0x63, 0x14, 0x08, 0x14, 0x63 } },
    { 'Y', { 0x07, 0x08, 0x70, 0x08, 0x07 } },
    { 'Z', { 0x61, 0x51, 0x49, 0x45, 0x43 } },
};

/* 自绘鼠标光标: X=黑边, O=白填充, .=透明, 热点在 (0,0) */
#define CURSOR_ART_W 12
#define CURSOR_ART_H 17
static const char *gCursorArt[CURSOR_ART_H] = {
    "X...........",
    "XX..........",
    "XOX.........",
    "XOOX........",
    "XOOOX.......",
    "XOOOOX......",
    "XOOOOOX.....",
    "XOOOOOOX....",
    "XOOOOOOOX...",
    "XOOOOOOOOX..",
    "XOOOOXXXXX..",
    "XOOXOOX.....",
    "XOX.XOOX....",
    "XX..XOOX....",
    "X....XOOX...",
    ".....XOOX...",
    "......XX....",
};

/* ---------------- 画布 → 后端缓冲 ---------------- */

static GpuBuffer canvas_to_gpu(const Canvas *c)
{
    GpuBuffer b;

    memset(&b, 0, sizeof(b));
    b.mem_type = GPU_MEM_FD;
    b.fd = c->fd;
    b.width = c->width;
    b.height = c->height;
    b.stride = c->pitch;
    b.format = GPU_FMT_ARGB8888;
    return b;
}

/*
 * 纯色实心矩形：两个三角形 + 内置白纹理，颜色由 GPU 调制。无抗锯齿需求，
 * 因此不用圆角矩形的 SDF 路径（后者会往外扩 1px 做 AA，实心填充不需要）。
 */
static int ui_fill(Canvas *c, int x, int y, int w, int h, uint32_t color)
{
    float x0 = (float)x;
    float y0 = (float)y;
    float x1 = (float)(x + w);
    float y1 = (float)(y + h);
    const GpuUiVertex verts[6] = {
        { x0, y0, 0.5f, 0.5f },
        { x1, y0, 0.5f, 0.5f },
        { x0, y1, 0.5f, 0.5f },
        { x1, y0, 0.5f, 0.5f },
        { x1, y1, 0.5f, 0.5f },
        { x0, y1, 0.5f, 0.5f },
    };

    if (c->fd < 0 || w <= 0 || h <= 0)
        return -1;
    gpu_ui_texture(GPU_UI_TEX_WHITE);
    gpu_ui_color(color);
    gpu_ui_draw(verts, 6);
    return 0;
}

/* ---------------- 字形图集（CPU 预栅格化一次，GPU 只做贴图） ---------------- */

static unsigned g_atlas_tex;
static int g_atlas_w;
static int g_atlas_h;

static int glyph_index(char c)
{
    size_t i;

    for (i = 0; i < sizeof(gFont) / sizeof(gFont[0]); i++)
        if (gFont[i].ch == c)
            return (int)i;
    return 0;   /* 未收录字符按空格处理 */
}

/* 首次使用时把 5x7 点阵铺进一张 RGBA 纹理，之后 GPU 直接放大贴图。 */
static int atlas_ensure(void)
{
    const int n = (int)(sizeof(gFont) / sizeof(gFont[0]));
    const int rows = (n + GLYPH_COLS - 1) / GLYPH_COLS;
    uint8_t *px;
    int i;

    if (g_atlas_tex)
        return 0;

    g_atlas_w = GLYPH_COLS * GLYPH_CELL_W;
    g_atlas_h = rows * GLYPH_CELL_H;
    px = calloc((size_t)g_atlas_w * g_atlas_h * 4, 1);
    if (!px) {
        LOG_E("ui: glyph atlas alloc failed\n");
        return -1;
    }
    for (i = 0; i < n; i++) {
        int gx = (i % GLYPH_COLS) * GLYPH_CELL_W;
        int gy = (i / GLYPH_COLS) * GLYPH_CELL_H;
        int col, row;

        for (col = 0; col < FONT_CW; col++)
            for (row = 0; row < FONT_CH; row++)
                if ((gFont[i].col[col] >> row) & 1) {
                    uint8_t *p = px + ((size_t)(gy + row) * g_atlas_w +
                                       (size_t)gx + col) * 4;
                    p[0] = p[1] = p[2] = p[3] = 0xFF;
                }
    }
    g_atlas_tex = gpu_ui_upload_texture(g_atlas_w, g_atlas_h, px);
    free(px);
    if (!g_atlas_tex) {
        LOG_E("ui: glyph atlas upload failed\n");
        return -1;
    }
    return 0;
}

static int text_width(const char *str, int scale)
{
    return (int)strlen(str) * (FONT_CW + 1) * scale - scale;
}

/* 逐字符提交纹理四边形，颜色由 GPU 调制，找到的字形按 scale 放大。 */
static void gpu_text(int x, int y, const char *str, int scale, uint32_t color)
{
    int cx = x;

    if (atlas_ensure() != 0)
        return;
    gpu_ui_texture(g_atlas_tex);
    gpu_ui_color(color);
    for (; *str; str++, cx += (FONT_CW + 1) * scale) {
        int gi = glyph_index(*str);
        int gx = (gi % GLYPH_COLS) * GLYPH_CELL_W;
        int gy = (gi / GLYPH_COLS) * GLYPH_CELL_H;
        float u0 = (float)gx / (float)g_atlas_w;
        float v0 = (float)gy / (float)g_atlas_h;
        float u1 = (float)(gx + FONT_CW) / (float)g_atlas_w;
        float v1 = (float)(gy + FONT_CH) / (float)g_atlas_h;
        float x0 = (float)cx;
        float y0 = (float)y;
        float x1 = (float)(cx + FONT_CW * scale);
        float y1 = (float)(y + FONT_CH * scale);
        const GpuUiVertex verts[6] = {
            { x0, y0, u0, v0 },
            { x1, y0, u1, v0 },
            { x0, y1, u0, v1 },
            { x1, y0, u1, v0 },
            { x1, y1, u1, v1 },
            { x0, y1, u0, v1 },
        };

        gpu_ui_draw(verts, 6);
    }
}

/* 梯形底座：上宽 tw、下宽 bw，用两个三角形替代原来的逐行 CPU 填充。 */
static void gpu_trapezoid(int cx, int y, int tw, int bw, int h, uint32_t color)
{
    float x0t = (float)cx - tw * 0.5f;
    float x1t = (float)cx + tw * 0.5f;
    float x0b = (float)cx - bw * 0.5f;
    float x1b = (float)cx + bw * 0.5f;
    float yt = (float)y;
    float yb = (float)(y + h);
    const GpuUiVertex verts[6] = {
        { x0t, yt, 0.5f, 0.5f },
        { x1t, yt, 0.5f, 0.5f },
        { x0b, yb, 0.5f, 0.5f },
        { x1t, yt, 0.5f, 0.5f },
        { x1b, yb, 0.5f, 0.5f },
        { x0b, yb, 0.5f, 0.5f },
    };

    gpu_ui_texture(GPU_UI_TEX_WHITE);
    gpu_ui_color(color);
    gpu_ui_draw(verts, 6);
}

/* ---------------- 对外绘制接口 ---------------- */

void draw_ui(Canvas *ui, DemoLayout *layout)
{
    int bodyX = layout->body_x, bodyY = layout->body_y;
    int bodyW = layout->body_w, bodyH = layout->body_h;
    int bodyBot = bodyY + bodyH;
    int screenX = layout->screen_x, screenY = layout->screen_y;
    int screenW = layout->screen_w, screenH = layout->screen_h;
    int r = bodyH / 12;
    int btnX, btnY;
    int logoX, logoY, bezelY, bezelH;
    int neckW, neckH, neckX, baseTop, baseBot, baseH;
    GpuBuffer g = canvas_to_gpu(ui);

    if (r < 16)
        r = 16;
    if (ui->fd < 0) {
        LOG_E("draw_ui: canvas has no dma-buf FD\n");
        return;
    }
    /*
     * 全部图元由 GPU 绘制：圆角外壳 / 高光条 / 按钮描边与文字走需要抗锯齿的
     * GPU 路径，纯色矩形走普通三角形，屏幕内区与 stats 区域则用 scissor+glClear
     * 擦成透明洞，露出下层 plane。
     */
    if (gpu_ui_begin(&g) != 0)
        return;

    /* 1. 桌面背景 (GPU 整屏实心矩形, 同时完成画布初始化) */
    ui_fill(ui, 0, 0, layout->width, layout->height, COLOR_BG);

    /* 2. 显示器外壳 (GPU 圆角矩形, SDF 抗锯齿) */
    gpu_ui_rounded_rect((float)bodyX, (float)bodyY, (float)bodyW, (float)bodyH,
                        (float)r, 0.0f, COLOR_BODY);

    /* 外壳顶部高光 + 底部暗影: 高光条带圆角端, 暗影是纯矩形 */
    gpu_ui_rounded_rect((float)bodyX, (float)bodyY, (float)bodyW,
                        (float)(r / 2), (float)r, 0.0f, COLOR_BODY_HI);
    gpu_ui_rounded_rect((float)(bodyX + r), (float)bodyY,
                        (float)(bodyW - 2 * r), (float)(r / 2), 0.0f, 0.0f,
                        COLOR_BODY_HI);
    ui_fill(ui, bodyX + r, bodyBot - r / 2, bodyW - 2 * r, r / 2, COLOR_BODY_LO);

    /* 3. 屏幕内区挖透明洞 (GPU scissor+clear), 露出下层视频 plane */
    gpu_ui_erase_rect((float)screenX, (float)screenY, (float)screenW,
                      (float)screenH);

    /* 4. 底座颈 + 颈顶高光 (GPU 纯色矩形) */
    neckW = bodyW / 9;
    neckH = bodyH / 12;
    neckX = bodyX + bodyW / 2 - neckW / 2;
    baseTop = neckW * 2;
    baseBot = bodyW * 32 / 100;
    baseH = bodyH / 10;
    if (neckH < 30) neckH = 30;
    if (baseH < 40) baseH = 40;
    ui_fill(ui, neckX, bodyBot, neckW, neckH, COLOR_STAND);
    ui_fill(ui, neckX, bodyBot, neckW, 4, COLOR_STAND_HI);

    /* 5. 梯形底座 + 内层高光 (GPU 三角形) */
    gpu_trapezoid(bodyX + bodyW / 2, bodyBot + neckH,
                  baseTop, baseBot, baseH, COLOR_STAND);
    gpu_trapezoid(bodyX + bodyW / 2, bodyBot + neckH,
                  baseTop - 8, baseBot - 16, baseH - 6, COLOR_STAND_HI);

    /* 6. 底部边框正中: "EASY EAI" logo (GPU 字形贴图) */
    bezelY = screenY + screenH;
    bezelH = bodyBot - bezelY;
    logoY = bezelY + (bezelH - FONT_CH * 2) / 2;
    logoX = bodyX + bodyW / 2 - text_width("EASY EAI", 2) / 2;
    gpu_text(logoX, logoY, "EASY EAI", 2, COLOR_LOGO);

    /* 7. SWAP 按键 (GPU 圆角填充 + 内描边 + 文字); 区域回填给命中测试 */
    btnX = screenX + screenW - UI_BTN_W - 24;
    btnY = screenY + screenH - UI_BTN_H - 24;
    layout->btn_x = btnX;
    layout->btn_y = btnY;
    layout->btn_w = UI_BTN_W;
    layout->btn_h = UI_BTN_H;
    gpu_ui_rounded_rect((float)btnX, (float)btnY, (float)UI_BTN_W,
                        (float)UI_BTN_H, (float)(UI_BTN_H / 4), 0.0f,
                        COLOR_BTN_IDLE);
    gpu_ui_rounded_rect((float)(btnX - 2), (float)(btnY - 2),
                        (float)(UI_BTN_W + 4), (float)(UI_BTN_H + 4),
                        (float)(UI_BTN_H / 4), 2.0f, COLOR_BTN_FRAME);
    gpu_text(btnX + (UI_BTN_W - text_width("SWAP L/R", 2)) / 2,
             btnY + (UI_BTN_H - FONT_CH * 2) / 2, "SWAP L/R", 2, COLOR_BTN_TEXT);

    /* 8. stats 区域挖透明洞 (GPU scissor+clear)：该处 ui 层保持透明, 由 stats plane 呈现 */
    gpu_ui_erase_rect((float)layout->stats_x, (float)layout->stats_y,
                      (float)STATS_W, (float)STATS_H);

    gpu_ui_end();
}

void draw_stats(Canvas *stats, const DemoStats *s)
{
    char text[64];
    int lineY;
    GpuBuffer g = canvas_to_gpu(stats);

    if (stats->fd < 0) {
        LOG_E("draw_stats: canvas has no dma-buf FD\n");
        return;
    }
    if (gpu_ui_begin(&g) != 0)
        return;

    /* 清屏用 GPU scissor+clear, 把上一帧面板与圆角外区域一并擦回全透明;
     * 面板底/描边/文字用 GPU 绘制。 */
    gpu_ui_erase_rect(0.0f, 0.0f, (float)STATS_W, (float)STATS_H);
    gpu_ui_rounded_rect(0.0f, 0.0f, (float)STATS_W, (float)STATS_H,
                        (float)STATS_R, 0.0f, COLOR_PANEL_BG);
    gpu_ui_rounded_rect(0.0f, 0.0f, (float)STATS_W, (float)STATS_H,
                        (float)STATS_R, 2.0f, COLOR_PANEL_FRAME);

    lineY = 10;
    snprintf(text, sizeof(text), "FPS: %.1f", s->fps);
    gpu_text(16, lineY, text, 2, COLOR_ACCENT);
    lineY += FONT_CH * 2 + 6;
    snprintf(text, sizeof(text), "RES: %dx%d", s->display_w, s->display_h);
    gpu_text(16, lineY, text, 2, COLOR_TEXT);
    lineY += FONT_CH * 2 + 6;
    snprintf(text, sizeof(text), "CPU: %.1f%%", s->cpu);
    gpu_text(16, lineY, text, 2, COLOR_TEXT);
    lineY += FONT_CH * 2 + 6;
    /* RGA / GPU 占用常驻显示，与 -C 选定的合成后端无关。 */
    if (s->rga >= 0.f)
        snprintf(text, sizeof(text), "RGA: %.0f%%", s->rga);
    else
        snprintf(text, sizeof(text), "RGA: N/A");
    gpu_text(16, lineY, text, 2, COLOR_TEXT);
    lineY += FONT_CH * 2 + 6;
    if (s->gpu >= 0.f)
        snprintf(text, sizeof(text), "GPU: %.0f%%", s->gpu);
    else
        snprintf(text, sizeof(text), "GPU: N/A");
    gpu_text(16, lineY, text, 2, COLOR_TEXT);
    lineY += FONT_CH * 2 + 6;
    snprintf(text, sizeof(text), "MEM: %ld/%ldM %.0f%%",
             s->mem_used_mb, s->mem_total_mb, s->mem_percent);
    gpu_text(16, lineY, text, 2, COLOR_TEXT);

    gpu_ui_end();
}

/* 光标只需一次静态光栅，直接 CPU 写像素，不占用 GPU。 */
void draw_cursor(Canvas *cursor)
{
    int row, col;
    Canvas *b = cursor;

    memset(b->map, 0, b->size);
    for (row = 0; row < CURSOR_ART_H; row++) {
        const char *line = gCursorArt[row];
        for (col = 0; col < CURSOR_ART_W; col++) {
            uint32_t color;

            if (line[col] == 'X')
                color = COLOR_CURSOR_EDGE;
            else if (line[col] == 'O')
                color = COLOR_CURSOR_FILL;
            else
                continue;
            if (col < b->width && row < b->height)
                ((uint32_t *)((uint8_t *)b->map + (size_t)row * b->pitch))[col] =
                    color;
        }
    }
}
