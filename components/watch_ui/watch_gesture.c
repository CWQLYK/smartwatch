/**
 * @file watch_gesture.c
 * @brief 手势识别与派发实现 —— **不用 LVGL 的 LV_EVENT_GESTURE**
 *
 * ## 为什么要自己实现（重要，别改回去）
 *
 * 一开始用的是 LVGL 的 `LV_EVENT_GESTURE` + `lv_indev_get_gesture_dir()`，
 * 配合 `LV_OBJ_FLAG_GESTURE_BUBBLE` 让事件冒泡到页面根对象。
 * 代码看着很正规，**但在实机上滑不动**。翻 LVGL 源码（`lv_indev.c`）才发现：
 *
 * ```c
 * lv_event_send(indev_obj_act, LV_EVENT_PRESSING, indev_act);
 * _lv_indev_scroll_handler(proc);     // ← 先把这次拖动"吃掉"当滚动
 * indev_gesture(proc);                // ← 内部第一行: if(scroll_obj) return;
 * ```
 *
 * 而 `lv_obj_create()` 造出来的对象**默认带 LV_OBJ_FLAG_SCROLLABLE**。
 * 于是：手指按在哪张卡片上，那张卡片就变成 scroll_obj，
 * 拖动被当成"滚动这张卡片"，`LV_EVENT_GESTURE` **一次都不会触发**。
 *
 * 想绕过去得保证"所有子对象都不滚动、只有页面根能滚"，
 * 条件很脆（以后加个列表就又不灵了），所以改成**自己算方向**：
 *
 * ```
 * PRESSED  → 记下起点
 * PRESSING → 算累计位移；超过阈值就判定方向并执行（每次触摸只执行一次）
 * RELEASED → 复位
 * ```
 *
 * 这套逻辑与滚动系统、手势阈值、冒泡规则**全都无关**，
 * 唯一依赖是 PRESSING 能冒泡到页面根 —— 靠给子树打
 * `LV_OBJ_FLAG_EVENT_BUBBLE` 保证。
 *
 * ## 另外两个必须注意的点
 *
 * 1. **滑块/可滚动控件要放行**：手指按在亮度滑块上横向拖，
 *    本意是调亮度，不能当成"切页"。判据用
 *    `lv_obj_get_scroll_dir(target) != LV_DIR_NONE`（滑块、下拉框等天然满足）。
 * 2. **切页不能直接调用**：当前在事件回调里，LVGL 正在遍历对象树，
 *    直接 `lv_scr_load_anim()` / `lv_obj_del()` 会崩。
 *    → 一律 `lv_async_call()` 推迟到本轮遍历结束。
 */
#include "watch_gesture.h"

#include <string.h>
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "GESTURE";

/* ==================== 参数 ====================
 * 240x280 的屏，30px 差不多是屏幕短边的 1/8，
 * 既能"轻轻一划就响应"，又不会被手指抖动误触发。
 * 实机上偏钝就往小调，太灵敏（误切页）就往大调。 */
#define SWIPE_TRIGGER_PX    30

/* ==================== 内部状态 ==================== */
static bool               s_inited = false;
static watch_gesture_cb_t s_cb;
static bool             (*s_is_overlay_open)(void) = NULL;
static bool             (*s_is_hor_taken)(void)    = NULL;

/* 当前这次触摸的跟踪状态 */
static bool       s_tracking = false;   /* 是否正在跟踪一次拖动 */
static lv_point_t s_start;              /* 起点（屏幕坐标） */
static bool       s_fired    = false;   /* 本次触摸是否已经执行过动作 */

/* ==================== 异步动作 ==================== */
typedef enum {
    ACT_NONE = 0,
    ACT_FORWARD,
    ACT_BACK,
    ACT_OPEN_OVERLAY,
    ACT_CLOSE_OVERLAY,
} action_t;

#define ACT_SLOTS  4
static action_t s_pending[ACT_SLOTS];

static void action_exec(void *arg)
{
    const action_t act = (action_t)(intptr_t)arg;

    for (int i = 0; i < ACT_SLOTS; i++) {
        if (s_pending[i] == act) {
            s_pending[i] = ACT_NONE;
            break;
        }
    }

    switch (act) {
    case ACT_FORWARD:       if (s_cb.go_forward)    s_cb.go_forward();    break;
    case ACT_BACK:          if (s_cb.go_back)       s_cb.go_back();       break;
    case ACT_OPEN_OVERLAY:  if (s_cb.open_overlay)  s_cb.open_overlay();  break;
    case ACT_CLOSE_OVERLAY: if (s_cb.close_overlay) s_cb.close_overlay(); break;
    default: break;
    }
}

/** @brief 排一个异步动作；同种动作不重复排队（防连滑一次切好几页） */
static void post(action_t act)
{
    for (int i = 0; i < ACT_SLOTS; i++) {
        if (s_pending[i] == act) {
            return;                     /* 已在队列里 */
        }
    }
    for (int i = 0; i < ACT_SLOTS; i++) {
        if (s_pending[i] == ACT_NONE) {
            s_pending[i] = act;
            lv_async_call(action_exec, (void *)(intptr_t)act);
            return;
        }
    }
    ESP_LOGW(TAG, "动作队列满，丢弃手势");
}

