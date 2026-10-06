/**
 * @file board.h
 * @brief 板级资源表 —— 所有引脚、I2C 地址、共享总线都收在这里
 *
 * 设计意图（对标 ESP-Brookesia 的「HAL 用 YAML 描述板子」思想）：
 *   全项目**只有这一个文件**知道 GPIO 号和外设地址。
 *   换板子 = 只改这个文件 + bsp.c，驱动/服务/UI 一行不动。
 *
 * 目录位置：components/board/
 *   上层谁需要引脚，就 include "board.h"，不要再硬编码 GPIO 号。
 */
#pragma once

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、引脚分配（来自原理图 V2.1）
 * ================================================================== */

/* ---- LCD：ST7789V2，4 线 SPI ---- */
#define BOARD_LCD_GPIO_SCLK     GPIO_NUM_6
#define BOARD_LCD_GPIO_MOSI     GPIO_NUM_7
#define BOARD_LCD_GPIO_RST      GPIO_NUM_8
#define BOARD_LCD_GPIO_DC       GPIO_NUM_4
#define BOARD_LCD_GPIO_CS       GPIO_NUM_5
#define BOARD_LCD_GPIO_BL       GPIO_NUM_15   /* 背光，LEDC PWM 调光 */

/* ---- 共享 I2C 总线：触摸 + IMU + RTC 都挂在这两条线上 ---- */
#define BOARD_I2C_PORT          0             /* I2C_NUM_0 */
#define BOARD_I2C_GPIO_SCL      GPIO_NUM_10
#define BOARD_I2C_GPIO_SDA      GPIO_NUM_11
#define BOARD_I2C_FREQ_HZ       400000        /* 400 kHz */

/* ---- 触摸：CST816T ---- */
#define BOARD_TOUCH_I2C_ADDR    0x15
#define BOARD_TOUCH_GPIO_RST    GPIO_NUM_13
#define BOARD_TOUCH_GPIO_INT    GPIO_NUM_14

/* ---- IMU：QMI8658C（P1 暂未使用，地址先登记，避免以后撞车）---- */
#define BOARD_IMU_I2C_ADDR      0x6B
#define BOARD_IMU_GPIO_INT2     GPIO_NUM_38

/* ---- RTC：PCF85063ATL ---- */
#define BOARD_RTC_I2C_ADDR      0x51
#define BOARD_RTC_GPIO_INT      GPIO_NUM_39

/* ---- 电池电压采样：B+ 经 R3=200K / R7=100K 分压到 GPIO1 ---- */
#define BOARD_BAT_GPIO_ADC      GPIO_NUM_1
#define BOARD_BAT_ADC_UNIT      ADC_UNIT_1
#define BOARD_BAT_ADC_CHANNEL   ADC_CHANNEL_0
#define BOARD_BAT_DIVIDER_NUM   3             /* (R3+R7)/R7 = 300/100 = 3 */
#define BOARD_BAT_DIVIDER_DEN   1

/* ---- 蜂鸣器：GPIO42，PWM 单音（不是喇叭）---- */
#define BOARD_BUZZER_GPIO       GPIO_NUM_42

/* ---- 空闲扩展口（做扩展要省着用）---- */
#define BOARD_GPIO_FREE_1       GPIO_NUM_2
#define BOARD_GPIO_FREE_2       GPIO_NUM_3
#define BOARD_GPIO_FREE_3       GPIO_NUM_17
#define BOARD_GPIO_FREE_4       GPIO_NUM_18

/* ==================================================================
 * 二、板级资源初始化 / 共享总线
 * ================================================================== */

/**
 * @brief 初始化板级公共资源（目前只有共享 I2C 总线）
 *
 * @note **幂等**，重复调用直接返回 ESP_OK。
 *       bsp_init() 内部会先调它，独立驱动组件也可以自己先调一次保证安全。
 */
esp_err_t board_init(void);

/**
 * @brief 取共享 I2C 总线句柄
 * @return 总线句柄；未初始化返回 NULL
 */
i2c_master_bus_handle_t board_i2c_bus(void);

/**
 * @brief 在共享总线上挂一个 I2C 从设备
 *
 * @param addr_7bit  7 位从机地址（如 0x51）
 * @param freq_hz    该设备的时钟频率，一般传 BOARD_I2C_FREQ_HZ
 * @param[out] out   返回的设备句柄
 *
 * @note 同一个地址重复调用会返回新句柄（底层会告警），请各驱动自己只调一次。
 */
esp_err_t board_i2c_add_device(uint8_t addr_7bit, uint32_t freq_hz,
                               i2c_master_dev_handle_t *out);

/**
 * @brief 读寄存器：先写 1 字节寄存器地址，再读 len 字节
 */
esp_err_t board_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                             uint8_t *buf, size_t len);

/**
 * @brief 写寄存器：写 1 字节寄存器地址 + len 字节数据
 */
esp_err_t board_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                              const uint8_t *buf, size_t len);

/**
 * @brief 探测某个 7 位地址上是否有设备应答（用于启动自检）
 * @return ESP_OK 有应答；ESP_ERR_NOT_FOUND 没有
 */
esp_err_t board_i2c_probe(uint8_t addr_7bit);

#ifdef __cplusplus
}
#endif
