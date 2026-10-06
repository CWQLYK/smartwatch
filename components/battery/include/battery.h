/**
 * @file battery.h
 * @brief 电池电压采样驱动（GPIO1 / BAT_ADC）
 *
 * 硬件：B+ 经 **R3=200K / R7=100K** 分压到 GPIO1
 *       → 分压比 = (200+100)/100 = **3**
 *       → 电池电压 = ADC 读到的电压 × 3
 *
 * 精度：**必须做 ADC 校准**，否则读数能差 0.1V 以上。
 *       ESP32-S3 走 adc_cali **曲线拟合**方案（芯片出厂校准曲线）。
 *
 * 锂电池经验值：4.20V 满 / 3.70V 标称 / 3.30V 低电告警 / 3.00V 空
 *
 * ⚠️ 注意：数据手册的"电压-电量"曲线是非线性的，用线性插值在 3.7V 附近
 *    会偏乐观（显示 60% 实际可能只有 45%）。P1 先用线性 + 分段修正，
 *    以后要更准就换成 OCV 查表 + 静置判断。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 电压阈值（单位 mV） ==================== */
#define BAT_FULL_MV        4200    ///< 满电
#define BAT_NOMINAL_MV     3700    ///< 标称
#define BAT_LOW_WARN_MV    3300    ///< 低电告警线（UI 变红 + 蜂鸣器提醒）
#define BAT_EMPTY_MV       3000    ///< 空电
#define BAT_PRESENT_MV     2500    ///< 低于此值认为**没接电池**（虚浮电压）
#define BAT_CHARGING_MV    4150    ///< 高于此值且电压在涨 → 判定为充电中

/* ==================== 状态结构 ==================== */
typedef struct {
    int  millivolt;   ///< 电池电压（已含分压还原），单位 mV
    int  percent;     ///< 0-100；**-1 表示无法估算**（没接电池）
    bool present;     ///< 是否检测到电池
    bool charging;    ///< 是否在充电（电压趋势推断）
} battery_reading_t;

/**
 * @brief 初始化电池采样（ADC oneshot + 校准）
 * @note 幂等。校准失败不会让初始化失败，而是退化成未校准模式（精度差但能用）。
 */
esp_err_t battery_init(void);

/**
 * @brief 采一次电压（**阻塞约几毫秒**，内部多次采样取中值）
 *
 * @param[out] out 读数
 * @note 建议由 battery_svc 以 1-5 秒周期调用，不要在 UI 任务里直接调。
 */
esp_err_t battery_read(battery_reading_t *out);

/**
 * @brief 只取电压（mV）；失败返回 -1
 */
int battery_get_millivolt(void);

/**
 * @brief 电压 → 电量百分比（线性 + 分段修正）
 * @return 0-100；-1 表示没接电池
 */
int battery_voltage_to_percent(int millivolt);

/**
 * @brief 是否正在充电
 *
 * @note 板上 **ETA6098 没有引出充电状态引脚**，所以只能靠
 *       "电压 ≥ 4.15V 且最近在上升"来推断，不是 100% 可靠。
 */
bool battery_is_charging(void);

/**
 * @brief 校准是否生效（调试用：串口会打印）
 */
bool battery_cali_ok(void);

#ifdef __cplusplus
}
#endif
