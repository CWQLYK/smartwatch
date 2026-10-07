/**
 * @file settings.c
 * @brief 设置服务实现 —— 表驱动 + NVS
 *
 * 一行一个设置项：{ NVS 键名, 默认值, 最小值, 最大值, 中文说明 }
 * 加新设置项只需在这里加一行，并在 settings.h 的枚举末尾加一项。
 */
#include "settings.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "event_bus.h"

static const char *TAG = "SETTINGS";

/* ==================== 设置项定义表 ==================== */
typedef struct {
    const char *nvs_key;    /* NVS 键名，最长 15 字符 */
    int32_t     def;        /* 编译期默认值 */
    int32_t     min;        /* 合法下限 */
    int32_t     max;        /* 合法上限 */
    const char *desc;       /* 中文说明，用于 settings_dump() */
} settings_desc_t;

static const settings_desc_t s_desc[SETTINGS_COUNT] = {
    [SETTINGS_BRIGHTNESS]     = { "bl",      70,     5,   100, "屏幕亮度(%)" },
    [SETTINGS_SCREEN_TIMEOUT] = { "scr_to",  30,     0,   120, "自动息屏(秒，0=常亮)" },
    [SETTINGS_STEP_GOAL]      = { "steps_gl",8000,1000, 50000, "每日步数目标" },
    [SETTINGS_TIMEZONE]       = { "tz",      32,    -48,    56, "时区(15分钟单位)" },
    [SETTINGS_TIME_VALID]     = { "t_valid",  0,      0,     1, "RTC 已校时" },
    [SETTINGS_TIME_EPOCH]     = { "t_epoch",  0,      0, INT32_MAX, "最后已知时间戳" },
    /* ---------- v0.5.0 新增：闹钟 / 倒计时 ---------- */
    [SETTINGS_ALARM_HOUR]     = { "alm_h",    7,      0,    23, "闹钟-时" },
    [SETTINGS_ALARM_MIN]      = { "alm_m",    0,      0,    59, "闹钟-分" },
    [SETTINGS_ALARM_EN]       = { "alm_en",   0,      0,     1, "闹钟开关" },
    [SETTINGS_CD_PRESET_SEC]  = { "cd_set", 300,     10, 86399, "倒计时时长(秒)" },
    [SETTINGS_CD_RING_SEC]    = { "cd_ring", 10,      1,   300, "倒计时响铃时长(秒)" },
};

/* ==================== 运行期状态 ==================== */
static nvs_handle_t     s_nvs   = 0;
static int32_t          s_cache[SETTINGS_COUNT];
static bool             s_ready = false;
static SemaphoreHandle_t s_lock = NULL;

/* 标记哪些项曾经真的存进过 NVS（用于 dump 时区分"默认"和"存过"） */
static bool             s_stored[SETTINGS_COUNT];

/* ==================== 内部工具 ==================== */
static bool key_valid(settings_key_t key)
{
    return (key >= 0) && (key < SETTINGS_COUNT);
}

static int32_t clamp_value(settings_key_t key, int32_t v)
{
    if (v < s_desc[key].min) return s_desc[key].min;
    if (v > s_desc[key].max) return s_desc[key].max;
    return v;
}

/** @brief 保证 NVS 已初始化（含"版本升级/键被占满"的恢复） */
static esp_err_t nvs_ensure_ready(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区需要重新初始化（%s），擦除后重试",
                 esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS 擦除失败");
        err = nvs_flash_init();
    }
    return err;
}

/* ==================== 对外接口 ==================== */
esp_err_t settings_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "设置锁创建失败");
    }

    ESP_RETURN_ON_ERROR(nvs_ensure_ready(), TAG, "NVS 初始化失败");
    ESP_RETURN_ON_ERROR(nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &s_nvs),
                        TAG, "打开 NVS 命名空间 '%s' 失败", SETTINGS_NVS_NAMESPACE);

    /* 先填默认值，再用 NVS 里存过的覆盖 */
    int loaded = 0;
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        s_cache[i]  = s_desc[i].def;
        s_stored[i] = false;

        int32_t v = 0;
        if (nvs_get_i32(s_nvs, s_desc[i].nvs_key, &v) == ESP_OK) {
            s_cache[i]  = clamp_value((settings_key_t)i, v);
            s_stored[i] = true;
            loaded++;
        }
    }

    s_ready = true;
    ESP_LOGI(TAG, "设置服务就绪：%d 项，其中 %d 项来自 NVS", SETTINGS_COUNT, loaded);

    /* 首次上电时把默认亮度真正写进硬件（由 app 层负责应用） */
    return ESP_OK;
}

