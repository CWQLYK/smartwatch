/**
 * @file watch_style.h
 * @brief 设计令牌 —— 配色 / 字号 / 间距 / 圆角
 *
 * 为什么要有它：
 *   界面一旦超过两个页面，颜色和字号就会开始各写各的
 *   （一处 0x00D4AA，另一处 0x00d4a9）。所有视觉常量收在这里，
 *   改主题只改这一个文件，页面代码一行不动。
 *
 * 配色取自原型的深色手表风：近黑底 + 卡片灰 + 青绿主色 + 品红心率。
 */
#pragma once

#include "lvgl.h"
#include "watch_fonts.h"    /* 中文字体入口（fonts 组件的生成产物） */

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、配色
 *
 * ⚠️ 这些是**宏**（函数式展开），所以：
 *    - 不能取地址：`&W_COLOR_TEXT` 编译不过
 *    - 传参时请用"值传递"，或者用本文件下面的 w_color_* 变量
 *    （踩过这个坑：一开始把 w_label_create 的颜色参数写成指针类型，
 *      结果 lvalue required as unary '&' operand 报了一片）
 * ================================================================== */
#define W_COLOR_BG        lv_color_hex(0x0B0E14)   /* 背景：近黑            */
#define W_COLOR_CARD      lv_color_hex(0x161B26)   /* 卡片                  */
#define W_COLOR_CARD_HI   lv_color_hex(0x1F2636)   /* 卡片（按下/高亮）      */
#define W_COLOR_TRACK     lv_color_hex(0x2A3242)   /* 轨道/分隔线           */
#define W_COLOR_ACCENT    lv_color_hex(0x00D4AA)   /* 主色：青绿            */
#define W_COLOR_HEART     lv_color_hex(0xFF4D6D)   /* 心率：品红            */
#define W_COLOR_WARN      lv_color_hex(0xFFB020)   /* 警告：琥珀            */
#define W_COLOR_DANGER    lv_color_hex(0xFF4D4D)   /* 危险/低电：红         */
#define W_COLOR_TEXT      lv_color_hex(0xE8ECF3)   /* 主文字                */
#define W_COLOR_TEXT_DIM  lv_color_hex(0x6B7688)   /* 次要文字              */
#define W_COLOR_TEXT_MUTE lv_color_hex(0x424B5C)   /* 禁用/占位文字         */

/**
 * @brief "用默认色"的哨兵值
 * @note 传它给控件工厂函数表示"这个颜色我不关心，用主题默认的"
 */
#define W_COLOR_AUTO      lv_color_hex(0xFFFFFFFF)

/* ==================================================================
 * 二、字号
 *
 * 两套字体并存（见 watch_fonts.h）：
 *   W_FONT_xx    ASCII 数字/英文（Montserrat，紧凑清晰）
 *   watch_font_cn() 中文（黑体点阵，只含项目用到的字）
 * ================================================================== */
#define W_FONT_TINY       (&lv_font_montserrat_12)
#define W_FONT_SMALL      (&lv_font_montserrat_14)
#define W_FONT_NORMAL     (&lv_font_montserrat_16)
#define W_FONT_MEDIUM     (&lv_font_montserrat_20)
#define W_FONT_LARGE      (&lv_font_montserrat_28)
#define W_FONT_HUGE       (&lv_font_montserrat_32)
#define W_FONT_CLOCK      (&lv_font_montserrat_40)   /* 表盘大时间        */

/* ==================================================================
 * 三、间距与尺寸
 * ================================================================== */
#define W_SCREEN_W        240
#define W_SCREEN_H        280

#define W_PAD_PAGE        12    /* 页面四周留白（圆角屏要留够）    */
#define W_PAD_CARD        10    /* 卡片内边距                     */
#define W_GAP             8     /* 卡片之间的间隙                 */

#define W_RADIUS_CARD     12
#define W_RADIUS_BTN      8
#define W_RADIUS_PILL     20    /* 胶囊形                         */

#define W_STATUSBAR_H     26    /* 顶部状态栏高度                 */

/* ==================================================================
 * 四、主题版本（串口日志确认用）
 * ================================================================== */
const char *watch_style_version(void);

#ifdef __cplusplus
}
#endif
