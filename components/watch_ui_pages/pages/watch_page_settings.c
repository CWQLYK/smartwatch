/**
 * @file watch_page_settings.c
 * @brief 设置子页 —— 从"轮播页"改成"模态子页"（v0.5.0）
 *
 * ## ★ v0.5.0 的结构变化
 *
 * | | v0.4.0 | v0.5.0 |
 * |---|---|---|
 * | 位置 | 轮播第 2 页 | 「功能列表」里的一个子页 |
 * | 布局 | **绝对定位**（自己算 y） | **flex column**（模态 body 自带的） |
 * | 行数 | 9 行 / 450px（要滚 170px） | 3 行 / 148px（一屏放得下） |
 *
 * 删掉的行（按用户要求）：
 *   - 身高 / 体重  -> 步长改为固定 50cm/90cm，设置项本身也删了
 *   - 抬腕亮屏     -> 功能取消
 *   - 步数目标     -> 只去掉了界面（后端 SETTINGS_STEP_GOAL 保留）
 *   - WiFi / 开发人员选项 -> 提升成「功能列表」的一级入口
 *
 * ## 布局改成 flex 之后要注意的事
 *
 * 模态的 body 已经是 `LV_FLEX_FLOW_COLUMN`（见 watch_modal.c 的 build_page），
 * 所以本文件里**所有卡片都不设 x/y** —— 用 `W_AUTO_POS`。
 * 还在用"自己累加 y"的写法会全部叠在 (0,0)。
 *
 * 宽度统一用 `LV_PCT(100)`（= 216），高度给固定值。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_modal.h"
#include "watch_strings.h"


#ifndef WATCH_FIRMWARE_VERSION
#define WATCH_FIRMWARE_VERSION "0.6.0"
#endif

#define ROW_H         40
#define BRIGHT_H      56

/* 自动息屏可选值（秒），0 = 常亮 */
static const int k_timeout_choices[] = { 15, 30, 60, 120, 0 };
#define TIMEOUT_CHOICES  ((int)(sizeof(k_timeout_choices) / sizeof(k_timeout_choices[0])))

static struct {
    lv_obj_t *slider;       /* 亮度滑块 */
    lv_obj_t *lb_bright;    /* 亮度数值 */
    lv_obj_t *lb_timeout;   /* 自动息屏值 */
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
    return choices[0];                  /* 当前值不在档位里 -> 回到第一档 */
}

/* ==================== 可点行回调 ==================== */
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

/* ==================== 造一行"标题 + 右侧值" ==================== */
static lv_obj_t *make_row(lv_obj_t *parent, const char *title, const char *value,
                          lv_event_cb_t cb, lv_obj_t **out_value)
{
    /* 给了回调 = 可点行；没给 = 只读信息行 */
    lv_obj_t *card = cb ? w_card_create_clickable(parent, W_AUTO_POS, W_AUTO_POS,
                                                  LV_PCT(100), ROW_H)
                        : w_card_create(parent, W_AUTO_POS, W_AUTO_POS,
                                        LV_PCT(100), ROW_H);

    lv_obj_t *t = w_label_create(card, title, watch_font_cn(16), W_COLOR_AUTO);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);

    /*
     * ⚠️ 值这一列会显示中文（"常亮"/"掉电丢失，待校时"），
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

/* ==================== 建界面（模态构建回调） ==================== */
static void settings_build(lv_obj_t *parent, void *user)
{
    (void)user;

    /* ---------- 亮度（滑块）---------- */
    const int bright = watch_ui_brightness_get();
    lv_obj_t *bcard = w_card_create(parent, W_AUTO_POS, W_AUTO_POS,
                                    LV_PCT(100), BRIGHT_H);

    lv_obj_t *bl = w_label_create(bcard, W_STR_BRIGHTNESS, watch_font_cn(16),
                                  W_COLOR_AUTO);
    lv_obj_align(bl, LV_ALIGN_TOP_LEFT, 0, 0);

    ui.lb_bright = w_label_create_auto(bcard, "---%", W_FONT_SMALL, 14,
                                       W_COLOR_ACCENT);
    lv_obj_align(ui.lb_bright, LV_ALIGN_TOP_RIGHT, 0, 0);
    w_label_set_text_fmt_auto(ui.lb_bright, "%d%%", bright);

    ui.slider = w_slider_create(bcard, W_SCREEN_W - 2 * W_PAD_PAGE - 2 * W_PAD_CARD,
                                5, 100, bright);
    lv_obj_align(ui.slider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(ui.slider, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---------- 自动息屏 ---------- */
    const int to = (int)watch_ui_setting_get(W_SETTING_AUTO_SCREEN, 30);
    make_row(parent, W_STR_AUTO_SCREEN, W_STR_NEVER, timeout_cb, &ui.lb_timeout);
    if (to != 0) {
        w_label_set_text_fmt_auto(ui.lb_timeout, "%d" W_STR_SECONDS_SUF, to);
    }

    /* ---------- 只读：时钟状态 ----------
     * 模态子页拿不到 watch_state_t，所以用 watch_ui_time_valid() 直接查。 */
    const bool tvalid = watch_ui_time_valid();
    make_row(parent, W_STR_RTC_STATUS,
             tvalid ? W_STR_RTC_OK : W_STR_RTC_LOST, NULL, &ui.lb_rtc);
    if (ui.lb_rtc) {
        lv_obj_set_style_text_color(ui.lb_rtc,
                                    tvalid ? W_COLOR_ACCENT : W_COLOR_WARN, 0);
    }
}

void watch_settings_open(void)
{
    w_modal_open(W_STR_SETTINGS, settings_build, NULL);
}

/* ==================================================================
 * 刷新
 *
 * 模态子页没有"数据回调"（那套是轮播页的 watch_page_t 才有的），
 * 所以这里由模态自己按需重建（w_modal_refresh）。
 * 需要跟着数据变的两处：
 *   1. 时钟状态 —— 用户从"时钟 > 手动校时"回来时，状态可能刚变
 *   2. 亮度     —— 可能被控制中心的滑块或串口 brightness 命令改过
 * 都在 build 里重新读一次即可（build 每次进页面/refresh 都会重跑）。
 * ================================================================== */
