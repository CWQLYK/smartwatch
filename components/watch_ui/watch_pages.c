/**
 * @file watch_pages.c
 * @brief 页面管理器实现 —— 跟手滑动轮播（Apple Watch 风格）
 *
 * ==================================================================
 * 结构：三槽窗口 + **手动定位**
 * ==================================================================
 *
 * ```
 *   scr                          screen 240x280（唯一的 screen）
 *   └── vp                       裁剪容器 240x280（不滚动，纯裁剪）
 *       ├── slot[0]  内容 x = off - 240    前一页
 *       ├── slot[1]  内容 x = off          当前页
 *       └── slot[2]  内容 x = off + 240    后一页
 * ```
 *
 * `off` 是"内容整体横向偏移"：
 *   off = 0      -> 当前页铺满视野（静止态）
 *   off < 0      -> 内容左移，右边露出后一页（正在看下一页）
 *   off > 0      -> 内容右移，左边露出前一页（正在看上一页）
 *
 * ## ⚠️ 为什么不用 LVGL 的滚动 / 滚动吸附（重要，别改回去）
 *
 * 第一版用的是"可滚动 viewport + `lv_obj_set_scroll_snap_x()`"，
 * 理论上能白拿跟手位移 + 速度预测 + 吸附动画，代码还更短。
 * **但在模拟器和实机上都完全滑不动**，实测数据：
 *
 *     三个槽放在 -240 / 0 / +240；
 *     lv_obj_get_scroll_right(vp) = 240，看起来行程够；
 *     但 lv_obj_scroll_by(vp, -40, 0, LV_ANIM_OFF) -> scroll_x 0 -> 0（纹丝不动）
 *     连 lv_obj_scroll_by_bounded() 也一样
 *
 * 也就是说 LVGL 的滚动坐标不是我们以为的"内容坐标系"：
 * `scroll_right` 会把内容外框的左边缘减掉，实际可达位置和槽位坐标对不上，
 * 硬编码或按公式推算都会错，而且错得很隐蔽（数值看着合理，就是不动）。
 * 与其继续逆推它的几何，不如**自己控制坐标** —— 所见即所得，一定能动。
 *
 * 代价：失去 LVGL 白送的"速度预测"（轻甩翻页）。
 * 补偿：用"位移超过 1/4 屏"判定翻页，手感够用；要轻甩可以再加速度判据。
 *
 * ## 三个必须注意的点
 *
 * 1. **`vp` 不能可滚动**（`LV_OBJ_FLAG_SCROLLABLE` 要清掉）。
 *    否则拖动会被滚动系统吃掉，`PRESSING` 里我们收不到连续的位移。
 * 2. **槽位 clippable**：LVGL 默认子对象会被父对象裁剪（默认就是），
 *    所以放在 ±240 的槽自然被 `vp` 裁掉，不会画到屏幕外。
 * 3. **重排必须在动画之后、且不能在对象树遍历中**：
 *    用 `lv_anim` 的 ready 回调 + `lv_obj_del_async()`。
 */
#include "watch_pages.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "ui_port.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_gesture.h"
#include "watch_overlay.h"
#include "watch_modal.h"
#include "watch_clock.h"
#include "watch_alert.h"
#include "watch_game.h"

static const char *TAG = "WATCH_UI";

#define PAGE_MAX_REGISTERED  12
#define SLOT_COUNT           3      /* 前 / 当前 / 后 —— 固定 3，实现依赖它 */

/**
 * 翻页判定阈值：拖动位移超过屏宽的 1/4 就算翻页。
 * 240 屏 -> 60px。调小更灵敏（容易误翻），调大更稳（要拖更远）。
 */
#define COMMIT_THRESHOLD_PX  (W_SCREEN_W / 4)

/** 收尾动画时长（毫秒）：弹回 / 落位 */
#define SNAP_ANIM_MS         180

/* ==================== 内部状态 ==================== */
typedef struct {
    lv_obj_t *cont;     /* 槽容器 */
    lv_obj_t *page;     /* 页面根 */
    int       idx;      /* 页面下标；-1 = 空 */
} slot_t;

static const watch_page_t *s_registry[PAGE_MAX_REGISTERED];
static int                 s_registered = 0;

static const watch_data_provider_t *s_provider = NULL;
static int                          s_sub_handle = -1;
static bool                         s_inited     = false;

static lv_obj_t *s_scr = NULL;      /* 轮播 screen */
static lv_obj_t *s_vp  = NULL;      /* 裁剪容器（不滚动） */
static slot_t    s_slot[SLOT_COUNT];
static int       s_cur   = 0;       /* 当前居中页下标 */
static int       s_count = 0;       /* 实际页数 */

/* 内容横向偏移：0 = 当前页铺满视野 */
static lv_coord_t s_off = 0;

/* 拖动状态 */
static bool       s_dragging = false;
static lv_point_t s_drag_start;
static lv_coord_t s_drag_max = 0;         /* 本次按下过程中出现过的最大位移 */
static bool       s_committing = false;   /* 正在重排，挡住新手势 */

/*
 * 本次按下时"当前页能纵向滚多少像素"，0 = 内容不满一屏。
 * 在 on_pressed 里算一次，PRESSING 里直接用（别每帧都查布局）。
 */
