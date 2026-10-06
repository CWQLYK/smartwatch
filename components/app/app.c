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
#include "app_console.h"

static const char *TAG = "APP";


/*
 * 固件版本号。
 * ⚠️ 改这里的时候，记得同步三处，否则日志/设置页/文档会对不上：
 *   1. components/watch_ui_pages/pages/watch_page_settings.c 的 WATCH_FIRMWARE_VERSION
 *   2. doc/version/vX.Y.Z.md（每个版本一个文档）
 *   3. doc/version/README.md 的版本列表
 */
#define APP_VERSION_STR  "0.3.0"

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

/* ---------- 设置项（界面层按名字寻址，这里映射到真正的 NVS 项） ---------- */
static int setting_get(const char *key, int def)
{
    if (!key) {
        return def;
    }
    if (strcmp(key, W_SETTING_AUTO_SCREEN) == 0) {
        return (int)settings_get_or_default(SETTINGS_SCREEN_TIMEOUT);
    }
    if (strcmp(key, W_SETTING_WRIST_WAKE) == 0) {
        return (int)settings_get_or_default(SETTINGS_WRIST_WAKE);
    }
    if (strcmp(key, W_SETTING_STEP_GOAL) == 0) {
        return (int)settings_get_or_default(SETTINGS_STEP_GOAL);
    }
    if (strcmp(key, W_SETTING_HEIGHT) == 0) {
        return (int)settings_get_or_default(SETTINGS_USER_HEIGHT);
    }
    if (strcmp(key, W_SETTING_WEIGHT) == 0) {
        return (int)settings_get_or_default(SETTINGS_USER_WEIGHT);
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
    } else if (strcmp(key, W_SETTING_WRIST_WAKE) == 0) {
        err = settings_set(SETTINGS_WRIST_WAKE, value);
    } else if (strcmp(key, W_SETTING_STEP_GOAL) == 0) {
        err = settings_set(SETTINGS_STEP_GOAL, value);
    } else if (strcmp(key, W_SETTING_HEIGHT) == 0) {
        err = settings_set(SETTINGS_USER_HEIGHT, value);
    } else if (strcmp(key, W_SETTING_WEIGHT) == 0) {
        err = settings_set(SETTINGS_USER_WEIGHT, value);
    } else {
        ESP_LOGW(TAG, "未知设置项键名 '%s'", key);
        return -1;
    }

    /* 身高体重直接影响步长和卡路里，改完要让计步算法立刻拿到新值 */
    if (err == ESP_OK &&
        (strcmp(key, W_SETTING_HEIGHT) == 0 ||
         strcmp(key, W_SETTING_WEIGHT) == 0)) {
        motion_apply_user_params();
    }

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
 * 四、启动流程
 * ================================================================== */
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

    /* ---------- 3.5 运动服务：IMU 采样 + 计步（P1 功能 6/7） ---------- */
    /*
     * 放在 data_hub 之后：它算完步数要推给 data_hub。
     * IMU 不在线不会让启动失败，只是步数显示 "--"。
     */
    if (motion_init() != ESP_OK) {
        ESP_LOGW(TAG, "运动服务初始化失败，步数将显示为 --");
    }

    /* ---------- 4. 把设置里的亮度真正应用出去 ---------- */
    const int bright = brightness_get();
    if (bsp_backlight_set(bright) != ESP_OK) {
        ESP_LOGW(TAG, "背光设置 %d%% 失败", bright);
    }
    ESP_LOGI(TAG, "亮度 %d%%", bright);

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

    /* 建首页并订阅数据 */
    esp_err_t ui_err = watch_ui_init(&s_provider, &watch_page_home, 4);
    if (ui_err != ESP_OK) {
        ESP_LOGE(TAG, "界面初始化失败: %s", esp_err_to_name(ui_err));
    }

    ui_port_unlock();

    /* ---------- 5.5 订阅"设置变了"事件 → 自动刷界面 ---------- */
    if (xTaskCreate(settings_event_task, "settings_ev", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "设置变更订阅任务创建失败");
    }

    /* ---------- 6. 串口命令行（校时 / 查状态） ---------- */
    if (app_console_start() != ESP_OK) {
        ESP_LOGW(TAG, "串口命令行启动失败；校时请改用 BLE（P2）");
    }

    ESP_LOGI(TAG, "================ 初始化完成 ================");
    ESP_LOGI(TAG, "操作：右滑翻页 / 左滑返回 / 上滑控制中心");
    data_hub_dump();

    return ESP_OK;
}
