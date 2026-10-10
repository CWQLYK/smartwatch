/**
 * @file app.c
 * @brief 应用装配层实现
 *
 * 这个文件**刻意写得很薄**：只有"接线"和"翻译"，没有业务逻辑、没有界面代码。
 * 判断标准：如果这里出现了 lv_xxx()（除了拿锁）或者寄存器操作，
 * 就说明活儿放错层了。
 *
 * 本文件干的三件事：
 *   1. **翻译**：把 data_hub 的 device_state_t 转成界面要的 watch_state_t
 *   2. **注入**：把数据源和亮度钩子交给 watch_ui
 *   3. **排序**：按依赖顺序初始化各模块，失败就降级（而不是整机起不来）
 */
#include "app.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp.h"
#include "board.h"
#include "event_bus.h"
#include "settings.h"
#include "data_hub.h"
#include "ui_port.h"
#include "watch_style.h"
#include "watch_pages.h"
#include "watch_overlay.h"
#include "motion.h"
#include "imu_logger.h"
#include "pwr_log.h"          /* ★ v0.7.1 复位黑盒 pwr_log_note_boot() */
#include "buzzer.h"
#include "power.h"
#include "wifi_service.h"
#include "weather_service.h"   /* ★ v0.7.0 天气（只在这个装配层 include） */
#include "app_console.h"

static const char *TAG = "APP";


/*
 * 固件版本号。
 * ⚠️ 改这里的时候，记得同步三处，否则日志/设置页/文档会对不上：
 *   1. components/watch_ui_pages/pages/watch_page_settings.c 的 WATCH_FIRMWARE_VERSION
 *   2. doc/version/vX.Y.Z.md（每个版本一个文档）
 *   3. doc/version/README.md 的版本列表
 */
#define APP_VERSION_STR  "0.7.2"

const char *app_version(void)
{
    return APP_VERSION_STR;
}

/* ==================================================================
 * 一、数据源适配器：device_state_t  →  watch_state_t
 *
 * 为什么不直接让界面用 device_state_t：
 *   那样 watch_ui 就得 include data_hub.h，而 data_hub 依赖 ESP-IDF，
 *   PC 模拟器就编不过了。隔一层结构体，界面才能"通用"。
 * ================================================================== */

static void hub_to_watch(const device_state_t *in, watch_state_t *out)
{
    memset(out, 0, sizeof(*out));

    out->time.year    = in->time.year;
    out->time.month   = in->time.month;
    out->time.day     = in->time.day;
    out->time.hour    = in->time.hour;
    out->time.minute  = in->time.minute;
    out->time.second  = in->time.second;
    out->time.weekday = in->time.weekday;
    out->time.valid   = in->time.valid;

    out->battery.millivolt = in->battery.millivolt;
    out->battery.percent   = in->battery.percent;
    out->battery.charging  = in->battery.charging;
    out->battery.present   = in->battery.present;

    out->steps.steps     = in->steps.steps;
    out->steps.kcal      = in->steps.kcal;
    out->steps.meters    = in->steps.meters;
    out->steps.available = in->steps.available;
}

/* data_hub 的变更位 → 界面的变更位（目前一一对应，留这层是为了以后能解耦） */
static uint32_t hub_changed_to_watch(uint32_t hub_changed)
{
    uint32_t out = 0;
    if (hub_changed & DATA_CHANGED_TIME)    out |= WATCH_CHANGED_TIME;
    if (hub_changed & DATA_CHANGED_BATTERY) out |= WATCH_CHANGED_BATTERY;
    if (hub_changed & DATA_CHANGED_STEPS)   out |= WATCH_CHANGED_STEPS;
    return out;
}

/* ---- provider 的三个函数 ---- */
static void provider_get_state(watch_state_t *out)
{
    device_state_t st;
    data_hub_snapshot(&st);
    hub_to_watch(&st, out);
}

static void on_hub_changed(const device_state_t *state, uint32_t hub_changed);

/* 静态槽位：整个固件只有一个界面订阅者，不需要动态分配 */
static watch_state_cb_t s_ui_cb = NULL;

static int provider_subscribe(watch_state_cb_t cb)
{
    /* data_hub 的回调签名和界面不同，用静态槽位把它们绑起来 */
    s_ui_cb = cb;
    return data_hub_on_change(on_hub_changed);
}

static void provider_unsubscribe(int handle)
{
    s_ui_cb = NULL;
    data_hub_off_change(handle);
}

