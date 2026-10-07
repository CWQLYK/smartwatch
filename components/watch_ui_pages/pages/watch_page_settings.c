/**
 * @file watch_page_settings.c
 * @brief 设置页 —— P1 功能 8（设置页）+ 功能 9（亮度调节）
 *
 * 布局（240×280，页面内边距 12 → 可用宽 216）：
 *
 *     ┌──────────────────────────┐
 *     │ 设置                      │
 *     │ ┌──────────────────────┐ │
 *     │ │ 亮度           70%   │ │  ← 滑块，直接调背光
 *     │ │ ●──────────────      │ │
 *     │ └──────────────────────┘ │
 *     │ 自动息屏 / 抬腕亮屏       │  ← 可点行，点一下切到下一个值
 *     │ 步数目标 / 身高 / 体重    │
 *     │ 时钟状态 / 版本           │  ← 只读信息
 *     └──────────────────────────┘
 *
 * ## 交互约定（小屏上最省事的做法）
 *
 * 可点行统一用**"点一下切到下一个值"**，不做二级页面也不做加减按钮：
 *   - 自动息屏：15 → 30 → 60 → 120 → 常亮 → 15 …
 *   - 抬腕亮屏：开 ⇄ 关
 *   - 步数目标 / 身高 / 体重：按档位循环
 *
 * 这样每项只要一个 `lv_obj_add_event_cb`，用户也不用学怎么操作。
 *
 * ## 值为什么直接读 settings
 *
 * 点完立刻回写显示，所见即所得。而**别的入口**改了设置
 * （控制中心的亮度滑块、串口 brightness 命令）时，
 * 本页在 refresh() 里再同步一次显示即可。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_modal.h"
#include "watch_strings.h"


#ifndef WATCH_FIRMWARE_VERSION
#define WATCH_FIRMWARE_VERSION "0.4.0"
#endif

#define ROW_TITLE_Y   0
#define ROW_LIST_Y    26
#define ROW_H         40
#define ROW_GAP       6
#define LIST_W        (W_SCREEN_W - 2 * W_PAD_PAGE)
#define BRIGHT_CARD_H (ROW_H + 16)

/* 自动息屏可选值（秒），0 = 常亮 */
static const int k_timeout_choices[] = { 15, 30, 60, 120, 0 };
#define TIMEOUT_CHOICES  ((int)(sizeof(k_timeout_choices) / sizeof(k_timeout_choices[0])))

/* 步数目标档位 */
static const int k_goal_choices[] = { 4000, 6000, 8000, 10000, 15000, 20000 };
#define GOAL_CHOICES  ((int)(sizeof(k_goal_choices) / sizeof(k_goal_choices[0])))

static struct {
    lv_obj_t *slider;       /* 亮度滑块 */
    lv_obj_t *lb_bright;    /* 亮度数值 */
    lv_obj_t *lb_timeout;   /* 自动息屏值 */
    lv_obj_t *lb_wrist;     /* 抬腕亮屏值 */
    lv_obj_t *lb_goal;      /* 步数目标值 */
    lv_obj_t *lb_height;    /* 身高值 */
    lv_obj_t *lb_weight;    /* 体重值 */
    lv_obj_t *lb_rtc;       /* RTC 状态 */
} ui;

/* ==================== 亮度 ==================== */
static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    const int val = (int)lv_slider_get_value(sl);

    watch_ui_brightness_set(val);      /* 内部会写 settings + 调背光 */
    if (ui.lb_bright) {
        w_label_set_text_fmt_auto(ui.lb_bright, "%d%%", val);
    }
}

/* ==================== 工具：在当前档位上取下一个值 ==================== */
static int cycle_int(int cur, const int *choices, int n)
{
    for (int i = 0; i < n; i++) {
        if (choices[i] == cur) {
            return choices[(i + 1) % n];
        }
    }
    return choices[0];                  /* 当前值不在档位里 → 回到第一档 */
}

/* ==================== 各设置行的点击回调 ==================== */
static void timeout_cb(lv_event_t *e)
{
    (void)e;
    const int cur  = (int)watch_ui_setting_get(W_SETTING_AUTO_SCREEN, 30);
    const int next = cycle_int(cur, k_timeout_choices, TIMEOUT_CHOICES);

    watch_ui_setting_set(W_SETTING_AUTO_SCREEN, next);

    if (ui.lb_timeout) {
        if (next == 0) {
            w_label_set_text_auto(ui.lb_timeout, W_STR_NEVER);
        } else {
            w_label_set_text_fmt_auto(ui.lb_timeout, "%d" W_STR_SECONDS_SUF, next);
        }
    }
}

