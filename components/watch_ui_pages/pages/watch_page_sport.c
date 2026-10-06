/**
 * @file watch_page_sport.c
 * @brief 运动页 —— 计步 / 卡路里 / 距离（P1 功能 6、7）
 *
 * 当前状态：**界面和刷新逻辑已完整**，只等 IMU 驱动把数据填进来。
 *
 * 数据链路（已经打通，只差最下面一环）：
 *
 *     QMI8658C ──I2C──▶ imu 驱动 ──▶ 计步算法 ──▶ data_hub.steps
 *                                                      │
 *                                              watch_data_provider_t
 *                                                      │
 *                                              watch_state_t.steps
 *                                                      │
 *                                              本页 sport_refresh()
 *
 * 也就是说：等 components/imu/ 写好后，只要在 data_hub 里
 * 周期更新 `s_state.steps` 并发 DATA_CHANGED_STEPS，
 * **本文件一行都不用改**（available 会自动变 true）。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_strings.h"

#define ROW_TITLE_Y   0
#define LIST_W        (W_SCREEN_W - 2 * W_PAD_PAGE)

static struct {
    lv_obj_t *lb_steps;   /* 大号步数        */
    lv_obj_t *lb_kcal;    /* 卡路里数值      */
    lv_obj_t *lb_dist;    /* 距离数值        */
    lv_obj_t *hint;       /* "待开发"提示    */
} ui;

/* ==================== 建界面 ==================== */
static lv_obj_t *sport_create(lv_obj_t *parent)
{
    lv_obj_t *scr = w_page_create(parent);

    lv_obj_t *title = w_label_create(scr, W_STR_SPORT, watch_font_cn(20), W_COLOR_AUTO);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, ROW_TITLE_Y);

    /* ---------- 大号步数 ---------- */
    lv_obj_t *card = w_card_create(scr, 0, 28, LIST_W, 116);

    lv_obj_t *st = w_label_create(card, W_STR_STEPS, watch_font_cn(14),
                                  W_COLOR_TEXT_DIM);
    lv_obj_align(st, LV_ALIGN_TOP_LEFT, 0, 0);

    ui.lb_steps = w_label_create_auto(card, W_STR_NO_DATA, W_FONT_CLOCK, 20,
                                 W_COLOR_ACCENT);
    lv_obj_align(ui.lb_steps, LV_ALIGN_CENTER, 0, 10);

    /* ---------- 卡路里 / 距离 ---------- */
    const lv_coord_t cw = (LIST_W - W_GAP) / 2;

    lv_obj_t *c1 = w_card_create(scr, 0, 156, cw, 84);
    lv_obj_t *t1 = w_label_create(c1, W_STR_CALORIE, watch_font_cn(14),
                                 W_COLOR_TEXT_DIM);
    lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 0);
    ui.lb_kcal = w_label_create_auto(c1, W_STR_NO_DATA, W_FONT_HUGE, 20,
                               W_COLOR_TEXT);
    lv_obj_align(ui.lb_kcal, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *c2 = w_card_create(scr, cw + W_GAP, 156, cw, 84);
    lv_obj_t *t2 = w_label_create(c2, W_STR_DISTANCE, watch_font_cn(14),
                                 W_COLOR_TEXT_DIM);
    lv_obj_align(t2, LV_ALIGN_TOP_MID, 0, 0);
    ui.lb_dist = w_label_create_auto(c2, W_STR_NO_DATA, W_FONT_HUGE, 20,
                               W_COLOR_TEXT);
    lv_obj_align(ui.lb_dist, LV_ALIGN_CENTER, 0, 10);

    /* ---------- 状态提示 ---------- */
    ui.hint = w_label_create(scr, W_STR_IMU_TODO, watch_font_cn(14),
                             W_COLOR_TEXT_MUTE);
    lv_obj_align(ui.hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    return scr;
}

/* ==================== 刷新 ==================== */
static void sport_refresh(lv_obj_t *root, const watch_state_t *state, uint32_t changed)
{
    (void)root;
    if (!state || !(changed & WATCH_CHANGED_STEPS)) {
        return;
    }

    const watch_steps_t *s = &state->steps;

    if (!s->available) {
        /* IMU 还没接：显示占位，并保留"待开发"提示 */
        w_label_set_text_auto(ui.lb_steps, W_STR_NO_DATA);
        w_set_placeholder(ui.lb_steps, true);
        w_label_set_text_auto(ui.lb_kcal, W_STR_NO_DATA);
        w_label_set_text_auto(ui.lb_dist, W_STR_NO_DATA);
        lv_obj_clear_flag(ui.hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    w_label_set_text_fmt_auto(ui.lb_steps, "%d", s->steps);
    w_set_placeholder(ui.lb_steps, false);

    /* 卡路里：整数就够了（一位小数在 40px 字体上放不下） */
    w_label_set_text_fmt_auto(ui.lb_kcal, "%d", s->kcal);

    /* 距离：小于 1km 显示米，方便室内看 */
    if (s->meters < 1000) {
        w_label_set_text_fmt_auto(ui.lb_dist, "%dm", s->meters);
    } else {
        w_label_set_text_fmt_auto(ui.lb_dist, "%d.%dkm",
                              s->meters / 1000, (s->meters % 1000) / 100);
    }

    /* 有真实数据了就把"待开发"提示藏起来 */
    lv_obj_add_flag(ui.hint, LV_OBJ_FLAG_HIDDEN);
}

const watch_page_t watch_page_sport = {
    .id      = "sport",
    .title   = W_STR_SPORT,
    .create  = sport_create,
    .refresh = sport_refresh,
    .destroy = NULL,
};
