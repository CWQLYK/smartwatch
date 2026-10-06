/**
 * @file rtc.c
 * @brief PCF85063ATL 驱动实现
 *
 * 数据流：BCD 寄存器  ⇄  rtc_pcf_time_t  ⇄  Unix 时间戳
 *
 * 三个关键点：
 *   1. **秒寄存器 bit7 是 OSF（停振标志）**，读时间时必须掩掉，
 *      否则秒数会读出 40-79 这种非法值。同时它也是我们判断"掉过电"的依据。
 *   2. 写时间**不要无条件清 OSF**：只有当上层确实给了新时间才清。
 *   3. 星期用 Sakamoto 算法自己算，不信芯片里那一位 ——
 *      因为串口/BLE 校时通常只给年月日时分秒。
 */
#include "rtc_pcf85063.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "board.h"

static const char *TAG = "RTC";

/* ==================== 内部状态 ==================== */
static i2c_master_dev_handle_t s_dev       = NULL;
static bool                    s_ready     = false;
static bool                    s_time_lost = true;   /* 保守：默认认为不可信 */

/* 2000-01-01 00:00:00 UTC 对应的 Unix 时间戳 */
#define RTC_EPOCH_2000  946684800UL

/* ==================== BCD 转换 ==================== */
static inline uint8_t bcd2bin(uint8_t bcd) { return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F)); }
static inline uint8_t bin2bcd(uint8_t bin) { return (uint8_t)(((bin / 10) << 4) | (bin % 10)); }

/* ==================== 日期算法 ==================== */

/**
 * @brief 天数 → 年月日（Howard Hinnant 的 civil_from_days，无分支、无查表）
 * @param days 自 1970-01-01 起的天数
 */
static void civil_from_days(int32_t days, int *y, int *m, int *d)
{
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);                    /* [0, 146096] */
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t yy = (int32_t)yoe + (int32_t)era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);         /* [0, 365]     */
    uint32_t mp = (5 * doy + 2) / 153;                              /* [0, 11]      */
    uint32_t dd = doy - (153 * mp + 2) / 5 + 1;                     /* [1, 31]      */
    uint32_t mm = mp + (mp < 10 ? 3 : -9);                          /* [1, 12]      */

    *y = (int)(yy + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}

/**
 * @brief 年月日 → 天数（civil_to_days），与上面互为逆运算
 */
static int32_t days_from_civil(int y, int m, int d)
{
    int32_t yy = y - (m <= 2);
    int32_t era = (yy >= 0 ? yy : yy - 399) / 400;
    uint32_t yoe = (uint32_t)(yy - era * 400);                      /* [0, 399]     */
    uint32_t doy = (uint32_t)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* [0, 146096]  */
    return era * 146097 + (int32_t)doe - 719468;
}

int rtc_pcf_weekday_from_date(int year, int month, int day)
{
    /* Sakamoto：只要年月日就能算星期，不依赖芯片里的 weekday 位 */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = year;
    if (month < 3) {
        y -= 1;
    }
    /* 0=周日 … 6=周六 → 转成 ISO 的 1=周一 … 7=周日 */
    int iso = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
    return (iso == 0) ? 7 : iso;
}

bool rtc_pcf_time_is_valid(const rtc_pcf_time_t *t)
{
    if (!t) return false;
    return (t->year >= 2000 && t->year <= 2099) &&
           (t->month >= 1 && t->month <= 12) &&
           (t->day   >= 1 && t->day   <= 31) &&
           (t->hour  >= 0 && t->hour  <= 23) &&
           (t->minute >= 0 && t->minute <= 59) &&
           (t->second >= 0 && t->second <= 59);
}

/* ==================== 寄存器访问 ==================== */
static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return board_i2c_read_reg(s_dev, reg, buf, len);
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    return board_i2c_write_reg(s_dev, reg, buf, len);
}

static esp_err_t reg_write_u8(uint8_t reg, uint8_t val)
{
    return reg_write(reg, &val, 1);
}

