/**
 * @file watch_wheel.c
 * @brief iOS 风格滚轮选择器弹窗实现
 *
 * 手势为什么不打架：见 watch_wheel.h 文件头（已读 LVGL 源码确认）。
 *
 * ## 为什么这里的中文是**字面量**而不是 watch_strings.h 的宏
 *
 * 本文件属于 `watch_ui` 组件，而 `watch_strings.h` 在 `watch_ui_pages` 里 ——
 * **依赖方向是反的，不许 include**（一旦 include，PC 模拟器就编不过）。
 *
 * 这是项目既有约定：`watch_ui` 里需要中文的地方一律直接写字面量
 * （`watch_keyboard.c` 的"空格/退格/完成/取消"、`watch_modal.c` 的"返回"、
 *  `watch_overlay.c` 的"亮度/下滑关闭" 全都是这么写的）。
 * 字库生成脚本扫的是**所有源码里的字符串字面量**，所以照样能进字库。
 *
 * ⚠️ 字面量归字面量，**字体必须用中文字库**：
 *    第一版这两个按钮传了 `W_FONT_NORMAL`（Montserrat，只有 ASCII），
 *    实机上"取消/确定"就是两个豆腐块。
 *    最安全的写法是 font 传 `NULL` —— `w_btn_create()` 会自动用中文字体。
 */
#include "watch_wheel.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_modal.h"

static const char *TAG = "WHEEL";

/* ==================== 内部状态 ==================== */

/*
 * cfg 的静态副本。
 * 模态的 build 回调可能被重跑（w_modal_refresh），所以不能引用调用方的栈变量。
 * 同一时刻只可能存在一个滚轮弹窗（模态栈顶），所以一份就够。
 */
static w_wheel_cfg_t s_cfg;
static bool          s_valid = false;

static lv_obj_t     *s_roller[W_WHEEL_MAX_COLS];

/* ==================================================================
 * 工具
 * ================================================================== */

/** @brief 取某列某位置在合法范围里的值（把 init 夹进 [min,max]） */
static int clamp_col(int col, int v)
{
    if (v < s_cfg.min[col]) return s_cfg.min[col];
    if (v > s_cfg.max[col]) return s_cfg.max[col];
    return v;
}

/**
 * @brief 生成一列的 roller 选项字符串（"00\n01\n...\n59"）
 *
 * 数字补零到固定宽度，滚起来才不会左右跳：
 *   最大值 <= 9   -> 1 位   "0".."9"
 *   最大值 <= 99  -> 2 位   "00".."59"
 *   否则          -> 3 位
 *
 * @note 用静态缓冲：LVGL 任务栈不大，而最坏情况 301 个选项 × 4 字节
 *       ≈ 1.2KB，放栈上有风险（项目里有过"缓冲放栈上撑爆任务栈"的坑）。
 *       `lv_roller_set_options()` 内部会**拷贝**字符串，所以复用一份安全。
 */
static const char *build_options(int col)
{
    static char buf[1400];
    const int lo = s_cfg.min[col];
    const int hi = s_cfg.max[col];
    const int width = (hi <= 9) ? 1 : ((hi <= 99) ? 2 : 3);

    int n = 0;
    for (int v = lo; v <= hi && n < (int)sizeof(buf) - 8; v++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%0*d", width, v);
        if (v < hi) {
            buf[n++] = '\n';
        }
    }
    if (n < (int)sizeof(buf)) {
        buf[n] = '\0';
    } else {
        buf[sizeof(buf) - 1] = '\0';
    }
    return buf;
}

/* ==================================================================
 * 按钮回调
 * ================================================================== */

/** @brief "取消"：直接弹栈 */
static void cancel_cb(lv_event_t *e)
{
    (void)e;
    w_modal_close();
}

/**
 * @brief "确定"：收集各列的值 -> 回调 -> 关弹窗
 *
 * 顺序很重要：先回调（写设置 / 改状态），再 w_modal_close()。
 * w_modal_close() 会自动重建上一层页面，所以上一层的显示是最新的。
 */
static void ok_cb(lv_event_t *e)
{
    (void)e;

    int val[W_WHEEL_MAX_COLS] = { 0 };
    for (int i = 0; i < s_cfg.cols; i++) {
        if (s_roller[i]) {
            /* get_selected 在 infinite 模式下返回的是"逻辑下标"（已取模） */
            val[i] = s_cfg.min[i] + (int)lv_roller_get_selected(s_roller[i]);
        }
    }

    ESP_LOGI(TAG, "确定：%d/%d/%d（%d 列）",
             val[0], s_cfg.cols > 1 ? val[1] : 0,
             s_cfg.cols > 2 ? val[2] : 0, s_cfg.cols);

    if (s_cfg.on_ok) {
        s_cfg.on_ok(val, s_cfg.user);
    }
    w_modal_close();
}

/* ==================================================================
 * 构建
 * ================================================================== */

