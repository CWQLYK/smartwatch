/**
 * @file weather_service.c
 * @brief 天气服务实现 —— 见 weather_service.h 的数据源与取舍说明
 *
 * ## 线程模型
 *
 *     weather_task（栈 6144，固定 CPU0，优先级 1）
 *         └── esp_http_client_perform()  ← 阻塞，所以不能在 LVGL 任务里跑
 *             └── cJSON 解析
 *                 └── 写 NVS 缓存 + 更新 s_status（加锁）
 *
 * 界面**从不**直接调这里的 HTTP；它只 `refresh()` 触发、靠 `get_status()` 轮询。
 * 和项目里"碰 lv_* 必须持锁、持锁时不做阻塞操作"是配套的。
 *
 * ## 缓存键名（自有命名，不复用参考工程的 day0_date 那套）
 *
 *     wx_d0_mmdd … wx_d6_mmdd      "MM/DD"
 *     wx_d0_code … wx_d6_code      WMO 代码
 *     wx_d0_max  … wx_d6_max       最高温
 *     wx_d0_min  … wx_d6_min       最低温
 *     wx_fetched                   抓取时刻（epoch，**参考工程没有这一项**）
 *
 * `wx_fetched` 是"过期判定"的关键：参考工程靠比较 "MM/DD" 字符串，
 * 跨月会算错（"03/01" < "02/28"）。我们改成"抓取日 ≠ 今天 → 过期"。
 */
#include "weather_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

static const char *TAG = "WEATHER";

#define WX_NVS_NS   "wx"

/* 响应缓冲：7 天 daily 大约 400~600 字节，给 2KB 很宽裕 */
#define WX_BUF_SZ   2048
/* HTTP 整体超时。参考工程想设 8s 总超时 + 5s 连接超时，
 * 但它的嫁接层把两者合成一个（实际 5s）—— 我们显式设一个。 */
#define WX_TIMEOUT_MS  8000

/* ==================== 平台注入点（前置声明）====================
 *
 * 定义在本文件末尾（weak）。必须在前面声明，
 * 否则 `local_ymd_of` 用到 `weather_tz_unit()` 时报
 * "implicit declaration"（本项目 -Werror=all，直接编不过）。*/
int32_t weather_tz_unit(void);
bool    weather_get_coords(int32_t *lat, int32_t *lon);

/* ==================== 内部状态 ==================== */
static weather_status_t  s_st;
static SemaphoreHandle_t s_lock  = NULL;
static TaskHandle_t      s_task  = NULL;
static volatile bool     s_busy  = false;    /* 防止重复触发 */

/* ==================== 小工具 ==================== */

static inline void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}
static inline void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/**
 * @brief 抓取时刻 → 本地"今天是哪天"（用于判过期）
 *
 * @note 和日历页一样自己算日历，不碰 `time.h` 的本地时区
 *       （那个在这个构建里跨平台不一致）。
 *       时区从 settings 读（15 分钟为单位）。
 */
static void local_ymd_of(uint32_t epoch, int *y, int *m, int *d)
{
    /* 时区：和 watch_page_timeset / watch_page_calendar 同一套算法。
     * weather_tz_unit() 是本文件里的 weak 默认实现，app 层会覆盖它。*/
    const int32_t tz_sec = weather_tz_unit() * 900;

    int64_t local = (int64_t)epoch + tz_sec;
    if (local < 0) {
        local = 0;
    }
    uint32_t days = (uint32_t)(local / 86400);

    int year = 1970;
    for (;;) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        const int diy = leap ? 366 : 365;
        if (days < (uint32_t)diy) {
            break;
        }
        days -= (uint32_t)diy;
        year++;
    }
    static const int mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int mon = 1;
    for (;;) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        int dim = mdays[mon - 1];
        if (mon == 2 && leap) {
            dim = 29;
        }
        if (days < (uint32_t)dim) {
            break;
        }
        days -= (uint32_t)dim;
        mon++;
    }
    *y = year;
    *m = mon;
    *d = (int)days + 1;
}

/* ==================== NVS 缓存 ==================== */

