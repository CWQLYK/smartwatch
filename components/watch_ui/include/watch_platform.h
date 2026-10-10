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

    /*
     * ★ v0.5.0：用"已保存的密码"连接失败时，服务会**自动清掉保存的凭据**
     *   （用户要求：避免路由器改了密码之后，本地存着个永远连不上的旧密码、
     *    而界面上又只能"重新扫描"，等于卡死）。
     *
     *   这个标志让界面能把原因说出来，而不是只显示"连接失败（密码错或信号太差）"。
     *   下一次成功发起连接时会被清掉。
     */
    bool           creds_cleared;
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

    /**
     * @brief 取"已保存的第 idx 个网络"（v0.7.0，多槽位）
     *
     * @param idx   0 起；越界返回非 0
     * @param ssid  输出 SSID
     * @param max   缓冲长度
     * @return 0 = 这一槽有内容；非 0 = 没有
     *
     * @note 为什么不直接改 `get_saved_ssid` 的签名：那个取的是
     *       "上次成功那一槽"，界面上多处靠它判断"点这个网络要不要问密码"，
     *       改签名会波及所有调用点。多一个按索引取的口子更安全。
     *       **任一个没接（模拟器就是）时界面必须能正常工作** —— 见各页的判空。
     */
    int  (*get_saved_ssid_at)(int idx, char *ssid, int max);

    /** @brief 已保存的网络数量（v0.7.0，界面显示"已记住 N 个网络"）*/
    int  (*saved_count)(void);
    int  (*sync_time)(void);
} w_wifi_hooks_t;

/**
 * @brief 注册 WiFi 钩子（app 层调；模拟器不调）
 */
void w_platform_set_wifi_hooks(const w_wifi_hooks_t *hooks);

/** @brief 取 WiFi 钩子；没注册返回 NULL（界面必须判空） */
const w_wifi_hooks_t *w_platform_wifi(void);

/* ==================== 天气（v0.7.0，对应 components/weather）====================
 *
 * ## 为什么字段名/结构要和 weather_service.h 对齐
 *
 * 界面层**不能** include `weather_service.h`（那个会带进 esp_http_client 等
 * ESP-IDF 头，PC 模拟器就编不过了）。所以这里再声明一份等价结构，
 * 由 app.c 在装配时逐字段搬运（和 wifi 那套"字段对齐"的约定完全一样）。
 *
 * ## 为什么状态里要有 fetching / failed
 *
 * 依据 `wifi_status_t` 的既有教训（见上面 syncing/sync_failed 的注释）：
 * **用户点了按钮之后界面必须有反馈**。"点了没反应"是这个项目反复踩的坑。
 */

/** @brief 预报天数（要和 weather_service.h 的 WEATHER_DAYS 一致）*/
#define W_WEATHER_DAYS   7

/** @brief 一天的预报（字段对齐 weather_day_t）*/
typedef struct {
    char   date[6];     /**< "MM/DD" */
    int8_t wmo;         /**< WMO 天气代码 */
    int8_t tmax;        /**< 最高温 ℃ */
    int8_t tmin;        /**< 最低温 ℃ */
} w_weather_day_t;

/** @brief 天气状态快照（字段对齐 weather_status_t）*/
typedef struct {
    bool            valid;          /**< 有没有可用的缓存 */
    uint8_t         count;          /**< 有效天数 1..7 */
    w_weather_day_t days[W_WEATHER_DAYS];
    uint32_t        fetched_epoch;  /**< 上次抓取时间（0 = 从未）*/
    bool            fetching;       /**< 正在抓取 → 界面转圈 */
    bool            failed;         /**< 上次失败，把原因说出来 */
} w_weather_status_t;

/** @brief 天气钩子表（字段对齐 weather_service 的对外接口）*/
typedef struct {
    /** @brief 触发一次抓取（非阻塞），0 = 已受理 */
    int  (*refresh)(void);
    /** @brief 取状态快照（服务内加锁，任何任务都能调） */
    void (*get_status)(w_weather_status_t *out);
    /** @brief WMO 代码 → 图标类别（0 晴 1 多云 2 阴 3 雾 4 雨 5 雪 6 雷） */
    int  (*wmo_class)(int wmo);
} w_weather_hooks_t;

/** @brief 注册天气钩子（app 层调；模拟器不调） */
void w_platform_set_weather_hooks(const w_weather_hooks_t *hooks);

