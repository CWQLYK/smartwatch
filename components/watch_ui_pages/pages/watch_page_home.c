/**
 * @file watch_page_home.c
 * @brief 表盘页 —— P1 功能 3（数字表盘）+ 功能 4（电量显示）
 *
 * 布局（240 × 280，页面内边距 12）：
 *
 *     ┌──────────────────────────┐  y=0
 *     │ 12:34          🔋 85%    │  状态栏（时间小字 + 电池图标）
 *     ├──────────────────────────┤  y=26  分隔线
 *     │                          │
 *     │        12:34             │  大号时间（Montserrat 40）
 *     │      2026年10月6日       │  日期
 *     │         周二             │  星期
 *     │                          │
 *     ├──────────────────────────┤
 *     │  ┌────┐┌────┐┌────┐      │  三张指标卡
 *     │  │步数││心率││电量│      │
 *     │  └────┘└────┘└────┘      │
 *     ├──────────────────────────┤
 *     │     左右滑动切页          │  底部提示
 *     └──────────────────────────┘  y=280
 *
 * 三张卡片的数据来源：
 *   步数 → IMU（P1 功能 6，未实现 → 显示 "--"）
 *   心率 → MAX30102（P4，未实现 → 显示 "--"）
 *   电量 → BAT_ADC（P1 功能 4，**已实现**）
 */
#include <stdio.h>
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_strings.h"

/* ==================== 布局常量 ==================== */
#define ROW_STATUSBAR_Y   0
#define ROW_DIVIDER_Y     26
#define ROW_CLOCK_Y       52
#define ROW_DATE_Y        104
#define ROW_WEEK_Y        128
#define ROW_CARDS_Y       170
#define CARD_W            68
#define CARD_H            66
#define CARD_GAP          10

/* ==================== 控件句柄 ==================== */
static struct {
    lv_obj_t *lb_bar_time;   /* 状态栏小时间        */
    lv_obj_t *bat_fill;      /* 状态栏电池条        */
    lv_obj_t *lb_bat_pct;    /* 状态栏电量百分比    */
    lv_obj_t *lb_clock;      /* 大号时间            */
    lv_obj_t *lb_date;       /* 日期                */
    lv_obj_t *lb_week;       /* 星期                */
    lv_obj_t *lb_steps;      /* 卡片：步数数值      */
    lv_obj_t *lb_bpm;        /* 卡片：心率数值      */
    lv_obj_t *lb_batt;       /* 卡片：电量数值      */
} ui;

/* ==================== 工具 ==================== */
static const char *weekday_text(int iso_weekday)
{
    switch (iso_weekday) {
    case 1: return W_STR_WEEK_MON;
    case 2: return W_STR_WEEK_TUE;
    case 3: return W_STR_WEEK_WED;
    case 4: return W_STR_WEEK_THU;
    case 5: return W_STR_WEEK_FRI;
    case 6: return W_STR_WEEK_SAT;
    case 7: return W_STR_WEEK_SUN;
    default: return W_STR_WEEK_UNKNOWN;
    }
}

static void home_set_time_labels(const watch_time_t *t)
{
    if (t->valid) {
        lv_label_set_text_fmt(ui.lb_clock, "%02d:%02d", t->hour, t->minute);
        lv_obj_set_style_text_color(ui.lb_clock, W_COLOR_TEXT, 0);

        lv_label_set_text_fmt(ui.lb_bar_time, "%02d:%02d", t->hour, t->minute);

        /*
         * 日期用 W_STR_DATE_FMT（定义在 watch_strings.h）。
         * 不要在页面里直接写 "%d年%d月%d日" —— 那样"年、月"不会进字库，
         * 屏幕上会变成两个豆腐块（踩过一次，编译运行都不报错，很隐蔽）。
         */
        lv_label_set_text_fmt(ui.lb_date, W_STR_DATE_FMT,
                              t->year, t->month, t->day);
        lv_label_set_text(ui.lb_week, weekday_text(t->weekday));
    } else {
        /* RTC 掉过电还没校时：明确告诉用户"没数据"，而不是显示假时间 */
        lv_label_set_text(ui.lb_clock, "--:--");
        lv_obj_set_style_text_color(ui.lb_clock, W_COLOR_TEXT_MUTE, 0);
        lv_label_set_text(ui.lb_bar_time, "--:--");
        w_label_set_text_auto(ui.lb_date, W_STR_TIME_UNSET);
        lv_label_set_text(ui.lb_week, W_STR_WEEK_UNKNOWN);
    }
}

