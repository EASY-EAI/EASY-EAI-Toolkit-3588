#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui_render.h"

/* DRM_FORMAT_ARGB8888, 小端 uint32 = A<<24 | R<<16 | G<<8 | B */
#define ARGB(a, r, g, b)  (((uint32_t)(a) << 24) | ((r) << 16) | ((g) << 8) | (b))

#define COLOR_TRANSPARENT   0x00000000

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

#define STATS_R      12

#define FONT_CW      5
#define FONT_CH      7

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

/* ---------------- 绘制原语 (操作 WinCanvas, 自带边界裁剪) ---------------- */

static void px(WinCanvas *b, int x, int y, uint32_t color)
{
    if (x >= 0 && x < b->width && y >= 0 && y < b->height)
        ((uint32_t *)((uint8_t *)b->map + (size_t)y * b->pitch))[x] = color;
}

static void fill_rect(WinCanvas *b, int x0, int y0, int w, int h, uint32_t color)
{
    int x, y, xe, ye;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    xe = (x0 + w > b->width) ? b->width : x0 + w;
    ye = (y0 + h > b->height) ? b->height : y0 + h;
    if (w <= 0 || h <= 0)
        return;
    for (y = y0; y < ye; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)b->map + (size_t)y * b->pitch);
        for (x = x0; x < xe; x++)
            row[x] = color;
    }
}

static void fill_rounded_rect(WinCanvas *b, int x, int y, int w, int h, int r, uint32_t color)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    int rr = r * r;
    int px_, py_;
    if (r < 0) r = 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > b->width) x1 = b->width;
    if (y1 > b->height) y1 = b->height;
    if (x1 <= x0 || y1 <= y0)
        return;
    for (py_ = y0; py_ < y1; py_++) {
        uint32_t *row = (uint32_t *)((uint8_t *)b->map + (size_t)py_ * b->pitch);
        for (px_ = x0; px_ < x1; px_++) {
            int corner = 0, ccx = 0, ccy = 0, dx, dy;
            if (px_ < x + r && py_ < y + r) corner = 1;
            else if (px_ >= x + w - r && py_ < y + r) corner = 2;
            else if (px_ < x + r && py_ >= y + h - r) corner = 3;
            else if (px_ >= x + w - r && py_ >= y + h - r) corner = 4;
            if (corner) {
                switch (corner) {
                case 1: ccx = x + r; ccy = y + r; break;
                case 2: ccx = x + w - 1 - r; ccy = y + r; break;
                case 3: ccx = x + r; ccy = y + h - 1 - r; break;
                default: ccx = x + w - 1 - r; ccy = y + h - 1 - r; break;
                }
                dx = px_ - ccx;
                dy = py_ - ccy;
                if (dx * dx + dy * dy > rr)
                    continue;
            }
            row[px_] = color;
        }
    }
}

static void rect_outline_rounded(WinCanvas *b, int x, int y, int w, int h, int r, int t, uint32_t color)
{
    int outerR2 = r * r;
    int innerR = r - t;
    int innerR2 = innerR * innerR;
    int ccx[4] = { x + r, x + w - 1 - r, x + r, x + w - 1 - r };
    int ccy[4] = { y + r, y + r, y + h - 1 - r, y + h - 1 - r };
    int k;
    if (t <= 0 || r <= 0)
        return;
    if (t > r) t = r;
    fill_rect(b, x + r, y, w - 2 * r, t, color);
    fill_rect(b, x + r, y + h - t, w - 2 * r, t, color);
    fill_rect(b, x, y + r, t, h - 2 * r, color);
    fill_rect(b, x + w - t, y + r, t, h - 2 * r, color);
    for (k = 0; k < 4; k++) {
        int bx = (k % 2 == 0) ? x : x + w - r;
        int by = (k < 2) ? y : y + h - r;
        int px_, py_;
        for (py_ = by; py_ < by + r; py_++) {
            uint32_t *row;
            if (py_ < 0 || py_ >= b->height)
                continue;
            row = (uint32_t *)((uint8_t *)b->map + (size_t)py_ * b->pitch);
            for (px_ = bx; px_ < bx + r; px_++) {
                int dx = px_ - ccx[k];
                int dy = py_ - ccy[k];
                int d2 = dx * dx + dy * dy;
                if (px_ < 0 || px_ >= b->width)
                    continue;
                if (d2 <= outerR2 && d2 >= innerR2)
                    row[px_] = color;
            }
        }
    }
}

