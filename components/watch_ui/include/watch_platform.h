/**
 * @file watch_platform.h
 * @brief 界面层能用的"平台能力"钩子汇总
 *
 * ## 为什么要有这个文件（重要设计约束）
 *
 * `watch_ui` / `watch_ui_pages` 里**不许 include 任何 ESP-IDF 头文件**，
 * 否则 PC 模拟器就编不过（模拟器没有 ESP-IDF）。
 *
 * 但界面确实需要一些"平台能力"：调背光、读写设置、连 WiFi。
 * 所以用**函数指针钩子**注入 —— app 层负责把真实现接上，
 * 模拟器上不接（界面退化成只读/提示不可用，但不会崩）。
 *
 * ## 三组钩子，三种套路
 *
 * | 能力 | 接口形式 | 为什么这样设计 |
 * |---|---|---|
 * | 亮度 | `watch_ui_set_brightness_hooks(get, set)` | 只有两个函数，直接给 |
 * | 设置项 | `watch_ui_set_setting_hooks(get, set)` | 按**名字**寻址，界面不认识 NVS |
 * | WiFi | 本文件的 `w_hooks_t` | 函数多（8 个），打包成结构体一次给完 |
 *
 * ⚠️ 这里**故意重新声明**一份 WiFi 相关类型，而不是 include
 *    `wifi_service.h` —— 就是为了不让 ESP-IDF 头文件渗进界面层。
 *    两边定义必须保持一致（字段名字对齐，靠约定维护）。
 *    改动时**两边一起改**，否则 app 层的赋值会静默错位。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== WiFi 相关（对应 components/wifi_service 的同名类型） ==================== */

#define W_WIFI_SCAN_MAX   12
#define W_WIFI_SSID_MAX   33
#define W_WIFI_PASS_MAX   64

/** @brief WiFi 状态（数值含义必须和 wifi_service.h 的 wifi_state_t 一致） */
typedef enum {
    W_WIFI_IDLE = 0,
    W_WIFI_CONNECTING,
    W_WIFI_CONNECTED,
    W_WIFI_FAILED,
} w_wifi_state_t;

/** @brief 一个扫描到的 AP（字段必须和 wifi_service.h 的 wifi_ap_t 对齐） */
typedef struct {
    char   ssid[W_WIFI_SSID_MAX];
    int8_t rssi;
    bool   secure;
} w_wifi_ap_t;

/** @brief WiFi 状态快照（字段必须和 wifi_status_t 对齐） */
typedef struct {
    bool           available;
    w_wifi_state_t state;
    bool           auto_reconnect;
    char           ssid[W_WIFI_SSID_MAX];
    int8_t         rssi;
    char           ip[16];
    bool           time_synced;
    uint32_t       synced_epoch;
    int            synced_count;
    /*
     * ★ 对时过程状态。
     *
     * 为什么需要：用户点"重新对时"之后原来**界面上什么都不会变** ——
     * 因为 `time_synced` 还是上一次的 true，文字照样显示"已对时"，
     * 用户完全不知道按钮有没有生效（实测反馈"点了没反应"）。
     *
     * 现在显式维护：
     *   syncing      = 正在对时（点了按钮到收到响应之间）
     *   sync_failed  = 这次对时超时/失败了
     */
    bool           syncing;
    bool           sync_failed;
    /** @brief 本次对时的起始时刻（us/1000），内部用来判超时 */
    uint32_t       sync_start;
    /** @brief 本次连接发起的时刻（us/1000），内部用来判连接超时 */
    uint32_t       conn_start;
} w_wifi_status_t;

/** @brief WiFi 钩子表（和 wifi_service.h 的 wifi_hooks_t 逐字段对齐） */
typedef struct {
    int  (*scan_start)(void);
    bool (*scan_busy)(void);
    int  (*get_scan_result)(w_wifi_ap_t *out, int max);
    int  (*connect)(const char *ssid, const char *password);
    int  (*disconnect)(void);

    /**
     * @brief 断开并**清除**已保存的密码（"取消保存"）
     *
     * @note 和 disconnect 分成两个，因为它们代表**两种不同的用户意图**：
     *         disconnect = "先不连了"（密码留着，下次自动重连）
     *         forget     = "我不想让这块表再连这个网了"（密码删掉）
     *       合成一个按钮的话，用户每次"先不连了"都得重输一遍密码。
     */
    int  (*forget)(void);
    void (*get_status)(w_wifi_status_t *out);
    void (*get_saved_ssid)(char *out, int max);
    int  (*sync_time)(void);
} w_wifi_hooks_t;