static lv_coord_t s_page_scroll_limit = 0;
/* 本次拖动已滚过的累计量，用来算增量（避免累加误差） */
static lv_coord_t s_page_dy = 0;

/* 平台钩子：亮度读写（控制中心用） */
static watch_brightness_get_fn s_br_get = NULL;
static watch_brightness_set_fn s_br_set = NULL;

/* ==================== 前向声明 ==================== */
static void on_state_changed(const watch_state_t *state, uint32_t changed);
static void refresh_current(const watch_state_t *state, uint32_t changed);
static void slot_build(int slot, int page_idx);
static void slot_destroy(int slot);
static void apply_offsets(lv_coord_t off);
bool watch_ui_carousel_active(void);
static esp_err_t scroll_one(int dir);
static lv_coord_t current_page_scroll_range(void);
static void scroll_page_to_top(void);

/* ==================== 亮度钩子 ==================== */
int watch_ui_brightness_get(void)
{
    return (s_br_get && s_br_set) ? s_br_get() : 70;
}

int watch_ui_brightness_set(int percent)
{
    return (s_br_get && s_br_set) ? s_br_set(percent) : 0;
}

void watch_ui_set_brightness_hooks(watch_brightness_get_fn get_fn,
                                   watch_brightness_set_fn set_fn)
{
    s_br_get = get_fn;
    s_br_set = set_fn;
}

/* ---------- 设置项读写钩子（设置页用，见 watch_pages.h 说明） ---------- */
static watch_setting_get_fn s_set_get = NULL;
static watch_setting_set_fn s_set_set = NULL;

void watch_ui_set_setting_hooks(watch_setting_get_fn get_fn,
                                watch_setting_set_fn set_fn)
{
    s_set_get = get_fn;
    s_set_set = set_fn;
}

int watch_ui_setting_get(const char *key, int def)
{
    return (s_set_get && key) ? s_set_get(key, def) : def;
}

int watch_ui_setting_set(const char *key, int value)
{
    return (s_set_set && key) ? s_set_set(key, value) : 0;
}

/* ==================== 小工具 ==================== */
static int idx_wrap(int i)
{
    if (s_count <= 0) {
        return 0;
    }
    while (i < 0) {
        i += s_count;
    }
    return i % s_count;
}

/** @brief 槽位 -> 页面下标（槽 0=前一页，1=当前页，2=后一页） */
static int slot_to_idx(int slot)
{
    return idx_wrap(s_cur + (slot - 1));
}

/** @brief 把三个槽摆到给定偏移处 */
static void apply_offsets(lv_coord_t off)
{
    s_off = off;
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (s_slot[i].cont) {
            lv_obj_set_x(s_slot[i].cont, off + (lv_coord_t)(i - 1) * W_SCREEN_W);
        }
    }
}

static int find_registered(const watch_page_t *page)
{
    for (int i = 0; i < s_registered; i++) {
        if (s_registry[i] == page ||
            (page && s_registry[i] && page->id && s_registry[i]->id &&
             strcmp(s_registry[i]->id, page->id) == 0)) {
            return i;
        }
    }
    return -1;
}

/* ==================== 槽构建 / 销毁 ==================== */
static void slot_destroy(int slot)
{
    if (s_slot[slot].cont) {
        /*
         * 用 del_async：本函数可能在 LVGL 遍历对象树 / 动画回调里被调用。
         * cont 是 page 的父对象，删 cont 会把 page 一起带走。
         */
        lv_obj_del_async(s_slot[slot].cont);
        s_slot[slot].cont = NULL;
    }
    s_slot[slot].page = NULL;
    s_slot[slot].idx  = -1;
}

static void slot_build(int slot, int page_idx)
{
    slot_destroy(slot);

    const watch_page_t *p = s_registry[page_idx];

    lv_obj_t *cont = lv_obj_create(s_vp);
    lv_obj_set_size(cont, W_SCREEN_W, W_SCREEN_H);
    lv_obj_set_y(cont, 0);
    /* x 由 apply_offsets() 统一摆 */
    lv_obj_set_x(cont, (lv_coord_t)(slot - 1) * W_SCREEN_W);

    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_bg_color(cont, W_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);

    /* 槽自己绝不滚动/不接收点击，免得抢事件 */
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_NONE);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);

    /* 页面根是普通对象，挂在槽下面 */
    lv_obj_t *root = (p && p->create) ? p->create(cont) : NULL;
    if (!root) {
        ESP_LOGE(TAG, "页面 '%s' 创建失败，用兜底页", (p && p->id) ? p->id : "?");
        root = w_page_create(cont);
        lv_obj_t *lbl = lv_label_create(root);
        lv_obj_set_style_text_font(lbl, watch_font_cn(16), 0);
        lv_obj_set_style_text_color(lbl, W_COLOR_TEXT_DIM, 0);
        lv_label_set_text_fmt(lbl, "页面 '%s' 建不起来", (p && p->id) ? p->id : "?");
        lv_obj_center(lbl);
    } else if (lv_obj_get_parent(root) != cont) {
        /*
         * 守卫：v0.3 之前页面用 w_screen_create() 造的是 **screen**，
         * screen 挂不到容器里，表现为"页面全白"。这里明确报出来。
         */
        ESP_LOGE(TAG, "页面 '%s' 的 create() 没把根挂到 parent 上！"
                      "请把 w_screen_create() 换成 w_page_create(parent)",
                 (p && p->id) ? p->id : "?");
    }

    /* 事件冒泡到页面根（手势在根上收），根上不再往上冒 */
    lv_obj_add_flag(cont, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (root) {
        lv_obj_clear_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE);
        watch_gesture_mark_subtree(root);
    }

    s_slot[slot].cont = cont;
    s_slot[slot].page = root;
    s_slot[slot].idx  = page_idx;
}

