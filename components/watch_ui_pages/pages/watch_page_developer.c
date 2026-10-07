/**
 * @file watch_page_developer.c
 * @brief 开发人员选项（子页面集合）—— 设置页底部入口
 *
 * ==================================================================
 * 结构
 * ==================================================================
 *
 * ```
 *   设置页
 *   └── [开发人员选项]                          ← 本文件的 dev_menu_build
 *       ├── [IMU 录制]      → dev_rec_build     （开始/停止/状态/删除）
 *       ├── [时间校准]      → dev_time_build    （年月日时分秒 ± 按钮）
 *       └── [系统信息]      → dev_info_build    （内存/固件/存储）
 * ```
 *
 * 全部走 `w_modal_open()`（盖在轮播上的普通容器 + 自带返回键），
 * 不新建 screen —— 理由见 watch_modal.h。
 *
 * ==================================================================
 * 时间校准：**填 UTC**
 * ==================================================================
 *
 * 用户明确要求"时间校准要存 UTC，方便后面切换时区"。
 * 所以这个页面填的就是 UTC，页面上也明确写着"UTC"，
 * 免得以后自己都忘了填的是哪个时区。
 * 显示时间（表盘）才按 settings 里的时区偏移换算。
 *
 * ==================================================================
 * IMU 录制
 * ==================================================================
 *
 * 录制期间会 `w_screen_hold_awake(true)` **禁止自动息屏** ——
 * 否则用户以为在录，其实背光早黑了，看不出状态（用户明确要求）。
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

static const char *TAG = "DEV";

/* 复用设置页那边的钩子（watch_ui.h 里声明） */
extern int watch_ui_setting_get(const char *key, int def);

/* ==================================================================
 * 一、开发人员选项 菜单
 * ================================================================== */
static void open_imu_rec(lv_event_t *e);
static void open_time_set(lv_event_t *e);
static void open_sysinfo(lv_event_t *e);

/** @brief 造一个"标题 + 副标题 + 右箭头"的可点菜单行 */
static lv_obj_t *menu_row(lv_obj_t *parent, const char *title, const char *sub,
                          lv_event_cb_t cb)
{
    lv_obj_t *card = w_card_create_clickable(parent, 0, 0, LV_PCT(100), 52);

    lv_obj_t *t = w_label_create(card, title, watch_font_cn(16), W_COLOR_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 2);

    if (sub) {
        lv_obj_t *s = w_label_create(card, sub, watch_font_cn(14), W_COLOR_TEXT_DIM);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    /* 右箭头提示"可以点进去" */
    lv_obj_t *a = w_label_create(card, ">", W_FONT_SMALL, W_COLOR_ACCENT);
    lv_obj_align(a, LV_ALIGN_RIGHT_MID, 0, 0);

    if (cb) {
        lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    }
    return card;
}

static void dev_menu_build(lv_obj_t *parent, void *user)
{
    (void)user;

    menu_row(parent, W_STR_DEV_IMU_REC, W_STR_DEV_IMU_REC_SUB, open_imu_rec);
    menu_row(parent, W_STR_DEV_TIME_SET, W_STR_DEV_TIME_SET_SUB, open_time_set);
    menu_row(parent, W_STR_DEV_SYSINFO, W_STR_DEV_SYSINFO_SUB, open_sysinfo);

    lv_obj_t *hint = w_label_create(parent, W_STR_DEV_HINT, watch_font_cn(14),
                                     W_COLOR_TEXT_DIM);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));
}

void watch_developer_open_menu(void)
{
    w_modal_open(W_STR_DEV_TITLE, dev_menu_build, NULL);
}

/* ==================================================================
 * 二、IMU 录制
 * ================================================================== */
static lv_obj_t *s_rec_status_lbl = NULL;

