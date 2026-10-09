/**
 * @file watch_page_tools.c
 * @brief 「工具」轮播页 —— v0.7.0 新增（轮播第 5 页）
 *
 *     ┌──────────────────────────┐
 *     │ 工具                      │
 *     │ ┌──────────────────────┐ │
 *     │ │ 计算器            >  │ │  加减乘除与百分号
 *     │ └──────────────────────┘ │
 *     │ 选中一个开始用。返回点右  │
 *     │ 上角。                    │
 *     └──────────────────────────┘
 *
 * ## 为什么单开一页，而不是塞进「功能列表」
 *
 * 「功能列表」已经 5 行占满：`28 + 5×43 + 4×2 = 251 ≤ 256`（内容区只有
 * `280 - 2×12`）。加第 6 行需要 269px —— 一旦溢出，该页就变成可滚动，
 * 而**手写滚动的副作用**是"上滑开控制中心"在该页失效
 * （v0.6.0 踩过，v0.6.1 才好；自检里有"可滚 0 px"的断言守着）。
 *
 * 也不做成「时钟」的第 5 个标签：标签栏按 216 宽度等分，
 * 5 个标签每个只剩 40px，而"倒计时"三个中文在 14px 下约 42px，会被压字。
 *
 * 所以走**游戏列表那一套**：本页只是入口列表，点一行开一个模态子页。
 * 好处是以后加"日历 / 天气 / 二维码"这些纯工具，都往这里挂。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_strings.h"

/* ==================== 各入口回调 ==================== */
static void open_calc(lv_event_t *e)     { (void)e; watch_calc_open(); }
static void open_calendar(lv_event_t *e) { (void)e; watch_calendar_open(); }
static void open_weather(lv_event_t *e)  { (void)e; watch_weather_open(); }

#define TOOL_ROWS   3

/* ==================== 建界面 ==================== */
static lv_obj_t *tools_create(lv_obj_t *parent)
{
    lv_obj_t *scr = w_page_create(parent);

    lv_obj_t *title = w_label_create(scr, W_STR_TOOLS, watch_font_cn(20),
                                     W_COLOR_AUTO);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    /*
     * 用 flex column 包住列表行：`w_menu_row_create()` 不设 x/y
     * （靠父容器的 flex 排布），而页面根是绝对定位的 ——
     * 不包这一层的话行会全叠在 (0,0)。
     */
    const int row_h = W_MENU_ROW_H;
    lv_obj_t *list = w_flex_col_create(scr, 0, 28,
                                       W_SCREEN_W - 2 * W_PAD_PAGE,
                                       TOOL_ROWS * row_h + (TOOL_ROWS - 1) * 2, 2);

    w_menu_row_create(list, W_STR_CALC_TITLE,     W_STR_FUNC_CALC_SUB, open_calc);
    w_menu_row_create(list, W_STR_CALENDAR_TITLE, W_STR_CALENDAR_SUB,  open_calendar);
    w_menu_row_create(list, W_STR_WX_TITLE,       W_STR_WX_SUB,        open_weather);

    /* 说明文字放在列表下面 */
    lv_obj_t *hint = w_label_create_auto(scr, W_STR_TOOLS_HINT,
                                         W_FONT_SMALL, 13, W_COLOR_TEXT_DIM);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0,
                 28 + TOOL_ROWS * row_h + (TOOL_ROWS - 1) * 2 + 8);

    return scr;
}

/*
 * 刷新：本页全是静态入口，没有会变的数值，所以不需要刷新。
 * （和功能列表页同理，保留 NULL 而不是空函数。）
 */
const watch_page_t watch_page_tools = {
    .id      = "tools",
    .title   = W_STR_TOOLS,
    .create  = tools_create,
    .refresh = NULL,
    .destroy = NULL,
};