/* ==================== 收尾动画 ==================== */
/** @brief 动画执行：把三个槽摆到插值出来的偏移 */
static void anim_off_cb(void *var, int32_t v)
{
    (void)var;
    apply_offsets((lv_coord_t)v);
}

/**
 * @brief 重排窗口：换页后重建三槽，并把偏移归零
 *
 * @note 由动画 ready 回调（或直接调用）触发，此时已不在事件派发中。
 */
static void rebuild_window(void)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        slot_build(i, slot_to_idx(i));
    }
    apply_offsets(0);

    /*
     * 新页从顶部开始显示。
     * 不加这句的话，从"滚到一半的设置页"翻走再翻回来，
     * 新页会停在上一页残留的滚动位置上，看起来像随机跳了一段。
     */
    scroll_page_to_top();
    s_page_scroll_limit = current_page_scroll_range();

    /* 新页露面时补一次数据（离开期间数据可能变了） */
    watch_ui_refresh();

    s_committing = false;

    ESP_LOGD(TAG, "换页 -> [%d] %s", s_cur,
             s_registry[s_cur] && s_registry[s_cur]->id ? s_registry[s_cur]->id : "?");
}

/** @brief 动画结束回调（弹回到位 / 落位到位后都走这里） */
static void anim_ready_cb(lv_anim_t *a)
{
    (void)a;
    rebuild_window();
}

/**
 * @brief 启动收尾动画
 *
 * @param to_off  动画目标偏移
 * @param commit  动画结束后是否重建窗口（= 真的换页了）
 *                false 时表示只是弹回原位，窗口不用动
 */
static void start_snap_anim(lv_coord_t to_off, bool commit)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_vp);
    lv_anim_set_exec_cb(&a, anim_off_cb);
    lv_anim_set_values(&a, s_off, to_off);
    lv_anim_set_time(&a, SNAP_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);

    if (commit) {
        /*
         * 换页：动画滚到"相邻页正好居中"的位置，ready 回调里重建窗口
         * 并把偏移归零 —— 内容整体挪一页 + 偏移归零正好抵消，看不到跳变。
         */
        lv_anim_set_ready_cb(&a, anim_ready_cb);
    } else {
        /* 弹回：动画结束后什么都不用做，位置已经是 0 */
    }
    lv_anim_start(&a);
}

/* ==================== 手势（跟手拖动） ==================== */

/**
 * @brief 当前页的内容是否超出屏幕高度（也就是"需不需要能滚"）
 *
 * 做法：把页面根临时标成可滚动，问 LVGL "下面还有多少内容"，
 * 问完立刻恢复标记。
 *
 * ⚠️ 为什么要临时改标记而不是让页面根常驻可滚动：
 *    一旦页面根是 SCROLLABLE，find_scroll_obj() 就会把它当滚动候选，
 *    **即使内容没超出**（LVGL 要留它做弹性回弹），
 *    于是 indev_gesture() 第一行 `if(scroll_obj) return;` 直接返回，
 *    **内容不满一屏的页面就再也不能上滑开控制中心了**。
 *    所以只在需要的时候借一下这个标记，平时保持 NONE。
 *
 * @note 每次按下只算一次（结果缓存在 s_page_scrollable），
 *       不要在 PRESSING 里反复算 —— 那会每次触摸事件都做一遍布局查询。
 */
static lv_coord_t current_page_scroll_range(void)
{
    lv_obj_t *page = s_slot[1].page;
    if (!page) {
        return 0;
    }

    /* 借标记：查"还能往下滚多少" */
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_update_layout(page);
    const lv_coord_t bottom = lv_obj_get_scroll_bottom(page);
    const lv_coord_t top    = lv_obj_get_scroll_top(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_NONE);

    const lv_coord_t total = top + bottom;
    return (total > 0) ? total : 0;
}

/** @brief 用 dx/dy 滚当前页（dy 是手指位移，内容要反向走） */
static void scroll_page_by(lv_coord_t dy)
{
    lv_obj_t *page = s_slot[1].page;
    if (!page) {
        return;
    }

    /* 同样借标记，让 bounds 计算走 LVGL 自己的逻辑（别自己推坐标系） */
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);

    /*
     * ⚠️ 符号：**直接用 +dy，不要取负**。
     *    第一版我写成 `-dy`，结果拖动方向是反的（用户实测反馈），
     *    别再"顺手"加个负号。
     *
     * 依据 LVGL 自己的约定：
     *   `lv_obj_get_scroll_y()` 返回的是**已经被滚掉的内容高度**，
     *   scroll_y 越大 → 内容越往上走 → 露出越靠下的部分。
     *   而 `lv_indev_scroll.c` 里手势滚动用的就是
     *   `lv_obj_scroll_by(obj, diff_x, diff_y, ANIM_OFF)`，
     *   diff_y 直接取自输入位移向量（向下为正），没有任何取反。
     *
     * 所以正确的映射是：
     *   手指向下拖(dy>0) → scroll_y 增大 → 内容上移 → 看到更下面的内容
     * 这正是所有触屏系统的标准行为（手指把内容"推"上去）。
     */
    lv_obj_scroll_by_bounded(page, 0, dy, LV_ANIM_OFF);

    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_NONE);
}

