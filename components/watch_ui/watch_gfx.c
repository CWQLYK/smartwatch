/**
 * @file watch_gfx.c
 * @brief 2bpp 索引画布绘图实现（布局依据见 watch_gfx.h 顶部说明）
 */
#include "watch_gfx.h"

#include <string.h>

/* ==================== 初始化 ==================== */
void w_gfx_init(w_gfx_t *g, void *buf, int w, int h)
{
    if (!g) {
        return;
    }
    /*
     * ★ 跳过调色板。
     *   整个项目里"画布位图指针"只在这里算一次，别处一律用 g->px ——
     *   少写一次 `+ 16` 就等着"清屏把调色板抹了、整屏全黑还不报错"。
     */
    g->px     = (uint8_t *)buf + W_GFX_PALETTE_BYTES;
    g->w      = (int16_t)w;
    g->h      = (int16_t)h;
    g->stride = (int16_t)(((w + 3) >> 2));
}

/* ==================== 基本操作 ==================== */
void w_gfx_clear(w_gfx_t *g, int idx)
{
    if (!g || !g->px) {
        return;
    }
    const uint8_t fill = (uint8_t)((idx & 3) * 0x55);   /* 4 个像素一组填同一个索引 */
    memset(g->px, fill, (size_t)g->stride * (size_t)g->h);
}

void w_gfx_px(w_gfx_t *g, int x, int y, int idx)
{
    if (!g || !g->px || x < 0 || y < 0 || x >= g->w || y >= g->h) {
        return;
    }
    uint8_t *p = &g->px[y * g->stride + (x >> 2)];
    const uint8_t shift = (uint8_t)(6 - 2 * (x & 3));
    *p = (uint8_t)((*p & ~(3u << shift)) | ((idx & 3) << shift));
}

int w_gfx_get(const w_gfx_t *g, int x, int y)
{
    if (!g || !g->px || x < 0 || y < 0 || x >= g->w || y >= g->h) {
        return 0;
    }
    const uint8_t v = g->px[y * g->stride + (x >> 2)];
    return (v >> (6 - 2 * (x & 3))) & 3;
}

void w_gfx_hline(w_gfx_t *g, int x, int y, int len, int idx)
{
    if (!g || !g->px || y < 0 || y >= g->h || len <= 0) {
        return;
    }
    if (x < 0) {
        len += x;
        x = 0;
    }
    if (x + len > g->w) {
        len = g->w - x;
    }
    if (len <= 0) {
        return;
    }

    /* 两头按位写，中间整字节直接填 —— 地面每帧要画，省一点是一点 */
    const uint8_t fill = (uint8_t)((idx & 3) * 0x55);
    uint8_t *row = &g->px[y * g->stride];

    int x_end = x + len;
    while (x < x_end && (x & 3)) {
        w_gfx_px(g, x, y, idx);
        x++;
    }
    while (x + 4 <= x_end) {
        row[x >> 2] = fill;
        x += 4;
    }
    while (x < x_end) {
        w_gfx_px(g, x, y, idx);
        x++;
    }
}

void w_gfx_vline(w_gfx_t *g, int x, int y, int len, int idx)
{
    for (int i = 0; i < len; i++) {
        w_gfx_px(g, x, y + i, idx);
    }
}

void w_gfx_rect(w_gfx_t *g, int x, int y, int w, int h, int idx)
{
    for (int j = 0; j < h; j++) {
        w_gfx_hline(g, x, y + j, w, idx);
    }
}

void w_gfx_frame(w_gfx_t *g, int x, int y, int w, int h, int idx)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    w_gfx_hline(g, x, y, w, idx);
    w_gfx_hline(g, x, y + h - 1, w, idx);
    w_gfx_vline(g, x, y, h, idx);
    w_gfx_vline(g, x + w - 1, y, h, idx);
}

/* ==================== 精灵 ==================== */
void w_gfx_blit(w_gfx_t *g, int x, int y, const w_gfx_sprite_t *s)
{
    if (!g || !g->px || !s || !s->bits) {
        return;
    }

    /* 先裁掉画布外的部分，内层循环就不用每像素判边界了 */
    int sx = 0, sy = 0;
    int w = s->w, h = s->h;
    if (x < 0) { sx = -x; w -= sx; x = 0; }
    if (y < 0) { sy = -y; h -= sy; y = 0; }
    if (x + w > g->w) { w = g->w - x; }
    if (y + h > g->h) { h = g->h - y; }
    if (w <= 0 || h <= 0) {
        return;
    }

    const uint8_t ink = s->ink & 3;
    for (int j = 0; j < h; j++) {
        const uint8_t *src = &s->bits[(sy + j) * s->stride];
        uint8_t *dst = &g->px[(y + j) * g->stride];
        for (int i = 0; i < w; i++) {
            const int si = sx + i;
            const int v = (src[si >> 2] >> (6 - 2 * (si & 3))) & 3;
            if (!v) {
                continue;                    /* 0 = 透明，露出背景 */
            }
            const int dx = x + i;
            uint8_t *p = &dst[dx >> 2];
            const uint8_t shift = (uint8_t)(6 - 2 * (dx & 3));
            *p = (uint8_t)((*p & ~(3u << shift)) | (ink << shift));
        }
    }
}

void w_gfx_blit_tiled_x(w_gfx_t *g, int x, int y, int total_w, int offset,
                        const w_gfx_sprite_t *s)
{
    if (!g || !s || s->w <= 0 || total_w <= 0) {
        return;
    }

    /*
     * 把 offset 规整到 [0, s->w)：
     * 负数取模在 C 里是负的（-3 % 1200 == -3），不处理会让地面整条消失。
     */
    int off = offset % s->w;
    if (off < 0) {
        off += s->w;
    }

    /*
     * 从"最左边那块被裁掉一半的瓦片"开始贴，一直贴到右边界。
     * 起点要用 x - off（可能是负的），blit 自己会裁掉画布外的部分。
     */
    const int end = x + total_w;
    for (int tx = x - off; tx < end; tx += s->w) {
        w_gfx_blit(g, tx, y, s);
    }
}

int w_gfx_count(const w_gfx_t *g, int idx)
{
    if (!g || !g->px) {
        return 0;
    }
    int n = 0;
    for (int y = 0; y < g->h; y++) {
        for (int x = 0; x < g->w; x++) {
            if (w_gfx_get(g, x, y) == (idx & 3)) {
                n++;
            }
        }
    }
    return n;
}
