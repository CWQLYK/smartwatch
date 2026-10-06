/**
 * @file data_hub.c
 * @brief 数据中枢实现 —— 周期采样 + 带锁快照 + 变化通知
 *
 * 时间语义（重要，容易踩坑）：
 *   **RTC 芯片里存的就是"墙上时间"（本地时间）**，不是 UTC。
 *   依据：项目里没有第二套时区数据源，而手表表盘要显示的就是本地点。
 *   时区（设置项 SETTINGS_TIMEZONE）只在需要真正 UTC 的场合
 *   （以后的 BLE CTS 同步、SNTP 校时、日志时间戳）才参与换算：
 *
 *       UTC = 本地时间 - 时区偏移
 *
 *   P1 的串口校时命令直接写"墙上时间"，不做时区换算 —— 所见即所得。
 */
#include "data_hub.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "rtc_pcf85063.h"
#include "battery.h"
#include "settings.h"
#include "event_bus.h"

static const char *TAG = "DATA_HUB";

/* ==================== 采样周期 ==================== */
#define HUB_BATTERY_PERIOD_MS   30000   /* 电量 30 秒采一次 */
#define HUB_TASK_PRIO           5
#define HUB_TASK_STACK          3072
#define HUB_NOTIFY_MAX          4       /* 最多 4 个变更回调 */

/* 电量变化超过这些阈值才通知（避免电压抖动导致 UI 一直重画）*/
#define HUB_BAT_PCT_DELTA       1
#define HUB_BAT_MV_DELTA        20

/* ==================== 内部状态 ==================== */
static device_state_t    s_state;
static SemaphoreHandle_t s_lock  = NULL;
static bool              s_ready = false;
static TaskHandle_t      s_task  = NULL;

static data_hub_cb_t     s_cbs[HUB_NOTIFY_MAX];
static bool              s_cb_used[HUB_NOTIFY_MAX];