/** @brief 把当前页滚回顶部（换页后调，免得新页停在上一页的滚动位置） */
static void scroll_page_to_top(void)
{
    lv_obj_t *page = s_slot[1].page;
    if (!page) {
        return;
    }
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_NONE);
}

/**
 * @brief 位移超过多少像素就认为"这次是滑动，不是点击"
 *
 * 10px 是 LVGL 自己的默认滚动阈值（`LV_INDEV_DEF_SCROLL_LIMIT`）。
 * 比它小的抖动算点击（手指按下去本来就会抖），比它大就一定是想滑动。
 */
#define DRAG_SUPPRESS_CLICK_PX   10

/** @brief 抬手之后异步清掉"拖动"标记（为什么要异步见 drag_end） */
static void drag_flag_clear_async(void *user)
{
    (void)user;
    w_touch_clear_drag();
}

/** @brief 按下：记起点，清掉上一次残留的"拖动"标记 */
static void drag_begin(const lv_point_t *pt)
{
    if (s_committing) {
        return;                     /* 正在重排，忽略这次触摸 */
    }

    lv_anim_del(s_vp, anim_off_cb); /* 抓住正在跑的收尾动画 */
    s_drag_start = *pt;
    s_drag_max   = 0;
    s_dragging   = true;
    s_page_dy    = 0;
    w_touch_clear_drag();

    /* 这次按下的"页面能不能纵向滚"，只算一次 */
    s_page_scroll_limit = current_page_scroll_range();
}

/** @brief 拖动中：主轴锁定 + 滚页面 / 跟手翻页 */
static void drag_move(const lv_point_t *pt, lv_obj_t *target)
{
    if (!s_dragging) {
        return;
    }

    /* 滑块等"自己会拖"的控件要放行（比如控制中心的亮度滑块）。
     * 那种情况 LVGL 自己会把 scroll_obj 设上，点击本来就轮不到我们，
     * 所以也不用标记。 */
    if (target && lv_obj_get_scroll_dir(target) != LV_DIR_NONE) {
        return;
    }

    const lv_coord_t dx = (lv_coord_t)(pt->x - s_drag_start.x);
    const lv_coord_t dy = (lv_coord_t)(pt->y - s_drag_start.y);

    /*
     * ★ 标记"这次触摸是拖动"（用户实测报的误触就靠这个拦）
     *
     * 用**过程中出现过的最大位移**，而不是"抬手时相对起点的位移"：
     *   用户往下拖 60px 滑列表、再往回拖到起点附近才松手的话，
     *   后者算出来是 0，就会被当成点击 —— 照样误进页面。
     *
     * ⚠️ 为什么必须自己记：轮播页的纵向滚动是本文件手写的，
     *    LVGL 的 scroll_obj 一直是 NULL，它那套"滚动就不发点击"
     *    对我们不生效（源码贴在 watch_widgets.h 里）。
     */
    const lv_coord_t adx = LV_ABS(dx);
    const lv_coord_t ady = LV_ABS(dy);
    const lv_coord_t moved = (adx > ady) ? adx : ady;
    if (moved > s_drag_max) {
        s_drag_max = moved;
        if (s_drag_max > DRAG_SUPPRESS_CLICK_PX) {
            w_touch_mark_drag();
        }
    }

    /*
     * 主轴锁定：谁位移大听谁的。
     *
     * 纵向分支：
     *   页面内容超过一屏时（功能列表 316px > 280px），纵向拖动应该**滚页面**。
     *   之前的版本直接 return，把纵向完全丢给手势层（上滑开控制中心），
     *   结果"开发人员选项"被挤到屏外且永远够不着 —— 用户实际遇到的问题。
     *
     *   为什么不用 LVGL 自带滚动：页面根一旦 SCROLLABLE，
     *   内容不满一屏的页面也会被当成滚动候选，导致上滑开控制中心失效
     *   （详见 current_page_scroll_range() 的说明）。
     *   所以这里只对"确实溢出"的页面做滚动，其余照旧交给手势层。
     */
    if (adx <= ady) {
        if (s_page_scroll_limit > 0) {
            /* 按"本次拖动增量"滚，避免累加误差 */
            const lv_coord_t step = dy - s_page_dy;
            s_page_dy = dy;
            if (step != 0) {
                scroll_page_by(step);
            }
        }
        return;
    }

    /*
     * 夹住偏移范围：
     *   off 最小 -W（后一页居中）、最大 +W（前一页居中）。
     * 越界时按 1/4 速度走，做出"橡皮筋"手感。
     */
    lv_coord_t want = dx;
    if (want < -W_SCREEN_W) {
        want = -W_SCREEN_W + (want + W_SCREEN_W) / 4;
    }
    if (want > W_SCREEN_W) {
        want = W_SCREEN_W - (W_SCREEN_W - want) / 4;
    }

    apply_offsets(want);
}