/* ==================== 子树标记 ==================== */
void watch_gesture_mark_subtree(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }

    /*
     * EVENT_BUBBLE 让事件从控件继续往父对象走，一路走到页面根。
     * 注意**不要**再设 GESTURE_BUBBLE —— 那是给 LV_EVENT_GESTURE 用的，
     * 我们已经不用那套机制了（见文件头说明）。
     */
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);

    const uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        watch_gesture_mark_subtree(lv_obj_get_child(obj, i));
    }
}

/* ==================== 事件处理 ==================== */
static void on_pressed(lv_event_t *e)
{
    (void)e;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }

    lv_indev_get_point(indev, &s_start);
    s_tracking = true;
    s_fired    = false;
}

static void on_pressing(lv_event_t *e)
{
    if (!s_tracking || s_fired) {
        return;
    }

    /*
     * 手指按在滑块上拖动 → 那是"调值"，不是"切页"，放行。
     * 判据：该控件自己声明了可滚动方向（滑块/下拉框/列表都满足）。
     */
    lv_obj_t *target = lv_event_get_target(e);
    if (target && lv_obj_get_scroll_dir(target) != LV_DIR_NONE) {
        return;
    }

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }

    lv_point_t cur;
    lv_indev_get_point(indev, &cur);

    const int dx = cur.x - s_start.x;
    const int dy = cur.y - s_start.y;

    if (LV_ABS(dx) < SWIPE_TRIGGER_PX && LV_ABS(dy) < SWIPE_TRIGGER_PX) {
        return;                         /* 还没划够远 */
    }

    const bool overlay = s_is_overlay_open ? s_is_overlay_open() : false;
    /*
     * 横向滑动是否已被别人接管（跟手轮播）。
     * 轮播页上横向归 LVGL 的滚动系统，这里再处理就会"一次滑两页"。
     */
    const bool hor_taken = s_is_hor_taken ? s_is_hor_taken() : false;

    if (LV_ABS(dx) >= LV_ABS(dy)) {
        /* 横向为主 */
        if (hor_taken) {
            return;                     /* 交给轮播，不插手 */
        }
        if (overlay) {
            post(ACT_CLOSE_OVERLAY);    /* 控制中心开着：横滑先关它 */
        } else if (dx > 0) {
            post(ACT_FORWARD);          /* 右滑 → 前进 */
        } else {
            post(ACT_BACK);             /* 左滑 → 返回 */
        }
    } else {
        /* 纵向为主 */
        if (dy < 0) {
            if (!overlay) {
                post(ACT_OPEN_OVERLAY); /* 上滑 → 控制中心 */
            }
        } else if (overlay) {
            post(ACT_CLOSE_OVERLAY);    /* 下滑 → 关闭控制中心 */
        }
    }

    s_fired = true;                     /* 一次触摸只执行一个动作 */
}

static void on_released(lv_event_t *e)
{
    (void)e;
    s_tracking = false;
    s_fired    = false;
}

/* ==================== 对外接口 ==================== */
void watch_gesture_init(const watch_gesture_cb_t *cbs)
{
    if (cbs) {
        s_cb = *cbs;
    } else {
        memset(&s_cb, 0, sizeof(s_cb));
    }
    memset(s_pending, 0, sizeof(s_pending));
    s_tracking = false;
    s_fired    = false;
    s_inited   = true;

    ESP_LOGI(TAG, "手势层就绪（右滑前进 / 左滑返回 / 上滑控制中心，阈值 %dpx）",
             SWIPE_TRIGGER_PX);
}

void watch_gesture_attach(lv_obj_t *scr)
{
    if (!scr) {
        return;
    }
    if (!s_inited) {
        ESP_LOGW(TAG, "手势层未初始化，忽略 attach");
        return;
    }

    /*
     * 页面根是冒泡链的终点：把它自己的 EVENT_BUBBLE 清掉，
     * 免得事件继续爬到 layer_top 上去（那儿没人处理）。
     */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 子树全部打上 EVENT_BUBBLE，事件才能爬到页面根 */
    watch_gesture_mark_subtree(scr);

    /*
     * 页面根**必须不可滚动** —— 这是关键：
     * 如果根可滚动，拖动会被根自己当成滚动（又是一次"滑不动"）。
     * 手表界面本来就整屏适配，不需要滚动。
     */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 挂事件。lv_obj_add_event_cb() 会自动给对象加 CLICKABLE，
     * 所以点空白区域也算在根对象上，正好。
     */
    lv_obj_add_event_cb(scr, on_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(scr, on_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, on_released, LV_EVENT_RELEASED, NULL);
}

void watch_gesture_set_overlay_probe(bool (*is_open)(void))
{
    s_is_overlay_open = is_open;
}

void watch_gesture_set_horizontal_probe(bool (*probe)(void))
{
    s_is_hor_taken = probe;
}