/* ==================== 初始化 ==================== */
esp_err_t rtc_pcf_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(board_init(), TAG, "板级资源初始化失败");
    ESP_RETURN_ON_ERROR(board_i2c_add_device(BOARD_RTC_I2C_ADDR, BOARD_I2C_FREQ_HZ, &s_dev),
                        TAG, "挂载 RTC(0x%02X) 到共享 I2C 失败", BOARD_RTC_I2C_ADDR);

    /* 先确认芯片在不在 —— 不在就让上层降级（而不是后面读到一堆 0xFF）*/
    esp_err_t err = board_i2c_probe(BOARD_RTC_I2C_ADDR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC(0x%02X) 无应答，请检查 I2C 接线 / 供电", BOARD_RTC_I2C_ADDR);
        return err;
    }

    /*
     * CTRL1 = 0x00：清 STOP(bit5) 让时钟走，24 小时制(bit1=0)，
     *                 内部电容 7pF/12.5pF 用默认值(bit0=0)。
     */
    ESP_RETURN_ON_ERROR(reg_write_u8(RTC_REG_CTRL1, 0x00), TAG, "写 CTRL1 失败");

    /* 读 RAM 魔数 + 秒寄存器，判断这块表历史上有没有被校时过 */
    uint8_t ram = 0;
    bool ram_ok = (reg_read(RTC_REG_RAM, &ram, 1) == ESP_OK) && (ram == RTC_RAM_MAGIC);

    uint8_t sec = 0;
    bool osf = true;
    if (reg_read(RTC_REG_SECONDS, &sec, 1) == ESP_OK) {
        osf = (sec & RTC_SEC_OSF) != 0;
    }

    s_time_lost = osf || !ram_ok;
    s_ready     = true;

    ESP_LOGI(TAG, "PCF85063 就绪 @0x%02X（%s，OSF=%d，RAM魔数=%s）",
             BOARD_RTC_I2C_ADDR, s_time_lost ? "时间不可信，需要校时" : "走时正常",
             (int)osf, ram_ok ? "对" : "不对");

    return ESP_OK;
}

/* ==================== 读时间 ==================== */
esp_err_t rtc_pcf_get_time(rtc_pcf_time_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out 为空");
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "RTC 未初始化");

    /* 0x04~0x0A 一次读 7 个字节，避免跨秒读到"半新半旧"的时间 */
    uint8_t r[7];
    ESP_RETURN_ON_ERROR(reg_read(RTC_REG_SECONDS, r, sizeof(r)), TAG, "读时间失败");

    s_time_lost = (r[0] & RTC_SEC_OSF) != 0;

    out->second  = bcd2bin(r[0] & 0x7F);        /* bit7 是 OSF，必须掩掉 */
    out->minute  = bcd2bin(r[1] & 0x7F);
    out->hour    = bcd2bin(r[2] & 0x3F);        /* 24 小时制：bit6 保留 */
    out->day     = bcd2bin(r[3] & 0x3F);
    out->weekday = bcd2bin(r[4] & 0x07);
    out->month   = bcd2bin(r[5] & 0x1F);        /* bit7 是世纪位 */
    out->year    = 2000 + bcd2bin(r[6]);

    /* 星期以我们算的为准（芯片那一位可能是别人乱写的）*/
    if (out->month >= 1 && out->month <= 12 && out->day >= 1 && out->day <= 31) {
        out->weekday = rtc_pcf_weekday_from_date(out->year, out->month, out->day);
    }

    return ESP_OK;
}

/* ==================== 写时间 ==================== */
esp_err_t rtc_pcf_set_time(const rtc_pcf_time_t *in)
{
    ESP_RETURN_ON_FALSE(in, ESP_ERR_INVALID_ARG, TAG, "in 为空");
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "RTC 未初始化");
    ESP_RETURN_ON_FALSE(rtc_pcf_time_is_valid(in), ESP_ERR_INVALID_ARG, TAG,
                        "时间字段非法: %04d-%02d-%02d %02d:%02d:%02d",
                        in->year, in->month, in->day, in->hour, in->minute, in->second);

    /* 星期不让调用者操心，按日期现算 */
    const int wd = rtc_pcf_weekday_from_date(in->year, in->month, in->day);

    const uint8_t w[7] = {
        bin2bcd((uint8_t)in->second),           /* 写 0：顺带清掉 OSF 停振标志 */
        bin2bcd((uint8_t)in->minute),
        bin2bcd((uint8_t)in->hour),             /* 24 小时制，bit6=0 */
        bin2bcd((uint8_t)in->day),
        bin2bcd((uint8_t)wd),
        bin2bcd((uint8_t)in->month),            /* bit7=0：21 世纪 */
        bin2bcd((uint8_t)(in->year % 100)),
    };

    ESP_RETURN_ON_ERROR(reg_write(RTC_REG_SECONDS, w, sizeof(w)), TAG, "写时间失败");

    /* 打个"已校时"戳，下次上电就知道时间可信 */
    esp_err_t err = reg_write_u8(RTC_REG_RAM, RTC_RAM_MAGIC);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RAM 魔数写入失败(%s)，掉电后可能被判定为未校时",
                 esp_err_to_name(err));
    }

    s_time_lost = false;
    ESP_LOGI(TAG, "RTC 校时成功：%04d-%02d-%02d %02d:%02d:%02d 星期%d",
             in->year, in->month, in->day, in->hour, in->minute, in->second, wd);
    return ESP_OK;
}

