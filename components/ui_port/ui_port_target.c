/**
 * @file ui_port_target.c
 * @brief UI 平台端口层 —— **手表端（ESP-IDF）实现**
 *
 * 这一份实现极薄：全部转调 BSP。
 * 想看 PC 端实现长什么样，去 lvgl_sim/src/ui_port_pc.c，两份一一对应。
 */
#include "ui_port.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "bsp.h"

static const char *TAG = "UI_PORT";

/*
 * 跨任务短临界区用的自旋锁（v0.6.1 新增，用途见 ui_port.h 的说明）。
 * 只有一个实例：临界区都是"几个赋值"，不会互相等太久。
 */
static portMUX_TYPE s_crit_mux = portMUX_INITIALIZER_UNLOCKED;

esp_err_t ui_port_init(void)
{
    ESP_LOGI(TAG, "平台 = 手表端（ESP32-S3 + esp_lvgl_port 独立任务）");
    return bsp_init();
}

lv_disp_t *ui_port_display(void)
{
    return bsp_display_get();
}

lv_indev_t *ui_port_indev(void)
{
    /* 手表端触摸设备由 BSP 注册并交给 esp_lvgl_port 管理 */
    return bsp_touch_get();
}

bool ui_port_lock(uint32_t timeout_ms)
{
    /* esp_lvgl_port 把 LVGL 放在独立任务里跑 → 必须加锁 */
    return bsp_display_lock(timeout_ms);
}

void ui_port_unlock(void)
{
    bsp_display_unlock();
}

void ui_port_critical_enter(void)
{
    taskENTER_CRITICAL(&s_crit_mux);
}

void ui_port_critical_exit(void)
{
    taskEXIT_CRITICAL(&s_crit_mux);
}

void ui_port_run(uint32_t sleep_ms)
{
    /*
     * 手表端：什么都不用做。
     * esp_lvgl_port 已经起了一个 LVGL 任务在内部调 lv_timer_handler()，
     * app_main 里的主任务可以安心挂起。
     */
    (void)sleep_ms;
}

const char *ui_port_name(void)
{
    return "ESP32-S3 target";
}
