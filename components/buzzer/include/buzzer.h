/**
 * @file buzzer.h
 * @brief 蜂鸣器驱动（GPIO42，LEDC PWM 单音）
 *
 * ⚠️ 这块板上的蜂鸣器是**无源蜂鸣器**（不是喇叭）：
 *    只能发单音/方波，放不了音乐，但足够做按键音、提醒音、闹钟。
 *
 * 资源占用：LEDC **TIMER_1 + CHANNEL_1**
 *          （TIMER_0 / CHANNEL_0 已经被 LCD 背光占了，别撞）
 *
 * 用法：
 *      buzzer_init();
 *      buzzer_beep(BUZZER_BEEP_OK);      // 异步，响完自己停
 *      buzzer_tone(2700);                // 手动起音
 *      buzzer_stop();
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 常用预设 ==================== */
typedef enum {
    BUZZER_BEEP_TICK = 0,   ///< 极短"嘀" 20ms —— 触摸反馈
    BUZZER_BEEP_OK,         ///< 短"嘀" 60ms
    BUZZER_BEEP_DOUBLE,     ///< 两短声 —— 收到通知
    BUZZER_BEEP_LONG,       ///< 长音 500ms —— 闹钟/倒计时结束
    BUZZER_BEEP_ERROR,      ///< 三短声 —— 错误/低电告警
    BUZZER_BEEP_COUNT
} buzzer_beep_t;

/**
 * @brief 初始化蜂鸣器（占用 LEDC TIMER_1/CHANNEL_1）
 * @note 幂等
 */
esp_err_t buzzer_init(void);

/**
 * @brief 起一个指定频率的持续音（**不阻塞**）
 * @param freq_hz 频率，建议 2000-4000（板上蜂鸣器谐振点一般在 2.7kHz 附近）
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t buzzer_tone(uint32_t freq_hz);

/**
 * @brief 停音
 */
esp_err_t buzzer_stop(void);

/**
 * @brief 响一段时间的音（**非阻塞**，时间到自动停）
 *
 * @param freq_hz   频率
 * @param duration_ms 持续时间
 *
 * @note 内部用 esp_timer 单次定时器关音，所以**不需要调用者起任务**，
 *       也不会阻塞 LVGL 任务。
 */
esp_err_t buzzer_tone_ms(uint32_t freq_hz, uint32_t duration_ms);

/**
 * @brief 播放一个预设提示音（非阻塞）
 */
esp_err_t buzzer_beep(buzzer_beep_t kind);

/**
 * @brief 当前是否在响
 */
bool buzzer_is_playing(void);

#ifdef __cplusplus
}
#endif