/* ==================== 内部工具 ==================== */
static void notify_locked(const device_state_t *st, uint32_t changed)
{
    /* 已在锁内：先把回调列表抄一份，避免回调里再注册/注销造成死锁 */
    data_hub_cb_t cbs[HUB_NOTIFY_MAX];
    int n = 0;
    for (int i = 0; i < HUB_NOTIFY_MAX; i++) {
        if (s_cb_used[i] && s_cbs[i]) {
            cbs[n++] = s_cbs[i];
        }
    }
    /* 锁外调用回调（回调里可能会调 data_hub_snapshot，不能持锁） */
    xSemaphoreGive(s_lock);
    for (int i = 0; i < n; i++) {
        cbs[i](st, changed);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

/* ==================== 采样 ==================== */
static uint32_t sample_time(void)
{
    dev_time_t t = { 0 };
    rtc_pcf_time_t rt;

    if (rtc_pcf_get_time(&rt) == ESP_OK) {
        t.year    = rt.year;
        t.month   = rt.month;
        t.day     = rt.day;
        t.hour    = rt.hour;
        t.minute  = rt.minute;
        t.second  = rt.second;
        t.weekday = rt.weekday;
        t.valid   = !rtc_pcf_time_lost() && rtc_pcf_time_is_valid(&rt);
    } else {
        t.valid = false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = memcmp(&s_state.time, &t, sizeof(t)) != 0;
    s_state.time = t;
    if (changed) {
        notify_locked(&s_state, DATA_CHANGED_TIME);
    }
    xSemaphoreGive(s_lock);

    if (changed && t.valid) {
        evt_time_t e = {
            .year = t.year, .month = t.month, .day = t.day,
            .hour = t.hour, .minute = t.minute, .second = t.second,
            .weekday = t.weekday, .valid = t.valid,
        };
        event_bus_publish_time(&e);
    }
    return changed ? DATA_CHANGED_TIME : 0;
}

static uint32_t sample_battery(void)
{
    battery_reading_t br;
    if (battery_read(&br) != ESP_OK) {
        ESP_LOGW(TAG, "电量采样失败");
        return 0;
    }

    dev_battery_t b = {
        .millivolt = br.millivolt,
        .percent   = br.percent,
        .present   = br.present,
        .charging  = br.charging,
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const dev_battery_t *old = &s_state.battery;
    const bool changed = (old->percent < 0) != (b.percent < 0) ||
                         (old->percent >= 0 && b.percent >= 0 &&
                          (b.percent - old->percent >= HUB_BAT_PCT_DELTA ||
                           old->percent - b.percent >= HUB_BAT_PCT_DELTA)) ||
                         (b.millivolt - old->millivolt >= HUB_BAT_MV_DELTA ||
                          old->millivolt - b.millivolt >= HUB_BAT_MV_DELTA) ||
                         (b.charging != old->charging) ||
                         (b.present  != old->present);
    s_state.battery = b;
    if (changed) {
        notify_locked(&s_state, DATA_CHANGED_BATTERY);
    }
    xSemaphoreGive(s_lock);

    if (changed) {
        evt_battery_t e = {
            .millivolt = b.millivolt,
            .percent   = b.percent,
            .charging  = b.charging,
            .present   = b.present,
        };
        event_bus_publish_battery(&e);
        ESP_LOGI(TAG, "电量：%d mV，%d%%%s%s",
                 b.millivolt, b.percent,
                 b.charging ? "，充电中" : "",
                 b.present ? "" : "（未检测到电池）");
    }
    return changed ? DATA_CHANGED_BATTERY : 0;
}

/* ==================== 采样任务 ==================== */
static void hub_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "数据中枢任务启动（时间 1Hz 对齐采样，电量 %d 秒一次）",
             HUB_BATTERY_PERIOD_MS / 1000);

    /* 先把电量采一次，UI 一上电就有数（而不是等 30 秒） */
    sample_battery();
    uint32_t bat_elapsed = 0;

    for (;;) {
        /*
         * 对齐到"整秒"边界再读 RTC：
         * 如果只是每 1000ms 硬延时，任务调度抖动会累积成肉眼可见的走时误差。
         */
        const int64_t now_us  = esp_timer_get_time();
        const int64_t to_next = 1000000 - (now_us % 1000000);

        uint32_t ms = (uint32_t)(to_next / 1000);
        if (ms < 20) {
            ms += 1000;                 /* 太近了就再等一秒，别 1ms 一次空转 */
        }
        vTaskDelay(pdMS_TO_TICKS(ms));

        sample_time();

        bat_elapsed += ms;
        if (bat_elapsed >= HUB_BATTERY_PERIOD_MS) {
            bat_elapsed = 0;
            sample_battery();
        }
    }
}

/* ==================== 对外接口 ==================== */
esp_err_t data_hub_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "数据中枢锁创建失败");
    }

    /* 幂等确保依赖就绪（失败不致命：对应数据会显示为"未知"）*/
    ESP_RETURN_ON_ERROR(rtc_pcf_init(), TAG, "RTC 初始化失败");
    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电池采样初始化失败，电量将显示为未知");
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.battery.percent = -1;       /* -1 = 未知 */
    s_state.steps.available = false;    /* P1 还没接 IMU */

    s_ready = true;

    BaseType_t ok = xTaskCreate(hub_task, "data_hub", HUB_TASK_STACK, NULL,
                                HUB_TASK_PRIO, &s_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "数据中枢任务创建失败");

    return ESP_OK;
}

void data_hub_snapshot(device_state_t *out)
{
    if (!out) return;
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        out->battery.percent = -1;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

void data_hub_get_time(dev_time_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state.time;
    xSemaphoreGive(s_lock);
}

void data_hub_get_battery(dev_battery_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); out->percent = -1; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state.battery;
    xSemaphoreGive(s_lock);
}

int data_hub_on_change(data_hub_cb_t cb)
{
    ESP_RETURN_ON_FALSE(cb, -1, TAG, "回调为空");
    ESP_RETURN_ON_FALSE(s_lock, -1, TAG, "数据中枢未初始化");

    int slot = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < HUB_NOTIFY_MAX; i++) {
        if (!s_cb_used[i]) { slot = i; break; }
    }
    if (slot >= 0) {
        s_cbs[slot]     = cb;
        s_cb_used[slot] = true;
    }
    xSemaphoreGive(s_lock);

    if (slot < 0) {
        ESP_LOGE(TAG, "变更回调槽位已满（最多 %d 个）", HUB_NOTIFY_MAX);
        return -1;
    }
    ESP_LOGI(TAG, "变更回调 #%d 注册", slot);
    return slot;
}

