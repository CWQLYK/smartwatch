/**
 * @file watch_toast.c
 * @brief 通用顶部提示条实现 —— 见 watch_toast.h 的设计说明
 *
 * ## 这个文件里最重要的一个决定：**对象常驻，永不删除**
 *
 * 参考工程（`Watch-Player_IDF/ui_manager.cpp`）的提示条是"每次显示新建对象、
 * 离场动画的 `ready_cb` 里删对象"。它因此在**离场那 200ms 窗口**里有一个真 bug：
 * `hide()` 一进来就把计时器删掉置 nullptr，而对象要等动画 ready 才删；
 * 这段时间内再来一条提示 → 走"已存在就早退"分支 → 文字改了、计时器空转、
 * 也没有反向滑入动画，**而已在跑的离场动画照样把容器删掉**
 * → 新提示一闪即逝、状态归零、再也不会显示。
 *
 * 我们换一条从根本上绕开这类问题的路：
 *
 *   1. **对象只建一次，永不删除** → 没有删除就没有 use-after-free、
 *      没有"同一棵树排两个 `lv_async_call`"（项目踩过，必炸）、
 *      也没有"清理只挂在 `ready_cb` 上是不可靠的"这个问题
 *      （`lv_anim_del()` 只调 `deleted_cb`、**绝不调** `ready_cb`，见 lv_anim.c:137-153）。
 *   2. 用一个**显式状态机**（而不是"指针非空就早退"）处理三种重入。
 *   3. 隐藏不靠 `LV_OBJ_FLAG_HIDDEN`（那会连带隐藏子对象、动画期间看着像没动），
 *      而是把整个容器**平移到屏幕外**——同时天然不参与触摸命中判定。
 */
#include "watch_toast.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "esp_log.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_fonts.h"
#include "watch_alert.h"    /* watch_alert_visible()：提醒层可见时丢弃提示 */

static const char *TAG = "TOAST";

/* ==================== 参数 ==================== */
#define TOAST_ANIM_MS     200    /* 滑入/滑出时长（对标参考工程实测值） */
#define TOAST_HOLD_MS    1500    /* 完全可见的保持时长（同上） */
#define TOAST_MAX_W       216    /* 药丸最大宽（240 屏，两侧各留 12）*/
#define TOAST_PAD_H        10    /* 水平内边距：和 W_PAD_CARD 一致 */
#define TOAST_PAD_V         8    /* 垂直内边距：比卡片紧一点，一行更矮 */
#define TOAST_CJK_SIZE     16    /* 中文正文字号（我们只有 14/16/20 三档）*/

/* ==================== 状态 ==================== */

/** @brief 提示条状态机（不用"指针非空"代替状态，避免参考工程那类竞态） */
typedef enum {
    TOAST_IDLE = 0,   /* 屏幕外，空闲           */
    TOAST_SHOWING,    /* 正在滑入               */
    TOAST_HOLDING,    /* 完全可见，正在计时消失 */
    TOAST_HIDING,     /* 正在滑出               */
} toast_state_t;

static lv_obj_t    *s_root;    /* 建在 lv_layer_top() 上的容器（常驻）*/
static lv_obj_t    *s_pill;    /* 可见的药丸                          */
static lv_obj_t    *s_label;   /* 文案                                */
static lv_timer_t  *s_hold;    /* 保持计时（常驻，空闲时 period=0）    */
static toast_state_t s_state = TOAST_IDLE;
static int          s_off_y;   /* 屏幕外那一档位移（>= 药丸高度）      */

/** @brief 屏幕外位移的兜底值（量不到高度时用）*/
#define TOAST_OFF_FALLBACK  48

/* ==================== 动画 ==================== */