/** @brief 抬手：决定换页还是弹回 */
static void drag_end(void)
{
    if (!s_dragging) {
        return;
    }
    s_dragging = false;

    /*
     * ★ "拖动"标记要**异步**清，不能在这里直接清。
     *
     * LVGL 的事件顺序是（见 lv_indev.c 的 indev_proc_release）：
     *     RELEASED（就是这里） → SHORT_CLICKED → CLICKED
     * 直接清掉的话，紧接着那个 CLICKED 读到的就是"没拖动过"，守卫白加。
     * 异步回调跑在同一次 lv_timer_handler 的后面，稳在 CLICKED 之后。
     */
    lv_async_call(drag_flag_clear_async, NULL);

    const lv_coord_t off = s_off;
    const lv_coord_t thresh = COMMIT_THRESHOLD_PX;

    int dir = 0;
    if (off <= -thresh) {
        dir = +1;                   /* 内容左移 -> 看下一页 */
    } else if (off >= thresh) {
        dir = -1;                   /* 内容右移 -> 看上一页 */
    }

    if (dir == 0) {
        start_snap_anim(0, false);  /* 没拖够：弹回 */
        return;
    }

    s_cur = idx_wrap(s_cur + dir);
    s_committing = true;
    start_snap_anim((lv_coord_t)(-dir) * W_SCREEN_W, true);
}

static void on_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    drag_begin(&pt);
}

static void on_pressing(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    drag_move(&pt, lv_event_get_target(e));
}

static void on_released(lv_event_t *e)
{
    (void)e;
    drag_end();
}

/* ==================== 数据回调 ==================== */
/** @brief 刷新当前可见页（中间槽） */
static void refresh_current(const watch_state_t *state, uint32_t changed)
{
    const int slot = 1;
    const int idx  = s_slot[slot].idx;
    if (s_slot[slot].page && idx >= 0 && idx < s_registered &&
        s_registry[idx]->refresh) {
        s_registry[idx]->refresh(s_slot[slot].page, state, changed);
    }
}

static void on_state_changed(const watch_state_t *state, uint32_t changed)
{
    if (!state) {
        return;
    }

    /*
     * ★ 先喂时钟服务，**而且要在拿 LVGL 锁之前**。
     *
     *   闹钟是按"墙上时间的时:分"比较的，每秒必须喂一次，一次都不能漏。
     *   如果放在下面的 `if (!ui_port_lock(0)) return;` 之后，
     *   LVGL 正忙的那一刻就会丢掉这一次时间 —— 极端情况下整分钟都被丢，
     *   闹钟就哑了。喂时间只是几个赋值，不碰 UI，不需要锁。
     */
    watch_clock_on_time(&state->time);

    /*
     * 拿不到锁说明 LVGL 正忙；丢一次刷新没关系，下一秒还会来。
     * 用 0 超时（不等待）—— 这是数据回调，不能阻塞采样任务。
     */
    if (!ui_port_lock(0)) {
        return;
    }

    if (watch_overlay_visible()) {
        watch_overlay_refresh(state, changed);
    }
    refresh_current(state, changed);

    ui_port_unlock();
}

/* ==================== 键盘导航（桌面模拟器用） ==================== */
static void on_key(lv_event_t *e)
{
    switch (lv_event_get_key(e)) {
    case LV_KEY_LEFT:  (void)scroll_one(-1); break;
    case LV_KEY_RIGHT: (void)scroll_one(+1); break;
    case LV_KEY_UP:    watch_overlay_show(); break;
    case LV_KEY_DOWN:  watch_overlay_hide(); break;
    default: break;
    }
}

/*
 * 手势层的导航回调签名是 void(void)，scroll_one 返回 esp_err_t，包一层。
 * 轮播页上的横向滑动其实由 on_pressing 直接接管（跟手位移），
 * 这两个只是兜底 —— 别的 screen（比如控制中心）想翻页时才会走到。
 */
static void nav_next(void) { (void)scroll_one(+1); }
static void nav_prev(void) { (void)scroll_one(-1); }

/* ==================== 对外接口 ==================== */
esp_err_t watch_ui_register_page(const watch_page_t *page)
{
    ESP_RETURN_ON_FALSE(page && page->id, ESP_ERR_INVALID_ARG, TAG, "页面定义非法");

    if (find_registered(page) >= 0) {
        return ESP_OK;                  /* 重复注册视为成功，幂等 */
    }
    ESP_RETURN_ON_FALSE(s_registered < PAGE_MAX_REGISTERED, ESP_ERR_NO_MEM, TAG,
                        "页面注册表满（%d）", PAGE_MAX_REGISTERED);

    s_registry[s_registered++] = page;
    ESP_LOGD(TAG, "注册页面 '%s'（%s）", page->id, page->title ? page->title : "");
    return ESP_OK;
}

