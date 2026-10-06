/**
 * @file watch_overlay.h
 * @brief 控制中心（上滑面板）—— 表盘之上的一层快捷开关
 *
 * 为什么放在框架层而不是页面层：
 *   `watch_ui` 的页面管理器需要知道"控制中心开着没"
 *   （决定左滑是"关面板"还是"返回上一页"），
 *   如果控制中心定义在页面层就会形成循环依赖。
 *   所以框架层声明接口，页面层提供实现。
 *
 * 界面内容（P1 范围）：
 *   ┌────────────────────┐
 *   │  12:34    [🔋 85%] │  当前时间 + 电量（对应 P1 功能 3、4）
 *   │  ────────────────  │
 *   │  亮度  ●────────   │  亮度滑块（P1 功能 9，联动设置项）
 *   │  ────────────────  │
 *   │  [息屏]   [关于]   │  占位按钮
 *   └────────────────────┘
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"
#include "watch_pages.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 建好控制中心（只建一次，之后只做显示/隐藏）
 *
 * @param provider 数据源（和页面管理器同一个）
 * @note 调用者要持 UI 锁
 */
esp_err_t watch_overlay_create(const watch_data_provider_t *provider);

/**
 * @brief 显示控制中心（从下往上滑入）
 */
void watch_overlay_show(void);

/**
 * @brief 隐藏控制中心（向下滑出）
 */
void watch_overlay_hide(void);

/**
 * @brief 控制中心是否正在显示
 *
 * @note 这个函数被手势层每帧查询，必须**极快且无副作用**
 *       （只是读一个 bool）。
 */
bool watch_overlay_visible(void);

/**
 * @brief 数据变了，刷新面板上的时间/电量
 */
void watch_overlay_refresh(const watch_state_t *state, uint32_t changed);

/**
 * @brief 面板上的控件是否已建好
 */
bool watch_overlay_ready(void);

#ifdef __cplusplus
}
#endif
