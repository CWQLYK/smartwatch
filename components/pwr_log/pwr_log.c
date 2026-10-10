/**
 * @file pwr_log.c
 * @brief 功耗日志实现 —— 见 pwr_log.h 的用途与取舍说明
 *
 * 线程模型：
 *   esp_timer（"pwr_log"，每 30 秒一次）→ `data_hub_snapshot()`（拿已采好的值）
 *                                        → 追加 16 字节 + `fflush`
 *
 *   `start/stop/dump/erase` 都在**串口命令行任务**里调用；
 *   `running/count` 被两处读（命令行 + 定时器回调），都是"读一个 bool/int"，
 *   最坏情况是命令行看到上一拍的瞬时值，不影响正确性。
 *
 * ⚠️ 不自己调 `battery_read()`：它内部有个**无锁的共享环形缓冲**，
 *    而它已经被 data_hub 任务和串口任务并发调用了（这是个已记录的隐患）。
 *    这里改成读 `data_hub` 的快照 —— 那里每 30 秒采一次、且加锁。
 */
#include "pwr_log.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "esp_system.h"      /* esp_reset_reason() —— 复位黑盒要用 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   /* uxTaskGetSystemState / xTaskCreatePinnedToCore —— 卡死哨兵要用 */
#include "data_hub.h"
#include "bsp.h"          /* g_bsp_lvgl_beat：界面任务心跳（卡死哨兵用） */

static const char *TAG = "PWRLOG";

/* 数据文件（和 imu_logger 共用 SPIFFS 分区 "storage"） */
#define PWR_LOG_PATH      "/spiffs/pwrlog.bin"
#define PWR_LOG_PARTITION "storage"

/* 打印抽样行时最多打多少行（dump all 不受此限） */
#define DUMP_MAX_ROWS     48

_Static_assert(sizeof(pwr_log_rec_t) == 16, "记录必须是定长 16 字节（落盘格式依赖它）");

static esp_timer_handle_t s_timer   = NULL;
static FILE              *s_fp      = NULL;
static bool               s_running = false;
static int                s_records = 0;    /* 文件里已有的条数（含本次会话写入的） */

/* ==================== 内部工具 ==================== */

/** @brief SPIFFS 是否已挂载（由 imu_logger 在开机时挂载） */
static bool fs_ready(void)
{
    size_t total = 0, used = 0;
    return esp_spiffs_info(PWR_LOG_PARTITION, &total, &used) == ESP_OK;
}

/** @brief 现有文件里有多少条记录（按文件大小推算；不完整尾部会被忽略） */
static int count_file(void)
{
    FILE *f = fopen(PWR_LOG_PATH, "rb");
    if (!f) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    const long sz = ftell(f);
    fclose(f);
    if (sz <= 0) {
        return 0;
    }
    return (int)(sz / (long)sizeof(pwr_log_rec_t));
}

/** @brief 采一条并落盘 */
static void sample_once(void)
{
    if (!s_running || !s_fp) {
        return;
    }

    device_state_t st;
    data_hub_snapshot(&st);

    pwr_log_rec_t r;
    memset(&r, 0, sizeof(r));
    r.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    if (st.time.valid) {
        r.year   = (uint16_t)st.time.year;
        r.month  = (uint8_t)st.time.month;
        r.day    = (uint8_t)st.time.day;
        r.hour   = (uint8_t)st.time.hour;
        r.minute = (uint8_t)st.time.minute;
        r.second = (uint8_t)st.time.second;
        r.flags |= PWR_LOG_F_TIME_VALID;
    }
    r.mv  = (uint16_t)(st.battery.millivolt > 0 ? st.battery.millivolt : 0);
    r.pct = (int8_t)st.battery.percent;
    if (st.battery.present) {
        r.flags |= PWR_LOG_F_PRESENT;
    }
    if (st.battery.charging) {
        r.flags |= PWR_LOG_F_CHARGING;
    }

    if (fwrite(&r, sizeof(r), 1, s_fp) != 1) {
        ESP_LOGE(TAG, "写入失败（Flash 满了？），停止记录");
        pwr_log_stop();
        return;
    }
    /*
     * ★ 每条都刷盘。理由和 imu_logger 那次一样：插 USB 会复位芯片，
     *   而复位时 RAM 里的东西全丢 —— 攒批就等于"数据看着有、其实是坏的"。
     *   30 秒写一次，对 Flash 寿命毫无压力。
     */
    fflush(s_fp);
    s_records++;

    if (s_records >= PWR_LOG_MAX_RECORDS) {
        ESP_LOGW(TAG, "已达条数上限 %d，停止记录（数据保留，用 power log dump 读）",
                 PWR_LOG_MAX_RECORDS);
        pwr_log_stop();
    }
}

