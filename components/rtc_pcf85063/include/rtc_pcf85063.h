/**
 * @file rtc.h
 * @brief PCF85063ATL 实时时钟驱动（I2C 0x51，共享总线）
 *
 * 定位：**只做芯片寄存器到"日历时间"的搬运**，不含时区/格式化/兜底逻辑
 *       （那些属于 time_svc 服务层，因为它还牵扯 NVS 和事件总线）。
 *
 * ⚠️ 已知硬件疑点（已写进项目文档 7.2 节风险 7）：
 *    原理图上 PCF85063 **只接了 32.768kHz 晶振，没看到备份电池/超级电容**。
 *    如果实测确认没有备份电源 → 主电断开后时间会丢。
 *    因此本驱动提供 rtc_pcf_time_lost() 让上层识别"时间不可信"，
 *    由 BLE（P2）/ SNTP（P2）/ 串口命令重新校时。
 *
 * 参考：官方示例 examples/esp-idf/03_PCF85063/main/pcf85063.c
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、寄存器地址（来自 PCF85063 数据手册 + SensorLib 常量表）
 * ================================================================== */
#define RTC_REG_CTRL1       0x00    /* bit5=STOP, bit1=12/24, bit0=CAP_SEL   */
#define RTC_REG_CTRL2       0x01    /* bit7=AIE, bit5=AF, bit4=MI, bit3=TF... */
#define RTC_REG_OFFSET      0x02
#define RTC_REG_RAM         0x03    /* 1 字节通用 RAM（我们用来存"已校时"魔数）*/
#define RTC_REG_SECONDS     0x04    /* bit7=OSF 停振标志！                    */
#define RTC_REG_MINUTES     0x05
#define RTC_REG_HOURS       0x06
#define RTC_REG_DAYS        0x07
#define RTC_REG_WEEKDAYS    0x08
#define RTC_REG_MONTHS      0x09    /* bit7=C 世纪位                         */
#define RTC_REG_YEARS       0x0A

#define RTC_CTRL1_STOP      (1u << 5)
#define RTC_CTRL1_12H       (1u << 1)
#define RTC_SEC_OSF         (1u << 7)   /* Oscillator Stop Flag：1 = 掉过电 */

/** @brief 写入 RTC RAM 的魔数，用于判断"这块表有没有被校时过" */
#define RTC_RAM_MAGIC       0x5A

/**
 * @brief 芯片温度的合理范围（单位 0.01°C）
 *
 * 超出这个范围就认为"这颗芯片没有温度通道"，
 * rtc_pcf_get_chip_temp() 会返回 ESP_ERR_NOT_SUPPORTED。
 * 实测这块板读出来是 -127.5°C（见该函数实现里的说明）。
 */
#define RTC_PCF_TEMP_MIN_CENTI   (-4000)    /* -40.00 °C */
#define RTC_PCF_TEMP_MAX_CENTI   ( 12500)   /* +125.00 °C */

/* ==================================================================
 * 二、时间结构
 * ================================================================== */

/**
 * @brief 日历时间（人类可读，字段范围已收敛）
 *
 * @note year 是**完整年份**（如 2026），底层是 2 位 BCD，驱动负责 2000 偏移。
 * @note weekday 是**ISO 习惯**：1=周一 … 7=周日（PCF85063 原生定义）
 */
typedef struct {
    int  year;      ///< 2000-2099
    int  month;     ///< 1-12
    int  day;       ///< 1-31
    int  hour;      ///< 0-23
    int  minute;    ///< 0-59
    int  second;    ///< 0-59
    int  weekday;   ///< 1=周一 … 7=周日
} rtc_pcf_time_t;

/* ==================================================================
 * 三、接口
 * ================================================================== */

/**
 * @brief 初始化 RTC（挂到共享 I2C 总线）
 *
 * 做三件事：
 *   1. 探测 0x51 是否应答（不应答直接返回错误，上层降级）
 *   2. CTRL1 设为 24 小时制 + 正常运行
 *   3. 读回 RAM 魔数，记录这块表历史上有没有被校时过
 *
 * @note 幂等
 */
esp_err_t rtc_pcf_init(void);

/**
 * @brief 读当前时间
 * @param[out] out 时间结构
 */
esp_err_t rtc_pcf_get_time(rtc_pcf_time_t *out);

/**
 * @brief 设置时间（会自动计算星期、清 OSF 停振标志、写 RAM 魔数）
 */
esp_err_t rtc_pcf_set_time(const rtc_pcf_time_t *in);

/**
 * @brief 用 Unix 时间戳设置时间（BLE/SNTP 校时用这个）
 * @param epoch_sec UTC 秒
 */
esp_err_t rtc_pcf_set_epoch(uint32_t epoch_sec);

/**
 * @brief 读成 Unix 时间戳（UTC 秒）
 * @return 0 表示读取失败或时间不可信（可用 rtc_pcf_time_lost() 区分）
 */
uint32_t rtc_pcf_get_epoch(void);

/**
 * @brief 时间是否"丢失/不可信"
 *
 * @return true  = 检测到停振标志 OSF 或 RAM 魔数不对 → 上层应显示 "--:--"
 * @return false = RTC 走时正常，时间可信
 */
bool rtc_pcf_time_lost(void);

/**
 * @brief 清掉停振标志（设置时间时驱动已自动清；这里供上层手动清）
 */
esp_err_t rtc_pcf_clear_lost(void);

/**
 * @brief 读芯片温度补偿寄存器（0x0B/0x0C），调试用；返回摄氏度 × 100
 */
esp_err_t rtc_pcf_get_chip_temp(int *centi_celsius);

/**
 * @brief 计算某天是星期几（ISO：1=周一 … 7=周日）
 *
 * @note 公开出来是因为 BLE 校时时可能只给日期不给星期，需要自己推。
 */
int rtc_pcf_weekday_from_date(int year, int month, int day);

/**
 * @brief 判断日期字段是否在合法范围（设置时间前自检）
 */
bool rtc_pcf_time_is_valid(const rtc_pcf_time_t *t);

#ifdef __cplusplus
}
#endif
