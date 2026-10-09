/**
 * @file watch_page_calendar.c
 * @brief 日历（模态子页，从「工具」页进去）—— v0.7.0
 *
 *     ┌──────────────────────────┐
 *     │ 日历              [返回]  │
 *     │ ┌──────────────────────┐ │
 *     │ │  2026年 10月      ▾  │ │  ← LVGL 自带的下拉头部（切年月）
 *     │ │  日 一 二 三 四 五 六 │ │  ← 中文星期名（运行时设置）
 *     │ │  1  2  3  4  5  6  7 │ │
 *     │ │  ...                  │ │  今天高亮
 *     │ └──────────────────────┘ │
 *     └──────────────────────────┘
 *
 * ## 这个页面几乎全是"白送"的
 *
 * `lv_calendar` 是 LVGL 自带控件，而且我们**早就编译进去了**
 * （`CONFIG_LV_USE_CALENDAR=y`，连两种头部都开着）。
 * 参考工程 `fs_calendar.cpp` 一共只有 76 行 —— 它是全工程最便宜的一个功能。
 *
 * ## 两个必须自己处理的点
 *
 * ### 1. 中文星期名：只有 `set_day_names()` 能在运行期改
 *
 * 星期名走 `lv_calendar_set_day_names()`（有运行时 setter）。
 * 但**月份名没有** —— 它来自编译期宏（`LV_CALENDAR_DEFAULT_MONTH_NAMES`），
 * 而 dropdown 头部的月份更是写死的 `"01".."12"` 数字串。
 * 所以这里保持"数字月份 + 中文星期"，不为它去改 LVGL 的宏。
 *
 * ### 2. 今天从哪来：**绝不能用 `time()/localtime()`**
 *
 * 这是界面层的硬约束（不许 include ESP-IDF 头，PC 模拟器要能编；
 * 而且 `time.h` 在这个构建里跨平台类型不一致）。
 * 所以走 `watch_ui_current_epoch()` 自己算日历 ——
 * 和 `watch_page_timeset.c` 里 `tf_load_now()` 是同一套做法，
 * 连闰年规则都照它抄（保持两处一致）。
 *
 *   ⚠️ 拿到的是 **UTC** epoch，要加时区偏移才是本地日期。
 *      时区存的是"15 分钟为单位"的整数（`W_SETTING_TZ`，+8 区 = 32）。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_strings.h"
#include "watch_modal.h"

/*
 * 日历头部的两个函数，这里**用原型声明**而不是 include 头文件。
 *
 * ⚠️ 为什么不 include（踩过）：
 *    它们在 LVGL 源码树的**子目录**里（`src/extra/widgets/calendar/`），
 *    而设备的 lvgl 组件（`esp.cmake` 只暴露 `src/`）和 PC 模拟器的 Makefile
 *    都**没有**把那个子目录加进 include 路径 —— 直接 include 两边都报
 *    "No such file or directory"。LVGL 自己的 .c 能编，是因为它们用相对路径。
 *
 *    与其去改 vendored 的构建脚本（改 esp.cmake 会在上游更新时被覆盖），
 *    不如就地声明 —— 就两个函数，签名很稳定。
 */
#if LV_USE_CALENDAR_HEADER_DROPDOWN
lv_obj_t *lv_calendar_header_dropdown_create(lv_obj_t *parent);
#elif LV_USE_CALENDAR_HEADER_ARROW
lv_obj_t *lv_calendar_header_arrow_create(lv_obj_t *parent);
#endif

/*
 * `lv_calendar_header_dropdown_set_year_list()` 是 **LVGL 8.4 新增**的，
 * 模拟器自带那份是 **8.3**，连实现都没有 —— 所以按版本守卫，
 * 只在设备端（8.4）声明并使用（见文件末尾"年份表"那段说明）。
 */
#if LV_USE_CALENDAR_HEADER_DROPDOWN && \
    (LVGL_VERSION_MAJOR > 8 || (LVGL_VERSION_MAJOR == 8 && LVGL_VERSION_MINOR >= 4))
#define W_CAL_HAVE_YEAR_LIST 1
void lv_calendar_header_dropdown_set_year_list(lv_obj_t *parent,
                                               const char *years_list);
#endif

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "CALENDAR";

/*
 * 星期名（从周日开始，和 lv_calendar 的默认顺序一致）。
 *
 * ★ v0.7.0：改成从 `watch_strings.h` 的 `W_STR_WEEKDAY_x` 取，
 *   而不是在这里再写一份字面量 —— 这样"哪些汉字需要进字库"
 *   只有一处可查（生成脚本扫 watch_strings.h 就够），
 *   以后再加页面也不会漏字。
 */
static const char *s_day_names[7] = {
    W_STR_WEEKDAY_0, W_STR_WEEKDAY_1, W_STR_WEEKDAY_2, W_STR_WEEKDAY_3,
    W_STR_WEEKDAY_4, W_STR_WEEKDAY_5, W_STR_WEEKDAY_6,
};

