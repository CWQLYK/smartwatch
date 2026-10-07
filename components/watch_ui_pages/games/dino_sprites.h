/**
 * @file dino_sprites.h
 * @brief 小恐龙精灵位图表 —— **本文件由脚本生成，不要手改**
 *
 * 生成命令：python tools/gen_dino_sprites.py
 *
 * 精灵类型用的是 watch_ui 里的公共定义 `w_gfx_sprite_t`（watch_gfx.h），
 * 不在这里另定义一个 —— 否则贴图函数收到的是「长得一样但类型不同」的
 * 指针：C 只给个警告就放过去了，字段顺序一旦不一致就是花屏。
 */
#pragma once

#include <stdint.h>
#include "watch_gfx.h"      /* w_gfx_sprite_t */

#ifdef __cplusplus
extern "C" {
#endif

extern const w_gfx_sprite_t dino_spr_dino_idle;   /* 40x43 */
extern const w_gfx_sprite_t dino_spr_dino_run1;   /* 40x43 */
extern const w_gfx_sprite_t dino_spr_dino_run2;   /* 40x43 */
extern const w_gfx_sprite_t dino_spr_dino_jump;   /* 40x43 */
extern const w_gfx_sprite_t dino_spr_dino_dead;   /* 40x43 */
extern const w_gfx_sprite_t dino_spr_cactus_small1;   /* 15x33 */
extern const w_gfx_sprite_t dino_spr_cactus_small2;   /* 15x33 */
extern const w_gfx_sprite_t dino_spr_cactus_large1;   /* 23x46 */
extern const w_gfx_sprite_t dino_spr_cactus_large2;   /* 23x46 */
extern const w_gfx_sprite_t dino_spr_bird1;   /* 46x32 */
extern const w_gfx_sprite_t dino_spr_bird2;   /* 46x32 */
extern const w_gfx_sprite_t dino_spr_cloud;   /* 42x14 */
extern const w_gfx_sprite_t dino_spr_ground;   /* 1200x10 */

#ifdef __cplusplus
}
#endif