/** @brief 取天气钩子；没注册返回 NULL（界面必须判空 → 显示"不支持"） */
const w_weather_hooks_t *w_platform_weather(void);

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

/* ==================== 蜂鸣器（v0.5.0 新增） ====================
 *
 * 为什么需要这组钩子：开发人员选项里的「蜂鸣器测试」要"按住响、松开停"，
 * 秒表结束/倒计时到点/闹钟到点也要响。而界面层不许 include ESP-IDF
 * 头文件（PC 模拟器编不过），所以只能走钩子。
 *
 * 模拟器上不注入 → 所有封装函数变成空操作，界面照跑不误。
 * ================================================================== */

/** @brief 蜂鸣器钩子表 */
typedef struct {
    /**
     * @brief 起一个持续音（**非阻塞**）
     * @param freq_hz 频率，板上蜂鸣器谐振点约 2700Hz
     * @return 0 成功
     */
    int  (*tone)(uint32_t freq_hz);

    /** @brief 停音。返回 0 成功 */
    int  (*stop)(void);
} w_buzzer_hooks_t;

void w_platform_set_buzzer_hooks(const w_buzzer_hooks_t *hooks);
const w_buzzer_hooks_t *w_platform_buzzer(void);

/** @brief 便捷封装：起音（没接钩子时什么都不做） */
void w_buzzer_tone(uint32_t freq_hz);

/** @brief 便捷封装：停音（没接钩子时什么都不做） */
void w_buzzer_stop(void);

/* ==================== 时间校准 ==================== */

/**
 * @brief 手动校时钩子（把**UTC**直接写进 RTC，不做时区换算）
 *
 * @note ⚠️ 历史遗留的命名陷阱：字段叫 `set_local`，但**实际语义是"写 UTC"**。
 *       用户明确要求"时间校准页填的是 UTC，方便以后切换时区"，
 *       app 层的实现（`time_hook_set_utc`）也确实不做任何换算，
 *       页面填什么就写什么。
 *       表盘显示时才按 settings 里的时区偏移换算（见 data_hub.c 顶部约定）。
 *
 *       （这里曾经写成"传的是本地时间，app 层按 settings 换算成 UTC"，
 *         和实现正好相反 —— 与交接文档坑 6「注释和实现说法相反」同类。）
 */
typedef struct {
    /** @brief 写时间：**UTC** 的年月日时分秒。返回 0 成功 */
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
 * @note v0.7.1 起"息屏"不再只是关背光：= 关背光 + 面板 DISPOFF（停显示输出）
 *       + LVGL 刷新放慢到 100ms + 电源轮询放慢到 200ms；触摸 I2C 在息屏空闲时
 *       会被 INT 门控跳过。**CPU 与 IMU 照跑**（CPU 降频 / light sleep 属第二档，未做）。
 *       详见 `doc/智能手表-低功耗实施文档.md`。
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
 *
 * @note ★ v0.6.1：如果这次调用**把屏幕从灭叫醒**了，内部会置一个
 *       "吞掉本次触摸"的标志（见下面两个函数）。
 */
void w_screen_notify_activity(void);

/**
 * @brief 这一次触摸是不是"只是用来把屏幕叫醒"的（v0.6.1 新增）
 *
 * @note 用法（见 bsp.c 的触摸读取回调）：
 *         按下时先调 w_screen_notify_activity()，再问这个函数；
 *         返回 true 就把这一帧上报成 **RELEASED**，不要把坐标和按下状态交给 LVGL。
 *
 *   为什么需要它：`power.c` 的注释一直写着"息屏后第一下靠
 *   `lv_indev_enable(false)` 吃掉"，但**全工程没有任何地方调过 false**；
 *   而且 LVGL 8.4 是在**调用读回调之前**检查 disabled 的
 *   （`lv_indev.c:78`），事后禁用根本来不及。
 *   实测后果：息屏状态下点一下屏幕，会既亮屏、又误触发手指下面的控件
 *   （例如在功能列表上直接进子页面）。
 *
 *   状态放在平台层而不是 bsp.c，是为了让**模拟器也能写自检**（bsp 只有手表端编译）。
 */
bool w_screen_should_swallow_touch(void);

/**
 * @brief 手指抬起：解除"吞掉"状态，下一次触摸正常生效
 */
void w_touch_settled(void);

#ifdef __cplusplus
}
#endif
