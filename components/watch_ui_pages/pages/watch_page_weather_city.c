/**
 * @file watch_page_weather_city.c
 * @brief 城市坐标设置（天气页的二级子页）—— v0.7.0
 *
 *     ┌──────────────────────────┐
 *     │ 城市坐标          [返回]  │
 *     │ 纬度            31.23000 │  ← 点一下弹数字键盘改
 *     │ 经度          121.47000  │
 *     │ 点数字改，格式如 31.23000 │
 *     └──────────────────────────┘
 *
 * ## 为什么让用户手输经纬度，而不是选城市名
 *
 * 选城市名要一张"城市 → 经纬度"的表：全国地级市 300+ 条、每条几十字节，
 * 就是十几 KB flash，还得持续维护。而用户只要**填一次**自己的坐标
 * （手机地图上长按就能看到）。所以用最省的做法：手输两个数。
 *
 * ## 存储格式：整数微度（1e-5 度）
 *
 * 界面层的设置钩子签名是 `int`（存不了小数），NVS 里整数也没有
 * float 的精度/类型问题。上海 = 3123000 / 12147000，精度约 1.1 米。
 * 输入 "31.23000" → 3123000；显示时反向格式化。
 *
 * ## 输入方式：复用现成的软键盘
 *
 * `w_keyboard_open()`（WiFi 密码页在用的那个）支持切到数字层，
 * 而且**回调触发后自己删自己**，不用我们管生命周期。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_strings.h"
#include "watch_modal.h"
#include "watch_keyboard.h"
#include "watch_toast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "WXCITY";

/* ==================== 状态 ==================== */
static struct {
    lv_obj_t *lb_lat;
    lv_obj_t *lb_lon;
    lv_obj_t *parent;       /* 当前内容容器（键盘要挂在这上面）*/
} ui;

/* ==================== 微度 ⇄ 文本 ==================== */

static void udeg_to_text(int32_t v, char *out, int len)
{
    const bool neg = (v < 0);
    const int32_t a = neg ? -v : v;
    snprintf(out, (size_t)len, "%s%ld.%05ld", neg ? "-" : "",
             (long)(a / 100000), (long)(a % 100000));
}

/**
 * @brief "31.23000" → 微度；非法返回 INT32_MIN
 *
 * @note 刻意**不调 weather 组件的 `weather_parse_udeg()`** ——
 *       界面层不能依赖那个组件（会带进 ESP-IDF 头，模拟器编不过）。
 *       两处实现要保持一致（改一处要想着另一处）。
 */
static int32_t text_to_udeg(const char *s)
{
    if (!s || !s[0]) {
        return INT32_MIN;
    }
    const char *p = s;
    bool neg = false;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    int64_t whole = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        if (++digits > 9) {
            return INT32_MIN;
        }
        p++;
    }
    if (digits == 0) {
        return INT32_MIN;           /* 必须以数字开头（".5" 不接受，免得歧义）*/
    }
    int64_t frac = 0;
    int fd = 0;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (fd < 5) {
                frac = frac * 10 + (*p - '0');
                fd++;
            }
            p++;
        }
    }
    if (*p != '\0') {
        return INT32_MIN;           /* 有非法字符 */
    }
    while (fd < 5) {
        frac *= 10;
        fd++;
    }
    int64_t v = whole * 100000 + frac;
    if (neg) {
        v = -v;
    }
    if (v < -18000000 || v > 18000000) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

/* ==================== 输入完成 ==================== */

static void lat_done(const char *text, bool ok, void *user)
{
    (void)user;
    if (!ok || !text) {
        return;                     /* 取消 */
    }
    const int32_t v = text_to_udeg(text);
    if (v == INT32_MIN || v < -9000000 || v > 9000000) {
        watch_toast_show(W_STR_WX_BAD_VALUE);
        return;
    }
    if (watch_ui_setting_set(W_SETTING_WX_LAT, (int)v) != 0) {
        watch_toast_show(W_STR_SETTING_SAVE_FAILED);
        return;
    }
    char buf[16];
    udeg_to_text(v, buf, sizeof(buf));
    if (ui.lb_lat) {
        w_label_set_text_auto(ui.lb_lat, buf);
    }
    watch_toast_show(W_STR_WX_SAVED);
    ESP_LOGI(TAG, "纬度已保存：%s（微度 %ld）", buf, (long)v);
}

