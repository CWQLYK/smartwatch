/**
 * @file watch_style.c
 * @brief 设计令牌实现
 *
 * 目前这里没有需要编译期实现的东西 —— 配色/字号/间距全是宏，
 * 中文字体入口在 fonts 组件的 watch_fonts.c 里。
 *
 * 暂时留一个"主题版本号"，方便串口日志确认烧进去的是哪版 UI。
 */
#include "watch_style.h"

const char *watch_style_version(void)
{
    return "watch-ui theme v1";
}