static void timer_cb(void *arg)
{
    (void)arg;
    sample_once();
}

/* ==================== 对外接口 ==================== */

esp_err_t pwr_log_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(fs_ready(), ESP_ERR_INVALID_STATE, TAG,
                        "SPIFFS 未挂载（分区 '%s'）—— 开机时 imu_logger_init() 会挂它",
                        PWR_LOG_PARTITION);

    s_records = count_file();
    if (s_records >= PWR_LOG_MAX_RECORDS) {
        ESP_LOGE(TAG, "已有 %d 条（上限 %d）：先 `power log erase` 或 `power log dump` 取走数据",
                 s_records, PWR_LOG_MAX_RECORDS);
        return ESP_ERR_NO_MEM;
    }

    /* 追加打开：同一次记录周期里重复 start 不会冲掉旧数据 */
    s_fp = fopen(PWR_LOG_PATH, "ab");
    ESP_RETURN_ON_FALSE(s_fp, ESP_FAIL, TAG, "打开 %s 失败", PWR_LOG_PATH);

    if (!s_timer) {
        const esp_timer_create_args_t ta = {
            .callback = timer_cb,
            .name     = "pwr_log",
        };
        const esp_err_t err = esp_timer_create(&ta, &s_timer);
        if (err != ESP_OK) {
            fclose(s_fp);
            s_fp = NULL;
            ESP_RETURN_ON_ERROR(err, TAG, "定时器创建失败");
        }
    }

    /* 先起定时器；起不来就把文件关掉（否则留下一个打开的文件句柄） */
    const esp_err_t terr = esp_timer_start_periodic(s_timer,
                                                    (uint64_t)PWR_LOG_INTERVAL_S * 1000000);
    if (terr != ESP_OK) {
        fclose(s_fp);
        s_fp = NULL;
        ESP_RETURN_ON_ERROR(terr, TAG, "定时器启动失败");
    }

    s_running = true;
    /* ★ 立刻采一条：这样"start 完马上拔线"也能拿到起点，不用等 30 秒 */
    sample_once();

    ESP_LOGI(TAG, "开始记录：每 %d 秒一条，已有 %d 条，文件 %s",
             PWR_LOG_INTERVAL_S, s_records, PWR_LOG_PATH);
    return ESP_OK;
}

esp_err_t pwr_log_stop(void)
{
    if (s_timer) {
        /* 没在跑时返回 ESP_ERR_INVALID_STATE，忽略即可 */
        (void)esp_timer_stop(s_timer);
    }
    if (s_fp) {
        fflush(s_fp);
        fclose(s_fp);
        s_fp = NULL;
    }
    if (s_running) {
        ESP_LOGI(TAG, "已停止记录（共 %d 条）", s_records);
    }
    s_running = false;
    return ESP_OK;
}

bool pwr_log_running(void)
{
    return s_running;
}

int pwr_log_count(void)
{
    if (!fs_ready()) {
        return -1;
    }
    return count_file();
}

int pwr_log_read(pwr_log_rec_t *out, int max)
{
    if (!out || max <= 0 || !fs_ready()) {
        return -1;
    }
    FILE *f = fopen(PWR_LOG_PATH, "rb");
    if (!f) {
        return 0;                   /* 没有文件 = 0 条，不算错误 */
    }
    int n = 0;
    while (n < max && fread(&out[n], sizeof(pwr_log_rec_t), 1, f) == 1) {
        n++;
    }
    fclose(f);
    return n;
}

