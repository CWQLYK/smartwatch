/**
 * @file watch_gfx.h
 * @brief 2bpp 索引画布的绘图工具 —— 游戏画面专用（v0.6.0 新增）
 *
 * ## 为什么要自己写一层，而不用 LVGL 的 `lv_canvas_set_px`
 *
 * 一个像素一点地调 `lv_canvas_set_px()`，每像素都要重新算描述符、判格式、
 * 走一遍边界检查 —— 240x280 一帧就是 6.7 万次调用，纯开销。
 * 直接拿到位图指针自己按位写，恐龙一帧只有几千次位运算，
 * 实测（模拟器）画满一屏也就几十微秒级。
 *
 * ## 画布的内存布局（**对着 LVGL 源码核过的，别凭印象改**）
 *
 * `LV_IMG_CF_INDEXED_2BIT` 的缓冲区：
 *
 *     +0   ┌──────────────────────────┐
 *          │ 调色板 4 项 x lv_color32_t│  16 字节
 *     +16  ├──────────────────────────┤
 *          │ 像素位图，每行按字节对齐   │  stride = (w + 3) / 4 字节/行
 *          └──────────────────────────┘
 *
 * 依据 `lv_img_buf.c`（源码里的原话是"Skip the palette"）：
 *   · 取像素前先 `buf_u8 += sizeof(lv_color32_t) * 4;` 跳过调色板
 *   · 行内 `x = x >> 2`，即 **4 个像素挤 1 字节，高位在前**
 *   · 行首 `((dsc->header.w + 3) >> 2) * y`，即每行**按字节对齐**（不是整幅紧排）
 * 且 `LV_IMG_BUF_SIZE_INDEXED_2BIT(w,h) == ((w+3)>>2)*h + 16`。
 *
 * ⚠️ 两个必须记住的点：
 *   1. **清屏时不能从缓冲区第 0 字节开始 memset** —— 那会把调色板抹掉，
 *      整幅画变成全黑（而且不报任何错）。所以本层统一用"跳过 16 字节"的指针。
 *   2. 调色板必须**在 `lv_canvas_set_buffer()` 之后**再设，
 *      因为 set_buffer 只是把指针记下来，调色板是写进那块内存的；
 *      顺序反了会被后面的初始化覆盖（或者根本没生效）。
 */
#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 调色板占用字节数（4 项 x lv_color32_t） */
#define W_GFX_PALETTE_BYTES   16

/* 调色板索引的含义（各游戏共用这套约定） */
#define W_GFX_IDX_BG      0    /**< 背景（天空） */
#define W_GFX_IDX_INK     1    /**< 前景：主角、障碍 */
#define W_GFX_IDX_DIM     2    /**< 次要：地面、云 */
#define W_GFX_IDX_HI      3    /**< 强调色（备用） */

/** @brief 一个精灵位图（2bpp 索引，0 = 透明） */
typedef struct {
    uint16_t       w;       /**< 宽（像素） */
    uint16_t       h;       /**< 高（像素） */
    uint16_t       stride;  /**< 每行字节数（= (w+3)/4，由生成脚本给出） */
    uint8_t        ink;     /**< 用哪个调色板索引画它（1 或 2） */
    const uint8_t *bits;    /**< 位图数据（在 flash 里，只读） */
} w_gfx_sprite_t;

/** @brief 画布句柄 */
typedef struct {
    uint8_t *px;      /**< 位图起点（**已经跳过 16 字节调色板**） */
    int16_t  w;       /**< 宽（像素） */
    int16_t  h;       /**< 高（像素） */
    int16_t  stride;  /**< 每行字节数 */
} w_gfx_t;

/**
 * @brief 绑定一块画布缓冲区
 *
 * @param g    句柄
 * @param buf  `lv_canvas` 的缓冲区**原始指针**（含调色板那 16 字节）
 * @param w,h  尺寸（像素）
 *
 * @note `buf` 的尺寸必须是 `((w+3)/4)*h + 16`，可用 LVGL 的
 *       `LV_IMG_BUF_SIZE_INDEXED_2BIT(w,h)` 算。
 */
void w_gfx_init(w_gfx_t *g, void *buf, int w, int h);

/** @brief 整幅填成某个索引 */
void w_gfx_clear(w_gfx_t *g, int idx);

/** @brief 画一个像素（越界自动忽略） */
void w_gfx_px(w_gfx_t *g, int x, int y, int idx);

/** @brief 读一个像素（越界返回 0） */
int  w_gfx_get(const w_gfx_t *g, int x, int y);

/** @brief 横线（最常用：地面） */
void w_gfx_hline(w_gfx_t *g, int x, int y, int len, int idx);

/** @brief 竖线 */
void w_gfx_vline(w_gfx_t *g, int x, int y, int len, int idx);

/** @brief 填充矩形 */
void w_gfx_rect(w_gfx_t *g, int x, int y, int w, int h, int idx);

/** @brief 矩形边框（线宽 1） */
void w_gfx_frame(w_gfx_t *g, int x, int y, int w, int h, int idx);

/**
 * @brief 贴一个精灵（索引 0 的像素透明，不覆盖背景）
 *
 * @note 自动裁剪到画布范围内，坐标可以为负。
 */
void w_gfx_blit(w_gfx_t *g, int x, int y, const w_gfx_sprite_t *s);

/**
 * @brief 横向平铺贴一个精灵（地面那种超长条用）
 *
 * @param offset  整体左移多少像素（做卷动）
 * @param total_w 要铺满的总宽
 */
void w_gfx_blit_tiled_x(w_gfx_t *g, int x, int y, int total_w, int offset,
                        const w_gfx_sprite_t *s);

/** @brief 统计某个索引的像素数（自检断言用） */
int  w_gfx_count(const w_gfx_t *g, int idx);

#ifdef __cplusplus
}
#endif
