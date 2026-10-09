/**
 * @file watch_pages_all.c
 * @brief 内置页面注册表
 *
 * 加一个新页面只要三步：
 *   1. 在 pages/ 下写一个 .c，导出一个 `const watch_page_t watch_page_xxx`
 *   2. 在下面的数组里加一行
 *   3. 改 watch_ui_pages/CMakeLists.txt 的 SRCS **和**
 *      lvgl_sim/Makefile 的源文件列表（模拟器要单独列）
 *
 * ## 页面顺序 = 右滑切换的顺序（★ v0.5.0 改过，v0.7.0 加了一页）
 *
 *     时间 → 运动 → 健康 → 功能列表 → 工具 → 回到时间
 *
 * v0.4.0 的顺序是"表盘 → 设置 → 健康 → 运动"，当时的理由是
 * "设置是最常用的第二页"。v0.5.0 按用户要求重排：
 * 日常最常看的是**时间和运动数据**，所以它们放最前；
 * 设置/时钟/WiFi 这些"要动手操作"的都收进**功能列表**页。
 *
 * ★ v0.7.0：新增**工具**页放"计算器"这类纯工具。
 *   为什么不继续往功能列表里加行：那一页 5 行已经占满 251/256px，
 *   第 6 行会溢出 → 该页变成可滚动 → "上滑开控制中心"失效
 *   （v0.6.0 踩过）。理由写在 watch_page_tools.c 的文件头。
 */
#include "watch_pages.h"

static const watch_page_t *const s_pages[] = {
    &watch_page_home,
    &watch_page_sport,
    &watch_page_health,
    &watch_page_functions,
    &watch_page_tools,
    NULL,                       /* 哨兵 */
};

const watch_page_t *const *watch_pages_all(void)
{
    return s_pages;
}
