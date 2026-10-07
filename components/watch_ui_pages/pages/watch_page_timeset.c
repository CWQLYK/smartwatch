/**
 * @file watch_page_timeset.c
 * @brief 手动校时子页 —— v0.5.0 从 watch_page_developer.c 搬过来
 *
 * ## 为什么搬家
 *
 * 用户要求「时钟」子页里要有"时间"这一栏，并且把开发人员选项里的
 * 「时间校准」挪过去。理由很自然：**校时是"时钟"这个功能的正常组成部分**，
 * 而不是开发调试手段。
 *
 *     搬之前：设置页 → 开发人员选项 → 时间校准
 *     搬之后：功能列表 → 时钟 → 时间 → [手动校时]
 *
 * ## 时间口径：**填的是 UTC**
 *
 * 用户明确要求"时间校准要存 UTC，方便后面切换时区"。
 * 所以这个页面填的就是 UTC，页面上也明确写着"UTC"，
 * 免得以后自己都忘了填的是哪个时区。
 *
 * ⚠️ 与 `watch_platform.h` 里那个字段名叫 `set_local` 的钩子对不上 ——
 *    **字段名是历史遗留的误导**，实际语义就是"写 UTC"，
 *    app 层的 `time_hook_set_utc()` 不做任何时区换算。
 *    表盘显示时才按 settings 里的时区偏移换算。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_strings.h"
#include "watch_platform.h"
#include "watch_modal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "TIMESET";

/* ==================== 可调字段 ==================== */
typedef enum {
    F_YEAR = 0, F_MONTH, F_DAY, F_HOUR, F_MIN, F_SEC, F_COUNT
} time_field_t;

static int32_t  s_tf[F_COUNT];
static lv_obj_t *s_tf_lbl[F_COUNT];

static const char *const s_tf_name[F_COUNT] = {
    "年", "月", "日", "时", "分", "秒"
};

/* 各字段的合法范围 */
static const int32_t s_tf_min[F_COUNT] = { 2024, 1,  1,  0,  0,  0 };
static const int32_t s_tf_max[F_COUNT] = { 2099, 12, 31, 23, 59, 59 };

/** @brief 闰年判断（月份天数要用） */
static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
    static const int d[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m < 1 || m > 12) {
        return 31;
    }
    if (m == 2 && is_leap(y)) {
        return 29;
    }
    return d[m - 1];
}

static void tf_refresh(void)
{
    for (int i = 0; i < F_COUNT; i++) {
        if (s_tf_lbl[i]) {
            w_label_set_text_fmt_auto(s_tf_lbl[i], "%" PRId32, s_tf[i]);
        }
    }
}

/** @brief 点 + / - 时改一个字段 */
static void tf_step(time_field_t f, int delta)
{
    s_tf[f] += delta;

    /* 日期字段越界时按"回绕"处理，比夹住好用（改月份不用先记住天数） */
    if (f == F_YEAR) {
        if (s_tf[f] < s_tf_min[f]) s_tf[f] = s_tf_max[f];
        if (s_tf[f] > s_tf_max[f]) s_tf[f] = s_tf_min[f];
    } else if (f == F_MONTH) {
        if (s_tf[f] < 1) s_tf[f] = 12;
        if (s_tf[f] > 12) s_tf[f] = 1;
    } else if (f == F_DAY) {
        /* 天数上限跟着年月走：2 月在闰年是 29 天 */
        const int dim = days_in_month((int)s_tf[F_YEAR], (int)s_tf[F_MONTH]);
        if (s_tf[f] < 1)    s_tf[f] = dim;
        if (s_tf[f] > dim)  s_tf[f] = 1;
    } else {
        if (s_tf[f] < s_tf_min[f]) s_tf[f] = s_tf_max[f];
        if (s_tf[f] > s_tf_max[f]) s_tf[f] = s_tf_min[f];
    }
    tf_refresh();
}

static void tf_plus_cb(lv_event_t *e)
{
    const intptr_t f = (intptr_t)lv_event_get_user_data(e);
    tf_step((time_field_t)f, +1);
}

static void tf_minus_cb(lv_event_t *e)
{
    const intptr_t f = (intptr_t)lv_event_get_user_data(e);
    tf_step((time_field_t)f, -1);
}

/**
 * @brief 用当前时间初始化各字段
 *
 * ⚠️ 这里**不能用 time()/localtime_r()**：
 *    界面层不许 include ESP-IDF 头文件（PC 模拟器编不过），
 *    而且 `time.h` 在这个构建里也不是随便能用的（跨平台类型不一致）。
 *    所以初值走界面层拿时间的正规途径：`watch_ui_current_epoch()`。
 *
 * 拿不到有效时间时给个合理起点（2026-01-01 12:00:00），
 * 免得用户从 1970 年一路按 + 上来。
 */
