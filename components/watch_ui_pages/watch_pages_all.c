/**
 * @file watch_pages_all.c
 * @brief 内置页面注册表
 *
 * 加一个新页面只要两步：
 *   1. 在 pages/ 下写一个 .c，导出一个 `const watch_page_t watch_page_xxx`
 *   2. 在下面的数组里加一行（并改 CMakeLists.txt 的 SRCS）
 *
 * 页面顺序 = 右滑切换的顺序（表盘 → 设置 → 健康 → 运动 → 回到表盘）。
 * 顺序刻意让"表盘 → 设置"相邻：设置是最常用的第二页。
 */
#include "watch_pages.h"

static const watch_page_t *const s_pages[] = {
    &watch_page_home,
    &watch_page_settings,
    &watch_page_health,
    &watch_page_sport,
    NULL,                       /* 哨兵 */
};

const watch_page_t *const *watch_pages_all(void)
{
    return s_pages;
}
