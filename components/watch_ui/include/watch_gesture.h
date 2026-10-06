/**
 * @file watch_gesture.h
 * @brief 手势识别与派发 —— 滑动切页 / 上滑控制中心
 *
 * 为什么单独一层：
 *   第一版是"点屏幕左半边返回、右半边前进"，这是开发板的做法，不像手表。
 *   真手表是**滑动**。而滑动识别要处理三件麻烦事：
 *
 *   1. LVGL 8 的 `LV_EVENT_GESTURE` **不冒泡**，必须挂在页面根对象上。
 *   2. 滑动手势里带不了方向，得自己去问 `lv_indev_get_gesture_dir()`。
 *   3. 调 `lv_scr_load_anim()` **不能从事件回调里直接调** ——
 *      此刻 LVGL 正在遍历对象树，把当前屏幕删掉会崩。
 *      必须 `lv_async_call()` 推迟到本轮遍历结束。
 *
 *   这些坑都收在本文件里，页面代码只需要知道"用户右滑了"。
 *
 * 手势约定（和真手表一致）：
 *
 *      ┌──────────────────────────────┐
 *      │  ↑ 上滑 → 打开控制中心        │
 *      │  ↓ 下滑 → 关闭控制中心        │
 *      │  → 右滑 → 前进（进下一层）    │
 *      │  ← 左滑 → 返回（回上一层）    │
 *      │  • 点击 → 页面内控件自己处理   │
 *      └──────────────────────────────┘
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 导航回调集合
 *
 * @note 每个回调都**已经**被推迟到安全的时机且**已经持锁**，
 *       实现里可以直接调 watch_ui_push() / lv_scr_load() 之类。
 */
typedef struct {
    void (*go_forward)(void);   ///< 右滑：进入下一层
    void (*go_back)(void);      ///< 左滑：返回上一层
    void (*open_overlay)(void); ///< 上滑：打开控制中心
    void (*close_overlay)(void);///< 下滑：关闭控制中心
} watch_gesture_cb_t;

/**
 * @brief 全局初始化手势层
 * @param cbs 回调集合（可长期有效，内部只存指针，不要传栈上临时变量）
 */
void watch_gesture_init(const watch_gesture_cb_t *cbs);

/**
 * @brief 给一个"独立 screen"挂手势（目前只有控制中心用）
 *
 * @param scr screen 或任意对象（事件从子控件沿 EVENT_BUBBLE 链爬到它）
 *
 * 实现见 watch_gesture.c 的文件头：我们**不用** LVGL 的
 * `LV_EVENT_GESTURE`（它会被滚动系统挡掉），而是自己在
 * PRESSED/PRESSING/RELEASED 里累加位移判定方向。
 *
 * @note v0.3 起 4 个主页面归**跟手轮播**管（页面根不可滚动、
 *       横向滑动交给 viewport），所以本函数只给控制中心这类
 *       独立 screen 用。轮播页不要再调它，否则一次滑动会翻两页。
 */
void watch_gesture_attach(lv_obj_t *scr);

/** @brief 递归给子树打上 EVENT_BUBBLE 标志（一般不用自己调） */
void watch_gesture_mark_subtree(lv_obj_t *obj);

/**
 * @brief 设置"控制中心是否打开"的探测回调
 *
 * 手势层靠它区分：左滑到底是"关闭控制中心"还是"返回上一页"。
 */
void watch_gesture_set_overlay_probe(bool (*is_open)(void));

/**
 * @brief 设置"横向滑动是否由别人接管"的探测回调
 *
 * ⚠️ v0.3 起滑动切页改成了**跟手轮播**（页面放在可横向滚动的 viewport 里，
 *    靠 LVGL 的滚动系统 + 滚动吸附实现）。此时**横向滑动必须让给轮播**，
 *    否则一次滑动会既滚轮播又触发手势翻页 —— 一次滑两页。
 *
 * 轮播建好后会把它设成 `watch_ui_carousel_active`：
 *   返回 true  → 手势层只处理纵向（上滑控制中心 / 下滑关闭）
 *   返回 false → 手势层照旧处理横向（比如控制中心自己的 screen）
 */
void watch_gesture_set_horizontal_probe(bool (*probe)(void));

#ifdef __cplusplus
}
#endif