static void wrist_cb(lv_event_t *e)
{
    (void)e;
    const int next = watch_ui_setting_get(W_SETTING_WRIST_WAKE, 1) ? 0 : 1;
    watch_ui_setting_set(W_SETTING_WRIST_WAKE, next);
    if (ui.lb_wrist) {
        w_label_set_text_auto(ui.lb_wrist, next ? W_STR_ON : W_STR_OFF);
    }
}

static void goal_cb(lv_event_t *e)
{
    (void)e;
    const int cur  = (int)watch_ui_setting_get(W_SETTING_STEP_GOAL, 8000);
    const int next = cycle_int(cur, k_goal_choices, GOAL_CHOICES);
    watch_ui_setting_set(W_SETTING_STEP_GOAL, next);
    if (ui.lb_goal) {
        w_label_set_text_fmt_auto(ui.lb_goal, "%d", next);
    }
}

static void height_cb(lv_event_t *e)
{
    (void)e;
    int next = (int)watch_ui_setting_get(W_SETTING_HEIGHT, 170) + 5;
    if (next > 200) {
        next = 150;                     /* 循环回起点 */
    }
    watch_ui_setting_set(W_SETTING_HEIGHT, next);
    if (ui.lb_height) {
        w_label_set_text_fmt_auto(ui.lb_height, "%d", next);
    }
}

static void weight_cb(lv_event_t *e)
{
    (void)e;
    int next = (int)watch_ui_setting_get(W_SETTING_WEIGHT, 65) + 5;
    if (next > 120) {
        next = 40;
    }
    watch_ui_setting_set(W_SETTING_WEIGHT, next);
    if (ui.lb_weight) {
        w_label_set_text_fmt_auto(ui.lb_weight, "%d", next);
    }
}

/* ==================== 两个子页面入口 ==================== */
static void wifi_entry_cb(lv_event_t *e)
{
    (void)e;
    watch_wifi_open();
}

static void dev_entry_cb(lv_event_t *e)
{
    (void)e;
    watch_developer_open_menu();
}

/* ==================== 造一行"标题 + 右侧值" ==================== */
static lv_obj_t *make_row(lv_obj_t *parent, lv_coord_t y,
                          const char *title, const char *value,
                          lv_event_cb_t cb, lv_obj_t **out_value)
{
    /* 给了回调 = 可点行；没给 = 只读信息行 */
    lv_obj_t *card = cb ? w_card_create_clickable(parent, 0, y, LIST_W, ROW_H)
                        : w_card_create(parent, 0, y, LIST_W, ROW_H);

    lv_obj_t *t = w_label_create(card, title, watch_font_cn(16), W_COLOR_AUTO);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);

    /*
     * ⚠️ 值这一列会显示中文（"开"/"关"/"常亮"/"掉电丢失，待校时"），
     *    所以必须用**自动字体**：内容是数字走 Montserrat（好看），
     *    出现中文自动切中文字库。写死 W_FONT_SMALL 的话就是一排豆腐块
     *    （这个坑本项目踩过三次）。
     */
    lv_obj_t *v = w_label_create_auto(card, value, W_FONT_SMALL, 14,
                                      W_COLOR_TEXT_DIM);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);

    if (cb) {
        lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    }
    if (out_value) {
        *out_value = v;
    }
    return card;
}

