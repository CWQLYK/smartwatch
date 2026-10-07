/**
 * @file watch_page_clock.c
 * @brief 时钟子页（时间 / 秒表 / 倒计时 / 闹钟）—— v0.5.0 新增
 *
 * ## 结构
 *
 *     功能列表 → [时钟]                       ← 本文件 clock_build
 *         ┌────────────────────────────┐
 *         │ 时钟                [返回]  │
 *         │ [时间|秒表|倒计时|闹钟]      │  ← 标签栏（btnmatrix，原生单选）
 *         ├────────────────────────────┤
 *         │  （当前标签的内容）          │
 *         └────────────────────────────┘
 *               ├ 时间   → 大号时间 + 日期星期 + [手动校时] → watch_time_set_open()
 *               ├ 秒表   → 大号 MM:SS.d + [开始/暂停][复位]
 *               ├ 倒计时 → 大号 MM:SS + 时长行 + [定时][响铃时长] + [开始/暂停][复位]
 *               └ 闹钟   → 大号 HH:MM + 时间行 + 启用行
 *
 * ## 两个关键设计
 *
 * ### 1. 状态在服务里，不在本页面里
 *
 * 秒表的计时值、倒计时的剩余时间、闹钟的时间**全部存在 watch_clock.c**。
 * 本页面只负责"渲染数字 + 按钮"。于是用户"起了秒表就切到闹钟标签"
 * 再切回来，秒表照样在走（用户明确要求）。
 *
 * ### 2. 刷新由服务侧的定时器驱动
 *
 * 本页面在 build 时把 `clock_render()` 注册给时钟服务
 * （`watch_clock_set_ui_refresh`），服务那个**全局** lv_timer 周期调它。
 * 页面被删掉时必须注销，否则会去碰已经释放的 label ——
 * 这里用 `LV_EVENT_DELETE` 事件兜住（LVGL 删对象前会发这个事件）。
 *
 *     lv_obj_clean(modal root)       ← 模态重建时先清空
 *          └─ 每个子对象收到 LV_EVENT_DELETE → 注销回调、清空 label 指针
 *     m->build(body, user)           ← 然后才重新 build → 重新注册
 *
 * ⚠️ 顺序是"先清空、再 build"，所以最终一定处于"已注册"状态。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_modal.h"
#include "watch_wheel.h"
#include "watch_clock.h"
#include "watch_strings.h"

#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "CLOCKPAGE";

/* ==================== 标签 ==================== */
typedef enum {
    TAB_TIME = 0,
    TAB_SW,
    TAB_CD,
    TAB_ALARM,
    TAB_COUNT
} clock_tab_t;

/* 标签状态必须放在静态：模态每次 refresh 都会重跑 build */
static clock_tab_t s_tab = TAB_TIME;

/* ==================== 控件句柄 ====================
 *
 * 每次重建内容时会全部清空（rebuild_content），
 * 渲染函数里逐个判空。 */
static lv_obj_t *s_content   = NULL;   /* 内容容器 */
static lv_obj_t *s_lb_big    = NULL;   /* 大号数字 */
static lv_obj_t *s_lb_sub    = NULL;   /* 副行（日期星期 / 提示 / 警告） */
static lv_obj_t *s_lb_dur    = NULL;   /* 时长行的值 */
static lv_obj_t *s_lb_ring   = NULL;   /* 响铃时长行的值 */
static lv_obj_t *s_lb_alarm  = NULL;   /* 闹钟时间行的值 */
static lv_obj_t *s_lb_en     = NULL;   /* 启用行的值 */
static lv_obj_t *s_lb_toggle = NULL;   /* [开始/暂停] 按钮上的文字 */

/* ==================== 秒表 ==================== */
static void sw_toggle_cb(lv_event_t *e)
{
    (void)e;
    watch_clock_sw_toggle();
}

static void sw_reset_cb(lv_event_t *e)
{
    (void)e;
    watch_clock_sw_reset();
}

/* ==================== 倒计时 ==================== */

