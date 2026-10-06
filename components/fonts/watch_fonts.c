/**
 * @file watch_fonts.c
 * @brief 中文字体入口实现
 */
#include "watch_fonts.h"

const lv_font_t *watch_font_cn(int size)
{
    if (size <= 14) return &lv_font_watch_14;
    if (size <= 16) return &lv_font_watch_16;
    return &lv_font_watch_20;
}

const lv_font_t *watch_font_cn_for_height(int height)
{
    if (height <= 14) return &lv_font_watch_14;
    if (height <= 16) return &lv_font_watch_16;
    return &lv_font_watch_20;
}
