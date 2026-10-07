/**
 * @file watch_page_functions.c
 * @brief 功能列表页 —— 轮播第 4 页（v0.5.0 新增，v0.6.0 加第 5 行）
 *
 * ## 为什么加这一页
 *
 * v0.4.0 的时候，"设置""WiFi 连接""开发人员选项"全挤在设置页里，
 * 一页 9 行、内容 450px，屏幕只有 280px，用户得滚半天才够得着最下面
 * （踩过坑，见 v0.4.0 文档「设置页滚不动」）。
 *
 * v0.5.0 按用户要求重排，v0.6.0 又加了「游戏」：
 *
 *     ┌──────────────────────────┐
 *     │ 功能列表                  │
 *     │ ┌──────────────────────┐ │
 *     │ │ 设置              >  │ │  亮度 / 自动息屏 / 时钟状态
 *     │ ├──────────────────────┤ │
 *     │ │ 时钟              >  │ │  时间 / 秒表 / 倒计时 / 闹钟
 *     │ ├──────────────────────┤ │
 *     │ │ WiFi 连接         >  │ │  扫描连接并自动校时
 *     │ ├──────────────────────┤ │
 *     │ │ 开发人员选项      >  │ │  录制 / 系统信息 / 蜂鸣器
 *     │ ├──────────────────────┤ │
 *     │ │ 游戏              >  │ │  小恐龙，玩一局   ← v0.6.0
 *     │ └──────────────────────┘ │
 *     └──────────────────────────┘
 *
 * ## ⚠️ 这一页现在是**可纵向滚动**的（v0.6.0 起）
 *
 * 5 行 x 52 + 4 x 6 间隙 = 284，加标题 32 共 316，比 280 高 —— 装不下了。
 * 用户明确选择"保持 52px 行高、允许上下滑动"，所以：
 *
 *   · 一屏只能看到前 4 行，**第 5 行「游戏」要往下拖才看得到**
 *   · 滚动的实现不在本文件里，在 watch_pages.c 的 on_pressing()：
 *     页面内容溢出时，纵向拖动改为滚当前页（`lv_obj_scroll_by_bounded`）。
 *     本页只要"内容真的超出"就会自动获得滚动能力，
 *     可用串口命令或 `watch_ui_debug_page_scroll()` 确认（应 > 0）。
 *
 * ## ⚠️ 副作用：在本页上滑不再打开控制中心
 *
 * 一旦页面内容溢出，纵向拖动优先给"滚页面"，手势层的上滑开控制中心
 * 在本页就失效了（其余 3 页不受影响）。这是滚动方案的必然代价，
 * 已记入 v0.6.0 版本文档的「已知限制」。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_strings.h"

#define ROW_TITLE_Y   0
#define LIST_W        (W_SCREEN_W - 2 * W_PAD_PAGE)
#define LIST_Y        32
#define LIST_GAP      6
#define ROW_H         52
#define LIST_H        (5 * ROW_H + 4 * LIST_GAP)   /* = 284，见上面说明 */

/* ==================== 各入口回调 ==================== */
static void open_settings(lv_event_t *e) { (void)e; watch_settings_open(); }
static void open_clock(lv_event_t *e)    { (void)e; watch_clock_open(); }
static void open_wifi(lv_event_t *e)     { (void)e; watch_wifi_open(); }
static void open_dev(lv_event_t *e)      { (void)e; watch_developer_open_menu(); }
static void open_games(lv_event_t *e)    { (void)e; watch_games_open(); }

/* ==================== 建界面 ==================== */
static lv_obj_t *functions_create(lv_obj_t *parent)
{
    lv_obj_t *scr = w_page_create(parent);

    lv_obj_t *title = w_label_create(scr, W_STR_FUNCTIONS, watch_font_cn(20),
                                     W_COLOR_AUTO);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, ROW_TITLE_Y);

    /*
     * 用一个 flex column 容器包住 5 行：
     * `w_menu_row_create()` 不设 x/y（靠父容器的 flex 排布），
     * 而轮播页的根是绝对定位的 —— 不包这一层的话 5 行会全叠在 (0,0)。
     */
    lv_obj_t *list = w_flex_col_create(scr, 0, LIST_Y, LIST_W, LIST_H, LIST_GAP);

    w_menu_row_create(list, W_STR_SETTINGS,    W_STR_FUNC_SET_SUB,   open_settings);
    w_menu_row_create(list, W_STR_CLOCK,       W_STR_FUNC_CLOCK_SUB, open_clock);
    w_menu_row_create(list, W_STR_WIFI_TITLE,  W_STR_FUNC_WIFI_SUB,  open_wifi);
    w_menu_row_create(list, W_STR_DEV_TITLE,   W_STR_FUNC_DEV_SUB,   open_dev);
    w_menu_row_create(list, W_STR_GAMES,       W_STR_FUNC_GAMES_SUB, open_games);

    return scr;
}

/*
 * 刷新：本页全是静态入口，没有会变的数值，所以不需要刷新。
 * （保留 NULL 而不是空函数，省一次调用。）
 */
const watch_page_t watch_page_functions = {
    .id      = "functions",
    .title   = W_STR_FUNCTIONS,
    .create  = functions_create,
    .refresh = NULL,
    .destroy = NULL,
};
