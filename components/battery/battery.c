/**
 * @file battery.c
 * @brief 电池电压采样实现 —— ADC oneshot + 曲线拟合校准 + 中值滤波
 *
 * 为什么要中值滤波而不是平均：
 *   这一路 ADC 分压电阻是 200K/100K，**源阻抗很高**（约 66K），
 *   ESP32-S3 的 ADC 采样保持电容充不满就会偏低，偶尔还会蹦出一个离群点。
 *   取中值能直接干掉离群点；取平均反而会被离群点带偏。
 */
#include "battery.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "board.h"

static const char *TAG = "BATTERY";

/* ==================== 内部状态 ==================== */
static adc_oneshot_unit_handle_t s_adc      = NULL;
static adc_cali_handle_t         s_cali     = NULL;
static bool                      s_ready    = false;
static bool                      s_cali_ok  = false;

/* 充电趋势判定：记最近几次滤波后的电压 */
#define BAT_TREND_N     4
static int  s_trend[BAT_TREND_N];
static int  s_trend_cnt = 0;
static int  s_trend_idx = 0;

/* 采样参数 */
#define BAT_SAMPLES     11      /* 取 11 个点的中值（奇数，好取中） */

/* ==================== 工具 ==================== */
static void trend_push(int mv)
{
    s_trend[s_trend_idx] = mv;
    s_trend_idx = (s_trend_idx + 1) % BAT_TREND_N;
    if (s_trend_cnt < BAT_TREND_N) {
        s_trend_cnt++;
    }
}

/** @brief 插入排序求中值（11 个元素，开销可以忽略） */
static int median_of(int *v, int n)
{
    for (int i = 1; i < n; i++) {
        int key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
    return v[n / 2];
}

/* ==================== 初始化 ==================== */
esp_err_t battery_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = BOARD_BAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG,
                        "ADC 单元初始化失败");

    /*
     * 12dB 衰减：量程约 0~3.1V。
     * 分压后电池最高 4.2/3 = 1.4V，落在量程中段，线性度最好。
     */
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BOARD_BAT_ADC_CHANNEL, &chan_cfg),
                        TAG, "ADC 通道配置失败");

    /* 校准：ESP32-S3 支持曲线拟合（用芯片出厂校准参数） */
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BOARD_BAT_ADC_UNIT,
        .chan     = BOARD_BAT_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
    if (err == ESP_OK) {
        s_cali_ok = true;
    } else {
        /* 校准失败不让初始化失败：退化成"原始码值线性换算"，精度差但能看 */
        s_cali    = NULL;
        s_cali_ok = false;
        ESP_LOGW(TAG, "ADC 校准创建失败(%s)，将退回未校准模式（读数会偏低）",
                 esp_err_to_name(err));
    }

    memset(s_trend, 0, sizeof(s_trend));
    s_trend_cnt = 0;
    s_trend_idx = 0;
    s_ready     = true;

    ESP_LOGI(TAG, "电池采样就绪（GPIO%d，分压比 %d/%d，校准=%s）",
             BOARD_BAT_GPIO_ADC, BOARD_BAT_DIVIDER_NUM, BOARD_BAT_DIVIDER_DEN,
             s_cali_ok ? "曲线拟合" : "未校准");
    return ESP_OK;
}

/* ==================== 采样 ==================== */
esp_err_t battery_read(battery_reading_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out 为空");
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "电池采样未初始化");

    int mv_samples[BAT_SAMPLES];
    int valid = 0;

    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BOARD_BAT_ADC_CHANNEL, &raw) != ESP_OK) {
            continue;
        }

        int mv_pin;
        if (s_cali_ok) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv_pin) != ESP_OK) {
                continue;
            }
        } else {
            /* 未校准兜底：3.1V 量程 / 4095 码 线性估算 */
            mv_pin = raw * 3100 / 4095;
        }
        mv_samples[valid++] = mv_pin;
    }

    ESP_RETURN_ON_FALSE(valid >= 3, ESP_FAIL, TAG,
                        "ADC 采样失败（只有 %d/%d 次成功）", valid, BAT_SAMPLES);

    const int mv_pin_median = median_of(mv_samples, valid);

    /* 分压还原：电池 = 引脚电压 × (R3+R7)/R7 */
    int mv_bat = mv_pin_median * BOARD_BAT_DIVIDER_NUM / BOARD_BAT_DIVIDER_DEN;

    trend_push(mv_bat);

    out->millivolt = mv_bat;
    out->present   = (mv_bat >= BAT_PRESENT_MV);
    out->percent   = out->present ? battery_voltage_to_percent(mv_bat) : -1;
    out->charging  = battery_is_charging();

    return ESP_OK;
}

int battery_get_millivolt(void)
{
    battery_reading_t r;
    if (battery_read(&r) != ESP_OK) {
        return -1;
    }
    return r.millivolt;
}

/* ==================== 电压 → 电量 ==================== */
int battery_voltage_to_percent(int millivolt)
{
    if (millivolt < BAT_PRESENT_MV) {
        return -1;                      /* 没接电池 */
    }
    if (millivolt <= BAT_EMPTY_MV) {
        return 0;
    }
    if (millivolt >= BAT_FULL_MV) {
        return 100;
    }

    /*
     * 锂电池 OCV 曲线不是直线：3.7~3.9V 这一段特别平，掉 0.2V 就掉一半电。
     * 所以用分段线性插值（数据是手机厂常用的一张经验表）。
     */
    static const struct { int mv; int pct; } curve[] = {
        { 3000,   0 },
        { 3300,   6 },
        { 3500,  11 },
        { 3600,  16 },
        { 3700,  31 },
        { 3800,  50 },
        { 3900,  70 },
        { 4000,  84 },
        { 4100,  94 },
        { 4200, 100 },
    };
    const int n = (int)(sizeof(curve) / sizeof(curve[0]));

    for (int i = 1; i < n; i++) {
        if (millivolt <= curve[i].mv) {
            const int dv = curve[i].mv - curve[i - 1].mv;
            const int dp = curve[i].pct - curve[i - 1].pct;
            const int offset = millivolt - curve[i - 1].mv;
            return curve[i - 1].pct + (offset * dp) / dv;
        }
    }
    return 100;
}

/* ==================== 充电判定 ==================== */
bool battery_is_charging(void)
{
    /*
     * ETA6098 没引出 STAT 引脚，只能推断：
     *   电压已经到 4.15V 以上，且最近几次采样整体在往上走。
     * 这是个启发式判断，虚警/漏报都可能，UI 上只用它决定要不要显示闪电图标。
     */
    if (s_trend_cnt < BAT_TREND_N) {
        return false;
    }

    const int newest = s_trend[(s_trend_idx + BAT_TREND_N - 1) % BAT_TREND_N];
    if (newest < BAT_CHARGING_MV) {
        return false;
    }

    /* 头尾比较：整体趋势向上 8mV 以上就认为在充 */
    const int oldest = s_trend[s_trend_idx];
    return (newest - oldest) >= 8;
}

bool battery_cali_ok(void)
{
    return s_cali_ok;
}