/** @brief 造一列：单位标签 + 滚轮 */
static void build_column(lv_obj_t *row, int col, lv_coord_t w)
{
    lv_obj_t *box = lv_obj_create(row);
    /* 高度用内容撑开：滚轮的 3 行高度是"行高+行距"算出来的，
     * 写死数字迟早对不上（见下面关于无限模式标签高度的说明）。 */
    lv_obj_set_size(box, w, LV_SIZE_CONTENT);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_NONE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 单位标签（中文，必须走中文字库） */
    if (s_cfg.unit[col]) {
        lv_obj_t *u = w_label_create(box, s_cfg.unit[col], watch_font_cn(14),
                                     W_COLOR_TEXT_DIM);
        (void)u;
    }

    lv_obj_t *r = lv_roller_create(box);

    /*
     * 行距必须**显式设置**，不能靠主题默认值。
     *
     * 为什么这里这么讲究（v0.5.0 连踩两个坑，靠模拟器截图 + 几何日志才查清）：
     *
     * 1) 无限模式把选项串重复 `LV_ROLLER_INF_PAGES` 遍塞进内部 label
     *    （lv_roller.c:122-130）。于是
     *        标签高度 = 选项数 × 页数 × (行高 + 行距)
     *    分/秒轮有 60 个选项；页数 7、行距 16（主题默认）时
     *        60 × 7 × (22 + 16) = 15960 px
     *    而 `LV_USE_LARGE_COORD=0` 时 lv_coord_t 是 int16，
     *    LV_COORD_MAX = 8191 -> 标签高度被截断，
     *    表现是"高亮框里的数字和实际选中的不是同一个"
     *    （实测选中 02 却显示 17；日志里标签高度正好是 8191）。
     *    -> 已在 sdkconfig.defaults / lvgl_sim/lv_conf.h 把页数改成 3，
     *       这里再把行距固定成 8：60 × 3 × 30 = 5400，余量充足。
     *
     * 2) `refr_position()` 用 "行高 + 行距" 算每行的位置，
     *    所以行距是它和标签排版**共用的**参数；写死一个明确值，
     *    就不会因为主题变了而"某天突然错位"。
     */
    lv_obj_set_style_text_line_space(r, 8, LV_PART_MAIN);

    lv_roller_set_options(r, build_options(col), LV_ROLLER_MODE_INFINITE);
    /* 高度由 LVGL 自己算 = (行高 + 行距) × 3，**不要再覆盖它**
     * —— 覆盖了它内部的"选中行居中"计算就和实际高度对不上。 */
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, LV_PCT(100));

    /*
     * ---- 样式：未选中暗、选中高亮成主色 ----
     *
     * PART_MAIN 和 PART_SELECTED 必须用**同一个字号**！
     *
     * 踩过的坑（模拟器截图看出来的）：给未选中用 16px、选中用 20px 时，
     * 高亮框里显示的数字和实际选中的不是同一个。
     *
     * 根因在 LVGL 内部（读过源码）：
     *   · `refr_position()`（lv_roller.c:627）用
     *     `id * (font_h + line_space)` 算标签该滚到哪，font_h 取 PART_MAIN；
     *   · `get_sel_area()`（:578）算高亮框高度用的是
     *     `(font_sel_h + font_main_h) / 2 + line_space`，混用了两个字体。
     *   两个字号不同时，行距和高亮框高度就对不上。
     *
     * 统一字号后两者一致；选中态靠颜色和底色区分，视觉上反而更干净
     * （240×280 上也不该让行高忽大忽小）。
     */
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(r, W_FONT_MEDIUM, LV_PART_MAIN);
    lv_obj_set_style_text_color(r, W_COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_set_style_bg_color(r, W_COLOR_CARD_HI, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_radius(r, W_RADIUS_BTN, LV_PART_SELECTED);
    lv_obj_set_style_text_font(r, W_FONT_MEDIUM, LV_PART_SELECTED);   /* 同上 */
    lv_obj_set_style_text_color(r, W_COLOR_ACCENT, LV_PART_SELECTED);

    /*
     * 先强制算一次布局，再设选中项。
     *
     * 为什么必须这样（实测踩到的坑）：
     *   `lv_roller_set_selected()` 内部会调 `refr_position()`，而它用
     *       h = lv_obj_get_content_height(roller)
     *   来算"标签要滚到哪一行才居中"。这个高度是坐标级的，
     *   布局没跑过时返回 0 —— 于是偏移算成 `0/2 - font_h/2 - id*行距`，
     *   整体差半行，而且是静默的（画面上只是高亮框里的数字不对、
     *   或者高亮行被上下切掉一半）。
     *   实测日志：滚轮高 0、标签 y=13（三列完全一样，明显没算对）。
     *
     *   布局跑过之后高度才是真实的，此时再设选中项才能算对居中偏移。
     *   （LVGL 只在尺寸变化时补发 LV_EVENT_SIZE_CHANGED 去重算位置，
     *     而这里尺寸在创建时就定好了，不会再有那个事件 —— 必须显式 update。）
     */
    lv_obj_update_layout(r);

    /* 初值：set_selected 传的是"本页内的逻辑下标"（LVGL 会自己折算到中页） */
    lv_roller_set_selected(r, (uint16_t)(clamp_col(col, s_cfg.init[col])
                                         - s_cfg.min[col]),
                           LV_ANIM_OFF);
    /* 再算一次：这回是拿真实高度定标签位置 */
    lv_obj_update_layout(r);

    s_roller[col] = r;

    /*
     * 几何自检日志。
     *
     * 为什么值得常驻：滚轮的"选中行居中"是 LVGL 内部用高度和字体行高
     * 算出来的（refr_position / get_sel_area），一旦我们改动了它的尺寸或字体，
     * 极容易出现"高亮框罩住的不是选中那一行"——而画面上只是数字不对，
     * 不报错、不崩溃，非常难查（v0.5.0 就栽在这上面）。
     * 把这几个数打出来，一眼能判断对不对：
     *     标签y 应 ≈ (滚轮高/2 - 行高/2) - 选中内部id × (行高 + 行距)
     *   其中"选中内部id"在无限模式下 = (页数/2) × 选项数 + 逻辑下标。
     */
    {
        lv_obj_t *lbl = lv_obj_get_child(r, 0);
        const lv_font_t *f = lv_obj_get_style_text_font(r, LV_PART_MAIN);
        ESP_LOGI(TAG, "col%d 选项%d 选中%d 滚轮高%d 标签高%d 标签y%d 行高%d 行距%d",
                 col, (int)lv_roller_get_option_cnt(r),
                 (int)lv_roller_get_selected(r),
                 (int)lv_obj_get_height(r),
                 lbl ? (int)lv_obj_get_height(lbl) : -1,
                 lbl ? (int)lv_obj_get_y(lbl) : -9999,
                 (int)lv_font_get_line_height(f),
                 (int)lv_obj_get_style_text_line_space(r, LV_PART_MAIN));
    }
}

/** @brief 模态构建回调 */
static void wheel_build(lv_obj_t *parent, void *user)
{
    (void)user;

    memset(s_roller, 0, sizeof(s_roller));

    if (!s_valid) {
        ESP_LOGE(TAG, "配置已失效，无法构建滚轮");
        return;
    }
    const int cols = s_cfg.cols;

    /* ---------- 滚轮行（高度由内容撑开）---------- */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 模态 body 宽度 = 240 - 2*12 = 216；n 列 + (n-1)*6 间隙 */
    const lv_coord_t col_w = (216 - (cols - 1) * 6) / cols;
    for (int i = 0; i < cols; i++) {
        build_column(row, i, col_w);
    }

    /* ---------- 取消 / 确定 ---------- */
    lv_obj_t *btns = lv_obj_create(parent);
    lv_obj_set_size(btns, LV_PCT(100), 44);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(btns, LV_DIR_NONE);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btns, 0, 0);
    lv_obj_set_style_pad_all(btns, 0, 0);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 两个按钮各占一半：216 宽 - 8 间隙 = 208，各 104。
     * 字体传 NULL -> w_btn_create() 用中文字库。
     * 第一版传的是 W_FONT_NORMAL（Montserrat，只有 ASCII），
     * 实机上"取消/确定"就是两个豆腐块（和项目里踩过四次的坑同类）。 */
    const lv_coord_t btn_w = (216 - 8) / 2;
    w_btn_create(btns, "取消", btn_w, 0,
                 W_COLOR_CARD, W_COLOR_TEXT, NULL, cancel_cb);
    w_btn_create(btns, "确定", btn_w, 0,
                 W_COLOR_ACCENT, W_COLOR_BG, NULL, ok_cb);
}