static void fill_trapezoid(WinCanvas *b, int cx, int y, int tw, int bw, int h, uint32_t color)
{
    int dy;
    for (dy = 0; dy < h; dy++) {
        int ww = tw + (bw - tw) * dy / h;
        int xx = cx - ww / 2;
        fill_rect(b, xx, y + dy, ww, 1, color);
    }
}

/* ---------------- 字体 ---------------- */

static const uint8_t *font_glyph(char c)
{
    size_t i;
    for (i = 0; i < sizeof(gFont) / sizeof(gFont[0]); i++)
        if (gFont[i].ch == c)
            return gFont[i].col;
    return gFont[0].col;   /* 未收录字符按空格处理 */
}

static void draw_char(WinCanvas *b, int x, int y, char c, int scale, uint32_t color)
{
    const uint8_t *g = font_glyph(c);
    int col, row;
    for (col = 0; col < FONT_CW; col++)
        for (row = 0; row < FONT_CH; row++)
            if ((g[col] >> row) & 1)
                fill_rect(b, x + col * scale, y + row * scale, scale, scale, color);
}

static void draw_text(WinCanvas *b, int x, int y, const char *str, int scale, uint32_t color)
{
    int cx = x;
    for (; *str; str++, cx += (FONT_CW + 1) * scale)
        draw_char(b, cx, y, *str, scale, color);
}

static int text_width(const char *str, int scale)
{
    return strlen(str) * (FONT_CW + 1) * scale - scale;
}

/* ---------------- 布局 ---------------- */

void ui_layout_init(Layout *l, int width, int height)
{
    int margin_x = width * 3 / 100;
    int bezel;

    memset(l, 0, sizeof(*l));
    l->width = width;
    l->height = height;
    /* 显示器造型布局 (与 vo_rockit_cursor 的 layout_chn_rects 一致) */
    l->body_x = margin_x;
    l->body_w = width - 2 * margin_x;
    l->body_y = height * 2 / 100;
    l->body_h = height * 66 / 100;
    bezel = l->body_w * 22 / 1000;
    if (bezel < 24)
        bezel = 24;
    l->screen_x = l->body_x + bezel;
    l->screen_y = l->body_y + bezel + bezel / 2;
    l->screen_w = l->body_w - 2 * bezel;
    l->screen_h = l->body_h - 2 * bezel - bezel / 2;

    l->stats_x = width - STATS_W - 18;
    l->stats_y = height - STATS_H - 18;
    if (l->stats_x < 0)
        l->stats_x = 0;
    if (l->stats_y < 0)
        l->stats_y = 0;

    l->btn_w = UI_BTN_W;
    l->btn_h = UI_BTN_H;
    l->btn_x = l->screen_x + l->screen_w - UI_BTN_W - 24;
    l->btn_y = l->screen_y + l->screen_h - UI_BTN_H - 24;
}

/* ---------------- 对外绘制接口 ---------------- */

void draw_shell(WinCanvas *c, const Layout *l)
{
    int bodyX = l->body_x, bodyY = l->body_y, bodyW = l->body_w, bodyH = l->body_h;
    int bodyBot = bodyY + bodyH;
    int screenX = l->screen_x, screenY = l->screen_y;
    int screenW = l->screen_w, screenH = l->screen_h;
    int r = bodyH / 12;
    int logoX, logoY, bezelY, bezelH;

    if (r < 16) r = 16;

    memset(c->map, 0, c->size);

    /* 1. 桌面背景 */
    fill_rect(c, 0, 0, l->width, l->height, COLOR_BG);

    /* 2. 显示器外壳 (圆角矩形, 金属深灰) */
    fill_rounded_rect(c, bodyX, bodyY, bodyW, bodyH, r, COLOR_BODY);

    /* 外壳顶部高光 + 底部暗影, 增强立体感 */
    fill_rounded_rect(c, bodyX, bodyY, bodyW, r / 2, r, COLOR_BODY_HI);
    fill_rect(c, bodyX + r, bodyY, bodyW - 2 * r, r / 2, COLOR_BODY_HI);
    fill_rect(c, bodyX + r, bodyBot - r / 2, bodyW - 2 * r, r / 2, COLOR_BODY_LO);

    /* 3. 屏幕内区挖透明洞, 露出下层视频 plane */
    fill_rect(c, screenX, screenY, screenW, screenH, COLOR_TRANSPARENT);

    /* 4. 显示器底座: 颈 + 梯形底座 */
    {
        int neckW = bodyW / 9;
        int neckH = bodyH / 12;
        int baseTop = neckW * 2;
        int baseBot = bodyW * 32 / 100;
        int baseH = bodyH / 10;
        int neckX = bodyX + bodyW / 2 - neckW / 2;
        if (neckH < 30) neckH = 30;
        if (baseH < 40) baseH = 40;
        fill_rect(c, neckX, bodyBot, neckW, neckH, COLOR_STAND);
        fill_rect(c, neckX, bodyBot, neckW, 4, COLOR_STAND_HI);
        fill_trapezoid(c, bodyX + bodyW / 2, bodyBot + neckH,
                       baseTop, baseBot, baseH, COLOR_STAND);
        fill_trapezoid(c, bodyX + bodyW / 2, bodyBot + neckH,
                       baseTop - 8, baseBot - 16, baseH - 6, COLOR_STAND_HI);
    }

    /* 5. 底部边框正中: "EASY EAI" logo */
    bezelY = screenY + screenH;
    bezelH = bodyBot - bezelY;
    logoY = bezelY + (bezelH - FONT_CH * 2) / 2;
    logoX = bodyX + bodyW / 2 - text_width("EASY EAI", 2) / 2;
    draw_text(c, logoX, logoY, "EASY EAI", 2, COLOR_LOGO);
}