/* 预设档位（秒）：1/3/5/10/15/30 分钟 */
static const int k_cd_preset[] = { 60, 180, 300, 600, 900, 1800 };
#define CD_PRESET_N  ((int)(sizeof(k_cd_preset) / sizeof(k_cd_preset[0])))

/** @brief 时长行：点一下切到下一个预设档位 */
static void cd_duration_cb(lv_event_t *e)
{
    (void)e;
    const int cur = watch_clock_cd_sec();
    int next = k_cd_preset[0];
    for (int i = 0; i < CD_PRESET_N; i++) {
        if (k_cd_preset[i] == cur) {
            next = k_cd_preset[(i + 1) % CD_PRESET_N];
            break;
        }
    }
    /* 当前是自定义值（不在档位里）→ 回到第一档 */
    watch_clock_cd_set_sec(next);
    if (s_lb_dur) {
        char buf[16];
        watch_clock_fmt_mmss(next, buf, sizeof(buf));
        w_label_set_text_auto(s_lb_dur, buf);
    }
}

/** @brief 自定义定时的滚轮回调 */
static void cd_custom_ok(const int *val, void *user)
{
    (void)user;
    const int sec = val[0] * 3600 + val[1] * 60 + val[2];
    if (sec < 1) {
        ESP_LOGW(TAG, "自定义定时为 0，忽略");
        return;
    }
    watch_clock_cd_set_sec(sec);
    ESP_LOGI(TAG, "自定义倒计时：%d 秒", sec);
}

/** @brief [定时] 按钮：弹滚轮设 时/分/秒 */
static void cd_custom_cb(lv_event_t *e)
{
    (void)e;
    const int total = watch_clock_cd_sec();

    const w_wheel_cfg_t cfg = {
        .title = W_STR_CD_SET_TITLE,
        .cols  = 3,
        .unit  = { W_STR_UNIT_HOUR, W_STR_UNIT_MIN, W_STR_UNIT_SEC },
        .min   = { 0, 0, 0 },
        .max   = { 23, 59, 59 },
        .init  = { total / 3600, (total / 60) % 60, total % 60 },
        .on_ok = cd_custom_ok,
        .user  = NULL,
    };
    w_wheel_open(&cfg);
}

/** @brief 响铃时长的滚轮回调 */
static void cd_ring_ok(const int *val, void *user)
{
    (void)user;
    const int sec = val[0] * 60 + val[1];
    if (sec < 1) {
        ESP_LOGW(TAG, "响铃时长为 0，忽略");
        return;
    }
    watch_clock_cd_set_ring_sec(sec);
}

/** @brief 响铃时长行：弹滚轮设 分/秒（默认 0 分 10 秒） */
static void cd_ring_cb(lv_event_t *e)
{
    (void)e;
    const int total = watch_clock_cd_ring_sec();

    const w_wheel_cfg_t cfg = {
        .title = W_STR_CD_RING_TITLE,
        .cols  = 2,
        .unit  = { W_STR_UNIT_MIN, W_STR_UNIT_SEC, NULL },
        .min   = { 0, 0, 0 },
        .max   = { 5, 59, 0 },
        .init  = { total / 60, total % 60, 0 },
        .on_ok = cd_ring_ok,
        .user  = NULL,
    };
    w_wheel_open(&cfg);
}

static void cd_toggle_cb(lv_event_t *e)
{
    (void)e;
    watch_clock_cd_toggle();
}

static void cd_reset_cb(lv_event_t *e)
{
    (void)e;
    watch_clock_cd_reset();
}

/* ==================== 闹钟 ==================== */
static void alarm_time_ok(const int *val, void *user)
{
    (void)user;
    watch_clock_alarm_set(val[0], val[1]);
    /* 顺手打开：用户既然专门设了时间，多半就是要用 */
    watch_clock_alarm_enable(true);
}

static void alarm_time_cb(lv_event_t *e)
{
    (void)e;

    const w_wheel_cfg_t cfg = {
        .title = W_STR_ALARM_TITLE,
        .cols  = 2,
        .unit  = { W_STR_UNIT_HOUR, W_STR_UNIT_MIN, NULL },
        .min   = { 0, 0, 0 },
        .max   = { 23, 59, 0 },
        .init  = { watch_clock_alarm_hour(), watch_clock_alarm_min(), 0 },
        .on_ok = alarm_time_ok,
        .user  = NULL,
    };
    w_wheel_open(&cfg);
}