/**
 * @brief 注册 WiFi 钩子（app 层调；模拟器不调）
 */
void w_platform_set_wifi_hooks(const w_wifi_hooks_t *hooks);

/** @brief 取 WiFi 钩子；没注册返回 NULL（界面必须判空） */
const w_wifi_hooks_t *w_platform_wifi(void);

/* ==================== IMU 录制相关（对应 components/imu_logger） ==================== */

/** @brief 录制状态（对齐 imu_logger.h 的 imu_logger_status_t） */
typedef struct {
    bool     mounted;
    bool     recording;
    uint32_t samples;
    uint32_t seconds;
    uint32_t bytes;
    uint32_t files;
    uint32_t max_files;
    uint32_t total_bytes;
    uint32_t used_bytes;
    char     current[32];
} w_rec_status_t;

/** @brief IMU 录制钩子表 */
typedef struct {
    bool (*available)(void);
    int  (*start)(void);
    int  (*stop)(void);
    void (*get_status)(w_rec_status_t *out);
    int  (*erase_all)(void);
} w_rec_hooks_t;

void w_platform_set_rec_hooks(const w_rec_hooks_t *hooks);
const w_rec_hooks_t *w_platform_rec(void);

/* ==================== 时间校准 ==================== */

/**
 * @brief 手动校时钩子（把**本地时间**写进 RTC）
 *
 * @note 传的是**本地时间**，app 层负责按 settings 里的时区换算成 UTC 再写 RTC。
 *       界面不做时区换算 —— 时区是配置，界面不该关心。
 */
typedef struct {
    /** @brief 写时间：本地年月日时分秒。返回 0 成功 */
    int  (*set_local)(int year, int month, int day,
                      int hour, int minute, int second);
    /** @brief 读当前时区（15 分钟单位），用于界面显示提示 */
    int  (*get_tz_quarters)(void);
} w_time_hooks_t;

void w_platform_set_time_hooks(const w_time_hooks_t *hooks);
const w_time_hooks_t *w_platform_time(void);

/* ==================== 息屏 / 亮屏控制 ==================== */

/**
 * @brief 屏幕开关钩子
 *
 * @note "息屏"目前只关背光，CPU 和 IMU 照跑（用户要求先不做低功耗）。
 *       界面层需要能**临时禁止息屏**（录制 IMU 时）和**主动亮屏**。
 */
typedef struct {
    /** @brief 主动点亮屏幕 */
    void (*wake)(void);
    /** @brief 屏幕当前是否亮着 */
    bool (*is_on)(void);
    /**
     * @brief 禁用/恢复自动息屏
     * @param hold true = 暂时不要息屏（引用计数）；false = 解除
     *
     * @note 用引用计数而不是布尔，是因为可能有多个持有者
     *       （录制 IMU、正在输密码…）。谁先要谁先加，各自解各自的。
     */
    void (*hold_awake)(bool hold);

    /**
     * @brief 上报"用户正在操作"
     *
     * @note ★ 由**触摸读取路径**（bsp 的 indev read_cb）在每次检测到按下时调用。
     *
     *   为什么必须走这里、而不是让电源模块自己轮询触摸中断脚：
     *   轮询（50ms 一次）会漏掉快速点击，导致"用户明明在连续操作，
     *   计时器却以为没人动 → 中途息屏"。实测用户就遇到了这个问题。
     *   从真正的触摸读取点上报，一次都不会漏。
     */
    void (*notify_activity)(void);
} w_screen_hooks_t;

void w_platform_set_screen_hooks(const w_screen_hooks_t *hooks);
const w_screen_hooks_t *w_platform_screen(void);

/* ==================== 便捷封装（内部已判空） ==================== */

/** @brief 屏幕是否亮着（没接钩子时返回 true，免得界面以为一直黑屏） */
bool w_screen_is_on(void);

/** @brief 主动亮屏 */
void w_screen_wake(void);

/** @brief 加/解"别息屏"的引用计数 */
void w_screen_hold_awake(bool hold);

/**
 * @brief 上报"用户正在操作"（由触摸读取路径调用）
 *
 * @note 每次检测到触摸按下都要调一次。屏幕灭着时它会**只亮屏**；
 *       亮着时它刷新息屏计时。实现见 power.c 的 hook_activity()。
 */
void w_screen_notify_activity(void);

#ifdef __cplusplus
}
#endif
