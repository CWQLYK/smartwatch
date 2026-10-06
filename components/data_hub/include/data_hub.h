/**
 * @file data_hub.h
 * @brief 数据中枢 —— 全网唯一的"设备当前状态"快照
 *
 * 为什么不让 UI 直接去问驱动：
 *   1. **线程上下文不对**：UI 跑在 LVGL 任务里，驱动读取要么慢（ADC 采样 10ms），
 *      要么可能失败（I2C 抖动）。让 UI 去阻塞读，界面就卡。
 *   2. **一致性**：表盘要同时显示时间/电量/步数，分开读会读到"不同时刻"的数据。
 *   3. **以后要接 BLE 上报（P2）和 AI 语音查询（P3）**：
 *      它们需要的也是"当前状态"，而不是自己去调 rtc_get_time()。
 *
 * 所以：
 *
 *      rtc / battery / (imu)          ← 驱动层
 *              │  周期采样
 *              ▼
 *        [ data_hub ]  ← 带锁的快照 + 变化通知
 *              │
 *              ├──▶ 事件总线（EVT_TIME_TICK / EVT_BATTERY）→ 给非 UI 的订阅者
 *              └──▶ 变更回调（data_hub_on_change）        → 给 UI 用
 *
 *   界面只做两件事：`data_hub_snapshot()` 拿数据、注册一个回调等通知。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、状态快照
 * ================================================================== */

/** @brief 时间部分 */
typedef struct {
    int  year;      ///< 完整年份
    int  month;     ///< 1-12
    int  day;       ///< 1-31
    int  hour;      ///< 0-23
    int  minute;    ///< 0-59
    int  second;    ///< 0-59
    int  weekday;   ///< 1=周一 … 7=周日
    bool valid;     ///< false = 未校时（RTC 掉过电），UI 应显示 "--:--"
} dev_time_t;

/** @brief 电量部分 */
typedef struct {
    int  millivolt;   ///< 电池电压 mV；0 = 未知
    int  percent;     ///< 0-100；-1 = 未知/没接电池
    bool present;     ///< 是否检测到电池
    bool charging;    ///< 是否在充电
} dev_battery_t;

/** @brief 运动部分（P1 未接入 IMU，字段先占位，接口以后不改） */
typedef struct {
    int  steps;        ///< 今日步数
    int  kcal;         ///< 估算消耗
    int  meters;       ///< 估算距离
    bool available;    ///< false = IMU 还没接/还没实现，UI 显示 "--"
} dev_steps_t;

/** @brief 设备状态总快照 */
typedef struct {
    dev_time_t    time;
    dev_battery_t battery;
    dev_steps_t   steps;
} device_state_t;

/** @brief 快照里哪一块变了（回调的 changed 参数） */
#define DATA_CHANGED_TIME      0x01u
#define DATA_CHANGED_BATTERY   0x02u
#define DATA_CHANGED_STEPS     0x04u

/**
 * @brief 状态变更回调
 * @param state   最新快照（只读，回调返回后失效，需要留存请自己拷贝）
 * @param changed DATA_CHANGED_xxx 的按位或
 *
 * @warning **在 data_hub 自己的任务上下文里被调用**。
 *          UI 回调里必须自己加锁（用 watch_ui 的 watch_ui_refresh_subscribe()
 *          包装一层，它会自动加锁）。
 */
typedef void (*data_hub_cb_t)(const device_state_t *state, uint32_t changed);

/* ==================================================================
 * 二、接口
 * ================================================================== */

/**
 * @brief 初始化数据中枢并启动采样任务
 *
 * 采样策略：
 *   - RTC：**对齐到整秒**读一次（不是每 1000ms 硬延，避免累积漂移）
 *   - 电池：每 30 秒一次；电压变化超过 1% 才通知（省得 UI 一直重画）
 *
 * @note 依赖 rtc_init() 和 battery_init() 已经调过；本函数内部会再 ensure 一次。
 */
esp_err_t data_hub_init(void);

/**
 * @brief 拷贝一份当前快照（线程安全，可从任意任务调用）
 */
void data_hub_snapshot(device_state_t *out);

/**
 * @brief 便捷读：取当前时间（UI 最常用）
 */
void data_hub_get_time(dev_time_t *out);

/**
 * @brief 便捷读：取当前电量
 */
void data_hub_get_battery(dev_battery_t *out);

/**
 * @brief 注册状态变更回调
 *
 * @param cb 回调（NULL 表示注销该槽位）
 * @return 槽位编号（>=0），失败返回 -1
 *
 * @note 最多 4 个回调槽位。回调在 data_hub 任务里执行，**要快**。
 */
int data_hub_on_change(data_hub_cb_t cb);

/**
 * @brief 注销回调
 */
void data_hub_off_change(int slot);

/**
 * @brief 设置 RTC 时间（同时刷新快照并立刻通知 UI）
 *
 * @param t 时间；内部会转成 UTC 后写入 RTC 芯片
 *
 * @note 这是"校时"的唯一入口：串口命令、以后的 BLE 同步(CTS)、
 *       SNTP 都走这里，避免各处各写一遍。
 */
esp_err_t data_hub_set_time(const dev_time_t *t);

/**
 * @brief 用 Unix 时间戳校时（BLE / SNTP 用）
 */
esp_err_t data_hub_set_epoch(uint32_t epoch_utc);

/**
 * @brief 更新运动数据（由 motion 服务调用）
 *
 * @param steps   今日步数
 * @param kcal    估算消耗（千卡）
 * @param meters  估算距离（米）
 *
 * @note 这是运动数据**唯一的写入点**。界面只读 data_hub、不直接问 motion，
 *       这样以后换 IMU、换计步算法，界面一行都不用改。
 *       传 steps < 0 表示"没有运动数据"（界面显示 "--"）。
 */
void data_hub_set_steps(int steps, int kcal, int meters);

/**
 * @brief 取最后一次已知的 Unix 时间戳（UTC）
 */
uint32_t data_hub_get_epoch(void);

/**
 * @brief 强制立刻重新采样一次（调试用）
 */
void data_hub_refresh_now(void);

/**
 * @brief 把快照打印到日志（排查问题用）
 */
void data_hub_dump(void);

#ifdef __cplusplus
}
#endif