esp_err_t watch_ui_init(const watch_data_provider_t *provider,
                        const watch_page_t *home,
                        int unused)
{
    (void)unused;

    ESP_RETURN_ON_FALSE(provider, ESP_ERR_INVALID_ARG, TAG, "数据源为空");
    ESP_RETURN_ON_FALSE(home, ESP_ERR_INVALID_ARG, TAG, "首页为空");

    if (find_registered(home) < 0) {
        ESP_RETURN_ON_ERROR(watch_ui_register_page(home), TAG, "注册首页失败");
    }
    ESP_RETURN_ON_FALSE(s_registered >= 2, ESP_ERR_INVALID_STATE, TAG,
                        "滑动轮播至少需要 2 个页面，当前只有 %d 个", s_registered);

    s_provider = provider;
    s_count    = s_registered;
    s_cur      = find_registered(home);
    if (s_cur < 0) {
        s_cur = 0;
    }
    s_off        = 0;
    s_dragging   = false;
    s_committing = false;

    /* ---------- 1. 手势层：横向交给轮播，只收纵向 ---------- */
    const watch_gesture_cb_t nav = {
        .go_forward    = nav_next,
        .go_back       = nav_prev,
        .open_overlay  = watch_overlay_show,
        .close_overlay = watch_overlay_hide,
    };
    watch_gesture_init(&nav);
    watch_gesture_set_overlay_probe(watch_overlay_visible);
    /* ★ 告诉手势层：轮播页上横向归轮播，别重复处理（否则一次滑两页） */
    watch_gesture_set_horizontal_probe(watch_ui_carousel_active);

    /* ---------- 2. 控制中心（独立 screen，不跟手） ---------- */
    if (watch_overlay_create(provider) != ESP_OK) {
        ESP_LOGW(TAG, "控制中心创建失败，上滑手势将无响应");
    }

    /* ---------- 2.5 时钟服务 + 到点提醒层（v0.5.0） ----------
     *
     * 时钟服务必须在建 screen 之前 init：它内部会起一个**全局** lv_timer,
     * 而 lv_timer 只要求 LVGL 已初始化（ui_port_init 时就好了），
     * 跟 screen 没关系。
     *
     * 提醒层要挂在 screen 上，所以放在 s_scr 建好之后（见下面第 6 步）。
     */
    watch_clock_init();

    /* ---------- 3. screen + 纯裁剪容器 ---------- */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, W_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_radius(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_vp = lv_obj_create(s_scr);
    lv_obj_set_size(s_vp, W_SCREEN_W, W_SCREEN_H);
    lv_obj_set_pos(s_vp, 0, 0);
    lv_obj_set_style_pad_all(s_vp, 0, 0);
    lv_obj_set_style_border_width(s_vp, 0, 0);
    lv_obj_set_style_radius(s_vp, 0, 0);
    lv_obj_set_style_bg_color(s_vp, W_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_vp, LV_OPA_COVER, 0);
    /*
     * ★ 关键：容器**不可滚动**。
     *   我们要在 PRESSING 里自己算位移；如果它能滚，LVGL 的滚动系统
     *   会先把拖动吃掉（这正是第一版滑不动的原因）。
     */
    lv_obj_clear_flag(s_vp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_vp, LV_DIR_NONE);
    lv_obj_clear_flag(s_vp, LV_OBJ_FLAG_CLICKABLE);   /* 点击交给页面根 */

    /*
     * 事件：手指按在卡片/标签上时事件要冒泡到 s_vp 才能连续收到 PRESSING。
     * 注意 s_vp 本身不可点击，但 EVENT_BUBBLE 让它仍然能收到子对象冒上来的事件。
     */
    lv_obj_add_flag(s_vp, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(s_vp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_vp, on_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_vp, on_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_vp, on_released, LV_EVENT_RELEASED, NULL);

    /*
     * 键盘导航（桌面模拟器方便；实机没键盘，不占资源）。
     * keypad 的按键发给"当前聚焦对象"，所以把 screen 加进默认 group 并聚焦。
     */
    lv_obj_add_event_cb(s_scr, on_key, LV_EVENT_KEY, NULL);
    lv_group_t *grp = lv_group_get_default();
    if (!grp) {
        grp = lv_group_create();
        lv_group_set_default(grp);
    }
    lv_group_add_obj(grp, s_scr);
    lv_group_focus_obj(s_scr);

    /* ---------- 4. 三槽窗口 ---------- */
    for (int i = 0; i < SLOT_COUNT; i++) {
        s_slot[i].cont = NULL;
        s_slot[i].page = NULL;
        s_slot[i].idx  = -1;
    }
    lv_obj_update_layout(s_vp);
    for (int i = 0; i < SLOT_COUNT; i++) {
        slot_build(i, slot_to_idx(i));
    }
    apply_offsets(0);

    /* ---------- 5. 显示 + 订阅数据 ---------- */
    lv_scr_load(s_scr);

    /*
     * 子页面容器挂到 **screen** 上（不是挂在 s_vp 里）。
     * 理由：子页面要盖满整屏，且不该跟着轮播容器被裁剪或平移。
     */
    w_modal_init(s_scr);

    /*
     * 到点提醒层也挂在 screen 上，和模态**同层**（是 s_vp 的兄弟）。
     *
     * 为什么不用独立 screen：模态子页都是挂在这个 screen 上的普通容器，
     * 一旦 lv_scr_load() 切走，模态还留在原 screen 上，切回来子页面栈就乱了。
     * 提醒层靠 lv_obj_move_foreground() 保证盖在模态之上（见 watch_alert.c）。
     */
    watch_alert_init(s_scr);

    /*
     * 游戏宿主（v0.6.0）也挂在 screen 上，同样是 s_vp 的兄弟。
     *
     * ⚠️ 顺序：必须排在提醒层**之后**创建 —— 游戏层打开时会 move_foreground
     *    置顶，而提醒层每次 poll 又把自己置顶，所以闹钟能盖住游戏。
     *    两层都建好之后，谁在最上面取决于运行期的 move_foreground，跟这里无关；
     *    但先建提醒层能保证"刚打开游戏、闹钟恰好在这一刻响"时提醒层不被漏掉。
     */
    watch_game_init(s_scr);

    watch_state_t st;
    s_provider->get_state(&st);
    refresh_current(&st, WATCH_CHANGED_ALL);

    s_sub_handle = s_provider->subscribe(on_state_changed);
    if (s_sub_handle < 0) {
        ESP_LOGW(TAG, "数据订阅失败，界面将只在换页时刷新");
    }

    s_inited = true;
    ESP_LOGI(TAG, "轮播就绪：%d 页，当前 [%d] '%s'（跟手滑动，阈值 %dpx）",
             s_count, s_cur,
             s_registry[s_cur]->id ? s_registry[s_cur]->id : "?",
             (int)COMMIT_THRESHOLD_PX);
    return ESP_OK;
}

/* ==================== 导航 ==================== */

bool watch_ui_carousel_active(void)
{
    return s_inited && s_vp != NULL && s_count >= 2;
}

/** @brief 内部实现：滚一页。dir = +1 下一页 / -1 上一页 */
static esp_err_t scroll_one(int dir)
{
    ESP_RETURN_ON_FALSE(s_vp, ESP_ERR_INVALID_STATE, TAG, "轮播未初始化");
    ESP_RETURN_ON_FALSE(!s_committing, ESP_ERR_INVALID_STATE, TAG, "正在换页");

    lv_anim_del(s_vp, anim_off_cb);
    apply_offsets(0);               /* 从居中位置开始 */

    s_cur = idx_wrap(s_cur + dir);
    s_committing = true;
    start_snap_anim((lv_coord_t)(-dir) * W_SCREEN_W, true);
    return ESP_OK;
}

esp_err_t watch_ui_goto(int index)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "轮播未初始化");

    const int want = idx_wrap(index);
    int dir = want - s_cur;
    /* 取最短方向（循环） */
    if (dir >  s_count / 2) {
        dir -= s_count;
    }
    if (dir < -s_count / 2) {
        dir += s_count;
    }
    if (dir == 0) {
        return ESP_OK;
    }
    /* 一次只翻一页；跨多页请多调几次（目前调用点都是 ±1） */
    return scroll_one(dir > 0 ? +1 : -1);
}