/* ==================== 建界面 ==================== */
static lv_obj_t *home_create(lv_obj_t *parent)
{
    lv_obj_t *scr = w_page_create(parent);

    /* ---------- 状态栏 ---------- */
    ui.lb_bar_time = w_label_create(scr, "--:--", W_FONT_SMALL,
                                    W_COLOR_TEXT_DIM);
    lv_obj_align(ui.lb_bar_time, LV_ALIGN_TOP_LEFT, 0, ROW_STATUSBAR_Y);

    /* 电量：图标 + 百分比，右对齐 */
    lv_obj_t *bat_box = lv_obj_create(scr);
    lv_obj_set_size(bat_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(bat_box, LV_ALIGN_TOP_RIGHT, 0, ROW_STATUSBAR_Y + 2);
    lv_obj_clear_flag(bat_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bat_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(bat_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bat_box, 0, 0);
    lv_obj_set_style_pad_all(bat_box, 0, 0);
    lv_obj_set_flex_flow(bat_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bat_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bat_box, 4, 0);

    w_battery_icon_create(bat_box, 26, 13, &ui.bat_fill);
    ui.lb_bat_pct = w_label_create_auto(bat_box, "--%", W_FONT_SMALL, 14,
                                   W_COLOR_TEXT_DIM);

    lv_obj_t *div = w_divider_create(scr, W_SCREEN_W - 2 * W_PAD_PAGE);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, ROW_DIVIDER_Y);

    /* ---------- 大号时间 ---------- */
    ui.lb_clock = w_label_create(scr, "--:--", W_FONT_CLOCK, W_COLOR_AUTO);
    lv_obj_align(ui.lb_clock, LV_ALIGN_TOP_MID, 0, ROW_CLOCK_Y);

    /* ---------- 日期 / 星期（中文，用生成的字库）---------- */
    ui.lb_date = w_label_create(scr, "----年--月--日", watch_font_cn(16),
                                W_COLOR_TEXT_DIM);
    lv_obj_align(ui.lb_date, LV_ALIGN_TOP_MID, 0, ROW_DATE_Y);

    ui.lb_week = w_label_create(scr, W_STR_WEEK_UNKNOWN, watch_font_cn(16),
                                W_COLOR_ACCENT);
    lv_obj_align(ui.lb_week, LV_ALIGN_TOP_MID, 0, ROW_WEEK_Y);

    /* ---------- 三张指标卡 ---------- */
    /* 可用宽度 = 240 - 2*12 = 216；三张卡 + 两个间隙 = 3*68 + 2*10 = 224 > 216
     * → 卡宽收到 64，间隙 8：3*64 + 2*8 = 208，居中后左右各留 16。 */
    const lv_coord_t card_w = 64;
    const lv_coord_t card_h = CARD_H;
    const lv_coord_t gap    = 8;
    const lv_coord_t total  = card_w * 3 + gap * 2;
    const lv_coord_t start  = (W_SCREEN_W - 2 * W_PAD_PAGE - total) / 2;

    w_metric_card_create(scr, start + 0 * (card_w + gap), ROW_CARDS_Y,
                         card_w, card_h, W_STR_STEPS, W_STR_NO_DATA, &ui.lb_steps);
    w_metric_card_create(scr, start + 1 * (card_w + gap), ROW_CARDS_Y,
                         card_w, card_h, W_STR_BPM, W_STR_NO_DATA, &ui.lb_bpm);
    w_metric_card_create(scr, start + 2 * (card_w + gap), ROW_CARDS_Y,
                         card_w, card_h, W_STR_BATTERY, W_STR_NO_DATA, &ui.lb_batt);

    /* ---------- 底部提示 ----------
     * ⚠️ 分隔符用 ASCII 的 "|"，不要用 "·"(U+00B7)：
     *    字库只包含 ASCII(0x20-0x7F) + 源码里出现的 CJK 字符，
     *    U+00B7 两边都不沾，会渲染成豆腐块（又踩了一次同类坑）。
     *    教训：**页面里出现的每一个非汉字字符，都要确认在字库里**。 */
    lv_obj_t *hint = w_label_create(scr, W_STR_HINT_SWIPE " | " W_STR_HINT_UP,
                                    watch_font_cn(14),
                                    W_COLOR_TEXT_MUTE);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    return scr;
}

/* ==================== 刷新 ==================== */
static void home_refresh(lv_obj_t *root, const watch_state_t *state, uint32_t changed)
{
    (void)root;
    if (!state) return;

    /* ---------- 时间 ---------- */
    if (changed & WATCH_CHANGED_TIME) {
        home_set_time_labels(&state->time);
    }

    /* ---------- 电量（P1 功能 4）---------- */
    if (changed & WATCH_CHANGED_BATTERY) {
        const int pct = state->battery.percent;

        w_battery_set(ui.bat_fill, pct, state->battery.charging);

        if (pct >= 0) {
            w_label_set_text_fmt_auto(ui.lb_bat_pct, "%d%%", pct);
            lv_obj_set_style_text_color(ui.lb_bat_pct, W_COLOR_TEXT_DIM, 0);
            w_label_set_text_fmt_auto(ui.lb_batt, "%d%%", pct);
            w_set_placeholder(ui.lb_batt, false);
        } else {
            w_label_set_text_auto(ui.lb_bat_pct, "--%");
            w_label_set_text_auto(ui.lb_batt, W_STR_NO_DATA);
            w_set_placeholder(ui.lb_batt, true);
        }
    }

    /* ---------- 步数（P1 功能 6，未实现）---------- */
    if ((changed & WATCH_CHANGED_STEPS) && ui.lb_steps) {
        if (state->steps.available) {
            w_label_set_text_fmt_auto(ui.lb_steps, "%d", state->steps.steps);
            w_set_placeholder(ui.lb_steps, false);
        } else {
            w_label_set_text_auto(ui.lb_steps, W_STR_NO_DATA);
            w_set_placeholder(ui.lb_steps, true);
        }
    }

    /* ---------- 心率（P4，未实现）----------
     * 故意不每帧重画：它永远是 "--"，重画纯浪费 CPU。 */
}

/* ==================== 页面定义 ==================== */
const watch_page_t watch_page_home = {
    .id      = "home",
    .title   = W_STR_HOME,
    .create  = home_create,
    .refresh = home_refresh,
    .destroy = NULL,
};
