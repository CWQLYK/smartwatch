/**
 * @file watch_alert.h
 * @brief 全局到点提醒层（倒计时 / 闹钟）—— v0.5.0 新增
 *
 * ## 为什么是"全局"的
 *
 * 用户明确要求："不管当前在哪个页面都响，并弹一个提示层带【停止】按钮"。
 * 所以它不是某个页面的一部分，而是盖在轮播 screen 上的一个全屏容器，
 * 由时钟服务的定时器驱动显隐（`watch_alert_poll()`）。
 *
 * ## 为什么不用独立 screen（像控制中心那样）
 *
 * 控制中心（`watch_overlay.c`）用的是 `lv_obj_create(NULL)` + `lv_scr_load_anim()`。
 * 提醒层**不能**这么做：模态子页（设置/时钟/WiFi…）都是挂在轮播 screen 上的
 * 普通容器，一旦 `lv_scr_load()` 切走，模态还在原来的 screen 上，
 * 切回来时子页面栈的状态就乱了。
 *
 * 所以提醒层做成**轮播 screen 上的兄弟容器**（和模态同层）：
 *   - 用 `lv_obj_move_foreground()` 保证它在模态之上（闹钟要盖住一切）
 *   - 每次 poll 时重新置顶 —— 万一提醒期间又开了新模态，也能盖住
 *   - 它自带 `LV_OBJ_FLAG_CLICKABLE`，吃掉底下的点击，防止误触
 *
 * ## 自动亮屏
 *
 * 到点时会调 `w_screen_wake()`。否则用户设了闹钟却因为屏幕息着
 * 而看不到提示（息屏只是关背光，LVGL 照跑，所以弹层本身是正常的）。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化提醒层
 *
 * @param host 挂到哪个容器上（传轮播的 screen，即 `watch_ui_screen()`）
 *
 * @note 只在 watch_pages.c 里调一次。会预建好容器并隐藏。
 */
void watch_alert_init(lv_obj_t *host);

/**
 * @brief 按当前提醒状态更新显隐与文案
 *
 * @note 由时钟服务的全局定时器周期调用（见 watch_clock.c 的 clock_timer_cb）。
 *       没有提醒时什么都不做（一个 bool 判断）。
 */
void watch_alert_poll(void);

/** @brief 当前是否显示着提醒层 */
bool watch_alert_visible(void);

#ifdef __cplusplus
}
#endif
