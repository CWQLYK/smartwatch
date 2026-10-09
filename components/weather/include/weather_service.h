/**
 * @file weather_service.h
 * @brief 天气服务（平台侧）—— 拉 Open-Meteo、解析、缓存到 NVS
 *
 * ## 为什么单独一个组件
 *
 * HTTP / JSON / NVS 全是 ESP-IDF 能力，而界面层（watch_ui_pages）
 * **不许 include ESP-IDF 头文件**（PC 模拟器要能编）。
 * 所以按项目既有分层：能力收在这里，界面只经 `watch_platform.h` 的钩子拿数据。
 *
 * ## 数据源与参数（照参考工程的实测参数，见对标文档）
 *
 *     http://api.open-meteo.com/v1/forecast
 *       ?latitude=%.5f&longitude=%.5f
 *       &daily=weathercode,temperature_2m_max,temperature_2m_min
 *       &timezone=Asia%%2FShanghai
 *       &forecast_days=7
 *
 * - **明文 http**：省掉证书包（参考工程就是这么用的）
 * - 显式 `forecast_days=7`：参考工程靠默认值，不保险
 * - 显式 User-Agent：便于排查（参考工程没设）
 *
 * ## 比参考工程好的四处（都是它明确的缺陷）
 *
 * | 它的做法 | 问题 | 我们的做法 |
 * |---|---|---|
 * | 缓存过期判定用 `"MM/DD"` **字符串比较** | **跨月出错**（3 月初会拿 2 月的旧数据） | 存 `fetched_epoch`，按"抓取日 ≠ 今天"判过期 |
 * | 图标是 10 张 100×100 未压缩 RGB565 = **200KB** | flash 占用巨大 | 界面层用**图元自绘**，不引任何位图 |
 * | `is_day` 所有调用点都传 1 | 两张夜间图标**从未被使用**（夜里显示太阳） | UI 用"云 + 太阳/月亮"组合 |
 * | 无"正在加载"以外的反馈 | 用户不知道为什么失败 | 状态里给 `fetching`/`failed` |
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 预报天数（和 URL 里的 forecast_days 一致）*/
#define WEATHER_DAYS   7

/** @brief 一天的预报 */
typedef struct {
    char    date[6];    /**< "MM/DD"（参考工程也是这个格式，照抄省得换算）*/
    int8_t  wmo;        /**< WMO 天气代码 */
    int8_t  tmax;       /**< 最高温 ℃ */
    int8_t  tmin;       /**< 最低温 ℃ */
} weather_day_t;

/** @brief 天气状态快照（给界面用）*/
typedef struct {
    bool          valid;            /**< 有没有可用的缓存 */
    uint8_t       count;            /**< 有效天数 1..7 */
    weather_day_t days[WEATHER_DAYS];
    uint32_t      fetched_epoch;    /**< 上次抓取时间（0 = 从未）*/
    bool          fetching;         /**< 正在抓取 → 界面转圈 */
    bool          failed;           /**< 上次抓取失败，把原因说出来 */
} weather_status_t;

/**
 * @brief 初始化（建任务、读缓存）
 *
 * @note 幂等。**不会**联网 —— 联网是 refresh() 的事。
 *       开机先渲染缓存（有的话），没缓存就显示"点一下获取"。
 */
esp_err_t weather_service_init(void);

/**
 * @brief 触发一次抓取（**非阻塞**，立刻返回）
 *
 * @return 0 = 已受理；非 0 = 已经在抓了 / 没初始化
 *
 * @note 真正的 HTTP + 解析跑在一个独立任务里（栈 6144，固定 CPU0）。
 *       完成后置 `fetching=false`，界面靠轮询看到结果。
 */
int weather_service_refresh(void);

/** @brief 取状态快照（内部加锁，任何任务都能调）*/
void weather_service_get_status(weather_status_t *out);

/** @brief 清掉缓存（调试/换城市用）*/
void weather_service_clear_cache(void);

/* ==================== 设置项（经纬度用整数微度）====================
 *
 * 为什么用整数微度而不是 float：
 *   1. 界面层的设置钩子签名是 `int`（watch_pages.h），存不了小数；
 *   2. NVS 里整数没有 float 的精度/类型问题。
 * 单位 1e-5 度：上海 = lat 3123000 / lon 12147000，精度约 1.1 米。
 * 默认值给上海。
 */
#define WEATHER_LAT_DEFAULT   3123000
#define WEATHER_LON_DEFAULT   12147000
#define WEATHER_LAT_MIN       (-9000000)
#define WEATHER_LAT_MAX       (9000000)
#define WEATHER_LON_MIN       (-18000000)
#define WEATHER_LON_MAX       (18000000)

/** @brief 把微度格式化成 "31.23000"（界面显示用） */
void weather_format_udeg(int32_t udeg, char *buf, int len);

/** @brief 解析 "31.23000" → 微度；失败返回 INT32_MIN */
int32_t weather_parse_udeg(const char *text);

#ifdef __cplusplus
}
#endif
