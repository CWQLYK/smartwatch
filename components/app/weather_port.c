/**
 * @file weather_port.c
 * @brief 天气服务的"平台注入点"—— 时区与经纬度都来自 settings
 *
 * ## 为什么单独一个文件
 *
 * `weather` 组件不该知道"设置项叫什么、存在哪" —— 那是 app 层的编排职责。
 * 它只 `extern` 声明两个函数，由**本文件提供实现**：
 *
 *     weather_tz_unit()              → settings 的 SETTINGS_TIMEZONE
 *     weather_get_coords(&lat,&lon)  → settings 的经纬度（整数微度）
 *
 * 好处：以后把天气搬到别的工程、或者把设置换成另一种存储，只改这一个文件。
 *
 * ## ★ 链接方式：weak 符号（踩过 undefined reference）
 *
 * `weather` 组件**不能**依赖 `app`（app 本来就依赖它，会成环）。
 * 所以 `weather_service.c` 里给这两个函数各留了一个 **weak 默认实现**
 * （返回上海的默认坐标），本文件的**强符号**在链接时自动覆盖它们。
 *
 * 第一版没加 weak，直接报 `undefined reference to weather_get_coords` ——
 * 因为链接器在 `libweather.a` 里找不到符号，而 app 是最后才链进去的。
 * 加了 weak 之后两边都能自洽：单编 weather 组件也能过，整机则用 app 的版本。
 *
 * ## 经纬度为什么是整数微度
 *
 * 界面层的设置钩子签名是 `int`（存不了小数），而且 NVS 里整数
 * 没有 float 的精度/类型问题。单位 1e-5 度，精度约 1.1 米，够用。
 */
#include <stdbool.h>
#include <stdint.h>

#include "settings.h"
#include "weather_service.h"
#include "esp_log.h"

static const char *TAG = "WXPORT";

/** @brief 时区（15 分钟为单位，+8 区 = 32）*/
int32_t weather_tz_unit(void)
{
    int32_t tz = 32;
    if (settings_get(SETTINGS_TIMEZONE, &tz) != ESP_OK) {
        tz = 32;
    }
    return tz;
}

/**
 * @brief 取经纬度（整数微度）
 *
 * @note 没设置过就用默认值（上海）。设成非法值时也退回默认 ——
 *       宁可查一个"错的城市"，也不要拿 0,0（几内亚湾）去请求。
 */
bool weather_get_coords(int32_t *lat, int32_t *lon)
{
    int32_t la = WEATHER_LAT_DEFAULT;
    int32_t lo = WEATHER_LON_DEFAULT;

    if (settings_get(SETTINGS_WX_LAT, &la) != ESP_OK ||
        la < WEATHER_LAT_MIN || la > WEATHER_LAT_MAX) {
        la = WEATHER_LAT_DEFAULT;
    }
    if (settings_get(SETTINGS_WX_LON, &lo) != ESP_OK ||
        lo < WEATHER_LON_MIN || lo > WEATHER_LON_MAX) {
        lo = WEATHER_LON_DEFAULT;
    }

    if (lat) *lat = la;
    if (lon) *lon = lo;
    return true;
}

/** @brief 存经纬度（设置页用；越界返回 false）*/
bool weather_set_coords(int32_t lat, int32_t lon)
{
    if (lat < WEATHER_LAT_MIN || lat > WEATHER_LAT_MAX ||
        lon < WEATHER_LON_MIN || lon > WEATHER_LON_MAX) {
        ESP_LOGW(TAG, "经纬度越界：%ld, %ld", (long)lat, (long)lon);
        return false;
    }
    settings_set(SETTINGS_WX_LAT, lat);
    settings_set(SETTINGS_WX_LON, lon);
    ESP_LOGI(TAG, "经纬度已保存：%ld, %ld（微度）", (long)lat, (long)lon);
    return true;
}
