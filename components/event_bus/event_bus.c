/**
 * @file event_bus.c
 * @brief 事件总线实现 —— 静态订阅者表 + FreeRTOS 队列
 *
 * 实现取舍：
 *   1. 订阅者表是**静态数组**，不在运行时 malloc —— 嵌入式里少一次分配少一个坑。
 *   2. 表用**互斥量**保护：发布临界区里要调 xQueueSend（FreeRTOS API），
 *      用普通互斥量比自旋锁更规矩，也便于以后加"发布者阻塞策略"。
 *   3. publish 全程非阻塞：xQueueSend 超时传 0，满了就丢并计数。
 */
#include "event_bus.h"

#include <inttypes.h>
#include <string.h>
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "EVENT";

#define EVENT_MAX_SUBS      8       /* 最多 8 个订阅者，够用（UI/日志/BLE/...） */
#define EVENT_DEFAULT_DEPTH 8

typedef struct {
    QueueHandle_t q;
    uint32_t      mask;
    bool          used;
} sub_slot_t;

static sub_slot_t      s_subs[EVENT_MAX_SUBS];
static SemaphoreHandle_t s_lock        = NULL;
static uint32_t        s_default_depth = EVENT_DEFAULT_DEPTH;
static uint32_t        s_dropped       = 0;

esp_err_t event_bus_init(uint32_t queue_depth)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "事件总线锁创建失败");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_subs, 0, sizeof(s_subs));
    s_default_depth = queue_depth ? queue_depth : EVENT_DEFAULT_DEPTH;
    s_dropped       = 0;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "事件总线就绪（最多 %d 个订阅者，默认队列深度 %" PRIu32 "）",
             EVENT_MAX_SUBS, s_default_depth);
    return ESP_OK;
}

event_sub_t event_bus_subscribe(uint32_t mask, uint32_t queue_depth)
{
    if (!mask) {
        ESP_LOGW(TAG, "订阅掩码为 0，忽略");
        return NULL;
    }
    if (!s_lock) {
        ESP_LOGE(TAG, "事件总线未初始化，请先 event_bus_init()");
        return NULL;
    }

    uint32_t depth = queue_depth ? queue_depth : s_default_depth;
    QueueHandle_t q = xQueueCreate(depth, sizeof(event_t));
    if (!q) {
        ESP_LOGE(TAG, "订阅者队列创建失败（内存不足？）");
        return NULL;
    }

    int slot = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < EVENT_MAX_SUBS; i++) {
        if (!s_subs[i].used) { slot = i; break; }
    }
    if (slot >= 0) {
        s_subs[slot].q    = q;
        s_subs[slot].mask = mask;
        s_subs[slot].used = true;
    }
    xSemaphoreGive(s_lock);

    if (slot < 0) {
        ESP_LOGE(TAG, "订阅者已满（%d 个），请调大 EVENT_MAX_SUBS", EVENT_MAX_SUBS);
        vQueueDelete(q);
        return NULL;
    }

    ESP_LOGI(TAG, "订阅者 #%d 注册，掩码 0x%08" PRIX32 "，队列深度 %" PRIu32,
             slot, mask, depth);
    return q;
}

void event_bus_unsubscribe(event_sub_t sub)
{
    if (!sub || !s_lock) return;

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < EVENT_MAX_SUBS; i++) {
        if (s_subs[i].used && s_subs[i].q == sub) {
            s_subs[i].used = false;
            s_subs[i].q    = NULL;
            s_subs[i].mask = 0;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);

    if (found) {
        vQueueDelete(sub);
    }
}

void event_bus_publish(const event_t *evt)
{
    if (!evt || evt->type == EVT_NONE || evt->type >= EVT_COUNT || !s_lock) {
        return;
    }

    const uint32_t bit = EVT_MASK(evt->type);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < EVENT_MAX_SUBS; i++) {
        if (!s_subs[i].used || !(s_subs[i].mask & bit)) {
            continue;
        }
        /* 非阻塞投递：满了就丢，绝不让发布者挂起 */
        if (xQueueSend(s_subs[i].q, evt, 0) != pdTRUE) {
            s_dropped++;
        }
    }
    xSemaphoreGive(s_lock);
}

void event_bus_publish_time(const evt_time_t *t)
{
    event_t e = { .type = EVT_TIME_TICK };
    e.data.time = *t;
    event_bus_publish(&e);
}

void event_bus_publish_battery(const evt_battery_t *b)
{
    event_t e = { .type = EVT_BATTERY };
    e.data.battery = *b;
    event_bus_publish(&e);
}

uint32_t event_bus_dropped(void)
{
    return s_dropped;
}