/**
 * @brief 让整棵提示条在竖直方向平移
 *
 * ## ★ 为什么用 `translate_y` 而不是 `lv_obj_set_y()`（踩过，截图+逐帧采样才定位到）
 *
 * 第一版用的是 `lv_obj_set_y(s_root, ...)` + `lv_obj_align(s_root, TOP_MID, 0, 0)`。
 * 结果：`show()` 返回瞬间 `y == 0`（对的），**但下一次 `lv_timer_handler` 一跑，
 * y 就跳回 `-35`**，提示条永远停在屏幕外。
 *
 * 根因：`s_root` 是 `LV_SIZE_CONTENT` 高、又带着 `LV_ALIGN_TOP_MID` 的对齐标志。
 * 布局重算时 LVGL 会**按对齐标志重新摆放**它，把动画刚设好的 y 覆盖掉 ——
 * 也就是"动画位移"和"对齐/布局"在同一个对象上打架。
 *
 * `translate_y` 是**样式属性（transform）**，不参与布局、也不会被对齐覆盖，
 * 所以拿它做"从屏幕外滑入"是稳的。
 */
static void anim_translate_cb(void *var, int32_t v)
{
    if (!lv_obj_is_valid((lv_obj_t *)var)) {
        return;
    }
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

/** @brief 滑入动画结束：进入"保持中"（计时从这一刻才开始）*/
static void anim_in_ready_cb(lv_anim_t *a)
{
    if (a && a->var != s_root) {
        return;                     /* 不是我们这一代动画（已被替换）*/
    }
    if (s_state != TOAST_SHOWING) {
        return;                     /* 中途被 show/hide 改过状态，别覆盖 */
    }
    s_state = TOAST_HOLDING;
    if (s_hold) {
        lv_timer_reset(s_hold);
        lv_timer_resume(s_hold);
    }
    ESP_LOGD(TAG, "已滑入，保持 %d ms", TOAST_HOLD_MS);
}

/** @brief 滑出动画结束：回到空闲 */
static void anim_out_ready_cb(lv_anim_t *a)
{
    if (a && a->var != s_root) {
        return;
    }
    if (s_state != TOAST_HIDING) {
        return;
    }
    s_state = TOAST_IDLE;
    if (s_root) {
        /* 藏起来：整棵树 HIDDEN，既不渲染也不参与触摸判定 */
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGD(TAG, "已滑出，空闲");
}

static void slide_to(int32_t target, lv_anim_ready_cb_t ready_cb)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_root);
    lv_anim_set_exec_cb(&a, anim_translate_cb);
    lv_anim_set_values(&a, lv_obj_get_style_translate_y(s_root, 0), target);
    lv_anim_set_time(&a, TOAST_ANIM_MS);
    /* 参考工程没设 path_cb，用的默认 linear，滑起来很生硬 → 我们改 ease_out */
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_start(&a);
}

/* ==================== 计时 ==================== */

/** @brief 保持时间到 → 滑出 */
static void hold_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_state != TOAST_HOLDING) {
        return;                     /* 已经有人在收尾了 */
    }
    s_state = TOAST_HIDING;
    if (s_hold) {
        lv_timer_pause(s_hold);     /* 空闲时停掉，不做无意义的每秒唤醒 */
    }
    if (s_off_y != 0) {
        slide_to(-s_off_y, anim_out_ready_cb);
    } else {
        /* 还没量到高度（理论上不会发生），直接收尾 */
        anim_out_ready_cb(NULL);
    }
}

/* ==================== 对外 ==================== */