esp_err_t pwr_log_erase(void)
{
    pwr_log_stop();
    if (!fs_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (remove(PWR_LOG_PATH) != 0) {
        ESP_LOGW(TAG, "删除 %s 失败（可能本来就没有）", PWR_LOG_PATH);
    }
    s_records = 0;
    ESP_LOGI(TAG, "日志已清空");
    return ESP_OK;
}

/* ==================== 打印 ==================== */

/**
 * @brief 把一条记录的时间和电压打成一行
 *
 * @note 这里**不先 snprintf 到中间缓冲**：`%u` 对编译器来说最大可输出 10 位数字，
 *       6 个字段的字符串会超过任何合理的小缓冲，GCC 直接报 `-Werror=format-truncation`
 *       （本工程开着 -Werror）。直接分行打印既没有缓冲，也保证 19 字符对齐。
 * @note 用 `%.4u`/`%.2u`（精度）而不是 `%04u`/`%02u`（宽度）：精度会**补零**且**限定最大位数**。
 */
static void print_row(int idx, const pwr_log_rec_t *r)
{
    const char *state = (r->flags & PWR_LOG_F_CHARGING) ? "充电中"
                      : ((r->flags & PWR_LOG_F_PRESENT) ? "-" : "无电池");

    if (r->flags & PWR_LOG_F_TIME_VALID) {
        printf("  %4d  %8u  %.4u-%.2u-%.2u %.2u:%.2u:%.2u  %5u  %4d  %s\n",
               idx, (unsigned)r->uptime_s,
               (unsigned)r->year, (unsigned)r->month, (unsigned)r->day,
               (unsigned)r->hour, (unsigned)r->minute, (unsigned)r->second,
               (unsigned)r->mv, (int)r->pct, state);
    } else {
        printf("  %4d  %8u  %-19s  %5u  %4d  %s\n",
               idx, (unsigned)r->uptime_s, "（未校时）",
               (unsigned)r->mv, (int)r->pct, state);
    }
}

void pwr_log_dump(bool all)
{
    const int n = pwr_log_count();
    if (n < 0) {
        printf("  文件系统未就绪（SPIFFS 没挂上？）\n");
        return;
    }
    if (n == 0) {
        printf("  还没有数据。用法：power log start → 拔线静置 → 插回线 → power log dump\n");
        return;
    }

    /* ---------- 第一遍：算趋势（只统计"有电池 + 没在充电"的样本）---------- */
    FILE *f = fopen(PWR_LOG_PATH, "rb");
    if (!f) {
        printf("  打不开 %s\n", PWR_LOG_PATH);
        return;
    }

    pwr_log_rec_t r;
    int      trend_first = -1, trend_last = -1;
    uint16_t mv_first = 0, mv_last = 0, mv_min = 0, mv_max = 0;
    uint32_t up_first = 0, up_last = 0;
    int      charging_cnt = 0, nopresent_cnt = 0;

    for (int i = 0; i < n && fread(&r, sizeof(r), 1, f) == 1; i++) {
        if (r.flags & PWR_LOG_F_CHARGING) {
            charging_cnt++;
            continue;                       /* 充电时电压会上升，不能算进趋势 */
        }
        if (!(r.flags & PWR_LOG_F_PRESENT) || r.mv == 0) {
            nopresent_cnt++;
            continue;
        }
        if (trend_first < 0) {
            trend_first = i;
            mv_first    = r.mv;
            mv_min = mv_max = r.mv;
            up_first    = r.uptime_s;
        }
        trend_last = i;
        mv_last    = r.mv;
        up_last    = r.uptime_s;
        if (r.mv < mv_min) { mv_min = r.mv; }
        if (r.mv > mv_max) { mv_max = r.mv; }
    }
    fclose(f);

    /* ---------- 汇总 ---------- */
    printf("\n  === 功耗日志 %s ===\n", PWR_LOG_PATH);
    printf("  样本 %d 条，间隔 %d 秒，覆盖约 %u 分 %u 秒（按开机秒数算）\n",
           n, PWR_LOG_INTERVAL_S, (unsigned)((up_last - up_first) / 60),
           (unsigned)((up_last - up_first) % 60));
    if (charging_cnt || nopresent_cnt) {
        printf("  已排除：充电中 %d 条、无电池 %d 条（充电时电压上升，算趋势没意义）\n",
               charging_cnt, nopresent_cnt);
    }

    if (trend_first >= 0 && trend_last > trend_first) {
        const uint32_t span_s = up_last - up_first;
        const int      dmv    = (int)mv_last - (int)mv_first;
        printf("  ★ 趋势：%u mV → %u mV（%+d mV / %u 分钟）\n",
               (unsigned)mv_first, (unsigned)mv_last, dmv, (unsigned)(span_s / 60));
        if (span_s >= 60) {
            /* 用"每毫伏多少分钟"表达更直观，也避免 mV/h 的小数点 */
            const int mv_per_h_x100 = (int)((int64_t)dmv * 3600 * 100 / (int64_t)span_s);
            printf("     = %d.%02d mV/小时（越接近 0 越省电）\n",
                   mv_per_h_x100 / 100, (mv_per_h_x100 % 100 + 100) % 100);
        }
        printf("     全程 min/max：%u / %u mV\n", (unsigned)mv_min, (unsigned)mv_max);
        printf("  用法提示：两次测量（旧行为 vs 新行为）要在**相近的起始电压**下比\n");
        /*
         * 拔线前那一条（或前两条）是"USB 还在供电"时采的，电压被 USB 顶住偏高，
         * 会让算出来的下降幅度偏大。板子上没有 VBUS 检测脚，固件无法自动区分，
         * 所以只能在这里提醒：分析时从拔线那一刻之后起算。
         */
        printf("  ⚠️ 最早那条可能是拔线前采的（USB 还在供电）——分析时请从拔线之后的样本起算\n");
    } else if (trend_first >= 0) {
        printf("  趋势：可用样本不足 2 条（或都落在充电/无电池区间），先多录一会儿\n");
    } else {
        printf("  趋势：没有可用的非充电样本\n");
    }

    /* ---------- 第二遍：逐行 ---------- */
    f = fopen(PWR_LOG_PATH, "rb");
    if (!f) {
        return;
    }
    const int step = all ? 1 : ((n + DUMP_MAX_ROWS - 1) / DUMP_MAX_ROWS);
    /* ⚠️ 表头里的百分号要写 `%%` —— 单个 `%` 会被当成格式说明符（这个坑踩过一次） */
    printf("\n  序号   开机s     墙上时间              mV   电量%%  状态%s\n",
           all ? "" : "（抽样，dump all 看全部）");
    for (int i = 0; i < n && fread(&r, sizeof(r), 1, f) == 1; i++) {
        if ((i % step) == 0 || i == n - 1) {
            print_row(i, &r);
        }
    }
    fclose(f);
    printf("\n");
}

/* ==================== 复位黑盒 ==================== */

/**
 * @brief 一条"启动记录"（16 字节定长）
 *
 * 不存自增序号：文件被裁剪（满 64 条保留最近一半）后序号会重复，反而误导；
 * 打印时按**行号**给序号，并说明只保留了最近若干条。
 */
typedef struct {
    uint16_t year;          /**< 墙上时间；未校时时全 0 */
    uint16_t mv;            /**< 记录时读到的电池电压 mV */
    uint8_t  month, day;
    uint8_t  hour, minute;
    int8_t   pct;           /**< 电量 % */
    uint8_t  reason;        /**< esp_reset_reason() 原值 */
    uint8_t  time_valid;    /**< 当时 RTC 是否已校时 */
    uint8_t  rsv[5];        /**< 占位：凑 16 字节，将来加字段不破坏对齐 */
} pwr_boot_rec_t;

_Static_assert(sizeof(pwr_boot_rec_t) == 16, "启动记录必须是 16 字节");

#define PWR_BOOT_PATH   "/spiffs/bootlog.bin"
#define PWR_BOOT_MAX    64

/** @brief 复位原因 → 人话（用枚举名而不是魔数，避免和 IDF 版本脱节） */
static const char *reset_reason_name(uint8_t r)
{
    switch ((esp_reset_reason_t)r) {
    case ESP_RST_POWERON:    return "正常上电（按 PWR 开机）";
    case ESP_RST_SW:         return "软件重启 esp_restart";
    case ESP_RST_PANIC:      return "★崩溃 panic（用 crash 看调用栈）";
    case ESP_RST_INT_WDT:    return "★中断看门狗（CPU 被占住 >300ms）";
    case ESP_RST_TASK_WDT:   return "★任务看门狗（任务卡住 >5s）";
    case ESP_RST_WDT:        return "★其它看门狗";
    case ESP_RST_BROWNOUT:   return "★★掉电复位（3V3 跌破阈值）";
    case ESP_RST_DEEPSLEEP:  return "深睡唤醒";
    case ESP_RST_USB:        return "USB 复位（插线/烧录）";
    case ESP_RST_JTAG:       return "JTAG 复位";
    case ESP_RST_PWR_GLITCH: return "★电源毛刺";
    case ESP_RST_CPU_LOCKUP: return "★CPU 死锁";
    case ESP_RST_EXT:        return "外部引脚复位";
    case ESP_RST_SDIO:       return "SDIO 复位";
    case ESP_RST_EFUSE:      return "eFuse 错误";
    default:                 return "未知";
    }
}

/** @brief 启动记录文件里已有多少条 */
static int count_boot_file(void)
{
    FILE *f = fopen(PWR_BOOT_PATH, "rb");
    if (!f) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    const long sz = ftell(f);
    fclose(f);
    if (sz <= 0) {
        return 0;
    }
    return (int)(sz / (long)sizeof(pwr_boot_rec_t));
}

void pwr_log_note_boot(void)
{
    if (!fs_ready()) {
        return;                     /* SPIFFS 还没挂上（imu_logger 没初始化？） */
    }

    int n = count_boot_file();

    /* 满了就只保留最近一半 —— 重启循环时保证看到的始终是最新记录 */
    if (n >= PWR_BOOT_MAX) {
        const int keep_n = PWR_BOOT_MAX / 2;
        pwr_boot_rec_t keep[PWR_BOOT_MAX / 2];
        FILE *in = fopen(PWR_BOOT_PATH, "rb");
        if (!in) {
            return;
        }
        if (fseek(in, (long)(n - keep_n) * (long)sizeof(pwr_boot_rec_t), SEEK_SET) != 0) {
            fclose(in);
            return;
        }
        const size_t got = fread(keep, sizeof(pwr_boot_rec_t), (size_t)keep_n, in);
        fclose(in);

        FILE *out = fopen(PWR_BOOT_PATH, "wb");
        if (!out) {
            return;
        }
        if (got > 0) {
            (void)fwrite(keep, sizeof(pwr_boot_rec_t), got, out);
        }
        fflush(out);
        fclose(out);
        n = (int)got;
        ESP_LOGW(TAG, "启动记录已满，裁剪为最近 %d 条", n);
    }

    device_state_t st;
    data_hub_snapshot(&st);

    pwr_boot_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.mv  = (uint16_t)(st.battery.millivolt > 0 ? st.battery.millivolt : 0);
    rec.pct = (int8_t)st.battery.percent;
    rec.reason = (uint8_t)esp_reset_reason();
    if (st.time.valid) {
        rec.year   = (uint16_t)st.time.year;
        rec.month  = (uint8_t)st.time.month;
        rec.day    = (uint8_t)st.time.day;
        rec.hour   = (uint8_t)st.time.hour;
        rec.minute = (uint8_t)st.time.minute;
        rec.time_valid = 1;
    }

    FILE *f = fopen(PWR_BOOT_PATH, "ab");
    if (!f) {
        ESP_LOGW(TAG, "打不开 %s，启动记录没写成", PWR_BOOT_PATH);
        return;
    }
    if (fwrite(&rec, sizeof(rec), 1, f) == 1) {
        fflush(f);                  /* 立刻落盘：下一次复位可能就在几秒后 */
    }
    fclose(f);

    ESP_LOGI(TAG, "第 %d 次启动：复位原因 = %s，电池 %u mV / %d%%",
             n + 1, reset_reason_name(rec.reason), (unsigned)rec.mv, (int)rec.pct);
}

void pwr_log_dump_boots(void)
{
    const int n = count_boot_file();
    if (n <= 0) {
        printf("  还没有启动记录（开机时会自动写一条）\n");
        return;
    }

    FILE *f = fopen(PWR_BOOT_PATH, "rb");
    if (!f) {
        printf("  打不开 %s\n", PWR_BOOT_PATH);
        return;
    }

    printf("\n  === 复位黑盒 %s（最近 %d 次启动）===\n", PWR_BOOT_PATH, n);
    /* ⚠️ 表头的百分号必须写 `%%`（单个 % 会被当成格式说明符 —— 这个坑踩了两次） */
    printf("  序号   时间              电池mV  电量%%  复位原因\n");

    pwr_boot_rec_t r;
    int cnt[32];
    memset(cnt, 0, sizeof(cnt));
    int n_brownout = 0, n_panic = 0, n_wdt = 0;

    for (int i = 0; i < n && fread(&r, sizeof(r), 1, f) == 1; i++) {
        if (r.reason < 32) {
            cnt[r.reason]++;
        }
        if (r.reason == (uint8_t)ESP_RST_BROWNOUT) {
            n_brownout++;
        } else if (r.reason == (uint8_t)ESP_RST_PANIC) {
            n_panic++;
        } else if (r.reason == (uint8_t)ESP_RST_TASK_WDT ||
                   r.reason == (uint8_t)ESP_RST_INT_WDT ||
                   r.reason == (uint8_t)ESP_RST_WDT) {
            n_wdt++;
        }

        if (r.time_valid) {
            printf("  %4d   %.4u-%.2u-%.2u %.2u:%.2u  %5u  %4d  %s\n",
                   i, (unsigned)r.year, (unsigned)r.month, (unsigned)r.day,
                   (unsigned)r.hour, (unsigned)r.minute,
                   (unsigned)r.mv, (int)r.pct, reset_reason_name(r.reason));
        } else {
            printf("  %4d   %-16s  %5u  %4d  %s\n",
                   i, "（未校时）", (unsigned)r.mv, (int)r.pct,
                   reset_reason_name(r.reason));
        }
    }
    fclose(f);

    printf("\n  小结：掉电复位 %d 次、崩溃 %d 次、看门狗 %d 次（共 %d 次启动记录）\n",
           n_brownout, n_panic, n_wdt, n);
    if (n_brownout > 0) {
        printf("  → 有掉电复位：**供电问题**（电池电量/内阻、接线接触、LDO 跌落）。\n"
               "     先看「电池mV」这一列：低于约 3.6V 基本就是电量不足或内阻过大。\n");
    }
    if (n_panic > 0 || n_wdt > 0) {
        printf("  → 有崩溃/看门狗：**软件问题**。看调用栈用 PC 上的\n"
               "     `idf.py -p <口> coredump-info`（⚠️ 不要用串口的 crash 命令——\n"
               "       它是**故意触发**一次崩溃，会把现场覆盖掉）。\n");
    }
    printf("\n");
}

/* ==================================================================
 * 卡死哨兵 —— 见 pwr_log.h 的说明
 *
 * 设计要点（都是被实机的"静默硬死"逼出来的）：
 *   1. **不依赖 esp_timer**：卡死时那条路可能一起死 → 用独立的 vTaskDelay 循环。
 *   2. **绑 CPU 1**：实测卡死时**串口控制台也没反应**（控制台固定在 CPU 0），
 *      说明至少 CPU 0 被占死 → 哨兵必须在另一个核上才活得下来。
 *   3. **判活不拿内核锁**：只读 `g_bsp_lvgl_beat`（bsp.c 里由 LVGL 任务自增）。
 *      这一点很关键：如果哨兵自己也去调 `uxTaskGetSystemState()`（要拿内核锁），
 *      而卡死方正好占着内核锁，哨兵就会跟着一起卡住 —— 那现场就永远写不出来了。
 *   4. **先写"最小标记"（不用任何内核 API），再试着补任务表**：
 *      补表要拿内核锁、有把自己也卡住的风险；但那时标记已经落盘了 ✔。
 * ================================================================== */
#define HANG_PATH          "/spiffs/hang.txt"
#define HANG_WATCH_TASK    "hang_wd"
#define HANG_STALL_SEC     5
#define HANG_MAX_TASKS     20

static TaskStatus_t s_hang_tasks[HANG_MAX_TASKS];

static const char *task_state_name(eTaskState st)
{
    switch (st) {
    case eRunning:   return "运行";
    case eReady:     return "就绪";
    case eBlocked:   return "阻塞";
    case eSuspended: return "挂起";
    case eDeleted:   return "已删";
    default:         return "?";
    }
}

/**
 * @brief 取任务的栈顶指针
 *
 * @note `TaskStatus_t` 里**没有** `pxTopOfStack`（xtensa 栈向下增长，那个字段被条件编译掉了），
 *       但 FreeRTOS 保证 **`pxTopOfStack` 是 TCB 的第一个成员**
 *       （tasks.c 原文：THIS MUST BE THE FIRST MEMBER OF THE TCB STRUCT）
 *       → 直接把任务句柄当 `StackType_t**` 解引用即可。仅用于诊断。
 */
static uint32_t task_stack_top(TaskHandle_t h)
{
    if (!h) {
        return 0;
    }
    return (uint32_t)(uintptr_t)(*(volatile StackType_t *const *)h);
}

/**
 * @brief 把任务栈顶的**寄存器内容**也 dump 出来（关键：PC 就在里面）
 *
 * 为什么不能只存 `pxTopOfStack` 指针：那是个 **DRAM 地址**（0x3fcxxxxx），
 * `addr2line` 翻译不出函数名。
 * 任务被切出去时，寄存器（含 PC）就压在栈顶那一小段里 ——
 * 而 xtensa 的保存帧布局随 port 版本变化，**与其猜偏移，不如把前 16 个字全 dump**，
 * 离线时把落在**代码段**（IRAM 0x40000000~0x40400000、flash text 0x42000000~0x43000000）
 * 的那些值拿去 `addr2line` 即可。
 */
static void hang_dump_stack_words(FILE *f, TaskHandle_t h)
{
    const uint32_t *sp = (const uint32_t *)(uintptr_t)task_stack_top(h);
    if (!sp || ((uintptr_t)sp & 3) != 0) {
        fprintf(f, "(栈顶不可用)");
        return;
    }
    for (int k = 0; k < 16; k++) {
        fprintf(f, "%08x ", (unsigned)sp[k]);
    }
}

/** @brief 第 1 步：写"最小标记"（不碰内核锁，几乎不可能失败） */
static void hang_write_marker(uint32_t beat, int stalled_sec)
{
    if (!fs_ready()) {
        return;
    }
    FILE *f = fopen(HANG_PATH, "w");
    if (!f) {
        return;
    }
    fprintf(f, "=== 卡死报告 ===\n");
    fprintf(f, "开机 %u 秒时：界面任务心跳连续 %d 秒没有推进（心跳值 %u）\n",
            (unsigned)(esp_timer_get_time() / 1000000), stalled_sec, (unsigned)beat);
    fprintf(f, "含义：LVGL 任务要么被阻塞、要么被饿死 → 界面已死。\n\n");
    fflush(f);
    fclose(f);
}

/** @brief 第 2 步：补任务表（要拿内核锁，有可能把自己也卡住 → 放在标记之后） */
static void hang_append_tasks(void)
{
    const UBaseType_t n = uxTaskGetSystemState(s_hang_tasks, HANG_MAX_TASKS, NULL);
    if (n == 0 || !fs_ready()) {
        return;
    }
    FILE *f = fopen(HANG_PATH, "a");
    if (!f) {
        return;
    }
    fprintf(f, "%-14s %-5s %-4s %-12s %-12s %-8s\n",
            "任务", "状态", "优先", "运行计数us", "栈顶地址", "栈余量");
    for (UBaseType_t i = 0; i < n; i++) {
        fprintf(f, "%-14s %-5s %-4u %-12u 0x%08x   %-8u\n",
                s_hang_tasks[i].pcTaskName ? s_hang_tasks[i].pcTaskName : "?",
                task_state_name(s_hang_tasks[i].eCurrentState),
                (unsigned)s_hang_tasks[i].uxCurrentPriority,
                (unsigned)s_hang_tasks[i].ulRunTimeCounter,
                (unsigned)task_stack_top(s_hang_tasks[i].xHandle),
                (unsigned)s_hang_tasks[i].usStackHighWaterMark);
        /* ★ 栈顶寄存器（含 PC）：任务被切出去时，寄存器就压在这里 */
        fprintf(f, "    栈顶寄存器: ");
        hang_dump_stack_words(f, s_hang_tasks[i].xHandle);
        fprintf(f, "\n");
    }
    fprintf(f, "\n离线翻译：把上面落在代码段的值拿去\n"
               "  xtensa-esp32s3-elf-addr2line -pfiaC -e build/smartwatch.elf 0x420xxxxx 0x403xxxxx ...\n"
               "  （代码段范围：IRAM 0x40000000~0x40400000、flash text 0x42000000~0x43000000）\n");
    fflush(f);
    fclose(f);
}

static void hang_watch_task(void *arg)
{
    (void)arg;

    uint32_t last  = g_bsp_lvgl_beat;
    int      stall = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        const uint32_t beat = g_bsp_lvgl_beat;
        if (beat == last) {
            stall++;
        } else {
            stall = 0;
            last  = beat;
        }

        if (stall >= HANG_STALL_SEC) {
            hang_write_marker(beat, stall);     /* 先落盘（不拿锁） */
            hang_append_tasks();                /* 再补细节（有风险，但标记已在） */
            ESP_LOGE(TAG, "界面任务已停 %d 秒，现场写入 %s", stall, HANG_PATH);
            /* 写完就停：避免反复调内核 API 把自己也拖进去 */
            vTaskDelete(NULL);
        }
    }
}

