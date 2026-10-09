/**
 * @file watch_page_weather.c
 * @brief 天气页（工具页进去）—— v0.7.0
 *
 *     ┌──────────────────────────┐
 *     │ 天气              [返回]  │
 *     │ ┌──────────────────────┐ │
 *     │ │ 10/09  ☀  晴   18~26 │ │  ← 7 天，一行一天
 *     │ │ 10/10  ☁  多云 17~24 │ │
 *     │ │ …                    │ │
 *     │ └──────────────────────┘ │
 *     │ [获取天气]   [城市设置]   │
 *     └──────────────────────────┘
 *
 * ## 数据从哪来（★ 界面层不碰网络）
 *
 * 全部经 `w_platform_weather()` 拿钩子：
 *   `refresh()`    触发抓取（非阻塞）
 *   `get_status()` 取快照（服务内加锁）
 *   `wmo_class()`  WMO 代码 → 图标类别
 *
 * 钩子为空（PC 模拟器 / 服务没起）时显示"天气不可用"，
 * **不是一个空白页** —— 项目一贯要求"钩子缺失要能优雅降级"。
 *
 * ## 图标：图元自绘，不引位图
 *
 * 参考工程的天气图标是 **10 张 100×100 未压缩 RGB565 位图 = 200KB**
 * （`resources/weather.h`）。我们不引：
 *   ① 省 200KB flash；② 那批像素是**无 LICENSE 项目里的图像资产**，不能用。
 * 改用 LVGL 图元画 7 类图标（晴/多云/阴/雾/雨/雪/雷），
 * 每类就是几个圆、线、矩形，代码几十行，零资源占用。
 *
 * ## 刷新：轮询，不是回调
 *
 * 天气任务在另一个任务里跑（HTTP 是阻塞的），完成后只改内部状态。
 * 界面这边用 `lv_timer` 每 500ms 看一眼状态 —— 这和 WiFi 页、
 * 时钟页的做法一致（**不在别的任务里碰 lv_\***）。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_strings.h"
#include "watch_modal.h"
#include "watch_toast.h"
#include "watch_platform.h"     /* ★ w_weather_hooks_t / w_platform_weather() */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "WXPAGE";

/** 轮询周期：天气任务最多跑 8 秒（HTTP 超时），500ms 够灵敏也不费电 */
#define WX_POLL_MS   500

/*
 * ==================== 布局（★ 这几个数必须加起来不超内容区）====================
 *
 * 模态内容区的可用高度（**从 watch_modal.c 读出来的**，别自己推算）：
 *
 *     280（屏高） - 2×W_PAD_PAGE(12) - TITLE_H(34) = **222**
 *
 * 三块内容分掉它：
 *
 *     STATUS_H(22) + LIST_H(170) + BTN_H(30) = 222  ✅
 *
 * 列表 7 行：`7 × WX_ROW_H(23) + 6 × WX_ROW_GAP(1) = 167 ≤ 170` ✅
 *
 * ⚠️ 这一页**必须一屏放下**：它没有滚动容器，超出就是被切掉
 *    （第一版 7 行 30px + 216 列表，最后两行被切在屏幕外，截图才发现）。
 *
 * ★ 两个教训（都踩过）：
 *   1. **别按"屏高 − padding"推算内容区**：第一版我按
 *      `280 - 2×12 - 26` 算成 230，而 `TITLE_H` 实际是 **34**，
 *      于是预算多给了 8px。要么去读 `watch_modal.c` 的 `TITLE_H`，
 *      要么按自检量出来的实际值反推。
 *   2. **几何问题断言抓不到，只有截图能发现** —— 这条就是看截图才发现被切的。
 */
#define WX_STATUS_H   22
#define WX_LIST_H     170
#define WX_ROW_H      23
#define WX_ROW_GAP    1
#define WX_BTN_H      30

/* ==================== 状态 ==================== */
static struct {
    lv_obj_t   *list;       /* 7 天列表容器 */
    lv_obj_t   *status;     /* 状态行（加载中 / 失败 / 未设置）*/
    lv_obj_t   *btn_get;    /* "获取天气"按钮（抓取中要禁用）*/
    lv_timer_t *poll;       /* 轮询定时器 */
    bool        last_fetching;
    bool        last_valid;
    bool        last_failed;
} ui;

/* ==================== 图元自绘图标 ==================== */

/**
 * @brief 画一个天气图标
 *
 * @param parent 父对象
 * @param cls    `wmo_class()` 给的类别：0 晴 1 多云 2 阴 3 雾 4 雨 5 雪 6 雷
 * @param size   图标边长（像素）
 *
 * @note 全部用基础图形拼，不引任何位图资源。颜色用主题色 + 强调色。
 */