void watch_toast_init(void)
{
    if (s_root) {
        return;                     /* 幂等 */
    }

    lv_obj_t *layer = lv_layer_top();
    if (!layer) {
        ESP_LOGW(TAG, "拿不到 lv_layer_top()，提示条不可用");
        return;
    }

    /*
     * ★ 关键：容器建在 layer_top 上，必须先清掉自己的 CLICKABLE。
     *
     *   `lv_obj_create(NULL)` 默认是"满屏 + 可点击"，如果不清，它会**吞掉整屏触摸**，
     *   所有页面都点不动 —— 项目里 `watch_overlay.c` 当年就是被这个现象误导，
     *   才以为"layer_top 不能用"（见 watch_toast.h 的说明）。
     */
    s_root = lv_obj_create(layer);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_root, LV_DIR_NONE);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_size(s_root, W_SCREEN_W, LV_SIZE_CONTENT);
    lv_obj_align(s_root, LV_ALIGN_TOP_MID, 0, 0);
    /* 起手就在屏幕外（translate 是样式，不会被布局重算覆盖）*/
    lv_obj_set_style_translate_y(s_root, -TOAST_OFF_FALLBACK, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    /* 可见的药丸（同样不吃触摸：它只是个提示） */
    s_pill = lv_obj_create(s_root);
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_pill, LV_DIR_NONE);
    lv_obj_set_style_bg_color(s_pill, W_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pill, 0, 0);
    lv_obj_set_style_radius(s_pill, W_RADIUS_CARD, 0);
    /* LVGL 8.4 没有 pad_hor/pad_ver，只有四个方向各自设（lv_obj_style_gen.h:585-593）*/
    lv_obj_set_style_pad_left(s_pill, TOAST_PAD_H, 0);
    lv_obj_set_style_pad_right(s_pill, TOAST_PAD_H, 0);
    lv_obj_set_style_pad_top(s_pill, TOAST_PAD_V, 0);
    lv_obj_set_style_pad_bottom(s_pill, TOAST_PAD_V, 0);
    lv_obj_set_style_shadow_width(s_pill, 0, 0);
    lv_obj_set_size(s_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_pill, TOAST_MAX_W, 0);
    /*
     * 药丸自己居中：不要靠"给外层容器设宽度然后期望子对象居中"——
     * 参考工程就是药丸靠左、标签在 240 宽容器里居中，差了约 12px（见其 ui_manager.cpp:1147-1160）。
     */
    lv_obj_align(s_pill, LV_ALIGN_TOP_MID, 0, 0);

    /*
     * 文案：用 w_label_create_auto() 才能登记进"自动字体表"，
     * 之后 w_label_set_text_auto() 换文案时才会自动在 ASCII/中文字体间切换
     * （不这么做的话，中文会变豆腐块 —— 项目 v0.5.0 踩过）。
     */
    s_label = w_label_create_auto(s_pill, "", W_FONT_NORMAL, TOAST_CJK_SIZE, W_COLOR_TEXT);
    /*
     * ★ 这里踩过一个坑（截图才发现的）：**不要给标签设 `LV_PCT(100)` 宽度**。
     *
     *   药丸是 `LV_SIZE_CONTENT`，标签再设成 `LV_PCT(100)` 就形成了退化的
     *   尺寸计算 —— 实测药丸被压成 **15px 宽、60px 高的一根竖条**，
     *   文字竖着排。截图一看就知道不对，但断言全过（尺寸类问题断言抓不到）。
     *
     *   正确做法：标签也用 `LV_SIZE_CONTENT`，让"文本 → 标签 → 药丸"这条链
     *   自下而上撑开；药丸的 max_width(216) 负责封顶，
     *   封顶之后 `LV_LABEL_LONG_WRAP` 才有确定宽度可换行（长文案变两行）。
     */
    lv_obj_set_width(s_label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);

    /* 保持计时器：常驻，空闲时 pause（不做无意义的周期唤醒） */
    s_hold = lv_timer_create(hold_timer_cb, TOAST_HOLD_MS, NULL);
    if (s_hold) {
        lv_timer_pause(s_hold);
    }

    s_state = TOAST_IDLE;
    ESP_LOGI(TAG, "提示条就绪（滑入/滑出 %d ms，保持 %d ms，挂 layer_top）",
             TOAST_ANIM_MS, TOAST_HOLD_MS);
}

