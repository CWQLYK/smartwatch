/**
 * @file power.h
 * @brief 电源/屏幕管理 —— 自动息屏、息屏后首次触摸只亮屏
 *
 * ## 本版本（v0.4.0）实现的范围
 *
 * **只关背光，不做低功耗。** CPU、LVGL、IMU 采样全部照跑。
 * 用户明确要求"先不做低功耗，先完成其他功能"。
 *
 * 好处是逻辑极简单、不会有"睡下去醒不过来"的坑；
 * 代价是息屏省不了多少电（背光是耗电大头，关掉它已经省下大头了）。
 *
 * ## 息屏后的第一次触摸：只亮屏，不触发界面动作
 *
 * 这是用户明确要求的（也是绝大多数手表的做法）：
 * 息屏时放口袋里很容易误触，如果那一下直接生效，会莫名其妙翻页或改设置。
 *
 * 实现方式：LVGL 的输入设备读到一个"按下"时，我们**先把这次按下吃掉**
 * （不转发给界面），只做亮屏；用户抬手再按第二下才正常操作。
 *
 * ## "别息屏"的引用计数
 *
 * 录制 IMU 时不能息屏（否则用户以为在录、其实看不出状态）。
 * 但以后可能还有别的持有者（正在输密码、正在校时…），
 * 所以用**引用计数**而不是布尔：谁先要谁加，各自解各自的，互不干扰。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化电源管理并注册界面钩子
 *
 * @note 幂等。它会把自己的实现注册成 w_platform 的 screen 钩子。
 */
esp_err_t power_init(void);

/**
 * @brief 通知"有用户操作"（重置息屏计时）
 *
 * @note 由触摸路径调用。息屏状态下调它会**只亮屏**并吃掉这次操作。
 */
void power_notify_activity(void);

/**
 * @brief 主动亮屏（不影响息屏计时，计时会一起重置）
 */
void power_screen_on(void);

/**
 * @brief 立即息屏（调试/串口命令用）
 */
void power_screen_off(void);

/** @brief 屏幕（背光）现在是否亮着 */
bool power_screen_is_on(void);

/**
 * @brief 加/解"别息屏"引用计数
 *
 * @param hold true = 加一次（暂时不要息屏）；false = 解一次
 *
 * @note 计数降到 0 时**重新开始**计时，而不是立刻息屏 ——
 *       否则录制一停就马上黑屏，体验很突兀。
 */
void power_hold_awake(bool hold);

/** @brief 当前引用计数（调试用） */
int power_hold_count(void);

/** @brief 打印状态（串口命令用） */
void power_dump(void);

#ifdef __cplusplus
}
#endif