static void alarm_en_cb(lv_event_t *e)
{
    (void)e;
    watch_clock_alarm_enable(!watch_clock_alarm_enabled());
}

/* ==================== 时间标签 ==================== */
static void time_set_cb(lv_event_t *e)
{
    (void)e;
    watch_time_set_open();
}

/* ==================================================================
 * 渲染（由时钟服务的全局定时器周期调用）
 * ================================================================== */
static void clock_render(void)
{
    if (!s_content) {
        return;                     /* 页面还没建好 / 已经删了 */
    }

    char buf[24];

    switch (s_tab) {
    case TAB_TIME: {
        watch_time_t t;
        if (watch_ui_now(&t) && t.valid) {
            char hm[8];
            snprintf(hm, sizeof(hm), "%02d:%02d", t.hour, t.minute);
            w_label_set_text_auto(s_lb_big, hm);
            if (s_lb_sub) {
                /* 第二行：日期 + 星期（和时间页的表盘口径一致） */
                char sub[48];
                snprintf(sub, sizeof(sub), W_STR_DATE_FMT " %s",
                         t.year, t.month, t.day, w_weekday_text(t.weekday));
                w_label_set_text_auto(s_lb_sub, sub);
            }
        } else {
            w_label_set_text_auto(s_lb_big, "--:--");
            if (s_lb_sub) {
                w_label_set_text_auto(s_lb_sub, W_STR_TIME_UNSET);
            }
        }
        break;
    }

    case TAB_SW: {
        watch_clock_fmt_stopwatch(watch_clock_sw_elapsed_ms(), buf, sizeof(buf));
        w_label_set_text_auto(s_lb_big, buf);
        if (s_lb_toggle) {
            w_label_set_text_auto(s_lb_toggle,
                                  watch_clock_sw_running() ? W_STR_PAUSE
                                                           : W_STR_START);
        }
        break;
    }

    case TAB_CD: {
        watch_clock_fmt_mmss(watch_clock_cd_remaining_sec(), buf, sizeof(buf));
        w_label_set_text_auto(s_lb_big, buf);

        if (s_lb_dur) {
            watch_clock_fmt_mmss(watch_clock_cd_sec(), buf, sizeof(buf));
            w_label_set_text_auto(s_lb_dur, buf);
        }
        if (s_lb_ring) {
            watch_clock_fmt_mmss(watch_clock_cd_ring_sec(), buf, sizeof(buf));
            w_label_set_text_auto(s_lb_ring, buf);
        }
        if (s_lb_toggle) {
            w_label_set_text_auto(s_lb_toggle,
                                  watch_clock_cd_running() ? W_STR_PAUSE
                                                           : W_STR_START);
        }
        break;
    }

    case TAB_ALARM: {
        snprintf(buf, sizeof(buf), "%02d:%02d",
                 watch_clock_alarm_hour(), watch_clock_alarm_min());
        w_label_set_text_auto(s_lb_big, buf);

        if (s_lb_alarm) {
            w_label_set_text_auto(s_lb_alarm, buf);
        }
        if (s_lb_en) {
            w_label_set_text_auto(s_lb_en, watch_clock_alarm_enabled()
                                               ? W_STR_ON : W_STR_OFF);
        }
        /* 时间没校准 -> 闹钟不会生效，必须明确告诉用户 */
        if (s_lb_sub) {
            if (!watch_clock_time_ok()) {
                w_label_set_text_auto(s_lb_sub, W_STR_ALARM_NO_TIME);
                lv_obj_set_style_text_color(s_lb_sub, W_COLOR_WARN, 0);
            } else {
                w_label_set_text_auto(s_lb_sub, W_STR_ALARM_HINT);
                lv_obj_set_style_text_color(s_lb_sub, W_COLOR_TEXT_MUTE, 0);
            }
        }
        break;
    }

    default:
        break;
    }
}