static void key_of(char *buf, size_t len, const char *prefix, int i)
{
    snprintf(buf, len, "wx_d%d_%s", i, prefix);
}

static void cache_save(void)
{
    nvs_handle_t h;
    if (nvs_open(WX_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    lock();
    const uint8_t cnt = s_st.count;
    for (int i = 0; i < WEATHER_DAYS; i++) {
        char k[16];
        if (i < cnt) {
            key_of(k, sizeof(k), "mmdd", i);
            nvs_set_str(h, k, s_st.days[i].date);
            key_of(k, sizeof(k), "code", i);
            nvs_set_i8(h, k, s_st.days[i].wmo);
            key_of(k, sizeof(k), "max", i);
            nvs_set_i8(h, k, s_st.days[i].tmax);
            key_of(k, sizeof(k), "min", i);
            nvs_set_i8(h, k, s_st.days[i].tmin);
        } else {
            /* 天数变少时把多余的键删掉，免得读到上一轮的残留 */
            key_of(k, sizeof(k), "mmdd", i);
            nvs_erase_key(h, k);
            key_of(k, sizeof(k), "code", i);
            nvs_erase_key(h, k);
            key_of(k, sizeof(k), "max", i);
            nvs_erase_key(h, k);
            key_of(k, sizeof(k), "min", i);
            nvs_erase_key(h, k);
        }
    }
    nvs_set_u32(h, "wx_fetched", s_st.fetched_epoch);
    nvs_set_u8(h, "wx_count", cnt);
    unlock();

    nvs_commit(h);
    nvs_close(h);
}

static void cache_load(void)
{
    nvs_handle_t h;
    if (nvs_open(WX_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    uint8_t cnt = 0;
    if (nvs_get_u8(h, "wx_count", &cnt) != ESP_OK || cnt == 0 ||
        cnt > WEATHER_DAYS) {
        nvs_close(h);
        return;
    }

    weather_status_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.count = cnt;

    for (int i = 0; i < cnt; i++) {
        char k[16];
        size_t n = sizeof(tmp.days[i].date);
        key_of(k, sizeof(k), "mmdd", i);
        if (nvs_get_str(h, k, tmp.days[i].date, &n) != ESP_OK) {
            nvs_close(h);
            return;
        }
        key_of(k, sizeof(k), "code", i);
        nvs_get_i8(h, k, &tmp.days[i].wmo);
        key_of(k, sizeof(k), "max", i);
        nvs_get_i8(h, k, &tmp.days[i].tmax);
        key_of(k, sizeof(k), "min", i);
        nvs_get_i8(h, k, &tmp.days[i].tmin);
    }
    nvs_get_u32(h, "wx_fetched", &tmp.fetched_epoch);
    nvs_close(h);

    tmp.valid = true;

    lock();
    s_st.valid         = true;
    s_st.count         = tmp.count;
    s_st.fetched_epoch = tmp.fetched_epoch;
    memcpy(s_st.days, tmp.days, sizeof(tmp.days));
    unlock();

    ESP_LOGI(TAG, "已从缓存装入 %d 天预报（抓取于 epoch %lu）",
             (int)cnt, (unsigned long)tmp.fetched_epoch);
}

void weather_service_clear_cache(void)
{
    nvs_handle_t h;
    if (nvs_open(WX_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);

    lock();
    memset(&s_st, 0, sizeof(s_st));
    unlock();
    ESP_LOGI(TAG, "天气缓存已清空");
}

/* ==================== WMO 代码映射（界面用）==================== */

/**
 * @brief WMO 天气代码 → 图标类别
 *
 * @note 分类和参考工程一致（晴/多云/阴/雨/雪/雷/雾），
 *       但**我们不引它的位图** —— 界面层用图元按类别自绘。
 */
int weather_wmo_class(int wmo)
{
    if (wmo == 0 || wmo == 1)                       return 0;  /* 晴 */
    if (wmo == 2)                                   return 1;  /* 多云 */
    if (wmo == 3)                                   return 2;  /* 阴 */
    if (wmo == 45 || wmo == 48)                     return 3;  /* 雾 */
    if (wmo >= 51 && wmo <= 67)                     return 4;  /* 毛毛雨/雨 */
    if (wmo >= 71 && wmo <= 77)                     return 5;  /* 雪 */
    if (wmo >= 80 && wmo <= 82)                     return 4;  /* 阵雨 */
    if (wmo >= 85 && wmo <= 86)                     return 5;  /* 阵雪 */
    if (wmo >= 95)                                  return 6;  /* 雷暴 */
    return 1;
}

/* ==================== HTTP + 解析 ==================== */

/**
 * @brief 把 "YYYY-MM-DD" 压成 "MM/DD"
 *
 * @note 缓冲正好 6 字节（5 字符 + '\0'）—— 参考工程那个
 *       `char dateMMDD[6]` 就是这么干的，改成含年份会溢出。
 */
static void date_to_mmdd(const char *src, char *out6)
{
    out6[0] = '\0';
    if (!src || strlen(src) < 10) {
        return;
    }
    /* "2026-10-09" → 下标 5,6 = 月；8,9 = 日 */
    out6[0] = src[5];
    out6[1] = src[6];
    out6[2] = '/';
    out6[3] = src[8];
    out6[4] = src[9];
    out6[5] = '\0';
}

static void parse_and_store(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, (size_t)len);
    if (!root) {
        ESP_LOGW(TAG, "JSON 解析失败（%d 字节）", len);
        return;
    }

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    cJSON *jt    = daily ? cJSON_GetObjectItem(daily, "time") : NULL;
    cJSON *jc    = daily ? cJSON_GetObjectItem(daily, "weathercode") : NULL;
    cJSON *jx    = daily ? cJSON_GetObjectItem(daily, "temperature_2m_max") : NULL;
    cJSON *jn    = daily ? cJSON_GetObjectItem(daily, "temperature_2m_min") : NULL;

    if (!cJSON_IsArray(jt) || !cJSON_IsArray(jc) ||
        !cJSON_IsArray(jx) || !cJSON_IsArray(jn)) {
        ESP_LOGW(TAG, "daily 字段缺失或不是数组（接口变了？）");
        cJSON_Delete(root);
        return;
    }

    /*
     * ★ 长度必须**四个数组一致**才采信。
     *   参考工程的坑：它按 time 的长度循环，缺的字段取到 0，
     *   而 0 在 WMO 里是"晴" —— 于是缺数据的日子**静默显示成大晴天**。
     */
    const int n = cJSON_GetArraySize(jt);
    if (cJSON_GetArraySize(jc) != n || cJSON_GetArraySize(jx) != n ||
        cJSON_GetArraySize(jn) != n) {
        ESP_LOGW(TAG, "daily 四个数组长度不一致 -> 丢弃本次结果（不写缓存）");
        cJSON_Delete(root);
        return;
    }

    weather_status_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    int cnt = (n < WEATHER_DAYS) ? n : WEATHER_DAYS;
    if (cnt <= 0) {
        ESP_LOGW(TAG, "daily 为空");
        cJSON_Delete(root);
        return;
    }

    for (int i = 0; i < cnt; i++) {
        cJSON *et = cJSON_GetArrayItem(jt, i);
        cJSON *ec = cJSON_GetArrayItem(jc, i);
        cJSON *ex = cJSON_GetArrayItem(jx, i);
        cJSON *en = cJSON_GetArrayItem(jn, i);

        if (!cJSON_IsString(et) || !cJSON_IsNumber(ec) ||
            !cJSON_IsNumber(ex) || !cJSON_IsNumber(en)) {
            ESP_LOGW(TAG, "第 %d 天字段类型不对 -> 丢弃本次结果", i);
            cJSON_Delete(root);
            return;
        }
        date_to_mmdd(et->valuestring, tmp.days[i].date);
        if (!tmp.days[i].date[0]) {
            ESP_LOGW(TAG, "第 %d 天日期格式不对 -> 丢弃本次结果", i);
            cJSON_Delete(root);
            return;
        }
        tmp.days[i].wmo  = (int8_t)ec->valueint;
        /* 温度存 int8：-128..127，地球上够用 */
        tmp.days[i].tmax = (int8_t)(ex->valuedouble + (ex->valuedouble >= 0 ? 0.5 : -0.5));
        tmp.days[i].tmin = (int8_t)(en->valuedouble + (en->valuedouble >= 0 ? 0.5 : -0.5));
    }
    cJSON_Delete(root);

    tmp.count         = (uint8_t)cnt;
    tmp.valid         = true;
    tmp.fetched_epoch = (uint32_t)(esp_timer_get_time() / 1000000);

    lock();
    memcpy(s_st.days, tmp.days, sizeof(tmp.days));
    s_st.count         = tmp.count;
    s_st.valid         = true;
    s_st.fetched_epoch = tmp.fetched_epoch;
    s_st.failed        = false;
    unlock();

    cache_save();
    ESP_LOGI(TAG, "天气已更新：%d 天，第一天 %s %d~%d℃",
             cnt, tmp.days[0].date, tmp.days[0].tmin, tmp.days[0].tmax);
}

/** @brief HTTP 事件：把响应体收进缓冲（够了就截断，不追求完整）*/
static esp_err_t http_evt_cb(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        char *buf = (char *)e->user_data;
        if (buf && e->data_len > 0) {
            const size_t used = strlen(buf);
            if (used + (size_t)e->data_len < WX_BUF_SZ) {
                memcpy(buf + used, e->data, (size_t)e->data_len);
                buf[used + (size_t)e->data_len] = '\0';
            }
        }
    }
    return ESP_OK;
}

/* ==================== 平台注入点（默认实现，app 层会覆盖）====================
 *
 * ## 为什么用 weak 符号
 *
 * `weather` 组件不能依赖 `app`（app 依赖它，会成环），
 * 但"时区/经纬度存哪"又是 app 层才知道的事。
 * 所以这里给**弱默认实现**（上海），app 层的 `weather_port.c`
 * 提供同名强符号并在链接时覆盖。
 *
 * 这样两头都自洽：单独编 weather 组件能过；整机链接时用 app 的版本。
 * （不这么做会报 `undefined reference to weather_get_coords` —— 踩过。）
 */
__attribute__((weak)) int32_t weather_tz_unit(void)
{
    return 32;                      /* +8 区（15 分钟为单位）*/
}

__attribute__((weak)) bool weather_get_coords(int32_t *lat, int32_t *lon)
{
    if (lat) *lat = WEATHER_LAT_DEFAULT;
    if (lon) *lon = WEATHER_LON_DEFAULT;
    return true;
}

/** @brief 抓一次（在这个任务里阻塞执行）*/
static bool fetch_once(void)
{
    int32_t lat = WEATHER_LAT_DEFAULT;
    int32_t lon = WEATHER_LON_DEFAULT;
    weather_get_coords(&lat, &lon);

    char url[192];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast"
             "?latitude=%.5f&longitude=%.5f"
             "&daily=weathercode,temperature_2m_max,temperature_2m_min"
             "&timezone=Asia%%2FShanghai&forecast_days=%d",
             (double)lat / 100000.0, (double)lon / 100000.0, WEATHER_DAYS);

    char *buf = calloc(1, WX_BUF_SZ);
    if (!buf) {
        ESP_LOGW(TAG, "响应缓冲分配失败");
        return false;
    }

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_GET,
        .timeout_ms     = WX_TIMEOUT_MS,
        .event_handler  = http_evt_cb,
        .user_data      = buf,
        .buffer_size    = 1024,
    };

    ESP_LOGI(TAG, "请求天气：%s", url);

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(buf);
        ESP_LOGW(TAG, "http client 初始化失败");
        return false;
    }
    /*
     * 显式 UA：参考工程没设，出了问题不好排查。
     *
     * ★ 注意：这也是**版本号**的一处（服务器端日志里能看到是哪版固件在抓）。
     *   v0.7.1 才发现它一直没跟着版本走（项目文档原先只列了三处，
     *   而天气服务是 v0.7.0 才加的，所以这个第 4 处一直没被踩到）。
     *   现已补进 `doc/version/README.md` 的"怎么加一个新版本"清单。
     */
    esp_http_client_set_header(cli, "User-Agent", "SmartWatch/0.7.3");

    const esp_err_t err = esp_http_client_perform(cli);
    const int code = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    bool ok = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP 失败：%s（WiFi 没连上？）", esp_err_to_name(err));
    } else if (code != 200) {
        ESP_LOGW(TAG, "HTTP 状态码 %d（不是 200）", code);
    } else {
        parse_and_store(buf, (int)strlen(buf));
        lock();
        ok = s_st.failed == false;
        unlock();
    }

    free(buf);
    return ok;
}