static void on_hub_changed(const device_state_t *state, uint32_t hub_changed)
{
    if (!s_ui_cb) {
        return;
    }
    watch_state_t ws;
    hub_to_watch(state, &ws);
    s_ui_cb(&ws, hub_changed_to_watch(hub_changed));
}

static const watch_data_provider_t s_provider = {
    .get_state   = provider_get_state,
    .subscribe   = provider_subscribe,
    .unsubscribe = provider_unsubscribe,
};

/* ==================================================================
 * 二、平台钩子：界面 ↔ 服务
 *
 * 界面层（watch_ui / watch_ui_pages）不许 include ESP-IDF 头文件，
 * 所以亮度、设置项都通过函数钩子注入。
 * 这里是**唯一**把两边接起来的地方 —— 想知道"界面怎么拿到数据/配置"，
 * 看这一段就够了。
 * ================================================================== */

/* ---------- 亮度 ---------- */
static int brightness_get(void)
{
    return (int)settings_get_or_default(SETTINGS_BRIGHTNESS);
}

static int brightness_set(int percent)
{
    /* 下限 5%：完全关掉的话用户会以为死机了，息屏另走功能 13 */
    if (percent < 5)   percent = 5;
    if (percent > 100) percent = 100;

    settings_set(SETTINGS_BRIGHTNESS, percent);

    esp_err_t err = bsp_backlight_set(percent);
    return (int)err;
}

/* ---------- 设置项（界面层按名字寻址，这里映射到真正的 NVS 项） ----------
 *
 * ★ v0.5.0：
 *   - 删除：`wrist_wake` / `height` / `weight`（抬腕亮屏取消、步长改固定值）
 *   - 新增：闹钟与倒计时的 5 项（时钟服务用，见 watch_pages.h 的 W_SETTING_*）
 * ★ v0.6.0：
 *   - 新增：`dino_best`（小恐龙最高分，游戏宿主读写）
 */
static int setting_get(const char *key, int def)
{
    if (!key) {
        return def;
    }
    if (strcmp(key, W_SETTING_AUTO_SCREEN) == 0) {
        return (int)settings_get_or_default(SETTINGS_SCREEN_TIMEOUT);
    }
    if (strcmp(key, W_SETTING_STEP_GOAL) == 0) {
        return (int)settings_get_or_default(SETTINGS_STEP_GOAL);
    }
    /* ---- v0.5.0 时钟服务 ---- */
    if (strcmp(key, W_SETTING_ALARM_HOUR) == 0) {
        return (int)settings_get_or_default(SETTINGS_ALARM_HOUR);
    }
    if (strcmp(key, W_SETTING_ALARM_MIN) == 0) {
        return (int)settings_get_or_default(SETTINGS_ALARM_MIN);
    }
    if (strcmp(key, W_SETTING_ALARM_EN) == 0) {
        return (int)settings_get_or_default(SETTINGS_ALARM_EN);
    }
    if (strcmp(key, W_SETTING_CD_PRESET) == 0) {
        return (int)settings_get_or_default(SETTINGS_CD_PRESET_SEC);
    }
    if (strcmp(key, W_SETTING_CD_RING) == 0) {
        return (int)settings_get_or_default(SETTINGS_CD_RING_SEC);
    }
    /* ---- v0.6.0 游戏存档 ---- */
    if (strcmp(key, W_SETTING_DINO_BEST) == 0) {
        return (int)settings_get_or_default(SETTINGS_DINO_BEST);
    }
    /* ---- v0.7.0 WiFi 开机自动连接 ---- */
    if (strcmp(key, W_SETTING_WIFI_AUTO) == 0) {
        return (int)settings_get_or_default(SETTINGS_WIFI_AUTO);
    }
    /* ---- v0.7.0 时区（日历页要把 UTC 换算成本地日期）---- */
    if (strcmp(key, W_SETTING_TZ) == 0) {
        return (int)settings_get_or_default(SETTINGS_TIMEZONE);
    }
    /* ---- v0.7.0 2048 存档 ---- */
    if (strcmp(key, W_SETTING_2048_BEST) == 0) {
        return (int)settings_get_or_default(SETTINGS_2048_BEST);
    }
    /* ---- v0.7.0 天气经纬度 ---- */
    if (strcmp(key, W_SETTING_WX_LAT) == 0) {
        return (int)settings_get_or_default(SETTINGS_WX_LAT);
    }
    if (strcmp(key, W_SETTING_WX_LON) == 0) {
        return (int)settings_get_or_default(SETTINGS_WX_LON);
    }
    ESP_LOGW(TAG, "未知设置项键名 '%s'，返回默认值 %d", key, def);
    return def;
}

