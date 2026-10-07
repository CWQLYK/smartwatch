/**
 * @file watch_page_developer.c
 * @brief 开发人员选项（子页面集合）—— 功能列表页的入口
 *
 * ==================================================================
 * 结构（v0.5.0）
 * ==================================================================
 *
 * ```
 *   功能列表
 *   └── [开发人员选项]                          ← 本文件的 dev_menu_build
 *       ├── [IMU 录制]      → dev_rec_build     （开始/停止/状态/删除）
 *       ├── [系统信息]      → dev_info_build    （内存/固件/存储）
 *       └── [蜂鸣器测试]    → dev_buzzer_build  （按住响、松开停）★ 新增
 * ```
 *
 * ★ v0.5.0 变化：
 *   1. **移走了「时间校准」** —— 用户要求把校时挪到「时钟 > 时间」里
 *      （校时是"时钟"这个功能的正常组成部分，不是开发调试手段）。
 *      代码搬到 `watch_page_timeset.c`，入口是 `watch_time_set_open()`。
 *   2. **新增「蜂鸣器测试」**。
 *   3. 菜单行和大按钮改用公共控件（`w_menu_row_create` / `w_btn_create`），
 *      和功能列表页、时钟页保持一致 —— 原来这两个是本文件里的 static 函数，
 *      三个页面各写一份的话圆角/内边距/字色一定会慢慢跑偏。
 *
 * 全部走 `w_modal_open()`（盖在轮播上的普通容器 + 自带返回键），
 * 不新建 screen —— 理由见 watch_modal.h。
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
static void open_sysinfo(lv_event_t *e);
static void open_buzzer(lv_event_t *e);

static void dev_menu_build(lv_obj_t *parent, void *user)
{
    (void)user;

    w_menu_row_create(parent, W_STR_DEV_IMU_REC, W_STR_DEV_IMU_REC_SUB, open_imu_rec);
    w_menu_row_create(parent, W_STR_DEV_SYSINFO, W_STR_DEV_SYSINFO_SUB, open_sysinfo);
    w_menu_row_create(parent, W_STR_DEV_BUZZER,  W_STR_DEV_BUZZER_SUB,  open_buzzer);

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

static void dev_rec_build(lv_obj_t *parent, void *user)
{
    (void)user;

    /* ---------- 状态 ---------- */
    lv_obj_t *card = w_card_create(parent, W_AUTO_POS, W_AUTO_POS,
                                   LV_PCT(100), 68);
    s_rec_status_lbl = w_label_create_auto(card, W_STR_REC_IDLE, watch_font_cn(14),
                                           14, W_COLOR_TEXT_DIM);
    lv_obj_align(s_rec_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_long_mode(s_rec_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_rec_status_lbl, LV_PCT(100));
    rec_refresh_labels();

    /* ---------- 两个大按钮 ---------- */
    w_btn_create(parent, W_STR_REC_START, W_AUTO_SIZE, 48,
                 W_COLOR_ACCENT, W_COLOR_BG, watch_font_cn(16), rec_start_cb);
    w_btn_create(parent, W_STR_REC_STOP, W_AUTO_SIZE, 48,
                 W_COLOR_CARD, W_COLOR_TEXT, watch_font_cn(16), rec_stop_cb);

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
    w_btn_create(parent, W_STR_REC_ERASE, W_AUTO_SIZE, 48,
                 W_COLOR_CARD, W_COLOR_TEXT, watch_font_cn(16), rec_erase_cb);

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
 * 三、蜂鸣器测试（v0.5.0 新增）
 *
 * 交互：**按住响，松开停**。
 *
 * ⚠️ 三个必须都挂的事件（少一个就会出 bug）：
 *      LV_EVENT_PRESSED    按下 -> 起音
 *      LV_EVENT_RELEASED   抬起 -> 停音
 *      LV_EVENT_PRESS_LOST 手指滑出按钮 -> 也必须停音！
 *    只挂 RELEASED 的话，用户按住后把手指滑出去，
 *    按钮收不到 RELEASED，蜂鸣器会**一直响下去**。
 *
 * 另外还要在页面被删除时兜底停音（LV_EVENT_DELETE）：
 * 否则"按住不放的时候恰好息屏/切页"也会留一个长鸣。
 *
 * 界面层不能 include ESP-IDF，所以起音/停音走
 * `w_buzzer_tone()` / `w_buzzer_stop()`（app 层注入的钩子）。
 * ================================================================== */

/** @brief 蜂鸣器谐振点，最响 */
#define BUZZER_TEST_HZ   2700

static bool s_buzzing = false;

static void buzz_start(void)
{
    if (s_buzzing) {
        return;
    }
    w_buzzer_tone(BUZZER_TEST_HZ);
    s_buzzing = true;
    ESP_LOGD(TAG, "蜂鸣器起音 %d Hz", BUZZER_TEST_HZ);
}

static void buzz_stop(void)
{
    if (!s_buzzing) {
        return;
    }
    w_buzzer_stop();
    s_buzzing = false;
    ESP_LOGD(TAG, "蜂鸣器停音");
}

static void buzz_press_cb(lv_event_t *e)
{
    (void)e;
    buzz_start();
}

static void buzz_release_cb(lv_event_t *e)
{
    (void)e;
    buzz_stop();
}

/** @brief 页面被删除时的兜底：别留一个长鸣 */
static void buzz_page_delete_cb(lv_event_t *e)
{
    (void)e;
    buzz_stop();
}

static void dev_buzzer_build(lv_obj_t *parent, void *user)
{
    (void)user;

    lv_obj_t *hint = w_label_create(parent, W_STR_BUZZER_HINT, watch_font_cn(14),
                                     W_COLOR_TEXT_DIM);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    /* 按住响、松开停的大按钮 */
    lv_obj_t *b = w_btn_create(parent, W_STR_BUZZER_HOLD, W_AUTO_SIZE, 96,
                               W_COLOR_ACCENT, W_COLOR_BG, watch_font_cn(20), NULL);
    lv_obj_add_event_cb(b, buzz_press_cb,   LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(b, buzz_release_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(b, buzz_release_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(b, buzz_page_delete_cb, LV_EVENT_DELETE, NULL);
}

void watch_developer_open_buzzer(void)
{
    w_modal_open(W_STR_DEV_BUZZER, dev_buzzer_build, NULL);
}

static void open_buzzer(lv_event_t *e)
{
    (void)e;
    watch_developer_open_buzzer();
}

/* ==================================================================
 * 四、系统信息
 * ================================================================== */
static void info_row(lv_obj_t *parent, const char *k, const char *v)
{
    lv_obj_t *row = w_card_create(parent, W_AUTO_POS, W_AUTO_POS,
                                  LV_PCT(100), 34);
    lv_obj_t *kl = w_label_create(row, k, watch_font_cn(14), W_COLOR_TEXT_DIM);
    lv_obj_align(kl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *vl = w_label_create_auto(row, v, W_FONT_SMALL, 14, W_COLOR_TEXT);
    lv_obj_align(vl, LV_ALIGN_RIGHT_MID, 0, 0);
}

/* 这些由 app 层注入（界面层拿不到 esp_* API）*/
static char s_info_fw[16]     = "0.6.0";
static char s_info_heap[24]   = "--";
static char s_info_psram[24]  = "--";
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