void pwr_log_hang_watch_start(void)
{
    static bool started = false;
    if (started) {
        return;
    }
    /* 优先级 18：高于界面/数据/控制台，低于 esp_timer(22)/ipc(24)；
     * 绑 CPU 1：卡死常常是某个核被占死（实测控制台也死，它固定在 CPU 0），
     * 哨兵得在另一个核上活着。栈 4096 够它写文件＋一次任务表。 */
    if (xTaskCreatePinnedToCore(hang_watch_task, HANG_WATCH_TASK, 4096, NULL, 18, NULL, 1) != pdPASS) {
        ESP_LOGW(TAG, "卡死哨兵创建失败（卡死时将无现场可查）");
        return;
    }
    started = true;
    ESP_LOGI(TAG, "卡死哨兵已启动（界面心跳停 %d 秒即写 %s）", HANG_STALL_SEC, HANG_PATH);
}

void pwr_log_dump_hang(void)
{
    if (!fs_ready()) {
        printf("  文件系统未就绪\n");
        return;
    }
    FILE *f = fopen(HANG_PATH, "r");
    if (!f) {
        printf("  还没有卡死报告（哨兵启动后没抓到卡死）\n");
        return;
    }
    printf("\n");
    char line[200];
    while (fgets(line, sizeof(line), f)) {
        printf("  %s", line);
    }
    fclose(f);
    printf("\n");
}
