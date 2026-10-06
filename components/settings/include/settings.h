/**
 * @file settings.h
 * @brief 设置服务 —— 所有可持久化配置的唯一入口（存 NVS，掉电不丢）
 *
 * 为什么要有它：
 *   亮度、息屏时间、步数目标、用户身高体重……这些项以后会被
 *   设置页 / 手机 BLE / AI 语音三处修改。如果各改各的 NVS 键，
 *   很快就会出现"键名拼错""取值范围没人管"的问题。
 *   所以全部收口到这一组 accessor。
 *
 * 设计：
 *   - **表驱动**：每个设置项一行（键名 / 默认值 / 上下限），
 *     加新设置 = 加一行 + 加一个枚举值，其余代码自动适配。
 *   - 修改后自动发布 EVT_SETTINGS_CHANGED，关心它的模块自己响应。
 *   - 内部有互斥量，多任务可安全读写。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、设置项清单
 *
 * 约定：每个枚举值对应 settings.h 里的一行表项。
 *       新增设置项时**只能往末尾加**（枚举值会被当作 NVS 索引使用）。
 * ================================================================== */
typedef enum {
    SETTINGS_BRIGHTNESS = 0,   ///< 屏幕亮度 5-100 (%)
    SETTINGS_SCREEN_TIMEOUT,   ///< 自动息屏时间 5-120 (秒)，0 = 永不
    SETTINGS_STEP_GOAL,        ///< 每日步数目标 1000-50000 (步)
    SETTINGS_USER_HEIGHT,      ///< 身高 100-250 (cm)，用于步长估算
    SETTINGS_USER_WEIGHT,      ///< 体重 30-200 (kg)，用于卡路里估算
    SETTINGS_WRIST_WAKE,       ///< 抬腕亮屏 0/1
    SETTINGS_TIMEZONE,         ///< 时区偏移，单位 15 分钟（+8 区 = 32）
    SETTINGS_TIME_VALID,       ///< RTC 是否已校时过 0/1（掉电检测用，见下）
    SETTINGS_TIME_EPOCH,       ///< 最后一次已知 Unix 时间戳（RTC 掉电后的兜底）
    SETTINGS_COUNT             ///< 哨兵，必须放最后
} settings_key_t;

/** @brief 变更位掩码：1UL << settings_key_t */
#define SETTINGS_BIT(key)   (1UL << (key))

/** @brief NVS 命名空间 */
#define SETTINGS_NVS_NAMESPACE  "watch"

/* ==================================================================
 * 二、四个 API（对齐全项目统一风格）
 * ================================================================== */

/**
 * @brief 初始化设置服务（内部会保证 NVS 已初始化）
 * @note 幂等
 */
esp_err_t settings_init(void);

/**
 * @brief 读一个设置项
 * @param key   设置项
 * @param[out] out 读出值
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NOT_FOUND（key 非法）
 */
esp_err_t settings_get(settings_key_t key, int32_t *out);

/**
 * @brief 写一个设置项（自动夹到合法范围并存 NVS）
 *
 * @note 只发布**实际发生变化**的项对应的 EVT_SETTINGS_CHANGED。
 *       任何任务都可以调；不要在中断里调。
 */
esp_err_t settings_set(settings_key_t key, int32_t value);

/**
 * @brief 便捷读：带默认值，读失败直接返回默认值（UI 里用得最多）
 */
int32_t settings_get_or_default(settings_key_t key);

/**
 * @brief 便捷写：一次改多个设置项，只发一次事件
 *
 * @param mask  要改哪些项，用 SETTINGS_BIT() 按位或
 * @param value 值（所有被 mask 选中的项都写成这个值）
 *
 * @note 主要用于"恢复出厂设置"这类批量操作。
 */
esp_err_t settings_set_masked(uint32_t mask, int32_t value);

/**
 * @brief 把某个设置项恢复成编译期默认值
 */
esp_err_t settings_reset(settings_key_t key);

/**
 * @brief 把**所有**设置项恢复默认值
 */
esp_err_t settings_reset_all(void);

/**
 * @brief 取某项的默认值（UI 显示"默认值"时用）
 */
int32_t settings_default(settings_key_t key);

/**
 * @brief 取某项的合法范围（UI 做滑块范围时用）
 */
void settings_range(settings_key_t key, int32_t *min, int32_t *max);

/**
 * @brief 把当前所有设置打印到日志（排查问题用）
 */
void settings_dump(void);

#ifdef __cplusplus
}
#endif