static lv_obj_t *icon_create(lv_obj_t *parent, int cls, int size)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, size, size);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    const int s = size;
    const int cx = s / 2;

    /* ---- 太阳：一个实心圆 + 四根短线 ---- */
    if (cls == 0 || cls == 1) {
        lv_obj_t *sun = lv_obj_create(box);
        lv_obj_set_size(sun, s / 2, s / 2);
        lv_obj_set_style_radius(sun, s / 4, 0);
        lv_obj_set_style_bg_color(sun, W_COLOR_WARN, 0);
        lv_obj_set_style_bg_opa(sun, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sun, 0, 0);
        lv_obj_clear_flag(sun, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(sun, LV_OBJ_FLAG_CLICKABLE);
        /* 晴天居中；多云时偏左上，给云留位置 */
        if (cls == 0) {
            lv_obj_center(sun);
        } else {
            lv_obj_align(sun, LV_ALIGN_TOP_LEFT, 1, 1);
        }
    }

    /* ---- 云：两个横条叠出来 ---- */
    if (cls == 1 || cls == 2 || cls == 4 || cls == 5 || cls == 6) {
        lv_obj_t *c1 = lv_obj_create(box);
        lv_obj_set_size(c1, s * 3 / 4, s / 3);
        lv_obj_set_style_radius(c1, s / 6, 0);
        lv_obj_set_style_bg_color(c1,
                                  (cls == 2) ? W_COLOR_TEXT_DIM : W_COLOR_TEXT, 0);
        lv_obj_set_style_bg_opa(c1, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c1, 0, 0);
        lv_obj_clear_flag(c1, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(c1, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(c1, LV_ALIGN_BOTTOM_MID, 0, -s / 6);

        lv_obj_t *c2 = lv_obj_create(box);
        lv_obj_set_size(c2, s / 2, s / 3);
        lv_obj_set_style_radius(c2, s / 6, 0);
        lv_obj_set_style_bg_color(c2, W_COLOR_TEXT, 0);
        lv_obj_set_style_bg_opa(c2, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c2, 0, 0);
        lv_obj_clear_flag(c2, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(c2, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(c2, LV_ALIGN_BOTTOM_LEFT, s / 6, -s / 3);
    }

    /* ---- 阴：一块盖满的暗色 ---- */
    if (cls == 2) {
        lv_obj_t *dark = lv_obj_create(box);
        lv_obj_set_size(dark, s * 4 / 5, s / 4);
        lv_obj_set_style_radius(dark, s / 8, 0);
        lv_obj_set_style_bg_color(dark, W_COLOR_TEXT_MUTE, 0);
        lv_obj_set_style_bg_opa(dark, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dark, 0, 0);
        lv_obj_clear_flag(dark, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dark, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dark, LV_ALIGN_TOP_MID, 0, 1);
    }

    /* ---- 雾：三条横线 ---- */
    if (cls == 3) {
        for (int i = 0; i < 3; i++) {
            lv_obj_t *ln = lv_obj_create(box);
            lv_obj_set_size(ln, s * 3 / 4, 2);
            lv_obj_set_style_bg_color(ln, W_COLOR_TEXT_DIM, 0);
            lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(ln, 0, 0);
            lv_obj_set_style_radius(ln, 1, 0);
            lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(ln, LV_ALIGN_TOP_MID, 0, s / 4 + i * (s / 5));
        }
    }

    /* ---- 雨：三根短竖线（云下面）---- */
    if (cls == 4) {
        for (int i = 0; i < 3; i++) {
            lv_obj_t *dr = lv_obj_create(box);
            lv_obj_set_size(dr, 2, s / 5);
            lv_obj_set_style_bg_color(dr, W_COLOR_ACCENT, 0);
            lv_obj_set_style_bg_opa(dr, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(dr, 0, 0);
            lv_obj_set_style_radius(dr, 1, 0);
            lv_obj_clear_flag(dr, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(dr, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(dr, LV_ALIGN_BOTTOM_LEFT,
                         s / 5 + i * (s / 5), 0);
        }
    }

    /* ---- 雪：三个小点 ---- */
    if (cls == 5) {
        for (int i = 0; i < 3; i++) {
            lv_obj_t *fl = lv_obj_create(box);
            lv_obj_set_size(fl, 3, 3);
            lv_obj_set_style_radius(fl, 2, 0);
            lv_obj_set_style_bg_color(fl, W_COLOR_TEXT, 0);
            lv_obj_set_style_bg_opa(fl, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(fl, 0, 0);
            lv_obj_clear_flag(fl, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(fl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(fl, LV_ALIGN_BOTTOM_LEFT, s / 5 + i * (s / 5), 0);
        }
    }

    /* ---- 雷：一根折线（用两个斜放的短条拼）---- */
    if (cls == 6) {
        lv_obj_t *b1 = lv_obj_create(box);
        lv_obj_set_size(b1, 3, s / 3);
        lv_obj_set_style_bg_color(b1, W_COLOR_WARN, 0);
        lv_obj_set_style_bg_opa(b1, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b1, 0, 0);
        lv_obj_set_style_radius(b1, 1, 0);
        lv_obj_clear_flag(b1, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(b1, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(b1, LV_ALIGN_BOTTOM_MID, -2, 0);
        lv_obj_set_style_transform_angle(b1, 300, 0);   /* 约 26° */

        lv_obj_t *b2 = lv_obj_create(box);
        lv_obj_set_size(b2, 3, s / 3);
        lv_obj_set_style_bg_color(b2, W_COLOR_WARN, 0);
        lv_obj_set_style_bg_opa(b2, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b2, 0, 0);
        lv_obj_set_style_radius(b2, 1, 0);
        lv_obj_clear_flag(b2, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(b2, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(b2, LV_ALIGN_BOTTOM_MID, 2, -s / 4);
        lv_obj_set_style_transform_angle(b2, 3300, 0);  /* 反向 */
    }

    (void)cx;
    return box;
}

/* ==================== 天气描述文字 ==================== */

/** @brief 图标类别 → 中文描述（两个字，省地方）*/
static const char *class_word(int cls)
{
    switch (cls) {
    case 0: return W_STR_WX_SUNNY;
    case 1: return W_STR_WX_CLOUDY;
    case 2: return W_STR_WX_OVERCAST;
    case 3: return W_STR_WX_FOG;
    case 4: return W_STR_WX_RAIN;
    case 5: return W_STR_WX_SNOW;
    case 6: return W_STR_WX_STORM;
    default: return W_STR_WX_UNKNOWN;
    }
}

/* ==================== 渲染 7 天 ==================== */

static void render_days(const w_weather_status_t *st)
{
    if (!ui.list) {
        return;
    }
    lv_obj_clean(ui.list);

    const w_weather_hooks_t *h = w_platform_weather();
    if (!h || !st || !st->valid || st->count == 0) {
        return;                     /* 状态行已经在说原因了 */
    }

    for (int i = 0; i < st->count && i < W_WEATHER_DAYS; i++) {
        const w_weather_day_t *d = &st->days[i];

        lv_obj_t *row = lv_obj_create(ui.list);
        lv_obj_set_size(row, LV_PCT(100), WX_ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

        /* 第一行（今天）用强调色，一眼能看出"这是今天" */
        const lv_color_t c = (i == 0) ? W_COLOR_ACCENT : W_COLOR_TEXT;

        lv_obj_t *date = w_label_create_auto(row, d->date, W_FONT_SMALL, 14, c);
        lv_obj_align(date, LV_ALIGN_LEFT_MID, 0, 0);

        /* 图标（图元自绘）*/
        const int cls = h->wmo_class ? h->wmo_class(d->wmo) : 1;
        lv_obj_t *ic = icon_create(row, cls, 22);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 44, 0);

        lv_obj_t *w = w_label_create_auto(row, class_word(cls),
                                          W_FONT_SMALL, 14, W_COLOR_TEXT_DIM);
        lv_obj_align(w, LV_ALIGN_LEFT_MID, 74, 0);

        /* 温度：最高/最低，右对齐 */
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%d~%d", (int)d->tmax, (int)d->tmin);
        lv_obj_t *t = w_label_create_auto(row, tbuf, W_FONT_SMALL, 14, W_COLOR_TEXT);
        lv_obj_align(t, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

/** @brief 刷新状态行 + 按钮可用性 */
static void render_status(const w_weather_status_t *st)
{
    if (!ui.status) {
        return;
    }

    const w_weather_hooks_t *h = w_platform_weather();
    if (!h) {
        w_label_set_text_auto(ui.status, W_STR_WX_UNAVAILABLE);
        lv_obj_set_style_text_color(ui.status, W_COLOR_WARN, 0);
        return;
    }

    if (st->fetching) {
        w_label_set_text_auto(ui.status, W_STR_WX_FETCHING);
        lv_obj_set_style_text_color(ui.status, W_COLOR_WARN, 0);
    } else if (st->failed) {
        w_label_set_text_auto(ui.status, W_STR_WX_FAILED);
        lv_obj_set_style_text_color(ui.status, W_COLOR_DANGER, 0);
    } else if (!st->valid) {
        w_label_set_text_auto(ui.status, W_STR_WX_NO_DATA);
        lv_obj_set_style_text_color(ui.status, W_COLOR_TEXT_DIM, 0);
    } else {
        w_label_set_text_auto(ui.status, W_STR_WX_CACHED_HINT);
        lv_obj_set_style_text_color(ui.status, W_COLOR_TEXT_MUTE, 0);
    }

    /* 抓取中禁用按钮，防止连点（服务侧也会挡，但界面要看得出来）*/
    if (ui.btn_get) {
        if (st->fetching) {
            lv_obj_add_state(ui.btn_get, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(ui.btn_get, LV_STATE_DISABLED);
        }
    }
}

/** @brief 读一次状态并刷新界面（只在"真的变了"时重建列表，省重绘）*/
static void refresh_ui(bool force)
{
    const w_weather_hooks_t *h = w_platform_weather();
    if (!h || !h->get_status) {
        render_status(NULL);
        return;
    }

    w_weather_status_t st;
    h->get_status(&st);

    const bool changed = force ||
                         (st.fetching != ui.last_fetching) ||
                         (st.valid    != ui.last_valid) ||
                         (st.failed   != ui.last_failed);

    if (changed) {
        ui.last_fetching = st.fetching;
        ui.last_valid    = st.valid;
        ui.last_failed   = st.failed;
        render_status(&st);
        render_days(&st);
    }
}

/* ==================== 事件 ==================== */

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    refresh_ui(false);
}

static void get_cb(lv_event_t *e)
{
    (void)e;
    const w_weather_hooks_t *h = w_platform_weather();
    if (!h || !h->refresh) {
        return;
    }
    /*
     * 用户点了"获取"必须有**立刻可见**的反馈 ——
     * 否则 HTTP 要跑几秒，界面看着像没反应（项目反复踩过的坑）。
     */
    if (h->refresh() != 0) {
        watch_toast_show(W_STR_WX_BUSY);
        return;
    }
    watch_toast_show(W_STR_WX_FETCHING);
    refresh_ui(true);
}

static void city_cb(lv_event_t *e)
{
    (void)e;
    watch_weather_city_open();
}

/* ==================== 建界面 ==================== */

/** @brief 页面被删时一定要删轮询定时器（★ WiFi 页那个 P0 的同款教训）*/
static void page_delete_cb(lv_event_t *e)
{
    (void)e;
    if (ui.poll) {
        lv_timer_del(ui.poll);
        ui.poll = NULL;
    }
    ui.list   = NULL;
    ui.status = NULL;
    ui.btn_get = NULL;
    ESP_LOGD(TAG, "天气页已销毁，轮询定时器已删");
}

static void weather_build(lv_obj_t *parent, void *user)
{
    (void)user;

    lv_obj_remove_event_cb(parent, page_delete_cb);
    lv_obj_add_event_cb(parent, page_delete_cb, LV_EVENT_DELETE, NULL);

    /* 旧的轮询定时器先杀掉（重建时）*/
    if (ui.poll) {
        lv_timer_del(ui.poll);
        ui.poll = NULL;
    }
    ui.list = NULL;
    ui.status = NULL;
    ui.btn_get = NULL;
    ui.last_fetching = false;
    ui.last_valid    = false;
    ui.last_failed   = false;

    /* ---------- 状态行 ---------- */
    ui.status = w_label_create_auto(parent, "", W_FONT_SMALL, 13, W_COLOR_TEXT_DIM);
    lv_obj_set_width(ui.status, LV_PCT(100));
    lv_obj_set_height(ui.status, WX_STATUS_H);
    lv_label_set_long_mode(ui.status, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui.status, LV_ALIGN_TOP_LEFT, 0, 0);

    /* ---------- 7 天列表（高度按 7 行精确算过，见文件头的布局说明）---------- */
    ui.list = w_flex_col_create(parent, 0, WX_STATUS_H, LV_PCT(100),
                                WX_LIST_H, WX_ROW_GAP);
    lv_obj_set_scrollbar_mode(ui.list, LV_SCROLLBAR_MODE_OFF);

    /* ---------- 按钮 ---------- */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), WX_BTN_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, WX_STATUS_H + WX_LIST_H);

    ui.btn_get = w_btn_create(row, W_STR_WX_GET, 100, WX_BTN_H, W_COLOR_CARD,
                              W_COLOR_TEXT, watch_font_cn(14), get_cb);
    lv_obj_align(ui.btn_get, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *city = w_btn_create(row, W_STR_WX_CITY, 100, WX_BTN_H, W_COLOR_CARD,
                                  W_COLOR_TEXT, watch_font_cn(14), city_cb);
    lv_obj_align(city, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ---------- 首次渲染 + 起轮询 ---------- */
    refresh_ui(true);
    ui.poll = lv_timer_create(poll_cb, WX_POLL_MS, NULL);

    ESP_LOGI(TAG, "天气页就绪");
}

void watch_weather_open(void)
{
    w_modal_open(W_STR_WX_TITLE, weather_build, NULL);
}