/* ==================== Unix 时间戳互转 ==================== */
esp_err_t rtc_pcf_set_epoch(uint32_t epoch_sec)
{
    ESP_RETURN_ON_FALSE(epoch_sec >= RTC_EPOCH_2000, ESP_ERR_INVALID_ARG, TAG,
                        "时间戳 %" PRIu32 " 早于 2000-01-01，RTC 芯片存不下",
                        epoch_sec);

    int32_t days = (int32_t)((epoch_sec - RTC_EPOCH_2000) / 86400UL);
    uint32_t rem = (uint32_t)((epoch_sec - RTC_EPOCH_2000) % 86400UL);

    rtc_pcf_time_t t;
    civil_from_days(days, &t.year, &t.month, &t.day);
    t.hour   = (int)(rem / 3600);
    t.minute = (int)((rem % 3600) / 60);
    t.second = (int)(rem % 60);
    t.weekday = rtc_pcf_weekday_from_date(t.year, t.month, t.day);

    return rtc_pcf_set_time(&t);
}

uint32_t rtc_pcf_get_epoch(void)
{
    rtc_pcf_time_t t;
    if (rtc_pcf_get_time(&t) != ESP_OK) {
        return 0;
    }
    if (!rtc_pcf_time_is_valid(&t)) {
        return 0;
    }

    const int32_t days = days_from_civil(t.year, t.month, t.day);
    return (uint32_t)((int64_t)days * 86400LL +
                      t.hour * 3600 + t.minute * 60 + t.second + RTC_EPOCH_2000);
}

/* ==================== 状态查询 ==================== */
bool rtc_pcf_time_lost(void)
{
    return s_time_lost;
}

esp_err_t rtc_pcf_clear_lost(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "RTC 未初始化");

    /* 读-改-写：只清 OSF，不动秒值 */
    uint8_t sec = 0;
    ESP_RETURN_ON_ERROR(reg_read(RTC_REG_SECONDS, &sec, 1), TAG, "读秒寄存器失败");
    sec &= (uint8_t)~RTC_SEC_OSF;
    ESP_RETURN_ON_ERROR(reg_write_u8(RTC_REG_SECONDS, sec), TAG, "清 OSF 失败");

    s_time_lost = false;
    ESP_LOGI(TAG, "已清除 OSF 停振标志");
    return ESP_OK;
}

esp_err_t rtc_pcf_get_chip_temp(int *centi_celsius)
{
    ESP_RETURN_ON_FALSE(centi_celsius, ESP_ERR_INVALID_ARG, TAG, "out 为空");
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "RTC 未初始化");

    /*
     * PCF85063 的温度是 **9 位 2 的补码**：
     *   寄存器 0x0B = 整数部分（8 位，bit7 是符号）
     *   寄存器 0x0C 的 bit7:6 = 小数部分（0.25°C/LSB）
     *   整数值 = 整数 + 小数/4，范围 -64 … +63.75 °C
     *
     * ⚠️ 小数位也要跟着符号走，所以先把 9 位整体当有符号数取出来再拆。
     *    一开始只把整数部分当补码、小数当无符号，读出来是 -127.50°C，
     *    一眼就知道是符号处理错了。
     */
    uint8_t r[2];
    ESP_RETURN_ON_ERROR(reg_read(0x0B, r, sizeof(r)), TAG, "读温度失败");

    int16_t raw = (int16_t)(((uint16_t)r[0] << 8) | (((uint16_t)r[1]) & 0xC0));
    int16_t signed_val = (int16_t)(raw >> 6);       /* 9 位有符号，单位 0.25°C */
    const int centi = (int)signed_val * 25;

    /*
     * 实测坑（2026-10-06，板 v2.1）：
     *   这块板上的 PCF85063ATL 读 0x0B/0x0C 得到 0x80/0x04 → 解出 -127.5°C，
     *   落在物理上不可能的范围里。说明该封装/批次没有可用的温度通道，
     *   或者寄存器映射和手册不一致。
     *
     * 处理方式：**超出合理芯片温度范围就判定为"没有温度通道"**，
     * 返回 ESP_ERR_NOT_SUPPORTED，而不是把一个荒唐的数字交给上层。
     * 这样以后谁看到这个接口都不会被误导。
     */
    if (centi < RTC_PCF_TEMP_MIN_CENTI || centi > RTC_PCF_TEMP_MAX_CENTI) {
        const char *sign = (centi < 0) ? "-" : "";
        const int mag = (centi < 0) ? -centi : centi;
        ESP_LOGW(TAG,
                 "温度读数异常（0x0B=0x%02X 0x0C=0x%02X → %s%d.%02d°C），判定为无温度通道",
                 r[0], r[1], sign, mag / 100, mag % 100);
        return ESP_ERR_NOT_SUPPORTED;
    }

    *centi_celsius = centi;
    return ESP_OK;
}