void data_hub_off_change(int slot)
{
    if (slot < 0 || slot >= HUB_NOTIFY_MAX || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cb_used[slot] = false;
    s_cbs[slot]     = NULL;
    xSemaphoreGive(s_lock);
}

esp_err_t data_hub_set_time(const dev_time_t *t)
{
    ESP_RETURN_ON_FALSE(t, ESP_ERR_INVALID_ARG, TAG, "时间为空");

    rtc_pcf_time_t rt = {
        .year    = t->year,
        .month   = t->month,
        .day     = t->day,
        .hour    = t->hour,
        .minute  = t->minute,
        .second  = t->second,
        .weekday = t->weekday ? t->weekday : 1,
    };
    ESP_RETURN_ON_ERROR(rtc_pcf_set_time(&rt), TAG, "写 RTC 失败");

    /* 记一笔"已校时"和时间戳，供掉电兜底（设置项已持久化到 NVS）*/
    settings_set(SETTINGS_TIME_VALID, 1);
    settings_set(SETTINGS_TIME_EPOCH, (int32_t)rtc_pcf_get_epoch());

    /* 立刻刷新快照并通知 UI，不用等下一次整秒 */
    sample_time();
    return ESP_OK;
}

esp_err_t data_hub_set_epoch(uint32_t epoch_utc)
{
    ESP_RETURN_ON_ERROR(rtc_pcf_set_epoch(epoch_utc), TAG, "写 RTC 失败");
    settings_set(SETTINGS_TIME_VALID, 1);
    settings_set(SETTINGS_TIME_EPOCH, (int32_t)epoch_utc);
    sample_time();
    return ESP_OK;
}

uint32_t data_hub_get_epoch(void)
{
    return rtc_pcf_get_epoch();
}

void data_hub_set_steps(int steps, int kcal, int meters)
{
    if (!s_lock) {
        return;
    }

    /* ---- 先更新快照，判断有没有真的变 ---- */
    xSemaphoreTake(s_lock, portMAX_DELAY);

    const dev_steps_t old = s_state.steps;

    if (steps < 0) {
        /* 没有运动数据（IMU 没接 / 初始化失败）→ 界面显示 "--" */
        s_state.steps.available = false;
        s_state.steps.steps     = 0;
        s_state.steps.kcal      = 0;
        s_state.steps.meters    = 0;
    } else {
        s_state.steps.available = true;
        s_state.steps.steps     = steps;
        s_state.steps.kcal      = kcal;
        s_state.steps.meters    = meters;
    }

    const bool changed = (old.available != s_state.steps.available) ||
                         (old.steps  != s_state.steps.steps)  ||
                         (old.kcal   != s_state.steps.kcal)   ||
                         (old.meters != s_state.steps.meters);

    dev_steps_t now = s_state.steps;
    if (changed) {
        /*
         * notify_locked() 内部会"释放锁 → 调回调 → 重新上锁"，
         * 返回时锁仍然持有，所以这里紧接着 give 一次即可。
         */
        notify_locked(&s_state, DATA_CHANGED_STEPS);
    }
    xSemaphoreGive(s_lock);

    if (!changed) {
        return;
    }

    /* 事件总线（给非 UI 的订阅者，比如以后的 BLE 上报） */
    event_t e = { .type = EVT_STEPS };
    e.data.steps.steps  = now.steps;
    e.data.steps.kcal   = now.kcal;
    e.data.steps.meters = now.meters;
    event_bus_publish(&e);

    ESP_LOGD(TAG, "运动数据：%d 步 / %d 千卡 / %d 米",
             now.steps, now.kcal, now.meters);
}

void data_hub_refresh_now(void)
{
    sample_time();
    sample_battery();
}

void data_hub_dump(void)
{
    device_state_t st;
    data_hub_snapshot(&st);

    ESP_LOGI(TAG, "---- 设备状态快照 ----");
    if (st.time.valid) {
        ESP_LOGI(TAG, "  时间：%04d-%02d-%02d %02d:%02d:%02d 星期%d",
                 st.time.year, st.time.month, st.time.day,
                 st.time.hour, st.time.minute, st.time.second, st.time.weekday);
    } else {
        ESP_LOGW(TAG, "  时间：**未校时**（RTC 掉过电），请用串口命令 set_time 校时");
    }
    if (st.battery.percent >= 0) {
        ESP_LOGI(TAG, "  电量：%d mV / %d%%%s",
                 st.battery.millivolt, st.battery.percent,
                 st.battery.charging ? "，充电中" : "");
    } else {
        ESP_LOGW(TAG, "  电量：未知（%d mV，可能没接电池）", st.battery.millivolt);
    }
    ESP_LOGI(TAG, "  步数：%s", st.steps.available ? "已接入" : "未接入（P1 不含 IMU）");
    ESP_LOGI(TAG, "  事件总线丢弃计数：%" PRIu32, event_bus_dropped());
    ESP_LOGI(TAG, "----------------------");
}