/** @brief 内容容器被删除时：注销刷新回调、清空所有句柄，避免野指针 */
static void content_delete_cb(lv_event_t *e)
{
    (void)e;
    watch_clock_set_ui_refresh(NULL);
    s_content   = NULL;
    s_lb_big    = NULL;
    s_lb_sub    = NULL;
    s_lb_dur    = NULL;
    s_lb_ring   = NULL;
    s_lb_alarm  = NULL;
    s_lb_en     = NULL;
    s_lb_toggle = NULL;
}

/* ==================================================================
 * 造一行"标题 + 右侧值"（可点）
 * ================================================================== */
static lv_obj_t *value_row(lv_obj_t *parent, const char *title, const char *value,
                           lv_event_cb_t cb, lv_obj_t **out_value)
{
    lv_obj_t *card = w_card_create_clickable(parent, W_AUTO_POS, W_AUTO_POS,
                                             LV_PCT(100), 36);

    lv_obj_t *t = w_label_create(card, title, watch_font_cn(16), W_COLOR_TEXT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = w_label_create_auto(card, value, W_FONT_NORMAL, 16,
                                      W_COLOR_ACCENT);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);

    if (cb) {
        lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    }
    if (out_value) {
        *out_value = v;
    }
    return card;
}

/** @brief 造一行"两个半宽按钮" */
static void btn_pair(lv_obj_t *parent, const char *ltxt, lv_event_cb_t lcb,
                     const char *rtxt, lv_event_cb_t rcb,
                     lv_obj_t **out_left_label)
{
    lv_obj_t *row = w_flex_col_create(parent, W_AUTO_POS, W_AUTO_POS,
                                      LV_PCT(100), W_AUTO_SIZE, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, W_GAP, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const lv_coord_t bw = (W_SCREEN_W - 2 * W_PAD_PAGE - W_GAP) / 2;

    lv_obj_t *lb = w_btn_create(row, ltxt, bw, 44,
                                W_COLOR_ACCENT, W_COLOR_BG,
                                watch_font_cn(16), lcb);
    w_btn_create(row, rtxt, bw, 44,
                 W_COLOR_CARD, W_COLOR_TEXT, watch_font_cn(16), rcb);

    if (out_left_label) {
        *out_left_label = lv_obj_get_child(lb, 0);   /* 按钮里的文字标签 */
    }
}

/** @brief 造大号数字标签 */
static void big_label(lv_obj_t *parent)
{
    s_lb_big = w_label_create_auto(parent, "--:--", W_FONT_CLOCK, 20,
                                   W_COLOR_TEXT);
    lv_obj_set_style_text_align(s_lb_big, LV_TEXT_ALIGN_CENTER, 0);
}

/* ==================================================================
 * 按标签造内容
 * ================================================================== */
static void build_tab_content(lv_obj_t *box)
{
    switch (s_tab) {
    /* ---------------- 时间 ---------------- */
    case TAB_TIME:
        big_label(box);
        s_lb_sub = w_label_create_auto(box, "", W_FONT_SMALL, 14, W_COLOR_TEXT_DIM);
        w_btn_create(box, W_STR_MANUAL_SET, W_AUTO_SIZE, 44,
                     W_COLOR_CARD, W_COLOR_TEXT, watch_font_cn(16), time_set_cb);
        break;

    /* ---------------- 秒表 ---------------- */
    case TAB_SW:
        big_label(box);
        s_lb_sub = w_label_create_auto(box, W_STR_SW_HINT, W_FONT_SMALL, 14,
                                       W_COLOR_TEXT_MUTE);
        btn_pair(box, W_STR_START, sw_toggle_cb, W_STR_RESET, sw_reset_cb,
                 &s_lb_toggle);
        break;

    /* ---------------- 倒计时 ---------------- */
    case TAB_CD:
        big_label(box);
        value_row(box, W_STR_CD_DURATION, "--:--", cd_duration_cb, &s_lb_dur);
        value_row(box, W_STR_CD_RING, "--:--", cd_ring_cb, &s_lb_ring);
        w_btn_create(box, W_STR_CD_CUSTOM, W_AUTO_SIZE, 44,
                     W_COLOR_CARD, W_COLOR_TEXT, watch_font_cn(16), cd_custom_cb);
        btn_pair(box, W_STR_START, cd_toggle_cb, W_STR_RESET, cd_reset_cb,
                 &s_lb_toggle);
        s_lb_sub = w_label_create_auto(box, W_STR_CD_HINT, W_FONT_SMALL, 14,
                                       W_COLOR_TEXT_MUTE);
        break;

    /* ---------------- 闹钟 ---------------- */
    case TAB_ALARM:
        big_label(box);
        value_row(box, W_STR_ALARM_TIME, "--:--", alarm_time_cb, &s_lb_alarm);
        value_row(box, W_STR_ALARM_ENABLE, W_STR_OFF, alarm_en_cb, &s_lb_en);
        s_lb_sub = w_label_create_auto(box, W_STR_ALARM_HINT, W_FONT_SMALL, 14,
                                       W_COLOR_TEXT_MUTE);
        lv_label_set_long_mode(s_lb_sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_lb_sub, LV_PCT(100));
        break;

    default:
        break;
    }
}

/** @brief 切标签 / 首次进入：重建内容容器里的东西 */
static void rebuild_content(void)
{
    if (!s_content) {
        return;
    }

    /* 先清句柄：clean 之后旧对象就没了，渲染函数不能再去碰 */
    s_lb_big    = NULL;
    s_lb_sub    = NULL;
    s_lb_dur    = NULL;
    s_lb_ring   = NULL;
    s_lb_alarm  = NULL;
    s_lb_en     = NULL;
    s_lb_toggle = NULL;

    lv_obj_clean(s_content);
    build_tab_content(s_content);
    clock_render();
}

static void tab_cb(int idx, void *user)
{
    (void)user;
    if (idx < 0 || idx >= TAB_COUNT) {
        return;
    }
    if ((clock_tab_t)idx == s_tab) {
        return;
    }
    s_tab = (clock_tab_t)idx;
    rebuild_content();
}

/* ==================================================================
 * 模态构建回调
 * ================================================================== */
static void clock_build(lv_obj_t *parent, void *user)
{
    (void)user;

    /* 标签栏（4 个等宽按钮，选中态自己维护） */
    w_tabbar_create(parent, W_STR_TABBAR, (uint16_t)s_tab, tab_cb, NULL);

    /*
     * 内容容器。
     *
     * ⚠️ **不要**给它 `lv_obj_set_flex_grow(s_content, 1)`。
     *    第一版加了这个（想让它吃掉标签栏剩下的高度），结果：
     *    父容器 body 的高度是 `LV_SIZE_CONTENT`，而 grow 又想从
     *    "剩余空间"里分高度 —— 两者冲突，实测容器被压成只剩一行高，
     *    而容器本身不可滚动 → **其余控件全被裁掉看不见**
     *    （倒计时标签只剩"时长"一行，被模拟器截图抓到）。
     *
     *    正确做法：让它 `LV_SIZE_CONTENT`，高度由子控件撑开；
     *    内容超过一屏时由**模态页**（它是可纵向滚动的）负责滚动。
     */
    s_content = w_flex_col_create(parent, W_AUTO_POS, W_AUTO_POS,
                                  LV_PCT(100), W_AUTO_SIZE, 6);

    /* 容器被删（页面关闭/重建）时注销刷新回调，避免野指针 */
    lv_obj_add_event_cb(s_content, content_delete_cb, LV_EVENT_DELETE, NULL);

    build_tab_content(s_content);

    /* 把渲染函数交给时钟服务的全局定时器 */
    watch_clock_set_ui_refresh(clock_render);
    clock_render();
}

void watch_clock_open(void)
{
    w_modal_open(W_STR_CLOCK, clock_build, NULL);
}

bool watch_clock_debug_select_tab(int tab)
{
    if (!s_content || tab < 0 || tab >= TAB_COUNT) {
        return false;
    }
    s_tab = (clock_tab_t)tab;
    w_tabbar_set_active((uint16_t)tab);     /* 标签栏的高亮也要跟着走 */
    rebuild_content();
    return true;
}