static void weather_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "天气任务启动（栈 %u）", (unsigned)uxTaskGetStackHighWaterMark(NULL));

    const bool ok = fetch_once();

    lock();
    s_st.fetching = false;
    if (!ok) {
        s_st.failed = true;
    }
    unlock();

    if (!ok) {
        ESP_LOGW(TAG, "本次抓取失败（缓存保留不动）");
    }

    s_busy = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ==================== 对外 ==================== */

esp_err_t weather_service_init(void)
{
    if (s_lock) {
        return ESP_OK;              /* 幂等 */
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_st, 0, sizeof(s_st));

    /*
     * 开机**不联网**，先把缓存渲染出来（和参考工程一致）。
     * 联网是用户点"获取天气"（或以后做自动刷新）才发生。
     */
    cache_load();

    ESP_LOGI(TAG, "天气服务就绪（缓存 %s）", s_st.valid ? "有" : "无");
    return ESP_OK;
}

int weather_service_refresh(void)
{
    if (!s_lock) {
        return -1;
    }
    if (s_busy) {
        return -1;                  /* 已经在抓了 */
    }
    s_busy = true;

    lock();
    s_st.fetching = true;
    s_st.failed   = false;
    unlock();

    /*
     * 固定 CPU0、优先级 1：和 WiFi 协议栈同核，别去挤 LVGL 那个核。
     * 栈 6144 —— HTTP + cJSON 的调用链比较深，按项目经验给足。
     */
    const BaseType_t r = xTaskCreatePinnedToCore(weather_task, "weather", 6144,
                                                 NULL, 1, &s_task, 0);
    if (r != pdPASS) {
        ESP_LOGW(TAG, "任务创建失败");
        s_busy = false;
        lock();
        s_st.fetching = false;
        s_st.failed   = true;
        unlock();
        return -1;
    }
    return 0;
}

