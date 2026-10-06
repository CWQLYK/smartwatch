/**
 * @file main.c
 * @brief 智能手表 —— 程序入口
 *
 * 这个文件**故意只有几十行**。
 *
 * v0.1 的时候它承担了全部界面代码（260 行），加一个功能就要动它；
 * v0.2 重构后它只做一件事：把控制权交给 app_start()，然后挂起。
 *
 * 想知道某段代码在哪：
 *   - 硬件驱动          → components/rtc_pcf85063, battery, buzzer, bsp, board
 *   - 服务/数据         → components/data_hub, settings, event_bus
 *   - 界面              → components/watch_ui, watch_ui_pages
 *   - 接线/装配         → components/app
 *   - 命令行            → components/app/app_console.c
 *
 * 详见 doc/代码架构说明.md
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    /* 装配整个应用：板级 → 服务 → 数据 → 界面 → 命令行 */
    esp_err_t err = app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "应用启动失败: %s，进入降级运行", esp_err_to_name(err));
    }

    /*
     * 主任务到这里就没活了：
     *   - LVGL 在 esp_lvgl_port 自己的任务里跑
     *   - 数据采样在 data_hub 任务里跑
     *   - 命令行在 console REPL 任务里跑
     * 所以这里只做低频健康巡检，顺便把内存水位打出来（查泄漏用）。
     */
    uint32_t tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        tick++;

        ESP_LOGI(TAG, "心跳 #%u｜空闲堆 %u 字节｜最小空闲堆 %u 字节",
                 (unsigned)tick,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
    }
}
