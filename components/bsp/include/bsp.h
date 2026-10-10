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
 * @brief LVGL 任务心跳（v0.7.1 诊断用）：每次触摸读回调自增
 *
 * 为什么放在这里：`touch_read_cb` 由 LVGL 任务每 `LV_INDEV_DEF_READ_PERIOD`(10ms) 调一次，
 * 是"界面任务还活着"最便宜、最不依赖内核的观测点（读一个 volatile 变量即可，
 * **不需要拿内核锁** —— 卡死哨兵就是靠它判断系统是不是停住了）。
 *
 * ⚠️ 只在诊断里读，业务逻辑不要用它。
 */
extern volatile uint32_t g_bsp_lvgl_beat;

/**
 * @brief 面板显示开关（ST7789 DISPON 0x29 / DISPOFF 0x28）—— v0.7.1 省电
 *
 * @param on true = 开显示输出；false = 关显示输出（面板不再扫描，省静态功耗）
 *
 * @note **非阻塞**：只发一条命令（`esp_lcd_panel_io_tx_param`），没有 delay，
 *       所以**可以在任意任务/定时器回调里调用**（包括 `power.c` 的 esp_timer 回调）。
 *
 * @note 它**只管面板显示输出**，不碰背光、不碰 LVGL 刷新 ——
 *       那两件事由 `power.c` 的 `power_screen_on/off()` 负责编排。
 *
 * @note 与"面板睡眠"（SLPIN/SLPOUT）不是一回事：那个更深，但 IDF 的 ST7789 驱动
 *       在 `esp_lcd_panel_disp_sleep()` 内部**无条件 vTaskDelay(100ms)**，
 *       谁调谁阻塞，**禁止**从 esp_timer 回调里调
 *       （见 `doc/智能手表-低功耗实施文档.md` §3.2 与 §四.5）。
 */
esp_err_t bsp_lcd_display(bool on);

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
