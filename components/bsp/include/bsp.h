/**
 * @file bsp.h
 * @brief 板级支持包 — Waveshare ESP32-S3-Touch-LCD-1.69
 *
 * 职责边界（重构后）：
 *   ✅ 做：LCD 初始化、触摸初始化、背光 PWM、LVGL 移植层
 *   ❌ 不做：引脚定义（搬到 board.h）、I2C 总线所有权（搬到 board 组件）
 *
 * 为什么把 I2C 搬走：
 *   板上触摸(0x15)/IMU(0x6B)/RTC(0x51) 共用一条 I2C，
 *   如果 BSP 持有总线，RTC 驱动就得反向依赖 BSP 才能读寄存器 —— 依赖方向不对。
 *   现在 BSP 只是总线的**使用者**（和 RTC 驱动平级），board 组件才是所有者。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"
/* 引脚宏都从这里来；老代码里的 BSP_LCD_GPIO_xx 名字保留为别名（见下） */
#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 屏幕参数 ==================== */
#define BSP_LCD_H_RES           240
#define BSP_LCD_V_RES           280

/* ==================== 引脚别名（兼容旧代码，实现在 board.h）==================== */
#define BSP_LCD_GPIO_SCLK       BOARD_LCD_GPIO_SCLK
#define BSP_LCD_GPIO_MOSI       BOARD_LCD_GPIO_MOSI
#define BSP_LCD_GPIO_RST        BOARD_LCD_GPIO_RST
#define BSP_LCD_GPIO_DC         BOARD_LCD_GPIO_DC
#define BSP_LCD_GPIO_CS         BOARD_LCD_GPIO_CS
#define BSP_LCD_GPIO_BL         BOARD_LCD_GPIO_BL

#define BSP_TOUCH_GPIO_SCL      BOARD_I2C_GPIO_SCL
#define BSP_TOUCH_GPIO_SDA      BOARD_I2C_GPIO_SDA
#define BSP_TOUCH_GPIO_RST      BOARD_TOUCH_GPIO_RST
#define BSP_TOUCH_GPIO_INT      BOARD_TOUCH_GPIO_INT
#define BSP_TOUCH_I2C_PORT      BOARD_I2C_PORT
#define BSP_TOUCH_I2C_ADDR      BOARD_TOUCH_I2C_ADDR

#define BSP_BUZZER_GPIO         BOARD_BUZZER_GPIO

/* ==================== 手势灵敏度 ====================
 *
 * ⚠️ 这两个值**必须在 lv_indev_drv_register() 之前**设进 driver 结构体，
 *    因为 LVGL 在注册时就把它们拷进 indev 内部了，之后再改不生效。
 *    所以放在 BSP 里设（BSP 是注册输入设备的地方）。
 *
 * LVGL 8.4 的默认值是 gesture_min_velocity=5 / gesture_limit=50：
 *   - limit=50 意味着"要划 50 像素才算手势"，240 宽的屏上要划掉五分之一，
 *     手感非常钝，必须调小。
 *   - min_velocity 是"每次输入采样之间至少要移动几个像素"，
 *     慢速拖动时每帧只动 1-2 像素，默认 5 会让慢划完全失效 → 调到 2。
 *
 * 见 components/lvgl/src/core/lv_indev.c 的 indev_gesture()。
 */
#define BSP_GESTURE_MIN_VELOCITY   2
#define BSP_GESTURE_LIMIT          10

/* ==================== 接口 ==================== */

/**
 * @brief 初始化整块板子（共享 I2C + 屏幕 + 触摸 + 背光 + LVGL）
 * @return ESP_OK 成功
 * @note 内部会先调用 board_init()，所以调用者不必自己初始化总线。
 */
esp_err_t bsp_init(void);

/**
 * @brief 获取 LVGL 显示对象
 * @note  LVGL 8.x 的类型名是 lv_disp_t（LVGL 9 才是 lv_display_t）
 */
lv_disp_t *bsp_display_get(void);

/**
 * @brief 获取 LVGL 输入设备（触摸）
 */
lv_indev_t *bsp_touch_get(void);

/**
 * @brief 设置背光亮度
 * @param percent 0-100
 */
esp_err_t bsp_backlight_set(int percent);

/**
 * @brief 获取背光当前亮度（0-100）
 */
int bsp_backlight_get(void);

/**
 * @brief 获取 LVGL 锁（操作 UI 前必须调用）
 * @param timeout_ms 超时，-1(UINT32_MAX) 表示永久等待，0 表示不等待
 */
bool bsp_display_lock(uint32_t timeout_ms);

/**
 * @brief 释放 LVGL 锁
 */
void bsp_display_unlock(void);

#ifdef __cplusplus
}
#endif