void draw_button(WinCanvas *c)
{
    int tw = text_width("SWAP L/R", 2);

    memset(c->map, 0, c->size);
    fill_rounded_rect(c, UI_BTN_PAD, UI_BTN_PAD, UI_BTN_W, UI_BTN_H, UI_BTN_H / 4,
                      COLOR_BTN_IDLE);
    rect_outline_rounded(c, 0, 0, UI_BTN_W + 2 * UI_BTN_PAD, UI_BTN_H + 2 * UI_BTN_PAD,
                         UI_BTN_H / 4, UI_BTN_PAD, COLOR_BTN_FRAME);
    draw_text(c, UI_BTN_PAD + (UI_BTN_W - tw) / 2,
              UI_BTN_PAD + (UI_BTN_H - FONT_CH * 2) / 2, "SWAP L/R", 2, COLOR_BTN_TEXT);
}

void draw_stats(WinCanvas *c, const DemoStats *s)
{
    char text[64];
    int lineY;

    memset(c->map, 0, c->size);
    fill_rounded_rect(c, 0, 0, STATS_W, STATS_H, STATS_R, COLOR_PANEL_BG);
    rect_outline_rounded(c, 0, 0, STATS_W, STATS_H, STATS_R, 2, COLOR_PANEL_FRAME);

    lineY = 10;
    snprintf(text, sizeof(text), "FPS: %.1f", s->fps);
    draw_text(c, 16, lineY, text, 2, COLOR_ACCENT);
    lineY += FONT_CH * 2 + 6;
    snprintf(text, sizeof(text), "RES: %dx%d", s->display_w, s->display_h);
    draw_text(c, 16, lineY, text, 2, COLOR_TEXT);
    lineY += FONT_CH * 2 + 6;
    snprintf(text, sizeof(text), "CPU: %.1f%%", s->cpu);
    draw_text(c, 16, lineY, text, 2, COLOR_TEXT);
    lineY += FONT_CH * 2 + 6;
    /* RGA / GPU 占用常驻显示，与 -C 选定的合成后端无关。 */
    if (s->rga >= 0.f)
        snprintf(text, sizeof(text), "RGA: %.0f%%", s->rga);
    else
        snprintf(text, sizeof(text), "RGA: N/A");
    draw_text(c, 16, lineY, text, 2, COLOR_TEXT);
    lineY += FONT_CH * 2 + 6;
    if (s->gpu >= 0.f)
        snprintf(text, sizeof(text), "GPU: %.0f%%", s->gpu);
    else
        snprintf(text, sizeof(text), "GPU: N/A");
    draw_text(c, 16, lineY, text, 2, COLOR_TEXT);
    lineY += FONT_CH * 2 + 6;
    snprintf(text, sizeof(text), "MEM: %ld/%ldM %.0f%%",
             s->mem_used_mb, s->mem_total_mb, s->mem_percent);
    draw_text(c, 16, lineY, text, 2, COLOR_TEXT);
}

void draw_cursor(WinCanvas *c)
{
    int row, col;

    memset(c->map, 0, c->size);
    for (row = 0; row < CURSOR_ART_H; row++) {
        const char *line = gCursorArt[row];
        for (col = 0; col < CURSOR_ART_W; col++) {
            if (line[col] == 'X')
                px(c, col, row, COLOR_CURSOR_EDGE);
            else if (line[col] == 'O')
                px(c, col, row, COLOR_CURSOR_FILL);
        }
    }
}
