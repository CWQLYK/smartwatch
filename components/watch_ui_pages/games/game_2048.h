/**
 * @file game_2048.h
 * @brief 2048 游戏本体（游戏列表第 2 款，v0.7.0）
 *
 * 实现见 game_2048.c；外壳/暂停/结算/防息屏全部由 watch_game.c 提供。
 */
#pragma once

#include "watch_game.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const w_game_t game_2048;

/* ==================================================================
 * 自检接口（PC 模拟器用）
 *
 * 正常开局棋盘是随机的，靠滑方向去断言结果很脆。
 * 所以给自检"注入已知棋盘 → 走一步 → 读回结果"的口子，
 * 把移动/合并/计分/判负这些纯逻辑确定性地锁死。
 * ================================================================== */

/** @brief 注入棋盘：g[16] 按行优先，值是**指数**（0=空，1=2，2=4 …） */
void game2048_debug_set_grid(const uint8_t g[16]);

/** @brief 读一格的指数 */
uint8_t game2048_debug_get(int r, int c);

/** @brief 直接走一步（dr/dc 是方向），返回 1 = 真的动了 */
int game2048_debug_move(int dr, int dc);

void     game2048_debug_set_score(uint32_t v);
uint32_t game2048_debug_score(void);

/** @brief 当前是否已无路可走（无空格且无相邻等值） */
int game2048_debug_no_moves(void);

/** @brief 把棋盘数值形态打成 "2,4,0,..."，自检打失败信息用 */
void game2048_debug_dump(char *buf, int len);

/**
 * @brief 自检用：直接画一帧（不看游戏状态机）
 *
 * 用来**看数字排版**：注入含 1024/2048 的棋盘再截图，
 * 确认多位数在 44px 格子里放得下。这类问题断言抓不到，只能看图。
 */
void game2048_debug_render(lv_obj_t *canvas);

#ifdef __cplusplus
}
#endif
