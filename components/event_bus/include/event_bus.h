/**
 * @file event_bus.h
 * @brief 事件总线 —— 服务与 UI 之间唯一的通信方式
 *
 * 为什么要它：
 *   手表里数据是"异步产生"的（RTC 每秒跳、电量每 30 秒采、IMU 50Hz 出步数）。
 *   如果让 UI 主动去问驱动，就会出现「谁在什么上下文里调谁」的混乱，
 *   而且以后加 BLE 通知、AI 语音查询时，调用关系会变成一张网。
 *
 *   事件总线把关系拉直：
 *
 *       驱动/服务  ──publish──▶  [ event_bus ]  ──▶ 订阅者队列 ──▶ UI / 日志 / 未来的 BLE
 *
 *   UI 不认识 RTC，RTC 也不认识 UI。
 *
 * 线程模型（重要）：
 *   - publish() 是**非阻塞**的：往订阅者队列里投递，队列满就**丢弃最新事件**并计数。
 *     丢事件不会卡住发布者，符合"手表 UI 卡一下也比死锁好"的取舍。
 *   - 订阅者自己在任务里调用 subscribe() 返回的队列句柄 receive。
 *   - 队列长度由订阅者自己决定，默认给 8。
 *
 * 生命周期：
 *   订阅后**不要删除队列**，直到 unsubscribe()。目前不做动态回收（订阅者都是长期存在的）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、事件类型
 *
 * 约定：一个事件类型 = 一个发布者 + 一个数据结构。
 *       订阅者用位掩码（1ULL << type）一次订阅多个，最多 32 种。
 * ================================================================== */
typedef enum {
    EVT_NONE = 0,

    EVT_TIME_TICK,        ///< 每秒一次，payload: evt_time_t
    EVT_BATTERY,          ///< 电量变化（周期采样或变化超阈值），payload: evt_battery_t
    EVT_STEPS,            ///< 步数变化，payload: evt_steps_t（P1 未接入 IMU）
    EVT_SETTINGS_CHANGED, ///< 设置项被改动，payload: evt_settings_t
    EVT_SCREEN,           ///< 息屏/亮屏，payload: evt_screen_t
    EVT_COUNT             ///< 哨兵，必须放最后
} event_type_t;

/** @brief 事件类型位掩码 */
#define EVT_MASK(type)   (1UL << (type))
#define EVT_MASK_ALL     0xFFFFFFFFUL

/* ==================================================================
 * 二、各事件的数据结构（全部是普通的 POD，可安全跨任务拷贝）
 * ================================================================== */

/**
 * @brief 时间事件 —— 由 time_svc 每秒发布
 */
typedef struct {
    int  year;        ///< 完整年份，如 2026
    int  month;       ///< 1-12
    int  day;         ///< 1-31
    int  hour;        ///< 0-23
    int  minute;      ///< 0-59
    int  second;      ///< 0-59
    int  weekday;     ///< 0=周日 … 6=周六
    bool valid;       ///< false = RTC 未校时（掉电丢失），UI 应显示 "--:--"
} evt_time_t;

/**
 * @brief 电量事件 —— 由 battery_svc 发布
 */
typedef struct {
    int  millivolt;   ///< 电池电压，单位 mV
    int  percent;     ///< 0-100；-1 表示无法估算（没接电池）
    bool charging;    ///< 是否在充电（USB 插入 + 电压上升推断）
    bool present;     ///< 是否检测到电池
} evt_battery_t;

/**
 * @brief 步数事件 —— 由 imu_drv 的计步算法发布
 */
typedef struct {
    int steps;        ///< 今日累计步数
    int kcal;         ///< 估算消耗（千卡）
    int meters;       ///< 估算距离（米）
} evt_steps_t;

/**
 * @brief 设置变化事件
 */
typedef struct {
    uint32_t changed_mask;  ///< 哪些设置项变了（见 settings.h 的 SETTINGS_BIT_*）
} evt_settings_t;

/**
 * @brief 屏幕状态事件
 */
typedef struct {
    bool on;          ///< true=亮屏 false=息屏
} evt_screen_t;

/** @brief 事件信封：类型 + 数据 */
typedef struct {
    event_type_t type;
    union {
        evt_time_t       time;
        evt_battery_t    battery;
        evt_steps_t      steps;
        evt_settings_t   settings;
        evt_screen_t     screen;
    } data;
} event_t;

/* ==================================================================
 * 三、接口
 * ================================================================== */

/** @brief 事件总线句柄（订阅者队列） */
typedef QueueHandle_t event_sub_t;

/**
 * @brief 初始化事件总线（在 app_main 里最先调用）
 *
 * @param queue_depth  每个订阅者队列的默认深度（0 = 用默认值 8）
 */
esp_err_t event_bus_init(uint32_t queue_depth);

/**
 * @brief 订阅一组事件
 *
 * @param mask        订阅哪些事件，用 EVT_MASK(EVT_XXX) 按位或
 * @param queue_depth 该订阅者队列深度，0 = 默认值
 * @return 订阅句柄（FreeRTOS 队列），失败返回 NULL
 *
 * @code
 *   event_sub_t sub = event_bus_subscribe(EVT_MASK(EVT_TIME_TICK));
 *   event_t evt;
 *   while (xQueueReceive(sub, &evt, portMAX_DELAY) == pdTRUE) { ... }
 * @endcode
 */
event_sub_t event_bus_subscribe(uint32_t mask, uint32_t queue_depth);

/**
 * @brief 取消订阅（会删除队列；调用后不要再使用该句柄）
 */
void event_bus_unsubscribe(event_sub_t sub);

/**
 * @brief 发布事件（非阻塞，可从任意任务/中断无关上下文调用）
 *
 * @note 队列满时丢弃该事件并累加丢弃计数（可用 event_bus_dropped() 查）。
 */
void event_bus_publish(const event_t *evt);

/**
 * @brief 便捷封装：发一个时间事件
 */
void event_bus_publish_time(const evt_time_t *t);

/**
 * @brief 便捷封装：发一个电量事件
 */
void event_bus_publish_battery(const evt_battery_t *b);

/**
 * @brief 累计被丢弃的事件数（诊断用；正常应为 0）
 */
uint32_t event_bus_dropped(void);

#ifdef __cplusplus
}
#endif