static void rec_refresh_labels(void)
{
    if (!s_rec_status_lbl) {
        return;
    }
    const w_rec_hooks_t *rh = w_platform_rec();
    if (!rh || !rh->available || !rh->available()) {
        w_label_set_text_auto(s_rec_status_lbl, W_STR_REC_UNAVAILABLE);
        lv_obj_set_style_text_color(s_rec_status_lbl, W_COLOR_WARN, 0);
        return;
    }

    w_rec_status_t st;
    rh->get_status(&st);

    if (st.recording) {
        lv_obj_set_style_text_color(s_rec_status_lbl, W_COLOR_ACCENT, 0);
        w_label_set_text_fmt_auto(s_rec_status_lbl,
                                  "%s\n%" PRIu32 " %s\n%" PRIu32 " %s / %" PRIu32 " KB",
                                  W_STR_REC_RECORDING, st.samples, W_STR_REC_FRAMES,
                                  st.seconds, W_STR_REC_SECONDS, st.bytes / 1024);
    } else {
        lv_obj_set_style_text_color(s_rec_status_lbl, W_COLOR_TEXT_DIM, 0);
        if (st.samples) {
            w_label_set_text_fmt_auto(s_rec_status_lbl,
                                      "%s\n%" PRIu32 " %s / %" PRIu32 " %s",
                                      W_STR_REC_IDLE, st.samples, W_STR_REC_FRAMES,
                                      st.seconds, W_STR_REC_SECONDS);
        } else {
            w_label_set_text_fmt_auto(s_rec_status_lbl,
                                      "%s\n%s %" PRIu32 " %s",
                                      W_STR_REC_IDLE, W_STR_REC_FILES,
                                      st.files, W_STR_REC_COUNT_SUF);
        }
    }
}

static void rec_start_cb(lv_event_t *e)
{
    (void)e;
    const w_rec_hooks_t *rh = w_platform_rec();
    if (!rh || !rh->start) {
        return;
    }
    if (rh->start() == 0) {
        /*
         * ★ 录制期间禁止自动息屏（用户明确要求）。
         *   用引用计数，stop 时对称解除。
         */
        w_screen_hold_awake(true);
        ESP_LOGI(TAG, "开始录制 IMU，已禁止自动息屏");
    }
    rec_refresh_labels();
    w_modal_refresh();      /* 按钮的可用状态要跟着变 */
}

static void rec_stop_cb(lv_event_t *e)
{
    (void)e;
    const w_rec_hooks_t *rh = w_platform_rec();
    if (!rh || !rh->stop) {
        return;
    }
    rh->stop();
    w_screen_hold_awake(false);
    ESP_LOGI(TAG, "停止录制 IMU，已恢复自动息屏");

    rec_refresh_labels();
    w_modal_refresh();
}

static void rec_erase_cb(lv_event_t *e)
{
    (void)e;
    const w_rec_hooks_t *rh = w_platform_rec();
    if (rh && rh->erase_all) {
        rh->erase_all();
    }
    rec_refresh_labels();
    w_modal_refresh();
}

/**
 * @brief 造一个"大按钮"
 *
 * @param color 按钮底色；用 W_COLOR_AUTO 表示默认卡片色
 */
static lv_obj_t *big_btn(lv_obj_t *parent, const char *text, lv_color_t color,
                         lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, LV_PCT(100), 48);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    if (color.full != W_COLOR_AUTO.full) {
        lv_obj_set_style_bg_color(b, color, 0);
    } else {
        lv_obj_set_style_bg_color(b, W_COLOR_CARD, 0);
    }
    lv_obj_t *l = w_label_create(b, text, watch_font_cn(16), W_COLOR_TEXT);
    lv_obj_center(l);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    }
    return b;
}

static void dev_rec_build(lv_obj_t *parent, void *user)
{
    (void)user;

    /* ---------- 状态 ---------- */
    lv_obj_t *card = w_card_create(parent, 0, 0, LV_PCT(100), 68);
    s_rec_status_lbl = w_label_create_auto(card, W_STR_REC_IDLE, watch_font_cn(14),
                                           14, W_COLOR_TEXT_DIM);
    lv_obj_align(s_rec_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_long_mode(s_rec_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_rec_status_lbl, LV_PCT(100));
    rec_refresh_labels();

    /* ---------- 两个大按钮 ---------- */
    big_btn(parent, W_STR_REC_START, W_COLOR_ACCENT, rec_start_cb);
    big_btn(parent, W_STR_REC_STOP, W_COLOR_CARD, rec_stop_cb);

    /* ---------- 容量 ---------- */
    const w_rec_hooks_t *rh = w_platform_rec();
    if (rh && rh->available && rh->available()) {
        w_rec_status_t st;
        rh->get_status(&st);
        lv_obj_t *cap = w_label_create_auto(parent, "", watch_font_cn(14), 14,
                                            W_COLOR_TEXT_DIM);
        w_label_set_text_fmt_auto(cap, "%s %" PRIu32 " / %" PRIu32 " KB",
                                  W_STR_REC_CAPACITY,
                                  st.used_bytes / 1024, st.total_bytes / 1024);
    }

    /* ---------- 删除 ---------- */
    big_btn(parent, W_STR_REC_ERASE, W_COLOR_CARD, rec_erase_cb);

    /* ---------- 使用说明（这个很关键，用户第一次用会踩坑） ---------- */
    lv_obj_t *tip = w_label_create(parent, W_STR_REC_TIP, watch_font_cn(14),
                                   W_COLOR_WARN);
    lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tip, LV_PCT(100));
}

