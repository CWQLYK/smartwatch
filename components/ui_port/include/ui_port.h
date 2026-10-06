/**
 * @file ui_port.h
 * @brief UI 平台端口层 —— 把"平台差异"关在这一个头文件后面
 *
 * 为什么需要它：
 *   我们希望**同一套界面代码**既能跑在手表上，也能跑在 PC 模拟器里
 *   （改 UI 几秒就能看效果，不用每次烧板子）。
 *   但两边有两处根本差异：
 *
 *   | 差异 | 手表端 | PC 端 |
 *   |---|---|---|
 *   | LVGL 跑在哪 | 独立 FreeRTOS 任务，**碰 UI 必须加锁** | 主线程里跑，天然无需锁 |
 *   | 显示/输入设备 | esp_lvgl_port 提供 | lv_drivers 的 win32drv 提供 |
 *
 *   于是：**界面代码只调用本头文件的函数**，两边各给一份实现：
 *     - 手表端：components/ui_port/ui_port_target.c   （加锁 = lvgl_port_lock）
 *     - PC 端： lvgl_sim/src/ui_port_pc.c             （加锁 = 空操作）
 *
 *   这样 watch_ui / watch_ui_pages 里**不会出现任何 ESP-IDF 头文件**。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 UI 平台层（显示 + 输入设备 + LVGL 就绪）
 *
 * @note 手表端：内部就是 bsp_init()（屏幕/触摸/背光/LVGL 任务）
 *       PC 端  ：内部是 win32drv 初始化，窗口 240x280
 */
esp_err_t ui_port_init(void);

/**
 * @brief 获取 LVGL 显示对象
 * @note  LVGL 8.x 的类型名是 lv_disp_t（LVGL 9 才是 lv_display_t）
 */
lv_disp_t *ui_port_display(void);

/**
 * @brief 获取触摸输入设备（可为 NULL，PC 端由 lv_drivers 自行注册）
 */
lv_indev_t *ui_port_indev(void);

/**
 * @brief 取 LVGL 锁（**碰任何 lv_xxx 之前必须调用**）
 *
 * @param timeout_ms 超时；0 = 不等待，-1(UINT32_MAX) = 永久等待
 * @return true 拿到锁
 *
 * @code
 *   if (ui_port_lock(0)) {
 *       lv_label_set_text(lbl, "ok");
 *       ui_port_unlock();
 *   }
 * @endcode
 */
bool ui_port_lock(uint32_t timeout_ms);

/**
 * @brief 释放 LVGL 锁
 */
void ui_port_unlock(void);

/**
 * @brief 平台主循环（**只有 PC 端需要调用**；手表端是空实现）
 *
 * 手表端 LVGL 在自己的 FreeRTOS 任务里跑，app_main 可以挂起；
 * PC 端必须自己转 lv_timer_handler()，否则界面不刷新。
 *
 * @param sleep_ms 每次循环的休眠毫秒数，建议 5
 */
void ui_port_run(uint32_t sleep_ms);

/**
 * @brief 平台名字（打日志区分"这块表跑在哪"用）
 */
const char *ui_port_name(void);

#ifdef __cplusplus
}
#endif