void watch_toast_show(const char *text)
{
    if (!s_root || !s_pill || !s_label) {
        return;                     /* 没初始化（模拟器/早期调用）→ 静默忽略 */
    }
    if (!text || !text[0]) {
        return;
    }

    /* 闹钟/倒计时提醒层是"必须盖住一切"的模态级提醒，别去挡它的按钮 */
    if (watch_alert_visible()) {
        ESP_LOGD(TAG, "提醒层可见，丢弃提示：%s", text);
        return;
    }

    /* 文案先设好，滑入时看到的就是最终内容（不会先显示上一条）*/
    w_label_set_text_auto(s_label, text);

    /*
     * 量一次高度，作为"屏幕外"的位移量。
     *
     * 为什么要 `lv_obj_update_layout()`：刚建好、或换过文案之后布局是脏的，
     * 脏布局下 `lv_obj_get_height()` 返回 0。只在"还没量过"的路径上补这一下，
     * 正常运行期不付这个代价。
     *
     * ⚠️ 连"量到的值是否可信"也要判：文案变长会让药丸变高，
     *    所以取"已量到的值"和"新量到的值"里更大的那个，保证一定移得出屏幕。
     */
    lv_obj_update_layout(s_root);
    const int h_now = lv_obj_get_height(s_root);
    if (h_now > s_off_y) {
        s_off_y = h_now;
    }
    if (s_off_y <= 0) {
        s_off_y = TOAST_OFF_FALLBACK;   /* 量不到就按一行高兜底 */
    }

    /*
     * 从屏幕外开始。用 translate 而不是 set_y（原因见 anim_translate_cb 的说明）。
     * 只在整个周期**结束**（IDLE）时才需要重新摆回屏幕外；
     * 滑出中途重入的情形交给动画自己从当前值反向滑回，不重置，免得跳一下。
     */
    if (s_state == TOAST_IDLE) {
        lv_obj_set_style_translate_y(s_root, -s_off_y, 0);
    }

    /* 先取消隐藏并抬到最前（layer_top 上也可能有别的浮层）*/
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);

    switch (s_state) {
    case TOAST_IDLE:
        s_state = TOAST_SHOWING;
        slide_to(0, anim_in_ready_cb);      /* 计时器在滑入结束后才开始 */
        break;

    case TOAST_SHOWING:
    case TOAST_HOLDING:
        /*
         * 已经看得见：只换文案 + 重新计时。
         * **不重播滑入动画** —— 否则同一条提示连续更新会来回抽动。
         */
        s_state = TOAST_HOLDING;
        if (s_hold) {
            lv_timer_reset(s_hold);
            lv_timer_resume(s_hold);
        }
        break;

    case TOAST_HIDING:
        /*
         * ★ 这一步是参考工程缺的：滑出进行到一半又来一条提示。
         *   反向滑回即可 —— LVGL 会按 (var, exec_cb) 自动取消那条滑出动画，
         *   所以旧动画的 ready_cb 不会再来捣乱（它内部也有状态判断兜底）。
         */
        s_state = TOAST_SHOWING;
        if (s_hold) {
            lv_timer_pause(s_hold);
        }
        slide_to(0, anim_in_ready_cb);
        break;
    }
}

void watch_toast_show_fmt(const char *fmt, ...)
{
    if (!fmt) {
        return;
    }
    /* 8 行中文文案够用；内部会 strdup，所以栈缓冲安全 */
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    watch_toast_show(buf);
}

bool watch_toast_visible(void)
{
    return s_state != TOAST_IDLE;
}

lv_obj_t *watch_toast_root_for_test(void)
{
    return s_root;
}

const char *watch_toast_state_name(void)
{
    switch (s_state) {
    case TOAST_IDLE:    return "空闲";
    case TOAST_SHOWING: return "滑入中";
    case TOAST_HOLDING: return "保持中";
    case TOAST_HIDING:  return "滑出中";
    default:            return "?";
    }
}

void watch_toast_deinit_for_test(void)
{
    /*
     * 只给自检用：把状态清干净，让下一次 show 走"从零开始"的路径。
     * 故意**不删对象** —— 与"常驻"的设计保持一致，避免自检本身引入删除时序问题。
     */
    if (s_hold) {
        lv_timer_pause(s_hold);
    }
    s_state = TOAST_IDLE;
    s_off_y = 0;
    if (s_root) {
        lv_obj_set_style_translate_y(s_root, -TOAST_OFF_FALLBACK, 0);
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
}