esp_err_t watch_ui_next(void)
{
    return scroll_one(+1);
}

esp_err_t watch_ui_prev(void)
{
    return scroll_one(-1);
}

const watch_page_t *watch_ui_current(void)
{
    if (!s_inited || s_count <= 0) {
        return NULL;
    }
    return s_registry[s_cur];
}

int watch_ui_current_index(void)
{
    return s_cur;
}

lv_obj_t *watch_ui_current_root(void)
{
    return s_slot[1].page;
}

lv_obj_t *watch_ui_screen(void)
{
    return s_scr;
}

/**
 * @brief 已注册且参与轮播的页面数
 *
 * @note 这个函数在 v0.5.0 之前**只有声明没有实现**（头文件里挂着，
 *       全工程没人调，所以一直没暴露成链接错误）。
 *       PC 模拟器自检要按实际页数算"下一页"才不会写死 4，
 *       所以补上实现。注册数在 watch_ui_init() 里定格到 s_count。
 */
int watch_pages_count(void)
{
    return s_count;
}

void watch_ui_refresh(void)
{
    if (!s_inited || !s_provider) {
        return;
    }
    watch_state_t st;
    s_provider->get_state(&st);
    refresh_current(&st, WATCH_CHANGED_ALL);
}

bool watch_ui_time_valid(void)
{
    if (!s_provider || !s_provider->get_state) {
        return false;
    }
    watch_state_t st;
    s_provider->get_state(&st);
    return st.time.valid;
}

bool watch_ui_now(watch_time_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!s_provider || !s_provider->get_state) {
        return false;
    }
    watch_state_t st;
    s_provider->get_state(&st);
    if (out) {
        *out = st.time;
    }
    return st.time.valid;
}

/**
 * @brief 取当前 UTC 时间戳（界面层拿时间的唯一途径）
 *
 * 界面层不许 include time.h（PC 模拟器跨平台编不过），
 * 所以这里**自己把日历换算成 Unix 时间戳**（等价于 timegm）。
 * 这段逻辑很经典，但很容易写错，所以：
 *   - 用"从 1970 逐年/逐月累加天数"的笨办法，一眼能验算
 *   - 不做闰秒、不做时区（RTC 里存的就是 UTC）
 *
 * @return Unix 时间戳；没有有效时间时返回 0
 */
