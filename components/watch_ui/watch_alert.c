/**
 * @file watch_alert.c
 * @brief 全局到点提醒层实现
 *
 * ## 为什么这里的中文是**字面量**而不是 watch_strings.h 的宏
 *
 * 本文件属于 `watch_ui`，而 `watch_strings.h` 在 `watch_ui_pages` 里 ——
 * 依赖方向是反的（见 watch_wheel.c 的同类说明，以及 watch_modal.c 的"返回"、
 * watch_overlay.c 的"亮度/下滑关闭"）。
 * 字库生成脚本扫所有源码的字符串字面量，所以照样进字库。
 */
#include "watch_alert.h"

#include <stdio.h>
#include "esp_log.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_clock.h"

static const char *TAG = "ALERT";

/* ==================== 内部状态 ==================== */
static lv_obj_t *s_root    = NULL;   /* 全屏容器 */
static lv_obj_t *s_lb_title = NULL;  /* "倒计时结束" / "闹钟" */
static lv_obj_t *s_lb_time  = NULL;  /* 大号时间 */
static lv_obj_t *s_lb_hint  = NULL;  /* "响铃中" / "铃声已停" */
static bool      s_visible  = false;

/* ==================== 按钮 ==================== */
static void stop_cb(lv_event_t *e)
{
    (void)e;
    /* 停声 + 清状态；下一轮 poll 会把本层隐藏掉 */
    watch_clock_alert_stop();
    ESP_LOGI(TAG, "用户点了停止");
}

/* ==================== 初始化 ==================== */
void watch_alert_init(lv_obj_t *host)
{
    if (s_root || !host) {
        return;
    }

    s_root = lv_obj_create(host);
    lv_obj_set_size(s_root, W_SCREEN_W, W_SCREEN_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, W_PAD_PAGE, 0);
    lv_obj_set_style_bg_color(s_root, W_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);

    /* 自己不吃滚动，但**要可点击**：吃掉底下轮播/模态的点击，防误触 */
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_root, LV_DIR_NONE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);

    /* 竖向排布，居中 */
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_root, 8, 0);

    /* 标题（中文，可能是"倒计时结束"或"闹钟"） */
    s_lb_title = w_label_create_auto(s_root, "闹钟", W_FONT_MEDIUM, 20,
                                     W_COLOR_ACCENT);

    /* 大号时间 */
    s_lb_time = w_label_create(s_root, "--:--", W_FONT_CLOCK, W_COLOR_TEXT);

    /* 副提示：响铃中 / 铃声已停 */
    s_lb_hint = w_label_create_auto(s_root, "", W_FONT_SMALL, 14, W_COLOR_TEXT_DIM);

    /* 停止按钮 */
    w_btn_create(s_root, "停止", 160, 52,
                 W_COLOR_ACCENT, W_COLOR_BG, watch_font_cn(20), stop_cb);

    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;

    ESP_LOGI(TAG, "提醒层就绪");
}

/* ==================== 显隐 ==================== */
static void show(void)
{
    if (!s_root || s_visible) {
        return;
    }
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
}

static void hide(void)
{
    if (!s_root || !s_visible) {
        return;
    }
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

bool watch_alert_visible(void)
{
    return s_visible;
}

/* ==================== 周期更新 ==================== */
void watch_alert_poll(void)
{
    if (!s_root) {
        return;
    }

    if (!watch_clock_alerting()) {
        hide();
        return;
    }

    const w_clock_alert_t kind = watch_clock_alert_kind();
    const bool is_alarm = (kind == W_CLOCK_ALERT_ALARM);

    /* ---- 文案 ---- */
    w_label_set_text_auto(s_lb_title, is_alarm ? "闹钟" : "倒计时结束");

    char buf[16];
    if (is_alarm) {
        /* 闹钟：显示当前墙上时间，一眼知道是几点的那次闹钟 */
        int hh = 0, mm = 0;
        if (watch_clock_last_time(&hh, &mm)) {
            snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
        } else {
            snprintf(buf, sizeof(buf), "--:--");
        }
    } else {
        /* 倒计时：显示刚才设的时长 */
        watch_clock_fmt_mmss(watch_clock_cd_sec(), buf, sizeof(buf));
    }
    w_label_set_text_auto(s_lb_time, buf);

    /* 副提示：倒计时响完会自动停声但提示层留着，说清楚免得用户以为坏了 */
    if (is_alarm || !watch_clock_alert_ring_done()) {
        w_label_set_text_auto(s_lb_hint, "响铃中");
    } else {
        w_label_set_text_auto(s_lb_hint, "铃声已停，点下面关闭");
    }

    /* ---- 显示并置顶 ---- */
    show();

    /*
     * 每次 poll 都重新置顶：
     * 提醒期间用户如果又开了别的模态子页，模态会盖住提醒层 ——
     * 而闹钟必须盖住一切。move_foreground 在已经是最前时是空操作，开销可忽略。
     */
    lv_obj_move_foreground(s_root);
}