/**
 * @brief 年份下拉的选项表（**必须降序**，而且必须包含当前年份）
 *
 * ## 为什么不能用 LVGL 的默认表（这里踩过，很隐蔽）
 *
 * 默认表是 `"2023\n2022\n...\n1901"` —— **顶年只到 2023**。
 * 而头部同步下拉时算的是（`lv_calendar_header_dropdown.c:174-192`）：
 *
 *     sel = 列表第一项(2023) - calendar->showed_year(2026) = -3
 *
 * 这个负值赋给 `uint16_t sel_opt` → 变成 65533 → 再被
 * `lv_dropdown_set_selected()` 钳到 `option_cnt - 1` = **最后一项 1901**。
 * 现象就是：日历格子明明是 2026-10、顶部年份却写着 **1901**
 * （截图才发现的，而且中间还被一张旧截图误导过一次）。
 *
 * 所以：**自己给一张覆盖当前年份的降序表**。
 * 范围 2030→1990 对手表够用（70 年，字符串约 200 字节）。
 */
static char s_year_list[256];

static void build_year_list(void)
{
    if (s_year_list[0]) {
        return;                     /* 建一次就够 */
    }
    char *p = s_year_list;
    const char *end = s_year_list + sizeof(s_year_list);
    for (int y = 2030; y >= 1990 && p < end - 6; y--) {
        const int n = snprintf(p, (size_t)(end - p), "%d", y);
        if (n <= 0) {
            break;
        }
        p += n;
        if (y > 1990) {
            *p++ = '\n';            /* 降序、以 \n 分隔 */
        }
    }
    *p = '\0';
}

/** @brief 判断闰年（和设备侧、时间校准页保持同一套规则）*/
static bool is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/**
 * @brief 把 UTC epoch 换算成本地日期
 *
 * @param[out] y/m/d 本地年月日
 * @return true = 时间有效（RTC 已校时），false = 调用方该显示"未校准"
 */
static bool local_ymd(int *y, int *m, int *d)
{
    const uint32_t epoch = watch_ui_current_epoch();
    /*
     * 阈值和 watch_page_timeset.c 一致（2023-11 之后才算有效）。
     * 低于它说明 RTC 掉过电、时间不可信 —— 宁可说不知道，
     * 也不要显示一个错的日期（项目一贯的语义）。
     */
    if (epoch <= 1700000000u) {
        return false;
    }

    /* 时区：15 分钟为单位 → 秒。默认 +8 区（32 × 900 = 28800） */
    const int tz_unit = watch_ui_setting_get(W_SETTING_TZ, 32);
    const int32_t tz_sec = tz_unit * 900;

    int64_t local = (int64_t)epoch + tz_sec;
    if (local < 0) {
        local = 0;                  /* 极端负时区，兜一下 */
    }

    uint32_t days = (uint32_t)(local / 86400);

    int year = 1970;
    for (;;) {
        const int diy = is_leap(year) ? 366 : 365;
        if (days < (uint32_t)diy) {
            break;
        }
        days -= (uint32_t)diy;
        year++;
    }

    static const int mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int mon = 1;
    for (;;) {
        int dim = mdays[mon - 1];
        if (mon == 2 && is_leap(year)) {
            dim = 29;
        }
        if (days < (uint32_t)dim) {
            break;
        }
        days -= (uint32_t)dim;
        mon++;
    }

    *y = year;
    *m = mon;
    *d = (int)days + 1;
    return true;
}

/* ==================== 建界面 ==================== */