static void lon_done(const char *text, bool ok, void *user)
{
    (void)user;
    if (!ok || !text) {
        return;
    }
    const int32_t v = text_to_udeg(text);
    if (v == INT32_MIN || v < -18000000 || v > 18000000) {
        watch_toast_show(W_STR_WX_BAD_VALUE);
        return;
    }
    if (watch_ui_setting_set(W_SETTING_WX_LON, (int)v) != 0) {
        watch_toast_show(W_STR_SETTING_SAVE_FAILED);
        return;
    }
    char buf[16];
    udeg_to_text(v, buf, sizeof(buf));
    if (ui.lb_lon) {
        w_label_set_text_auto(ui.lb_lon, buf);
    }
    watch_toast_show(W_STR_WX_SAVED);
    ESP_LOGI(TAG, "经度已保存：%s（微度 %ld）", buf, (long)v);
}

/* ==================== 行点击 ==================== */

/* 当前正在编辑哪个字段（"31.23000" 的初值从对应 label 取）*/
static void lat_row_cb(lv_event_t *e)
{
    (void)e;
    /*
     * ★ 必须走这个守卫：`w_card_create_clickable` 造的卡片是**可点**的，
     *   而拖动手势也会冒泡成 CLICKED。项目 v0.6.0 就是因此出现过
     *   "列表里滑一下就误进选项"，修法是统一的 `w_touch_was_drag()` 判定。
     */
    if (w_touch_was_drag()) {
        return;
    }
    const int lat = watch_ui_setting_get(W_SETTING_WX_LAT, 3123000);
    char init[16];
    udeg_to_text(lat, init, sizeof(init));
    if (!w_keyboard_open(ui.parent, W_STR_WX_LAT_LABEL, false, init,
                         lat_done, NULL)) {
        ESP_LOGW(TAG, "键盘打开失败");
    }
}

static void lon_row_cb(lv_event_t *e)
{
    (void)e;
    if (w_touch_was_drag()) {
        return;
    }
    const int lon = watch_ui_setting_get(W_SETTING_WX_LON, 12147000);
    char init[16];
    udeg_to_text(lon, init, sizeof(init));
    if (!w_keyboard_open(ui.parent, W_STR_WX_LON_LABEL, false, init,
                         lon_done, NULL)) {
        ESP_LOGW(TAG, "键盘打开失败");
    }
}

/* ==================== 建界面 ==================== */

/** @brief 造一行"标题 + 右侧可点的值" */
static void value_row(lv_obj_t *parent, const char *title, const char *value,
                      lv_event_cb_t cb, lv_obj_t **out_lb)
{
    lv_obj_t *card = w_card_create_clickable(parent, W_AUTO_POS, W_AUTO_POS,
                                             LV_PCT(100), 46);
    lv_obj_t *t = w_label_create(card, title, watch_font_cn(16), W_COLOR_TEXT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = w_label_create_auto(card, value, W_FONT_NORMAL, 16,
                                      W_COLOR_ACCENT);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    if (out_lb) {
        *out_lb = v;
    }
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
}

static void city_build(lv_obj_t *parent, void *user)
{
    (void)user;

    ui.parent  = parent;
    ui.lb_lat  = NULL;
    ui.lb_lon  = NULL;

    char lat_txt[16], lon_txt[16];
    udeg_to_text(watch_ui_setting_get(W_SETTING_WX_LAT, 3123000),
                 lat_txt, sizeof(lat_txt));
    udeg_to_text(watch_ui_setting_get(W_SETTING_WX_LON, 12147000),
                 lon_txt, sizeof(lon_txt));

    value_row(parent, W_STR_WX_LAT_LABEL, lat_txt, lat_row_cb, &ui.lb_lat);
    value_row(parent, W_STR_WX_LON_LABEL, lon_txt, lon_row_cb, &ui.lb_lon);

    lv_obj_t *hint = w_label_create_auto(parent, W_STR_WX_CITY_HINT,
                                         W_FONT_SMALL, 13, W_COLOR_TEXT_DIM);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    ESP_LOGI(TAG, "城市坐标页就绪：%s, %s", lat_txt, lon_txt);
}

void watch_weather_city_open(void)
{
    w_modal_open(W_STR_WX_CITY_TITLE, city_build, NULL);
}
