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
 * 约定：每个枚举值对应 settings.c 里 s_desc[] 的**一行（下标）**。
 *
 * ⚠️ 这里曾经写的是"枚举值会被当作 NVS 索引使用"，**那是错的**：
 *    NVS 存的是**字符串键名**（s_desc[i].nvs_key，如 "bl"/"scr_to"），
 *    枚举值只是 s_desc[] 的数组下标，两者没有关系。
 *    所以：
 *      · 删掉中间某一项**不会**让别的项读错（旧 NVS 键只是被忽略）
 *      · 但 SETTINGS_BIT() 掩码是按枚举值算的，运行期事件掩码会变
 *    新增设置项仍然**一律加在末尾**（省得翻历史、也方便对照版本记录）。
 * ================================================================== */
typedef enum {
    SETTINGS_BRIGHTNESS = 0,   ///< 屏幕亮度 5-100 (%)
    SETTINGS_SCREEN_TIMEOUT,   ///< 自动息屏时间 5-120 (秒)，0 = 永不
    SETTINGS_STEP_GOAL,        ///< 每日步数目标 1000-50000 (步)
    SETTINGS_TIMEZONE,         ///< 时区偏移，单位 15 分钟（+8 区 = 32）
    SETTINGS_TIME_VALID,       ///< RTC 是否已校时过 0/1（掉电检测用，见下）
    SETTINGS_TIME_EPOCH,       ///< 最后一次已知 Unix 时间戳（RTC 掉电后的兜底）
    /* ---------- v0.5.0 新增：闹钟 / 倒计时 ---------- */
    SETTINGS_ALARM_HOUR,       ///< 闹钟-时 0-23
    SETTINGS_ALARM_MIN,        ///< 闹钟-分 0-59
    SETTINGS_ALARM_EN,         ///< 闹钟开关 0/1
    SETTINGS_CD_PRESET_SEC,    ///< 倒计时上次选的时长（秒）
    SETTINGS_CD_RING_SEC,      ///< 倒计时到点后的响铃时长（秒），默认 10
    SETTINGS_COUNT             ///< 哨兵，必须放最后
} settings_key_t;

/*
 * v0.5.0 删除的三项（按用户要求），**连枚举和 NVS 键一起删**：
 *   SETTINGS_USER_HEIGHT / SETTINGS_USER_WEIGHT  —— 步长改为固定值，不再按身高推算
 *   SETTINGS_WRIST_WAKE                          —— 抬腕亮屏功能取消
 * 老设备 NVS 里残留的 "height"/"weight"/"wrist" 三个键会被直接忽略，
 * 不会影响其它设置项（理由见上面的说明）。
 */

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