static void open_imu_rec(lv_event_t *e)
{
    (void)e;
    w_modal_open(W_STR_DEV_IMU_REC, dev_rec_build, NULL);
}

/* ==================================================================
 * 三、时间校准（填 UTC）
 * ================================================================== */

/** 可调字段 */
typedef enum {
    F_YEAR = 0, F_MONTH, F_DAY, F_HOUR, F_MIN, F_SEC, F_COUNT
} time_field_t;

static int32_t s_tf[F_COUNT];
static lv_obj_t *s_tf_lbl[F_COUNT];

static const char *const s_tf_name[F_COUNT] = {
    "年", "月", "日", "时", "分", "秒"
};

/* 各字段的合法范围和步进 */
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
            w_label_set_text_fmt_auto(s_tf_lbl[i], "%s%" PRId32, "", s_tf[i]);
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
 *    而 `time.h` 在这个构建里也不是随便能用的（跨平台类型不一致）。
 *    所以初值走 data_hub 提供的**设备状态快照** —— 那本来就是
 *    界面层拿时间的唯一正规途径。
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
     * 如果能从 data_hub 拿到有效 UTC，就把它当初值 ——
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
    w_modal_refresh();
}

/** @brief 造一行 "字段名 [- ] 值 [ +]" */
static void make_tf_row(lv_obj_t *parent, time_field_t f)
{
    lv_obj_t *row = w_card_create(parent, 0, 0, LV_PCT(100), 40);

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

static void dev_time_build(lv_obj_t *parent, void *user)
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

    big_btn(parent, W_STR_TIME_APPLY, W_COLOR_ACCENT, ts_apply_cb);
}

static void open_time_set(lv_event_t *e)
{
    (void)e;
    w_modal_open(W_STR_DEV_TIME_SET, dev_time_build, NULL);
}

/* ==================================================================
 * 四、系统信息
 * ================================================================== */
static void info_row(lv_obj_t *parent, const char *k, const char *v)
{
    lv_obj_t *row = w_card_create(parent, 0, 0, LV_PCT(100), 34);
    lv_obj_t *kl = w_label_create(row, k, watch_font_cn(14), W_COLOR_TEXT_DIM);
    lv_obj_align(kl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *vl = w_label_create_auto(row, v, W_FONT_SMALL, 14, W_COLOR_TEXT);
    lv_obj_align(vl, LV_ALIGN_RIGHT_MID, 0, 0);
}

/* 这些由 app 层注入（界面层拿不到 esp_* API）*/
static char s_info_fw[16]   = "0.4.0";
static char s_info_heap[24] = "--";
static char s_info_psram[24] = "--";
static char s_info_uptime[24] = "--";

void watch_developer_set_sysinfo(const char *fw, const char *heap,
                                 const char *psram, const char *uptime)
{
    if (fw)     snprintf(s_info_fw, sizeof(s_info_fw), "%s", fw);
    if (heap)   snprintf(s_info_heap, sizeof(s_info_heap), "%s", heap);
    if (psram)  snprintf(s_info_psram, sizeof(s_info_psram), "%s", psram);
    if (uptime) snprintf(s_info_uptime, sizeof(s_info_uptime), "%s", uptime);
}

static void dev_info_build(lv_obj_t *parent, void *user)
{
    (void)user;

    info_row(parent, W_STR_INFO_FW, s_info_fw);
    info_row(parent, W_STR_INFO_HEAP, s_info_heap);
    info_row(parent, W_STR_INFO_PSRAM, s_info_psram);
    info_row(parent, W_STR_INFO_UPTIME, s_info_uptime);

    /* 存储（IMU 录制用） */
    const w_rec_hooks_t *rh = w_platform_rec();
    if (rh && rh->available && rh->available()) {
        w_rec_status_t st;
        rh->get_status(&st);
        char buf[32];
        snprintf(buf, sizeof(buf), "%" PRIu32 "/%" PRIu32 " KB",
                 st.used_bytes / 1024, st.total_bytes / 1024);
        info_row(parent, W_STR_INFO_STORAGE, buf);
    }
}

static void open_sysinfo(lv_event_t *e)
{
    (void)e;
    w_modal_open(W_STR_DEV_SYSINFO, dev_info_build, NULL);
}
