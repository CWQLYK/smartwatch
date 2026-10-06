/**
 * @file watch_overlay.c
 * @brief 控制中心（上滑面板）实现
 *
 * 实现方式：**独立的一个 screen**，而不是 layer_top 上的浮层。
 *
 * 为什么不放 layer_top：
 *   1. `lv_layer_top()` 一旦设为可点击就会**吞掉整屏触摸**，页面全点不动。
 *   2. 浮层需要自己处理"点击遮罩关闭""动画位移"，代码更多。
 *   用独立 screen + `lv_scr_load_anim()` 反而更简单：
 *   LVGL 会替我们做好滑入滑出动画，页面 screen 也原封不动留着。
 *
 * 面板上目前是：时间 + 电量（P1 功能 3、4）+ 亮度滑块（P1 功能 9）。
 */
#include "watch_overlay.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "ui_port.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_gesture.h"

static const char *TAG = "OVERLAY";

/* ==================== 内部状态 ==================== */
static const watch_data_provider_t *s_provider = NULL;

static lv_obj_t *s_scr       = NULL;   /* 面板 screen               */
static lv_obj_t *s_lb_time   = NULL;   /* 面板上的时间              */
static lv_obj_t *s_lb_batt   = NULL;   /* 面板上的电量百分比        */
static lv_obj_t *s_bat_fill  = NULL;   /* 面板上的电池条            */
static lv_obj_t *s_slider    = NULL;   /* 亮度滑块                  */

static bool      s_created   = false;
static bool      s_visible   = false;

/* ==================== 亮度滑块 ==================== */
static void brightness_slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    const int val = (int)lv_slider_get_value(sl);

    const int err = watch_ui_brightness_set(val);
    if (err != 0) {
        ESP_LOGW(TAG, "设置亮度 %d%% 失败 (err=%d)", val, err);
    } else {
        ESP_LOGD(TAG, "亮度 -> %d%%", val);
    }
}

/* ==================== 建面板 ==================== */
esp_err_t watch_overlay_create(const watch_data_provider_t *provider)
{
    ESP_RETURN_ON_FALSE(provider, ESP_ERR_INVALID_ARG, TAG, "数据源为空");

    if (s_created) {
        return ESP_OK;
    }
    s_provider = provider;

    /*
     * 控制中心是**独立 screen**（不是轮播里的页面）——
     * 它是模态覆盖层，用 lv_scr_load_anim 从下往上滑入，不需要跟手。
     * 所以这里直接造 screen（v0.3 起页面用的是 w_page_create(parent)，
     * 那个只适用于轮播里的页面）。
     */
    s_scr = lv_obj_create(NULL);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, W_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_radius(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, W_PAD_PAGE, 0);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);

    /* ---- 顶部：时间 + 电量 ---- */
    s_lb_time = w_label_create(s_scr, "--:--", W_FONT_HUGE, W_COLOR_TEXT);
    lv_obj_align(s_lb_time, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t *bat_box = lv_obj_create(s_scr);
    lv_obj_set_size(bat_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(bat_box, LV_ALIGN_TOP_RIGHT, 0, 10);
    lv_obj_clear_flag(bat_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bat_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(bat_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bat_box, 0, 0);
    lv_obj_set_style_pad_all(bat_box, 0, 0);
    lv_obj_set_flex_flow(bat_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bat_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bat_box, 4, 0);

    w_battery_icon_create(bat_box, 26, 13, &s_bat_fill);
    s_lb_batt = w_label_create(bat_box, "--%", W_FONT_SMALL, W_COLOR_TEXT_DIM);

    lv_obj_t *div = w_divider_create(s_scr, W_SCREEN_W - 2 * W_PAD_PAGE);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 52);

    /* ---- 亮度行 ---- */
    lv_obj_t *br_title = w_label_create(s_scr, "亮度", watch_font_cn(16),
                                        W_COLOR_TEXT);
    lv_obj_align(br_title, LV_ALIGN_TOP_LEFT, 0, 70);

    s_slider = w_slider_create(s_scr, W_SCREEN_W - 2 * W_PAD_PAGE, 5, 100,
                               watch_ui_brightness_get());
    lv_obj_align(s_slider, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_add_event_cb(s_slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *br_hint = w_label_create(s_scr, "左右拖动调整屏幕亮度", watch_font_cn(14),
                                       W_COLOR_TEXT_MUTE);
    lv_obj_align(br_hint, LV_ALIGN_TOP_MID, 0, 122);

    /* ---- 底部提示 ---- */
    lv_obj_t *hint = w_label_create(s_scr, "下滑关闭", watch_font_cn(14),
                                    W_COLOR_TEXT_MUTE);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* 手势：面板自己也要能收手势（下滑关闭、左滑关闭） */
    watch_gesture_attach(s_scr);

    s_created = true;
    ESP_LOGI(TAG, "控制中心就绪（时间 / 电量 / 亮度滑块）");
    return ESP_OK;
}

/* ==================== 显示 / 隐藏 ==================== */
void watch_overlay_show(void)
{
    if (!s_created) {
        ESP_LOGW(TAG, "控制中心未建好，忽略上滑");
        return;
    }
    if (s_visible) {
        return;
    }

    /* 每次弹出都重新读一次亮度，避免和设置页改的值不一致 */
    if (s_slider) {
        lv_slider_set_value(s_slider, watch_ui_brightness_get(), LV_ANIM_OFF);
    }

    /* 立刻用最新数据填一遍，别等下一个整秒 */
    if (s_provider) {
        watch_state_t st;
        s_provider->get_state(&st);
        watch_overlay_refresh(&st, WATCH_CHANGED_ALL);
    }

    /* 从下往上滑入 */
    lv_scr_load_anim(s_scr, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 200, 0, false);
    s_visible = true;
    ESP_LOGD(TAG, "控制中心打开");
}

void watch_overlay_hide(void)
{
    if (!s_created || !s_visible) {
        return;
    }

    /* 回到页面栈顶：向页面管理器要当前页即可 */
    lv_obj_t *back = watch_ui_current_root();
    if (!back) {
        s_visible = false;
        return;
    }

    lv_scr_load_anim(back, LV_SCR_LOAD_ANIM_MOVE_TOP, 200, 0, false);
    s_visible = false;
    ESP_LOGD(TAG, "控制中心关闭");
}

bool watch_overlay_visible(void)
{
    return s_visible;
}

bool watch_overlay_ready(void)
{
    return s_created;
}

/* ==================== 刷新 ==================== */
void watch_overlay_refresh(const watch_state_t *state, uint32_t changed)
{
    if (!s_created || !state) {
        return;
    }
    if (!(changed & (WATCH_CHANGED_TIME | WATCH_CHANGED_BATTERY))) {
        return;
    }

    if ((changed & WATCH_CHANGED_TIME) && s_lb_time) {
        if (state->time.valid) {
            lv_label_set_text_fmt(s_lb_time, "%02d:%02d",
                                  state->time.hour, state->time.minute);
        } else {
            lv_label_set_text(s_lb_time, "--:--");
        }
    }

    if ((changed & WATCH_CHANGED_BATTERY) && s_lb_batt) {
        if (state->battery.percent >= 0) {
            lv_label_set_text_fmt(s_lb_batt, "%d%%", state->battery.percent);
        } else {
            lv_label_set_text(s_lb_batt, "--%");
        }
        w_battery_set(s_bat_fill, state->battery.percent, state->battery.charging);
    }
}
