/**
 * @file watch_wheel.h
 * @brief iOS 风格滚轮选择器弹窗（v0.5.0 新增）
 *
 * ## 为什么要有它
 *
 * 时钟页要用它设三样东西：自定义倒计时时长（时/分/秒）、
 * 倒计时响铃时长（分/秒）、闹钟时间（时/分）。
 * 三处交互完全一样，所以抽成一个可复用组件，而不是写三遍。
 *
 * ## 交互
 *
 * 用户明确要求"像苹果系统那样"：每个位置上下滑动来增减数值。
 * 实现用 LVGL 的 `lv_roller` + `LV_ROLLER_MODE_INFINITE`（循环滚动）。
 *
 * ```
 *   ┌──────────────────────────────┐
 *   │  自定义定时           [返回]  │
 *   │   ┌────┐  ┌────┐  ┌────┐    │
 *   │   │ 时 │  │ 分 │  │ 秒 │    │   ← 单位标签（watch_font_cn(14)）
 *   │   │  0 │  │ 05 │  │ 30 │    │   ← 选中行高亮成主色
 *   │   │  1 │  │ 06 │  │ 31 │    │
 *   │   └────┘  └────┘  └────┘    │   ← 上下拖动增减
 *   │   [   取消   ] [   确定   ]  │
 *   └──────────────────────────────┘
 * ```
 *
 * ## 手势冲突（★ 这一条已经读过 LVGL 源码确认，别再怀疑）
 *
 * 模态子页的容器是**可纵向滚动**的，roller 自己也要吃纵向拖动 ——
 * 看上去会打架，实际不会：
 *
 *   `lv_roller` 的构造函数里做了两件事（`lv_roller.c:301-302`）：
 *       lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
 *       lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
 *   于是 `find_scroll_obj()` 走到 roller 时命中这一条
 *   （`lv_indev_scroll.c:305-308`）：
 *       if(SCROLLABLE == false) {
 *           if(SCROLL_CHAIN_VER == false && ver_en) break;   // ★ 搜索到此为止
 *       }
 *   **搜索直接 break，不会继续往上找到模态页**。
 *
 * 也就是说 roller 是"自己处理拖动、并且主动阻断向父级链式滚动"的，
 * 拖动滚轮时整个页面**不会跟着晃**。所以本组件不需要动 watch_modal.c。
 *
 * （如果以后换成别的控件，一定要重新确认这一条 —— 项目里已经有
 *   "find_scroll_obj 把无溢出的 SCROLLABLE 也当候选" 的前车之鉴。）
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 最多几列（时/分/秒） */
#define W_WHEEL_MAX_COLS  3

/** @brief 滚轮配置 */
typedef struct {
    const char *title;                      /**< 子页标题（中文） */

    int         cols;                       /**< 列数，1~3 */

    /**
     * @brief 每列上方的单位标签（中文，如 {"时","分","秒"}）
     * @note 传 NULL 表示该列不显示单位。都传 NULL 就不画单位行。
     */
    const char *unit[W_WHEEL_MAX_COLS];

    int         min[W_WHEEL_MAX_COLS];      /**< 每列最小值（含） */
    int         max[W_WHEEL_MAX_COLS];      /**< 每列最大值（含） */
    int         init[W_WHEEL_MAX_COLS];     /**< 初值；越界会自动夹到 [min,max] */

    /**
     * @brief 点"确定"时回调
     *
     * @param val  各列的最终值（长度 = cols，**已经把 min 加回去了**）
     * @param user 原样透传的 user 指针
     *
     * @note ⚠️ 回调里**不要**碰 modal（别调 w_modal_refresh / w_modal_close）：
     *       本组件在回调返回后会自己 `w_modal_close()` 关掉弹窗，
     *       而 `w_modal_close()` 会自动重建上一层页面 —— 值就刷新了。
     */
    void      (*on_ok)(const int *val, void *user);

    void       *user;                       /**< 透传给 on_ok */
} w_wheel_cfg_t;

/**
 * @brief 打开滚轮选择器
 *
 * @param cfg 配置（**调用方可以放在栈上**，内部会拷贝一份）
 * @return 0 成功；-1 失败（参数非法 / 模态栈满）
 *
 * @note 内部把 cfg 拷贝到静态存储，所以 cfg 里的 `unit[]` 请指向
 *       生命周期足够长的字符串（一般就是字面量）。
 */
int w_wheel_open(const w_wheel_cfg_t *cfg);

/**
 * @brief 调试/自检用：读回当前各列的选中值（**不关闭**弹窗）
 *
 * @param val    输出数组
 * @param max_n  数组长度
 * @return 实际写入的列数；弹窗没开时返回 -1
 *
 * @note 为什么需要它：滚轮的"选中项"在 LVGL 里是
 *       `sel_opt_id`（infinite 模式下要取模），肉眼在 240×280 的小屏上
 *       盯着数字看**不可靠**。PC 模拟器自检用这个接口断言
 *       "init 填进去的值能不能原样读回来"，一眼就能发现错位。
 */
int w_wheel_debug_get(int *val, int max_n);

#ifdef __cplusplus
}
#endif
