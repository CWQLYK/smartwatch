/**
 * @file watch_page_health.c
 * @brief 健康页 —— **P4 占位页**（心率 / 血氧 / 脉搏波）
 *
 * 为什么现在就把它做出来：
 *   1. 多页面切换（P1 功能 5）需要不止一页才看得出效果；
 *   2. 页面 + 数据源 + 刷新回调这套结构**先把坑趟平**，
 *      等 MAX30102 到了，只需要把 refresh 里的 "--" 换成真实数值；
 *   3. 界面上明确写清楚"需要外接 MAX30102"，避免误以为是软件 bug。
 *
 * 硬件现实（见文档 板载资源与功能能力分析.md 第四节）：
 *   板上**没有** PPG 传感器，心率/血氧必须外接 MAX30102 模块。
 *   接线：模块 SCL/SDA 接扩展口 GPIO10/11（I2C 地址要避开 0x15/0x6B/0x51），
 *         INT 可以接 GPIO2/3/17/18 里的任意一个。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_strings.h"

#define ROW_TITLE_Y   0
#define LIST_W        (W_SCREEN_W - 2 * W_PAD_PAGE)

static lv_obj_t *health_create(lv_obj_t *parent)
{
    lv_obj_t *scr = w_page_create(parent);

    lv_obj_t *title = w_label_create(scr, W_STR_HEALTH, watch_font_cn(20), W_COLOR_AUTO);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, ROW_TITLE_Y);

    /* ---------- 脉搏波占位区 ----------
     * 以后这里放 lv_chart_create()，用 MAX30102 的红光/红外原始值画波形。
     * 现在先画一块空卡片 + 说明，形状和以后一致，替换时不用改布局。 */
    lv_obj_t *wave = w_card_create(scr, 0, 28, LIST_W, 120);

    lv_obj_t *wt = w_label_create(wave, W_STR_PPG, watch_font_cn(14),
                                  W_COLOR_TEXT_DIM);
    lv_obj_align(wt, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = w_label_create(wave, W_STR_PPG_TODO, watch_font_cn(14),
                                    W_COLOR_TEXT_MUTE);
    lv_obj_center(hint);

    /* ---------- 心率和血氧：两张卡 ---------- */
    const lv_coord_t cw = (LIST_W - W_GAP) / 2;

    lv_obj_t *c1 = w_card_create(scr, 0, 156, cw, 84);
    lv_obj_t *t1 = w_label_create(c1, W_STR_BPM, watch_font_cn(14),
                                 W_COLOR_TEXT_DIM);
    lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *v1 = w_label_create_auto(c1, W_STR_NO_DATA, W_FONT_HUGE, 20,
                                 W_COLOR_HEART);
    lv_obj_align(v1, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *c2 = w_card_create(scr, cw + W_GAP, 156, cw, 84);
    lv_obj_t *t2 = w_label_create(c2, W_STR_SPO2, watch_font_cn(14),
                                 W_COLOR_TEXT_DIM);
    lv_obj_align(t2, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *v2 = w_label_create_auto(c2, W_STR_NO_DATA, W_FONT_HUGE, 20,
                                 W_COLOR_ACCENT);
    lv_obj_align(v2, LV_ALIGN_CENTER, 0, 10);

    return scr;
}

/**
 * @brief 刷新
 *
 * @note 现在**刻意什么都不做**（数据恒为 "--"）。
 *       P4 接入 MAX30102 后在这里读 provider 的心率/血氧字段即可。
 */
static void health_refresh(lv_obj_t *root, const watch_state_t *state, uint32_t changed)
{
    (void)root; (void)state; (void)changed;
}

const watch_page_t watch_page_health = {
    .id      = "health",
    .title   = W_STR_HEALTH,
    .create  = health_create,
    .refresh = health_refresh,
    .destroy = NULL,
};
