/**
 * @file watch_platform.c
 * @brief 平台钩子的存取实现
 *
 * 就一个静态指针表 + 判空封装。放单独文件是为了让 watch_pages.c 别太臃肿，
 * 而且这些钩子是**跨页面共享**的（WiFi 页、录制页、息屏都要用）。
 */
#include "watch_platform.h"

#include "esp_log.h"

static const char *TAG = "PLATFORM";

/* ==================== 钩子存放 ==================== */
static const w_wifi_hooks_t   *s_wifi   = NULL;
static const w_rec_hooks_t    *s_rec    = NULL;
static const w_time_hooks_t   *s_time   = NULL;
static const w_screen_hooks_t *s_screen = NULL;
static const w_buzzer_hooks_t *s_buzzer = NULL;

void w_platform_set_wifi_hooks(const w_wifi_hooks_t *hooks)
{
    s_wifi = hooks;
    ESP_LOGI(TAG, "WiFi 钩子%s", hooks ? "已注册" : "已清除");
}

const w_wifi_hooks_t *w_platform_wifi(void)
{
    return s_wifi;
}

void w_platform_set_rec_hooks(const w_rec_hooks_t *hooks)
{
    s_rec = hooks;
    ESP_LOGI(TAG, "IMU 录制钩子%s", hooks ? "已注册" : "已清除");
}

const w_rec_hooks_t *w_platform_rec(void)
{
    return s_rec;
}

void w_platform_set_time_hooks(const w_time_hooks_t *hooks)
{
    s_time = hooks;
    ESP_LOGI(TAG, "校时钩子%s", hooks ? "已注册" : "已清除");
}

const w_time_hooks_t *w_platform_time(void)
{
    return s_time;
}

void w_platform_set_screen_hooks(const w_screen_hooks_t *hooks)
{
    s_screen = hooks;
    ESP_LOGI(TAG, "屏幕钩子%s", hooks ? "已注册" : "已清除");
}

const w_screen_hooks_t *w_platform_screen(void)
{
    return s_screen;
}

void w_platform_set_buzzer_hooks(const w_buzzer_hooks_t *hooks)
{
    s_buzzer = hooks;
    ESP_LOGI(TAG, "蜂鸣器钩子%s", hooks ? "已注册" : "已清除");
}

const w_buzzer_hooks_t *w_platform_buzzer(void)
{
    return s_buzzer;
}

/* ==================== 便捷封装 ==================== */

/*
 * 注意每个判空分支的**默认值选择**：
 * 没接钩子时（PC 模拟器），界面不该因为"钩子为空"而显示成
 * "屏幕黑着"或"功能坏了"，所以默认返回"一切正常"。
 * 模拟器上你看到的就是正常界面，只是操作不落盘/不真连网。
 */
bool w_screen_is_on(void)
{
    return (s_screen && s_screen->is_on) ? s_screen->is_on() : true;
}

void w_screen_wake(void)
{
    if (s_screen && s_screen->wake) {
        s_screen->wake();
    }
}

void w_screen_hold_awake(bool hold)
{
    if (s_screen && s_screen->hold_awake) {
        s_screen->hold_awake(hold);
    }
}

void w_screen_notify_activity(void)
{
    if (s_screen && s_screen->notify_activity) {
        s_screen->notify_activity();
    }
}

void w_buzzer_tone(uint32_t freq_hz)
{
    if (s_buzzer && s_buzzer->tone) {
        s_buzzer->tone(freq_hz);
    }
}

void w_buzzer_stop(void)
{
    if (s_buzzer && s_buzzer->stop) {
        s_buzzer->stop();
    }
}