static void tf_load_now(void)
{
    /* 合理起点：即使 RTC 没校过，也从这里开始调 */
    s_tf[F_YEAR]  = 2026;
    s_tf[F_MONTH] = 1;
    s_tf[F_DAY]   = 1;
    s_tf[F_HOUR]  = 12;
    s_tf[F_MIN]   = 0;
    s_tf[F_SEC]   = 0;

    /*
     * 如果能拿到有效 UTC，就把它当初值 ——
     * 这样"只微调几分钟"时不用按几百下。
     */
    const uint32_t epoch = watch_ui_current_epoch();
    if (epoch > 1700000000u) {          /* 2023-11 之后才算有效 */
        /* 自己算日历，不碰 time.h */
        uint32_t days = epoch / 86400u;
        uint32_t rem  = epoch % 86400u;
        s_tf[F_HOUR] = (int32_t)(rem / 3600u);
        s_tf[F_MIN]  = (int32_t)((rem % 3600u) / 60u);
        s_tf[F_SEC]  = (int32_t)(rem % 60u);

        int year = 1970;
        for (;;) {
            const int diy = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
                            ? 366 : 365;
            if (days < (uint32_t)diy) {
                break;
            }
            days -= (uint32_t)diy;
            year++;
        }
        s_tf[F_YEAR] = year;
        int mon = 1;
        for (;;) {
            static const int d[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
            int dim = d[mon - 1];
            if (mon == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
                dim = 29;
            }
            if (days < (uint32_t)dim) {
                break;
            }
            days -= (uint32_t)dim;
            mon++;
        }
        s_tf[F_MONTH] = mon;
        s_tf[F_DAY]   = (int32_t)days + 1;
    }
}

static void ts_apply_cb(lv_event_t *e)
{
    (void)e;
    const w_time_hooks_t *th = w_platform_time();
    if (!th || !th->set_local) {
        ESP_LOGW(TAG, "没接校时钩子，无法写入");
        return;
    }

    const int err = th->set_local((int)s_tf[F_YEAR], (int)s_tf[F_MONTH],
                                  (int)s_tf[F_DAY], (int)s_tf[F_HOUR],
                                  (int)s_tf[F_MIN], (int)s_tf[F_SEC]);
    ESP_LOGI(TAG, "校准时间：%04d-%02d-%02d %02d:%02d:%02d UTC -> %s",
             (int)s_tf[F_YEAR], (int)s_tf[F_MONTH], (int)s_tf[F_DAY],
             (int)s_tf[F_HOUR], (int)s_tf[F_MIN], (int)s_tf[F_SEC],
             err == 0 ? "成功" : "失败");

    watch_ui_refresh();
    /* 重建本页（数值刷新）并让上一层（时钟）也跟着更新 */
    w_modal_refresh();
}

/** @brief 造一行 "字段名 [- ] 值 [ +]" */
static void make_tf_row(lv_obj_t *parent, time_field_t f)
{
    lv_obj_t *row = w_card_create(parent, W_AUTO_POS, W_AUTO_POS,
                                  LV_PCT(100), 40);

    lv_obj_t *n = w_label_create(row, s_tf_name[f], watch_font_cn(16), W_COLOR_TEXT);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);

    /* 先放 + 在最右 */
    lv_obj_t *plus = lv_btn_create(row);
    lv_obj_set_size(plus, 40, 30);
    lv_obj_set_style_radius(plus, 6, 0);
    lv_obj_set_style_bg_color(plus, W_COLOR_CARD, 0);
    lv_obj_set_style_shadow_width(plus, 0, 0);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *pl = w_label_create(plus, "+", W_FONT_NORMAL, W_COLOR_ACCENT);
    lv_obj_center(pl);
    lv_obj_add_event_cb(plus, tf_plus_cb, LV_EVENT_CLICKED, (void *)(intptr_t)f);

    /* 减号放 + 左边 */
    lv_obj_t *minus = lv_btn_create(row);
    lv_obj_set_size(minus, 40, 30);
    lv_obj_set_style_radius(minus, 6, 0);
    lv_obj_set_style_bg_color(minus, W_COLOR_CARD, 0);
    lv_obj_set_style_shadow_width(minus, 0, 0);
    lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -46, 0);
    lv_obj_t *ml = w_label_create(minus, "-", W_FONT_NORMAL, W_COLOR_TEXT);
    lv_obj_center(ml);
    lv_obj_add_event_cb(minus, tf_minus_cb, LV_EVENT_CLICKED, (void *)(intptr_t)f);

    /* 数值放减号左边 */
    s_tf_lbl[f] = w_label_create_auto(row, "0", W_FONT_NORMAL, 16, W_COLOR_ACCENT);
    lv_obj_align(s_tf_lbl[f], LV_ALIGN_RIGHT_MID, -94, 0);
}

/* ==================== 模态构建 ==================== */
static void timeset_build(lv_obj_t *parent, void *user)
{
    (void)user;

    /* 每次进入都用当前时间刷新初值 */
    tf_load_now();
    memset(s_tf_lbl, 0, sizeof(s_tf_lbl));

    lv_obj_t *hint = w_label_create(parent, W_STR_TIME_UTC_HINT, watch_font_cn(14),
                                     W_COLOR_WARN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    for (int f = 0; f < F_COUNT; f++) {
        make_tf_row(parent, (time_field_t)f);
    }
    tf_refresh();

    w_btn_create(parent, W_STR_TIME_APPLY, W_AUTO_SIZE, 48,
                 W_COLOR_ACCENT, W_COLOR_BG, watch_font_cn(16), ts_apply_cb);
}

void watch_time_set_open(void)
{
    w_modal_open(W_STR_DEV_TIME_SET, timeset_build, NULL);
}
