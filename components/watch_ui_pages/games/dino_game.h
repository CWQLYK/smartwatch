/**
 * @file dino_game.h
 * @brief 小恐龙游戏 —— 游戏本体接口（v0.6.0 新增）
 *
 * 外壳（全屏层/帧循环/开始暂停结束面板/存档）在 watch_ui 的 watch_game.c 里，
 * 本文件只负责**这一款游戏**：物理、碰撞、障碍生成、画到画布上。
 */
#pragma once

#include "watch_game.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 小恐龙游戏定义（给宿主 w_game_open() 用） */
extern const w_game_t dino_game;

/* ==================================================================
 * 自检钩子（PC 模拟器用；实机不用）
 *
 * ## 为什么游戏必须能"确定性重放"
 *
 * 难度公平性是这款游戏唯一真正会劝退人的地方：
 * 障碍间隔、跳跃高度、加速曲线这三者只要有一个没配好，
 * 就会出现"明明跳了还是撞死"或者"高分段无解"。
 * 靠人肉试玩是查不出来的（而且我这一侧没有实机）。
 *
 * 所以：随机数种子固定 + 每帧手动推进 + 暴露下面几个量，
 * 自检里就能让一个"机器玩家"跑满几分钟，断言它一直活着。
 * ================================================================== */

/** @brief 固定随机种子（自检用；实机开局会用时间做种，每局都不一样） */
void dino_debug_seed(uint32_t seed);

/** @brief 当前速度（像素/秒） */
float dino_debug_speed(void);

/**
 * @brief 最近障碍的**碰撞盒左边缘**到恐龙**碰撞盒右边缘**的距离
 *
 * @return 像素；前方没有障碍返回 9999
 *
 * @note 口径是碰撞盒（图形边缘各缩了 2~4px），因为自检里的机器玩家
 *       就是靠这个数决定起跳时机的 —— 用图形边缘会有 6px 系统偏差。
 */
float dino_debug_nearest_gap(void);

/** @brief 恐龙是否站在地上 */
bool dino_debug_on_ground(void);

/** @brief 当前是第几次起跳（自检用来确认"点了真的跳了"） */
int dino_debug_jumps(void);

/** @brief 当前这一局撞了几次（正常最多 1 次，自检用） */
int dino_debug_hits(void);

/** @brief 恐龙当前的碰撞盒（x, y, w, h） */
void dino_debug_hitbox(int *x, int *y, int *w, int *h);

/** @brief 障碍物数量（自检用来确认生成器在工作） */
int dino_debug_obstacles(void);

/** @brief 画布上墨色像素数（自检用来确认真的画上去了，而不是一片空白） */
int dino_debug_ink_pixels(void);

/**
 * @brief "生成了理论上跳不过去的障碍"的次数
 *
 * @note **必须恒为 0**。障碍宽度是按当前速度反推上限的
 *       （见 dino_game.c 的 max_clearable_width），
 *       这个计数大于 0 就说明速度曲线被改得越过约束了 ——
 *       玩家会遇到"怎么跳都死"的组合，而且完全无法复现。
 *       自检会断言它为 0。
 */
int dino_debug_unfair_spawns(void);

#ifdef __cplusplus
}
#endif