static void calendar_build(lv_obj_t *parent, void *user)
{
    (void)user;

    /* 和计算器一样：内容就一块日历，不需要滚动条 */
    if (parent) {
        lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
        lv_obj_t *root = lv_obj_get_parent(parent);
        if (root) {
            lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
        }
    }

    lv_obj_t *cal = lv_calendar_create(parent);
    if (!cal) {
        ESP_LOGW(TAG, "lv_calendar 创建失败（LV_USE_CALENDAR 关了？）");
        return;
    }

    /*
     * 尺寸：屏幕 240 宽、模态内容区 216 宽。
     * 216 宽 / 7 列 ≈ 30 一列，够放两位数日期。
     *
     * ⚠️ 高度留了余量给底部那行"今天 …"：日历 194 + 间隙 + 文字 ≈ 220，
     *    必须塞进模态内容区 ~230（截图里第一版 216 高时，
     *    底部文字被切在屏幕外了）。
     */
    lv_obj_set_size(cal, 216, 194);
    lv_obj_align(cal, LV_ALIGN_TOP_MID, 0, 0);

    /* 中文星期名（运行期可改的那个） */
    lv_calendar_set_day_names(cal, s_day_names);

    /* ---------- 先把"今天"与要显示的月份定下来 ---------- */
    /*
     * ★ 顺序很关键：**必须在创建头部之前设好 showed_date**（这里踩过）。
     *
     *   头部（dropdown）构造时会把年月下拉同步到日历的 showed_date：
     *       `lv_dropdown_set_selected(year_dd, year - cur_date->year)`
     *   （见 lv_calendar_header_dropdown.c:174-192，
     *     其中 `year` 是年份列表的**第一项**，默认 1900）。
     *   所以如果先建头部再设日期，下拉就会停在默认的 1901 ——
     *   日历格子显示的是对的（2026-10），但顶部写着 1901，
     *   看起来像"年份错了"（截图当场发现的）。
     */
    int y = 0, m = 0, d = 0;
    const bool have_time = local_ymd(&y, &m, &d);

    if (have_time) {
        lv_calendar_set_today_date(cal, (uint32_t)y, (uint32_t)m, (uint32_t)d);
        lv_calendar_set_showed_date(cal, (uint32_t)y, (uint32_t)m);
    } else {
        /* RTC 没校过：给个固定月份展示，但**不标今天**（不猜日期）*/
        lv_calendar_set_showed_date(cal, 2026, 1);
    }

    /*
     * 头部：dropdown 还是 arrow。
     *
     * 我们两个宏都开着，LVGL 的 `#if` 里 **dropdown 优先**
     * （和参考工程编译出来的行为一致）。
     * 它自带"点年份/月份弹列表选择"，正是竖屏小屏需要的交互。
     */
#if LV_USE_CALENDAR_HEADER_DROPDOWN
    lv_obj_t *hdr = lv_calendar_header_dropdown_create(cal);
    if (hdr) {
#ifdef W_CAL_HAVE_YEAR_LIST
        /*
         * ★ 换上我们自己的年份表，**然后再让头部重新同步一次下拉**。
         *
         *   头部构造时那次同步用的是 LVGL 的默认年份表
         *   `"2023\n2022\n...\n1901"`（**顶年只到 2023**），
         *   而它算的是 `sel = 列表首项(2023) - showed_year(2026) = -3` ——
         *   负值赋给 uint16_t 会下溢，再被 lv_dropdown_set_selected() 钳到
         *   最后一项 = **1901**。现象就是"日历格子明明是 2026-10、
         *   顶部年份却写着 1901"（截图才发现，中间还被旧截图误导过两次）。
         *
         *   换表后用新表首项（2030）重算：2030-2026 = 4 → 正好指向 2026。
         */
        build_year_list();
        lv_calendar_header_dropdown_set_year_list(hdr, s_year_list);
        lv_event_send(hdr, LV_EVENT_VALUE_CHANGED, NULL);
#else
        /*
         * 模拟器（LVGL 8.3）走这里：8.3 **没有** set_year_list，无法换表，
         * 所以年份下拉会停在默认表的边界（1901）—— 这是**模拟器的预期表现，
         * 不是 bug**，实机（8.4）上是对的。
         * 自检里那条"显示的年月"断言的就是这件事（断言日历内部的值，
         * 不去断言模拟器画不出来的下拉文本）。
         */
#endif
    }
#elif LV_USE_CALENDAR_HEADER_ARROW
    lv_calendar_header_arrow_create(cal);
#endif

    /* 字体：控件默认字体是 montserrat_14（只有 ASCII），
     * 中文星期名会变豆腐块 —— 所以整棵子树换成中文字体。
     * （text_font 是继承属性，设在日历对象上，子对象都跟着变。）
     *
     * ⚠️ 要设在头部**之后**调用才覆盖得到下拉里的文字：
     *    新建的对象会继承当前样式，所以这里再设一次是幂等的，
     *    但放在后面更保险。 */
    lv_obj_set_style_text_font(cal, watch_font_cn(14), 0);

    /* ---------- 底部说明 ---------- */
    if (have_time) {
        lv_obj_t *lb = w_label_create_auto(parent, "", W_FONT_SMALL, 14,
                                           W_COLOR_TEXT_DIM);
        lv_obj_set_width(lb, LV_PCT(100));
        lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
        lv_obj_align(lb, LV_ALIGN_BOTTOM_LEFT, 0, 0);   /* 贴内容区底部，别再往下伸 */
        w_label_set_text_fmt_auto(lb, W_STR_CALENDAR_TODAY_FMT, y, m, d);
        ESP_LOGI(TAG, "日历就绪，今天 %04d-%02d-%02d（本地）", y, m, d);
    } else {
        /*
         * RTC 没校过：不猜日期，直接说明白。
         * （项目语义：宁可说不知道，也不要显示一个错的时间。）
         */
        lv_obj_t *lb = w_label_create_auto(parent, W_STR_CALENDAR_NO_TIME,
                                           W_FONT_SMALL, 14, W_COLOR_WARN);
        lv_obj_set_width(lb, LV_PCT(100));
        lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
        lv_obj_align(lb, LV_ALIGN_BOTTOM_LEFT, 0, 0);   /* 贴内容区底部，别再往下伸 */
        ESP_LOGW(TAG, "RTC 未校时，日历只展示不标注今天");
    }
}

void watch_calendar_open(void)
{
    w_modal_open(W_STR_CALENDAR_TITLE, calendar_build, NULL);
}