void weather_service_get_status(weather_status_t *out)
{
    if (!out) {
        return;
    }
    lock();
    memcpy(out, &s_st, sizeof(*out));
    unlock();
}

/* ==================== 经纬度的格式化 / 解析 ==================== */

void weather_format_udeg(int32_t udeg, char *buf, int len)
{
    if (!buf || len <= 0) {
        return;
    }
    const bool neg = (udeg < 0);
    const int32_t a = neg ? -udeg : udeg;
    snprintf(buf, (size_t)len, "%s%ld.%05ld",
             neg ? "-" : "",
             (long)(a / 100000),
             (long)(a % 100000));
}

int32_t weather_parse_udeg(const char *text)
{
    if (!text || !text[0]) {
        return INT32_MIN;
    }
    const char *p = text;
    bool neg = false;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }

    /* 整数部分 */
    int64_t whole = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        p++;
        digits++;
        if (whole > 1000000) {
            return INT32_MIN;       /* 明显越界 */
        }
    }
    if (digits == 0) {
        return INT32_MIN;
    }

    /* 小数部分：最多取 5 位（微度精度）*/
    int64_t frac = 0;
    int fdigits = 0;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (fdigits < 5) {
                frac = frac * 10 + (*p - '0');
                fdigits++;
            }
            p++;
        }
    }
    if (*p != '\0') {
        return INT32_MIN;           /* 有非法字符 */
    }
    /* 小数位补齐到 5 位 */
    while (fdigits < 5) {
        frac *= 10;
        fdigits++;
    }

    int64_t v = whole * 100000 + frac;
    if (neg) {
        v = -v;
    }
    if (v < WEATHER_LAT_MIN || v > WEATHER_LON_MAX) {
        return INT32_MIN;           /* 粗筛；具体范围由调用方按纬度/经度判断 */
    }
    return (int32_t)v;
}