/* ==================================================================
 * 对外
 * ================================================================== */

int w_wheel_debug_get(int *val, int max_n)
{
    if (!s_valid || !val || max_n <= 0) {
        return -1;
    }
    int n = 0;
    for (int i = 0; i < s_cfg.cols && n < max_n; i++) {
        if (!s_roller[i]) {
            return n;
        }
        val[n++] = s_cfg.min[i] + (int)lv_roller_get_selected(s_roller[i]);
    }
    return n;
}

int w_wheel_open(const w_wheel_cfg_t *cfg)
{
    if (!cfg || !cfg->on_ok) {
        ESP_LOGE(TAG, "配置非法（cfg 为空或没给 on_ok）");
        return -1;
    }
    if (cfg->cols < 1 || cfg->cols > W_WHEEL_MAX_COLS) {
        ESP_LOGE(TAG, "列数非法：%d（允许 1~%d）", cfg->cols, W_WHEEL_MAX_COLS);
        return -1;
    }
    for (int i = 0; i < cfg->cols; i++) {
        if (cfg->max[i] < cfg->min[i]) {
            ESP_LOGE(TAG, "第 %d 列范围非法：%d..%d", i, cfg->min[i], cfg->max[i]);
            return -1;
        }
    }

    s_cfg   = *cfg;         /* 浅拷贝：unit[] 指向字面量，生命周期没问题 */
    s_valid = true;

    const int r = w_modal_open(cfg->title, wheel_build, NULL);
    if (r != 0) {
        /* 模态栈满：把状态清掉，免得下次 build 用到脏配置 */
        s_valid = false;
    }
    return r;
}