esp_err_t settings_get(settings_key_t key, int32_t *out)
{
    ESP_RETURN_ON_FALSE(key_valid(key), ESP_ERR_INVALID_ARG, TAG, "key 越界: %d", key);
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out 为空");
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "设置服务未初始化");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cache[key];
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

int32_t settings_get_or_default(settings_key_t key)
{
    if (!key_valid(key)) {
        return 0;
    }
    if (!s_ready) {
        return s_desc[key].def;
    }
    int32_t v = s_desc[key].def;
    settings_get(key, &v);
    return v;
}

esp_err_t settings_set(settings_key_t key, int32_t value)
{
    ESP_RETURN_ON_FALSE(key_valid(key), ESP_ERR_INVALID_ARG, TAG, "key 越界: %d", key);
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "设置服务未初始化");

    const int32_t v = clamp_value(key, value);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = (s_cache[key] != v);
    if (changed) {
        s_cache[key] = v;
    }
    xSemaphoreGive(s_lock);

    if (!changed) {
        return ESP_OK;      /* 值没变：不写 flash、不发事件，省擦写寿命 */
    }

    esp_err_t err = nvs_set_i32(s_nvs, s_desc[key].nvs_key, v);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 '%s' 失败: %s", s_desc[key].nvs_key, esp_err_to_name(err));
        return err;
    }

    s_stored[key] = true;
    ESP_LOGI(TAG, "设置变更：%s = %" PRId32 " (%s)", s_desc[key].nvs_key, v,
             s_desc[key].desc);

    /* 通知关心的人（UI 靠这个自动刷新） */
    event_t e = { .type = EVT_SETTINGS_CHANGED };
    e.data.settings.changed_mask = SETTINGS_BIT(key);
    event_bus_publish(&e);

    return ESP_OK;
}

esp_err_t settings_set_masked(uint32_t mask, int32_t value)
{
    esp_err_t first_err = ESP_OK;

    for (int i = 0; i < SETTINGS_COUNT; i++) {
        if (!(mask & SETTINGS_BIT(i))) {
            continue;
        }
        esp_err_t err = settings_set((settings_key_t)i, value);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }
    return first_err;
}

esp_err_t settings_reset(settings_key_t key)
{
    if (!key_valid(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(settings_set(key, s_desc[key].def), TAG, "重置失败");

    /* 恢复默认 = 语义上"这项没被用户改过"，所以把 NVS 键也删掉 */
    nvs_erase_key(s_nvs, s_desc[key].nvs_key);
    nvs_commit(s_nvs);
    s_stored[key] = false;

    ESP_LOGI(TAG, "已恢复默认：%s = %" PRId32, s_desc[key].desc, s_desc[key].def);
    return ESP_OK;
}

esp_err_t settings_reset_all(void)
{
    ESP_LOGI(TAG, "恢复出厂设置：擦除命名空间 '%s'", SETTINGS_NVS_NAMESPACE);

    esp_err_t err = nvs_erase_all(s_nvs);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "擦除 NVS 失败");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        s_cache[i]  = s_desc[i].def;
        s_stored[i] = false;
    }
    xSemaphoreGive(s_lock);

    event_t e = { .type = EVT_SETTINGS_CHANGED };
    e.data.settings.changed_mask = EVT_MASK_ALL & ((1UL << SETTINGS_COUNT) - 1);
    event_bus_publish(&e);
    return ESP_OK;
}

int32_t settings_default(settings_key_t key)
{
    return key_valid(key) ? s_desc[key].def : 0;
}

void settings_range(settings_key_t key, int32_t *min, int32_t *max)
{
    if (!key_valid(key)) {
        if (min) *min = 0;
        if (max) *max = 0;
        return;
    }
    if (min) *min = s_desc[key].min;
    if (max) *max = s_desc[key].max;
}

void settings_dump(void)
{
    ESP_LOGI(TAG, "---- 当前设置 ----");
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        ESP_LOGI(TAG, "  %-16s %-10s = %" PRId32 "  (范围 %" PRId32 "~%" PRId32 "%s)",
                 s_desc[i].nvs_key, s_desc[i].desc, s_cache[i],
                 s_desc[i].min, s_desc[i].max,
                 s_stored[i] ? "" : "，默认值");
    }
    ESP_LOGI(TAG, "------------------");
}
