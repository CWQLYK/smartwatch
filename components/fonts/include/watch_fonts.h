/**
 * @file watch_fonts.h
 * @brief 中文字体入口 —— 屏蔽"生成出来的字体叫什么名字"这件事
 *
 * 中文字体是 `lv_font_conv` 从 simhei.ttf 抽出来的点阵，
 * 只包含**项目里实际用到的汉字**（全量 CJK 要几十 MB，Flash 装不下）。
 *
 * ⚠️ 字体文件是**生成产物**，不要手改：
 *      改完界面文字后，跑 `firmware/smartwatch/tools/gen_font.ps1` 重新生成。
 *      忘了跑的症状很迷惑：字形查得到、adv_w 正常，但屏幕上是**豆腐块**。
 *
 * 另外 ⚠️ 生成时必须加 `--no-compress`（LVGL 8.x 读不了压缩位图）。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 生成出来的字体对象（定义在 components/fonts 下的 .c 文件里） */
extern const lv_font_t lv_font_watch_14;    /* 小字：标签、单位、日期     */
extern const lv_font_t lv_font_watch_16;    /* 正文：菜单项、标题         */
extern const lv_font_t lv_font_watch_20;    /* 大字：状态栏时间、卡片数值 */

/**
 * @brief 按字号取中文字体
 * @param size 14 / 16 / 20；其它值返回最接近的一档
 * @return 字体指针，永不为 NULL
 */
const lv_font_t *watch_font_cn(int size);

/**
 * @brief 按给定高度取一个"能装下中文"的字体（取 >= height 的最小档，不够就用最大档）
 * @note 做自适应布局时用，平时直接用 watch_font_cn() 更直观
 */
const lv_font_t *watch_font_cn_for_height(int height);

#ifdef __cplusplus
}
#endif