/* ==================== 建界面 ==================== */
static lv_obj_t *settings_create(lv_obj_t *parent)
{
    lv_obj_t *scr = w_page_create(parent);

    lv_obj_t *title = w_label_create(scr, W_STR_SETTINGS, watch_font_cn(20),
                                     W_COLOR_AUTO);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, ROW_TITLE_Y);

    /* ---------- 亮度（滑块）---------- */
    const int bright = watch_ui_brightness_get();
    lv_obj_t *bcard = w_card_create(scr, 0, ROW_LIST_Y, LIST_W, BRIGHT_CARD_H);

    lv_obj_t *bl = w_label_create(bcard, W_STR_BRIGHTNESS, watch_font_cn(16),
                                  W_COLOR_AUTO);
    lv_obj_align(bl, LV_ALIGN_TOP_LEFT, 0, 0);

    ui.lb_bright = w_label_create_auto(bcard, "---%", W_FONT_SMALL, 14,
                                       W_COLOR_ACCENT);
    lv_obj_align(ui.lb_bright, LV_ALIGN_TOP_RIGHT, 0, 0);
    w_label_set_text_fmt_auto(ui.lb_bright, "%d%%", bright);

    ui.slider = w_slider_create(bcard, LIST_W - 2 * W_PAD_CARD, 5, 100, bright);
    lv_obj_align(ui.slider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(ui.slider, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---------- 可点设置行 ---------- */
    lv_coord_t y = ROW_LIST_Y + BRIGHT_CARD_H + ROW_GAP;

    const int to = (int)watch_ui_setting_get(W_SETTING_AUTO_SCREEN, 30);
    make_row(scr, y, W_STR_AUTO_SCREEN, W_STR_NEVER, timeout_cb, &ui.lb_timeout);
    if (to != 0) {
        w_label_set_text_fmt_auto(ui.lb_timeout, "%d" W_STR_SECONDS_SUF, to);
    }
    y += ROW_H + ROW_GAP;

    make_row(scr, y, W_STR_WRIST_WAKE,
             watch_ui_setting_get(W_SETTING_WRIST_WAKE, 1) ? W_STR_ON : W_STR_OFF,
             wrist_cb, &ui.lb_wrist);
    y += ROW_H + ROW_GAP;

    make_row(scr, y, W_STR_STEP_GOAL, "0", goal_cb, &ui.lb_goal);
    w_label_set_text_fmt_auto(ui.lb_goal, "%d",
                              (int)watch_ui_setting_get(W_SETTING_STEP_GOAL, 8000));
    y += ROW_H + ROW_GAP;

    make_row(scr, y, W_STR_USER_HEIGHT, "0", height_cb, &ui.lb_height);
    w_label_set_text_fmt_auto(ui.lb_height, "%d",
                              (int)watch_ui_setting_get(W_SETTING_HEIGHT, 170));
    y += ROW_H + ROW_GAP;

    make_row(scr, y, W_STR_USER_WEIGHT, "0", weight_cb, &ui.lb_weight);
    w_label_set_text_fmt_auto(ui.lb_weight, "%d",
                              (int)watch_ui_setting_get(W_SETTING_WEIGHT, 65));
    y += ROW_H + ROW_GAP;

    /* ---------- 只读信息 ---------- */
    make_row(scr, y, W_STR_RTC_STATUS, W_STR_RTC_LOST, NULL, &ui.lb_rtc);
    y += ROW_H + ROW_GAP;

    /*
     * ---------- 子页面入口 ----------
     *
     * WiFi 放在设置页（不是开发人员页里）：普通用户也要能自己连网校时。
     * 开发人员选项放最底部 —— 里面的东西（IMU 录制等）普通使用不需要碰。
     */
    make_row(scr, y, W_STR_WIFI_TITLE, "", wifi_entry_cb, NULL);
    y += ROW_H + ROW_GAP;

    make_row(scr, y, W_STR_DEV_TITLE, "", dev_entry_cb, NULL);

    return scr;
}

/* ==================== 刷新 ==================== */
static void settings_refresh(lv_obj_t *root, const watch_state_t *state, uint32_t changed)
{
    (void)root;
    if (!state) return;

    /* RTC 状态：这一行让"时间不准"一眼可诊断 */
    if ((changed & WATCH_CHANGED_TIME) && ui.lb_rtc) {
        if (state->time.valid) {
            w_label_set_text_auto(ui.lb_rtc, W_STR_RTC_OK);
            lv_obj_set_style_text_color(ui.lb_rtc, W_COLOR_ACCENT, 0);
        } else {
            w_label_set_text_auto(ui.lb_rtc, W_STR_RTC_LOST);
            lv_obj_set_style_text_color(ui.lb_rtc, W_COLOR_WARN, 0);
        }
    }

    /*
     * 亮度可能被**别的入口**改了（控制中心的滑块、串口 brightness 命令），
     * 回到本页时同步一次显示，免得显示和实际不一致。
     */
    if (ui.slider && ui.lb_bright) {
        const int cur = watch_ui_brightness_get();
        if ((int)lv_slider_get_value(ui.slider) != cur) {
            lv_slider_set_value(ui.slider, cur, LV_ANIM_OFF);
            w_label_set_text_fmt_auto(ui.lb_bright, "%d%%", cur);
        }
    }
}

/* ==================== 页面定义 ==================== */
const watch_page_t watch_page_settings = {
    .id      = "settings",
    .title   = W_STR_SETTINGS,
    .create  = settings_create,
    .refresh = settings_refresh,
    .destroy = NULL,
};

/* ==================================================================
 * 备案：还有两个 P1 设置项只存了开关，没接实际动作
 *
 *  1. **自动息屏真正生效**（功能 13）：本页只把秒数存进 settings。
 *     实际"无触摸 N 秒后关背光"需要一个电源服务监听触摸并调
 *     bsp_backlight_set(0)。接口已就绪（W_SETTING_AUTO_SCREEN），
 *     差一个 components/power/ 组件。
 *
 *  2. **抬腕亮屏真正生效**（功能 12）：需要 IMU 角度变化判据。
 *     IMU 驱动（components/imu）已能读加速度，动作检测算法待写。
 *
 * 这两项做完后本页一行都不用改 —— 只是把存下来的开关接到实际动作上。
 * ================================================================== */