uint32_t watch_ui_current_epoch(void)
{
    if (!s_provider || !s_provider->get_state) {
        return 0;
    }
    watch_state_t st;
    s_provider->get_state(&st);
    if (!st.time.valid) {
        return 0;
    }

    /* ---- 日期合法性粗筛，防止脏数据算出天文数字 ---- */
    const int y = st.time.year, mo = st.time.month, d = st.time.day;
    if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        return 0;
    }

    /* ---- 年 -> 天数 ---- */
    uint32_t days = 0;
    for (int yy = 1970; yy < y; yy++) {
        const bool leap = (yy % 4 == 0 && yy % 100 != 0) || (yy % 400 == 0);
        days += leap ? 366u : 365u;
    }

    /* ---- 月 -> 天数 ---- */
    static const int mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    for (int mm = 1; mm < mo; mm++) {
        days += (uint32_t)mdays[mm - 1];
        if (mm == 2 && leap) {
            days += 1;
        }
    }

    days += (uint32_t)(d - 1);

    return days * 86400u
         + (uint32_t)st.time.hour * 3600u
         + (uint32_t)st.time.minute * 60u
         + (uint32_t)st.time.second;
}

int watch_ui_debug_page_scroll(void)
{
    return (int)current_page_scroll_range();
}

/*
 * ==================================================================
 * 自检用：模拟一次触摸（按下 / 移动 / 抬手）
 *
 * 为什么需要：用户报的"滑动后误进列表项"这个问题，
 * 只有"按下 → 移动一段 → 抬手"这一整条路径才暴露得出来，
 * 而模拟器**没法真的注入触摸**（这个环境的 SendInput 不生效）。
 *
 * 所以这里把三个事件处理函数的**函数体**（drag_begin / drag_move / drag_end）
 * 直接暴露出来 —— 注意不是另抄一份逻辑：
 * 真正的 lv_event 回调 on_pressed/on_pressing/on_released
 * 只是"取点 → 调这三个函数"，所以自检走的和真手指是同一条路径。
 *
 * ⚠️ 唯一没覆盖的是 LVGL 的**命中测试**（哪个控件收到事件）：
 *    这里传的 target 固定为 NULL（表示"不是滑块"），
 *    行控件收不收得到点击由自检自己用 lv_event_send() 补上。
 * ==================================================================
 */
void watch_ui_debug_touch_begin(int x, int y)
{
    lv_point_t pt = { (lv_coord_t)x, (lv_coord_t)y };
    drag_begin(&pt);
}

void watch_ui_debug_touch_move(int x, int y)
{
    lv_point_t pt = { (lv_coord_t)x, (lv_coord_t)y };
    drag_move(&pt, NULL);
}

void watch_ui_debug_touch_end(void)
{
    drag_end();
}

/** @brief 自检用：每个槽位里的页各有多少个子对象（0 = 空槽） */
void watch_ui_debug_slot_page_children(int *out, int max)
{
    for (int i = 0; i < SLOT_COUNT && i < max; i++) {
        out[i] = s_slot[i].page ? (int)lv_obj_get_child_cnt(s_slot[i].page) : -1;
    }
}

int watch_ui_debug_scroll_by(int dy)
{
    /*
     * 走**和手指拖动完全同一条路径**（scroll_page_by），不是另抄一份逻辑。
     * 这样自检通过就说明真机上拖得动 —— 抄一份的话，自检永远通过，
     * 而真机可能因为标记/边界条件不同滚不动（本项目踩过同类坑）。
     */
    if (current_page_scroll_range() <= 0) {
        return 0;                       /* 内容不满一屏，本来就不该滚 */
    }
    scroll_page_by((lv_coord_t)dy);

    /*
     * 返回**还剩多少可滚距离**（不是总范围）。
     * ⚠️ 别拿 current_page_scroll_range() 当返回值：它算的是 top+bottom，
     *    也就是"总范围"，滚到底之后它还是 60 而不是 0 ——
     *    自检里按"剩 0"断言会一直失败（我第一版就是这么写的）。
     */
    lv_obj_t *page = s_slot[1].page;
    if (!page) {
        return 0;
    }
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    const int left = (int)lv_obj_get_scroll_bottom(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_NONE);
    return left;
}

/** @brief 当前页滚到哪了（0 = 顶部） */
int watch_ui_debug_page_scroll_y(void)
{
    lv_obj_t *page = s_slot[1].page;
    if (!page) {
        return 0;
    }
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    const int y = (int)lv_obj_get_scroll_y(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_NONE);
    return y;
}

int watch_ui_debug_scroll_x(void)
{
    return (int)s_off;
}

void watch_ui_debug_dump(void)
{
    ESP_LOGI(TAG, "[dbg] 当前页可纵向滚动 %d px（0 = 内容不满一屏）",
             (int)current_page_scroll_range());
    ESP_LOGI(TAG, "[dbg] 偏移 off=%d，当前页 [%d] '%s'，共 %d 页",
             (int)s_off, s_cur,
             s_registry[s_cur] && s_registry[s_cur]->id ? s_registry[s_cur]->id : "?",
             s_count);
    for (int i = 0; i < SLOT_COUNT; i++) {
        ESP_LOGI(TAG, "[dbg]   槽%d 页[%d] x=%d w=%d %s", i, s_slot[i].idx,
                 s_slot[i].cont ? (int)lv_obj_get_x(s_slot[i].cont) : -999,
                 s_slot[i].cont ? (int)lv_obj_get_width(s_slot[i].cont) : 0,
                 (i == 1) ? "<-当前" : "");
    }
}

void watch_ui_dump(void)
{
    ESP_LOGI(TAG, "---- 页面状态 ----");
    watch_ui_debug_dump();
    ESP_LOGI(TAG, "------------------");
}