static int setting_set(const char *key, int value)
{
    if (!key) {
        return 0;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;

    if (strcmp(key, W_SETTING_AUTO_SCREEN) == 0) {
        err = settings_set(SETTINGS_SCREEN_TIMEOUT, value);
    } else if (strcmp(key, W_SETTING_STEP_GOAL) == 0) {
        err = settings_set(SETTINGS_STEP_GOAL, value);
    }
    /* ---- v0.5.0 时钟服务 ---- */
    else if (strcmp(key, W_SETTING_ALARM_HOUR) == 0) {
        err = settings_set(SETTINGS_ALARM_HOUR, value);
    } else if (strcmp(key, W_SETTING_ALARM_MIN) == 0) {
        err = settings_set(SETTINGS_ALARM_MIN, value);
    } else if (strcmp(key, W_SETTING_ALARM_EN) == 0) {
        err = settings_set(SETTINGS_ALARM_EN, value);
    } else if (strcmp(key, W_SETTING_CD_PRESET) == 0) {
        err = settings_set(SETTINGS_CD_PRESET_SEC, value);
    } else if (strcmp(key, W_SETTING_CD_RING) == 0) {
        err = settings_set(SETTINGS_CD_RING_SEC, value);
    }
    /* ---- v0.6.0 游戏存档 ---- */
    else if (strcmp(key, W_SETTING_DINO_BEST) == 0) {
        err = settings_set(SETTINGS_DINO_BEST, value);
    }
    /* ---- v0.7.0 WiFi 开机自动连接 ---- */
    else if (strcmp(key, W_SETTING_WIFI_AUTO) == 0) {
        err = settings_set(SETTINGS_WIFI_AUTO, value);
    }
    /* ---- v0.7.0 2048 存档 ---- */
    else if (strcmp(key, W_SETTING_2048_BEST) == 0) {
        err = settings_set(SETTINGS_2048_BEST, value);
    }
    /* ---- v0.7.0 天气经纬度 ---- */
    else if (strcmp(key, W_SETTING_WX_LAT) == 0) {
        err = settings_set(SETTINGS_WX_LAT, value);
    }
    else if (strcmp(key, W_SETTING_WX_LON) == 0) {
        err = settings_set(SETTINGS_WX_LON, value);
    } else {
        ESP_LOGW(TAG, "未知设置项键名 '%s'", key);
        return -1;
    }

    /*
     * 注：v0.5.0 之前这里还有一段"身高体重变了要通知计步算法"的逻辑。
     *     现在步长是固定值、体重设置项已删除，不再需要。
     */
    return (int)err;
}

/* ==================================================================
 * 三、事件总线订阅：把"设置变了"转成"界面刷新"
 *
 * 这是 event_bus 的第一个真实消费者 —— 在此之前它只是搭好了没人用。
 * 数据变化走的是 data_hub 的变更回调（链路更短），
 * 而设置变化用事件总线更合适：将来 BLE 改设置、串口改设置都自动生效。
 * ================================================================== */
static void settings_event_task(void *arg)
{
    (void)arg;

    /*
     * 队列深度 4：设置变更本来就是低频事件（用户手点一下才有一两条），
     * 4 条足够吸收突发，再多是浪费内存。
     */
    event_sub_t sub = event_bus_subscribe(EVT_MASK(EVT_SETTINGS_CHANGED), 4);
    if (!sub) {
        ESP_LOGW(TAG, "订阅设置变更事件失败，界面不会跟随外部改动");
        vTaskDelete(NULL);
        return;
    }

    event_t e;
    for (;;) {
        if (xQueueReceive(sub, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /*
         * 刷界面要在 LVGL 锁内，而这里是独立任务，
         * 所以自己加锁（和 data_hub 变更回调同样的规矩）。
         */
        if (ui_port_lock(portMAX_DELAY)) {
            watch_ui_refresh();
            ui_port_unlock();
        }
    }
}

/* ==================================================================
 * 五、界面 <-> 服务的钩子适配（v0.4.0 新增）
 *
 * 界面层不许 include ESP-IDF 头文件，所以"录 IMU""手动校时"这些能力
 * 都要在这里翻译成界面认识的钩子。这个文件就是界面和硬件之间的
 * **全部粘合代码** —— 想知道"界面点了按钮之后到底调了什么"，看这段。
 *
 * 注意类型：wifi_service.h 已经把 wifi_hooks_t 定义成
 * watch_platform.h 里 w_wifi_hooks_t 的**别名**，所以不需要转换，
 * 直接传指针即可（同一份定义，不可能错位）。
 * ================================================================== */

/* WiFi 钩子是否就绪。协议栈**不在开机时启动**：它吃 50KB+ RAM，
 * 而绝大多数场景根本不会点 WiFi 页。等真要用再起。 */
static bool s_wifi_hooks_ready = false;

/* 系统信息（开发人员页显示）。只有 app 层拿得到 esp_* 数据。 */
static char s_info_heap[24]   = "--";
static char s_info_psram[24]  = "--";
static char s_info_uptime[24] = "--";

/* ---------- 天气钩子（★ v0.7.0）----------
 *
 * 界面层不能 include weather_service.h（会带进 esp_http_client 等 IDF 头），
 * 所以这里把服务的接口**逐字段搬**成 w_weather_hooks_t。
 * 和 wifi 那套"字段对齐"的约定完全一样。
 *
 * ⚠️ get_status 这里必须**做结构体转换**（weather_status_t → w_weather_status_t），
 *    不能直接把指针转过去 —— 两个结构体的字段顺序/类型虽然现在一致，
 *    但它们的对齐（padding）不保证相同，强转是未定义行为。
 */
static void weather_hook_get_status(w_weather_status_t *out)
{
    if (!out) {
        return;
    }
    weather_status_t src;
    weather_service_get_status(&src);

    memset(out, 0, sizeof(*out));
    out->valid         = src.valid;
    out->count         = src.count;
    out->fetched_epoch = src.fetched_epoch;
    out->fetching      = src.fetching;
    out->failed        = src.failed;
    for (int i = 0; i < W_WEATHER_DAYS && i < WEATHER_DAYS; i++) {
        out->days[i].date[0] = src.days[i].date[0];
        out->days[i].date[1] = src.days[i].date[1];
        out->days[i].date[2] = src.days[i].date[2];
        out->days[i].date[3] = src.days[i].date[3];
        out->days[i].date[4] = src.days[i].date[4];
        out->days[i].date[5] = '\0';
        out->days[i].wmo     = src.days[i].wmo;
        out->days[i].tmax    = src.days[i].tmax;
        out->days[i].tmin    = src.days[i].tmin;
    }
}

static int weather_hook_refresh(void)
{
    return weather_service_refresh();
}

static int weather_hook_wmo_class(int wmo)
{
    /* 声明在 weather_service.c 里；它不需要任何 IDF 类型，纯查表 */
    extern int weather_wmo_class(int wmo);
    return weather_wmo_class(wmo);
}

static const w_weather_hooks_t s_weather_hooks = {
    .refresh    = weather_hook_refresh,
    .get_status = weather_hook_get_status,
    .wmo_class  = weather_hook_wmo_class,
};

/* ---------- IMU 录制钩子 ---------- */
static bool rec_hook_available(void)
{
    return imu_logger_available();
}

static int rec_hook_start(void)
{
    return (int)imu_logger_start();
}

static int rec_hook_stop(void)
{
    return (int)imu_logger_stop();
}

static void rec_hook_status(w_rec_status_t *out)
{
    if (out) {
        imu_logger_get_status((imu_logger_status_t *)out);
    }
}

static int rec_hook_erase(void)
{
    return (int)imu_logger_erase_all();
}

static const w_rec_hooks_t s_rec_hooks = {
    .available  = rec_hook_available,
    .start      = rec_hook_start,
    .stop       = rec_hook_stop,
    .get_status = rec_hook_status,
    .erase_all  = rec_hook_erase,
};

/* ---------- 手动校时钩子（页面填的是 **UTC**）----------
 *
 * 用户明确要求"时间校准要存 UTC，方便后面切换时区"，
 * 所以这里**不做任何时区换算**：页面填什么就写什么。
 * 表盘显示时才按 settings 里的时区偏移换算。
 */
static int time_hook_set_utc(int year, int month, int day,
                             int hour, int minute, int second)
{
    dev_time_t t = {
        .year    = year,
        .month   = month,
        .day     = day,
        .hour    = hour,
        .minute  = minute,
        .second  = second,
        /* 星期由 RTC 驱动按日期自己算，这里给占位值 */
        .weekday = 1,
        .valid   = true,
    };

    esp_err_t err = data_hub_set_time(&t);
    if (err == ESP_OK) {
        settings_set(SETTINGS_TIME_VALID, 1);
        ESP_LOGI(TAG, "手动校时（UTC）：%04d-%02d-%02d %02d:%02d:%02d",
                 year, month, day, hour, minute, second);
    } else {
        ESP_LOGE(TAG, "手动校时失败: %s", esp_err_to_name(err));
    }
    return (int)err;
}

static int time_hook_get_tz(void)
{
    return (int)settings_get_or_default(SETTINGS_TIMEZONE);
}

static const w_time_hooks_t s_time_hooks = {
    .set_local       = time_hook_set_utc,
    .get_tz_quarters = time_hook_get_tz,
};

/* ---------- 蜂鸣器钩子（v0.5.0）----------
 *
 * 界面层要做"按住响、松开停"，还要给倒计时/闹钟报时，但不能 include
 * ESP-IDF，所以走这里注入。
 *
 * ⚠️ 踩过的坑：`buzzer_init()` 在 v0.5.0 之前**全工程没有任何调用方** ——
 *    驱动写得再完整，没初始化就是按了不响。
 */
static int buzzer_hook_tone(uint32_t freq_hz)
{
    return (int)buzzer_tone(freq_hz);
}

static int buzzer_hook_stop(void)
{
    return (int)buzzer_stop();
}

static const w_buzzer_hooks_t s_buzzer_hooks = {
    .tone = buzzer_hook_tone,
    .stop = buzzer_hook_stop,
};

/* ==================================================================
 * 六、SNTP 对时成功 -> 写 RTC
 *
 * ⚠️ 这里曾经写的是"SNTP 给的就是 UTC，和 RTC 存储口径一致，直接写"，
 *    **那是错的**，直接导致北京时间慢 8 小时（用户实测报的 bug）。
 *
 *    实际约定（以 data_hub.c 顶部注释为准）：
 *      **RTC 芯片里存的是"墙上时间"**，`sample_time()` 从 RTC 直读、
 *      表盘直接显示，**不做二次时区换算**。
 *    所以 SNTP 的 UTC 必须先加时区偏移再写进去。
 *
 *    顺带一提：`SETTINGS_TIMEZONE` 之前是个**摆设** ——
 *    `time_hook_get_tz` / `get_tz_quarters` 钩子定义了却没有任何调用方。
 *    时区换算补在这里之后，这个设置项才真正生效。
 * ================================================================== */
static void on_sntp_synced(uint32_t utc_epoch)
{
    if (utc_epoch < 1700000000u) {      /* 早于 2023-11 视为无效 */
        ESP_LOGW(TAG, "SNTP 给的时间不合理（%u），忽略", (unsigned)utc_epoch);
        return;
    }

    /*
     * ★ 把 UTC 换算成**本地墙上时间**再写 RTC —— 这一步以前漏了。
     *
     * 本项目的约定（见 data_hub.c 顶部）：**RTC 芯片里存的就是墙上时间**，
     * `sample_time()` 直接从 RTC 读、表盘直接显示，**不做二次换算**。
     *
     * ⚠️ 踩过的坑：SNTP 给的是 **UTC**，原来原样写进 RTC，
     *    于是北京时间比实际**慢 8 小时**（用户实测："时间页面没有 +8 小时"）。
     *    UTC 不等于墙上时间，写 RTC 前必须加上时区偏移。
     *
     * 时区设置的单位是 15 分钟（北京 +8 区 = 32），默认值就是 32。
     * 加了这个换算之后，`SETTINGS_TIMEZONE` 才真正被用上
     * （之前全工程没有任何地方读它，是个摆设）。
     */
    const int tz15 = settings_get_or_default(SETTINGS_TIMEZONE);
    const int32_t offset_sec = (int32_t)tz15 * 15 * 60;
    const uint32_t local_epoch = (uint32_t)((int64_t)utc_epoch + offset_sec);

    ESP_LOGI(TAG, "SNTP 校时：UTC %u + 时区 %d(15分单位) = 本地 %u",
             (unsigned)utc_epoch, tz15, (unsigned)local_epoch);

    esp_err_t err = data_hub_set_epoch(local_epoch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写 RTC 失败: %s", esp_err_to_name(err));
        return;
    }
    settings_set(SETTINGS_TIME_VALID, 1);
    ESP_LOGI(TAG, "RTC 已更新为本地时间 %u", (unsigned)local_epoch);

    /* 立刻刷界面，用户能马上看到表盘时间变了 */
    if (ui_port_lock(portMAX_DELAY)) {
        watch_ui_refresh();
        ui_port_unlock();
    }
}

/* ==================================================================
 * 四、启动流程
 * ================================================================== */
/* ==================================================================
 * 背光淡入（v0.6.1）
 *
 * 为什么加：原来开机是"直接设到目标亮度" —— 黑屏里突然亮起来有点刺眼，
 * 而且如果 LVGL 首帧还没画完，那一下会把花屏/半成品界面暴露出来。
 *
 * 做法（对标参考工程的 boot_anim）：
 *   1. 启动早期把背光保持在 0（屏幕黑着，用户看不到半成品界面）；
 *   2. **界面初始化完成之后**才开始 0 → 目标亮度 的淡入（约 300ms）。
 *
 * 取舍：不做"触摸打断淡入" —— 全程只有 300ms，为它再加一套状态不值当。
 *       淡入期间如果用户改亮度，会被淡入的最后一步覆盖回旧值（下次改就好）。
 * ================================================================== */
static esp_timer_handle_t s_fade_timer  = NULL;
static int                s_fade_target = 0;
static int                s_fade_cur    = 0;

#define BL_FADE_STEP_MS   30      /* 每步时长 */
#define BL_FADE_STEPS     10      /* 步数 → 总时长约 300ms */
#define BL_MIN_VISIBLE    5       /* 低于这个值基本看不见，直接给 5 */

static void bl_fade_cb(void *arg)
{
    (void)arg;

    const int step = (s_fade_target / BL_FADE_STEPS > 0)
                         ? (s_fade_target / BL_FADE_STEPS) : 1;
    s_fade_cur += step;

    if (s_fade_cur >= s_fade_target) {
        bsp_backlight_set(s_fade_target);
        if (s_fade_timer) {
            esp_timer_stop(s_fade_timer);
        }
        s_fade_cur = s_fade_target;
        ESP_LOGI(TAG, "背光淡入完成（%d%%）", s_fade_target);
        return;
    }
    bsp_backlight_set((s_fade_cur > BL_MIN_VISIBLE) ? s_fade_cur : BL_MIN_VISIBLE);
}

/** @brief 开始淡入（0 → target）。target<=0 表示用户设了"最暗"，那就一直黑 */
static void backlight_fade_in(int target)
{
    if (target <= 0) {
        bsp_backlight_set(0);
        return;
    }

    s_fade_target = (target > 100) ? 100 : target;
    s_fade_cur    = 0;
    bsp_backlight_set(0);

    if (!s_fade_timer) {
        const esp_timer_create_args_t ta = {
            .callback = bl_fade_cb,
            .name     = "bl_fade",
        };
        if (esp_timer_create(&ta, &s_fade_timer) != ESP_OK) {
            /* 建不出定时器就直接亮：宁可没有淡入，也不能黑着屏 */
            bsp_backlight_set(s_fade_target);
            ESP_LOGW(TAG, "淡入定时器创建失败，直接点亮 %d%%", s_fade_target);
            return;
        }
    }
    if (esp_timer_start_periodic(s_fade_timer, BL_FADE_STEP_MS * 1000) != ESP_OK) {
        bsp_backlight_set(s_fade_target);
        ESP_LOGW(TAG, "淡入定时器启动失败，直接点亮 %d%%", s_fade_target);
    }
}

esp_err_t app_start(void)
{
    ESP_LOGI(TAG, "================ 智能手表 v%s 启动 ================",
             APP_VERSION_STR);

    /* ---------- 1. 板级：屏幕 / 触摸 / 背光 / LVGL 任务 ---------- */
    ESP_RETURN_ON_ERROR(ui_port_init(), TAG, "板级初始化失败（屏幕或触摸没起来）");
    ESP_LOGI(TAG, "板级就绪：%s，屏幕 %dx%d",
             ui_port_name(), W_SCREEN_W, W_SCREEN_H);

    /* 顺手登记一下三个板载 I2C 器件在不在，起不来时省得到处找 */
    ESP_LOGI(TAG, "I2C 器件自检：触摸(0x%02X)=%s IMU(0x%02X)=%s RTC(0x%02X)=%s",
             BOARD_TOUCH_I2C_ADDR,
             board_i2c_probe(BOARD_TOUCH_I2C_ADDR) == ESP_OK ? "在" : "无应答",
             BOARD_IMU_I2C_ADDR,
             board_i2c_probe(BOARD_IMU_I2C_ADDR) == ESP_OK ? "在" : "无应答",
             BOARD_RTC_I2C_ADDR,
             board_i2c_probe(BOARD_RTC_I2C_ADDR) == ESP_OK ? "在" : "无应答");

    /* ---------- 2. 基础设施：事件总线 + 设置 ---------- */
    ESP_RETURN_ON_ERROR(event_bus_init(8), TAG, "事件总线初始化失败");

    if (settings_init() != ESP_OK) {
        /* 设置读不出来也要能用，只是亮度等参数回默认值 */
        ESP_LOGW(TAG, "设置服务初始化失败，本次运行使用默认参数");
    } else {
        settings_dump();
    }

    /* ---------- 3. 数据中枢：RTC + 电量采样任务 ---------- */
    if (data_hub_init() != ESP_OK) {
        ESP_LOGE(TAG, "数据中枢初始化失败：界面会显示占位符，但手表仍可用");
    }

    /* ---------- 3.2 电源管理：自动息屏 / 亮屏调度 ---------- */
    /*
     * 放这里是因为它要读 settings（息屏秒数、亮度），
     * 而 settings 在上一步已经初始化好。
     * 它顺便把屏幕钩子注册给界面层。
     */
    if (power_init() != ESP_OK) {
        ESP_LOGW(TAG, "电源管理初始化失败，自动息屏不会生效");
    }

    /* ---------- 3.5 蜂鸣器（v0.5.0 起真正启用） ----------
     *
     * 放这里是因为它对别的组件没有依赖，早初始化早可用。
     * 失败不算致命：手表其余功能照常，只是没有提示音。
     *
     * ⚠️ 资源占用：LEDC **TIMER_1 + CHANNEL_1**
     *    （TIMER_0 / CHANNEL_0 已经被 LCD 背光占了，别撞）。
     */
    if (buzzer_init() != ESP_OK) {
        ESP_LOGW(TAG, "蜂鸣器初始化失败，提示音将不可用");
    }

    /* ---------- 3.6 运动服务：IMU 采样 + 计步（P1 功能 6/7） ---------- */
    /*
     * ⚠️ 记录器必须在 motion 之前初始化：
     *    motion_init() 会立刻启动 50Hz 采样任务，而采样任务里会调
     *    imu_logger_feed()。记录器没就绪时 feed() 会安全返回（不崩），
     *    但用户点"开始录制"时若文件系统还没挂上就会录不上。
     */
    if (imu_logger_init() != ESP_OK) {
        ESP_LOGW(TAG, "IMU 记录器不可用（存储挂载失败），录制功能将不可用");
    }

    /*
     * ★ 复位黑盒（v0.7.1）：把"这次为什么重启 + 当时的电池电压"记进 Flash。
     *
     * 放在这里是因为它要用 SPIFFS，而 SPIFFS 由上面的 imu_logger_init() 挂载。
     * 为什么需要：**电池供电时没有串口**（USB 一插上就变成 USB 供电，故障往往不复现），
     * 而"每隔几秒重启一次"这类问题恰恰只在电池下出现 —— 只能靠这份记录事后判断
     * 是掉电复位（供电问题）还是崩溃/看门狗（软件问题）。用 `power log boot` 读。
     */
    pwr_log_note_boot();

    /*
     * ★ 卡死哨兵（v0.7.1）：实测出现过"静默硬死"——屏幕冻住、连串口控制台都没反应，
     *   而且**不复位、无 panic、无 coredump**（事后取证手段全部失效）。
     *   哨兵独立跑在 CPU 1 上、不依赖 esp_timer，盯着界面任务的心跳；
     *   一旦停住就把现场写进 /spiffs/hang.txt（用 `power log hang` 读）。
     *   详见 components/pwr_log/include/pwr_log.h。
     */
    pwr_log_hang_watch_start();

    /*
     * 放在 data_hub 之后：它算完步数要推给 data_hub。
     * IMU 不在线不会让启动失败，只是步数显示为 --。
     */
    if (motion_init() != ESP_OK) {
        ESP_LOGW(TAG, "运动服务初始化失败，步数将显示为 --");
    }

    /* ---------- 4. 记下目标亮度，但**先别点亮**（v0.6.1） ----------
     *
     * 原来这里直接 bsp_backlight_set(bright)：屏幕一亮，LVGL 首帧可能还没画完，
     * 用户会看到花屏或半成品界面。现在保持黑屏，等界面装好再淡入（见下面第 5.6 步）。
     */
    const int bright = brightness_get();
    bsp_backlight_set(0);
    ESP_LOGI(TAG, "亮度设置 %d%%（界面就绪后淡入）", bright);

    /* ---------- 5. 装配界面 ---------- */
    /*
     * ⚠️ 碰任何 lv_xxx 之前必须拿锁。
     *    这里用 -1（永久等待）：开机阶段还没人抢锁，等一会儿没关系；
     *    但用 0 的话一旦 LVGL 正忙就会静默失败 —— 界面直接空白，很难查。
     */
    if (!ui_port_lock(UINT32_MAX)) {
        ESP_LOGE(TAG, "拿不到 LVGL 锁，界面无法初始化");
        return ESP_ERR_TIMEOUT;
    }

    /* 注册所有内置页面（表盘 / 设置 / 健康 / 运动） */
    const watch_page_t *const *pages = watch_pages_all();
    for (int i = 0; pages && pages[i]; i++) {
        watch_ui_register_page(pages[i]);
    }

    /* 把"亮度"这条路接起来（界面层不认识 NVS 和背光） */
    watch_ui_set_brightness_hooks(brightness_get, brightness_set);
    watch_ui_set_setting_hooks(setting_get, setting_set);
    w_platform_set_rec_hooks(&s_rec_hooks);
    w_platform_set_time_hooks(&s_time_hooks);
    w_platform_set_buzzer_hooks(&s_buzzer_hooks);
    w_platform_set_wifi_hooks(wifi_service_get_hooks());
    w_platform_set_weather_hooks(&s_weather_hooks);   /* ★ v0.7.0 天气 */

    /* 建首页并订阅数据 */
    esp_err_t ui_err = watch_ui_init(&s_provider, &watch_page_home, 4);
    if (ui_err != ESP_OK) {
        ESP_LOGE(TAG, "界面初始化失败: %s", esp_err_to_name(ui_err));
    }

    ui_port_unlock();

    /* ---------- 5.6 界面已就绪 → 背光淡入（v0.6.1） ----------
     *
     * 放在这里（而不是更早）是有意的：此时首页已经建好，
     * 淡入过程中露出来的就是**成品界面**，不会看到半成品或花屏。
     * 注意：界面初始化失败时也照样点亮 —— 否则用户面对一块黑屏，
     * 连"哪里出错了"都看不到（错误在串口日志里）。
     */
    backlight_fade_in(bright);

    /* ---------- 5.5 订阅"设置变了"事件 → 自动刷界面 ---------- */
    if (xTaskCreate(settings_event_task, "settings_ev", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "设置变更订阅任务创建失败");
    }

    /* ---------- 5.8 WiFi：注册对时回调 + 启动服务 ---------- */
    /*
     * ⚠️ 这里改过主意，注释一度和代码相反（"只注册回调、不启动协议栈"，
     *    结果下一段就 init 了）。现在只有一套说法，如下：
     *
     *   一开始为了省 50KB RAM，约定"等用户点进 WiFi 页再启动"。
     *   但那样用户点"扫描"时协议栈还没起来 —— 虽然 hook_scan_start()
     *   会按需启动（不会再"点了没反应"），可首次要等 8 秒 RF 校准，
     *   体验很差。
     *   WiFi 是核心功能（RTC 没备份电池，全靠它对时），所以改成**开机就起**。
     *   实测 3.8 秒完成，RF 校准结果会缓存到 NVS，后续开机更快。
     */
    if (wifi_service_init() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi 服务启动失败，WiFi 页会提示不可用");
    }
    wifi_service_set_time_cb(on_sntp_synced);
    s_wifi_hooks_ready = true;

    /*
     * ★ v0.7.0 天气服务：只建状态 + 读缓存，**开机不联网**。
     *   联网是用户在天气页点"获取"才发生（和参考工程一致）——
     *   开机自动请求会因为"WiFi 还没连上"而必然失败一次。
     */
    if (weather_service_init() != ESP_OK) {
        ESP_LOGW(TAG, "天气服务启动失败，天气页会提示不可用");
    }

    /* ---------- 5.9 系统信息注入给开发人员页 ---------- */
    snprintf(s_info_heap, sizeof(s_info_heap), "%u KB",
             (unsigned)(esp_get_free_heap_size() / 1024));
    snprintf(s_info_psram, sizeof(s_info_psram), "%u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    snprintf(s_info_uptime, sizeof(s_info_uptime), "%lld s",
             esp_timer_get_time() / 1000000LL);
    watch_developer_set_sysinfo(APP_VERSION_STR, s_info_heap,
                                s_info_psram, s_info_uptime);

    /* ---------- 6. 串口命令行（校时 / 查状态） ---------- */
    if (app_console_start() != ESP_OK) {
        ESP_LOGW(TAG, "串口命令行启动失败；校时请改用 BLE（P2）");
    }

    ESP_LOGI(TAG, "================ 初始化完成 ================");
    ESP_LOGI(TAG, "操作：右滑翻页 / 左滑返回 / 上滑控制中心");
    data_hub_dump();

    return ESP_OK;
}
