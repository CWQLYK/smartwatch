/**
 * @file app_console.c
 * @brief 串口命令行实现（esp_console + linenoise）
 *
 * 实现说明：
 *   - 走 **USB-Serial-JTAG**（`esp_console_new_repl_usb_serial_jtag`），
 *     不是 UART0 —— 这块板的 Type-C 直接接 ESP32-S3 原生 USB，
 *     UART0 只引到扩展口。详见 app_console_start() 里的说明。
 *   - 命令解析用 sscanf 手工做，比 argtable 直观，
 *     而且时间格式（YYYY-MM-DD HH:MM:SS）用 sscanf 一行就能拆。
 *   - 所有命令都在 REPL 自己的任务里跑，**不要在里面碰 lv_xxx**。
 */
#include "app_console.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_debug_helpers.h"    /* esp_backtrace_print_all_tasks：bt 命令（v0.6.1） */
#include "bsp.h"
#include "settings.h"
#include "data_hub.h"
#include "battery.h"
#include <inttypes.h>
#include "rtc_pcf85063.h"
#include "motion.h"
#include "qmi8658c.h"
#include "imu_logger.h"
#include "watch_pages.h"
#include "wifi_service.h"
#include "power.h"

static const char *TAG = "CONSOLE";

/* ==================================================================
 * 命令：time —— 看当前状态
 * ================================================================== */
/**
 * @brief time 命令：显示状态 / 校时
 *
 * 两种用法：
 *     time                                看时间/电量/状态
 *     time set <YYYY-MM-DD> <HH:MM:SS>    按 **UTC** 校时
 *
 * ⚠️ 这里填的是 **UTC**，和老的 `set_time` 不一样（那个按本地时间）。
 *    两个都留着：set_time 是"我知道本地几点"，time set 是"我知道 UTC"。
 */
static int cmd_time_set(int argc, char **argv);

static int cmd_time(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "set") == 0) {
        return cmd_time_set(argc, argv);
    }

    device_state_t st;
    data_hub_snapshot(&st);

    printf("\n");
    if (st.time.valid) {
        printf("  时间   : %04d-%02d-%02d %02d:%02d:%02d  星期%d\n",
               st.time.year, st.time.month, st.time.day,
               st.time.hour, st.time.minute, st.time.second, st.time.weekday);
    } else {
        printf("  时间   : 未校准（RTC 掉过电）\n");
        printf("           校时命令: set_time 2026-10-06 14:30:00\n");
    }

    if (st.battery.percent >= 0) {
        printf("  电池   : %d mV  %d%%%s\n", st.battery.millivolt,
               st.battery.percent, st.battery.charging ? "  [充电中]" : "");
    } else {
        printf("  电池   : 未知 (%d mV，可能没插电池)\n", st.battery.millivolt);
    }

    if (st.steps.available) {
        printf("  步数   : %d 步 / %d 千卡 / %d 米（详细用 motion 命令）\n",
               st.steps.steps, st.steps.kcal, st.steps.meters);
    } else {
        printf("  步数   : 未接入（IMU 没起来，用 status 查 0x6B）\n");
    }
    printf("  亮度   : %d%%\n", (int)settings_get_or_default(SETTINGS_BRIGHTNESS));
    printf("  运行   : %lld 秒\n", esp_timer_get_time() / 1000000LL);
    printf("  空闲堆 : %u 字节\n", (unsigned)esp_get_free_heap_size());
    printf("\n");
    return 0;
}

/* ==================================================================
 * 命令：set_time —— 校时（P1 唯一的校时入口）
 * ================================================================== */
static int cmd_set_time(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        printf("用法: set_time <YYYY-MM-DD> <HH:MM:SS>\n");
        printf("      set_time utc <YYYY-MM-DD> <HH:MM:SS>   按 UTC 校时\n");
        return 1;
    }

    const bool is_utc    = (argc == 4) && (strcmp(argv[1], "utc") == 0);
    const char *date_str = is_utc ? argv[2] : argv[1];
    const char *time_str = is_utc ? argv[3] : argv[2];

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf(date_str, "%d-%d-%d", &y, &mo, &d) != 3 ||
        sscanf(time_str, "%d:%d:%d", &h, &mi, &s) != 3) {
        printf("解析失败。正确格式: set_time 2026-10-06 14:30:00\n");
        return 1;
    }

    if (is_utc) {
        /*
         * UTC 校时：先按 UTC 算成时间戳，再交给 data_hub。
         * 这是 P2 的 BLE CTS / SNTP 同步将来会走的同一条路径。
         */
        rtc_pcf_time_t rt = { .year = y, .month = mo, .day = d,
                          .hour = h, .minute = mi, .second = s, .weekday = 1 };
        if (!rtc_pcf_time_is_valid(&rt)) {
            printf("时间字段非法: %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
            return 1;
        }
        rtc_pcf_set_time(&rt);                  /* 借 RTC 驱动算时间戳 */
        const uint32_t epoch = rtc_pcf_get_epoch();

        esp_err_t err = data_hub_set_epoch(epoch);
        if (err != ESP_OK) {
            printf("UTC 校时失败: %s\n", esp_err_to_name(err));
            return 1;
        }
    } else {
        /* 墙上时间校时：所见即所得，不做时区换算 */
        dev_time_t t = {
            .year = y, .month = mo, .day = d,
            .hour = h, .minute = mi, .second = s,
            .weekday = 0,                   /* 0 = 让驱动按日期自己算 */
        };
        esp_err_t err = data_hub_set_time(&t);
        if (err != ESP_OK) {
            printf("校时失败: %s\n", esp_err_to_name(err));
            return 1;
        }
    }

    device_state_t st;
    data_hub_snapshot(&st);
    printf("校时成功 -> %04d-%02d-%02d %02d:%02d:%02d 星期%d%s\n",
           st.time.year, st.time.month, st.time.day,
           st.time.hour, st.time.minute, st.time.second, st.time.weekday,
           is_utc ? "  (UTC 输入)" : "");
    return 0;
}

/* ==================================================================
 * 命令：battery —— 对照万用表验证 ADC 精度
 * ================================================================== */
static int cmd_battery(int argc, char **argv)
{
    (void)argc; (void)argv;

    battery_reading_t r;
    if (battery_read(&r) != ESP_OK) {
        printf("采样失败\n");
        return 1;
    }

    printf("\n");
    printf("  电池电压 : %d mV   (%.3f V)\n", r.millivolt, r.millivolt / 1000.0f);
    if (r.percent >= 0) {
        printf("  电量     : %d%%\n", r.percent);
    } else {
        printf("  电量     : 未知\n");
    }
    printf("  充电中   : %s\n", r.charging ? "是" : "否");
    printf("  检测到电池: %s\n", r.present ? "是" : "否");
    printf("  ADC 校准 : %s\n", battery_cali_ok() ? "曲线拟合（准确）" : "未校准（偏低）");
    printf("\n  对照方法：用万用表量电池正负极，应该和上面的 mV 差 < 100mV\n\n");
    return 0;
}

/* ==================================================================
 * 命令：brightness
 * ================================================================== */
static int cmd_brightness(int argc, char **argv)
{
    if (argc == 1) {
        printf("当前亮度: %d%%\n", bsp_backlight_get());
        return 0;
    }

    const int val = atoi(argv[1]);
    if (val < 5 || val > 100) {
        printf("亮度范围 5-100\n");
        return 1;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(settings_set(SETTINGS_BRIGHTNESS, val));
    if (bsp_backlight_set(val) != ESP_OK) {
        printf("背光设置失败\n");
        return 1;
    }
    printf("亮度 -> %d%%\n", val);
    return 0;
}

/* ==================================================================
 * 命令：settings / reset_settings
 * ================================================================== */
static int cmd_settings(int argc, char **argv)
{
    (void)argc; (void)argv;
    settings_dump();
    return 0;
}

static int cmd_reset_settings(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (settings_reset_all() != ESP_OK) {
        printf("恢复出厂设置失败\n");
        return 1;
    }
    printf("已恢复出厂设置（亮度等回默认值，需重启生效）\n");
    return 0;
}

/* ==================================================================
 * 命令：watch_status —— 打印硬件自检和页面栈（排查用）
 * ================================================================== */
static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;

    data_hub_dump();

    /*
     * RTC 芯片内部温度（不是环境温度，只能当"芯片热不热"的参考）。
     * 用整数拆符号打印：ESP-IDF 的 printf 默认不带浮点格式化。
     *
     * ⚠️ 实测这块板的 PCF85063ATL 读不到温度（恒为 0x80/0x00），
     *    驱动会返回 ESP_ERR_NOT_SUPPORTED，这里就明确说出来，
     *    免得以后有人对着一串乱码温度查半天。
     */
    int centi = 0;
    esp_err_t terr = rtc_pcf_get_chip_temp(&centi);
    if (terr == ESP_OK) {
        const char *sign = (centi < 0) ? "-" : "";
        const int mag = (centi < 0) ? -centi : centi;
        printf("  RTC 芯片温度: %s%d.%02d C\n", sign, mag / 100, mag % 100);
    } else {
        printf("  RTC 芯片温度: 读不到（%s，该批芯片无温度通道）\n",
               esp_err_to_name(terr));
    }
    printf("  RTC 时间可信: %s\n", rtc_pcf_time_lost() ? "否（掉过电）" : "是");
    return 0;
}

/* ==================================================================
 * 命令：motion —— 计步状态 + 遥测（调参用）
 * ================================================================== */
static int cmd_motion(int argc, char **argv)
{
    (void)argc; (void)argv;

    motion_status_t st;
    motion_get_status(&st);

    printf("\n");
    if (!st.imu_ok) {
        printf("  IMU      : 未就绪（步数显示为 --）\n");
        printf("             检查 I2C 上 0x6B 在不在（命令 status 可看）\n\n");
        return 1;
    }

    static const char *act[3] = { "静止", "走路", "跑步" };
    printf("  IMU      : 就绪，采样任务%s\n", st.running ? "运行中" : "**未运行**");
    printf("  步数     : %" PRIu32 "  (待确认 %" PRIu32 ")\n",
           st.steps, st.steps_pending);
    printf("  步频     : %" PRId32 " 步/分   状态: %s\n",
           st.cadence_spm, act[st.activity < 3 ? st.activity : 0]);
    printf("  步长     : %" PRId32 " cm\n", st.stride_cm);
    printf("  距离     : %" PRIu32 " m\n", st.distance_m);
    printf("  卡路里   : %" PRIu32 " kcal\n", st.kcal);
    printf("  遥测     : 信号 %" PRId32 " mg / 门限 %" PRId32 " mg\n",
           st.sig_mg, st.thr_mg);
    printf("\n");
    return 0;
}

/**
 * @brief 连续打印遥测，用来现场调参：motion_watch [秒数]
 *
 * 调参方法（详见 imu 组件的注释）：
 *   1) 静止 60 秒，看"信号 mg"的峰峰值 -> peak_thresh 取它的 3~5 倍
 *   2) 走 100 步，对比步数：偏少降 crest_min，偏多升 refractory
 *   3) 打字/坐车/挥手各 2 分钟，应该 0 步
 */
static int cmd_motion_watch(int argc, char **argv)
{
    const int secs = (argc > 1) ? atoi(argv[1]) : 10;
    printf("连续采样 %d 秒，每秒一行。建议同时做动作（走路/静止）观察：\n", secs);
    printf("ms,活动,信号mg,门限mg,步频,步数,待确认,距离m\n");
    for (int i = 0; i < secs; i++) {
        motion_dump_telemetry();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("结束。\n");
    return 0;
}

/** @brief 步数清零 */
static int cmd_steps_reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    motion_reset_steps();
    printf("步数已清零\n");
    return 0;
}

/* ==================================================================
 * 命令组：rec —— IMU 录制（离线调参用）
 *
 * 为什么要这个功能：
 *   调计步参数需要真实走路数据，但串口线太短，只能坐着甩手，
 *   而甩手和走路的信号特征完全不是一回事。所以让手表自己录到 Flash，
 *   走完回来插 USB 再导出到电脑。
 *
 * 命令：
 *   rec start          开始录制
 *   rec stop           停止（并把缓冲落盘）
 *   rec status         看状态
 *   rec list           列出所有日志文件
 *   rec get <文件>     导出成 CSV 到串口（默认）
 *   rec get <文件> hex 导出成十六进制
 *   rec erase          删除所有日志
 * ================================================================== */

/** @brief 导出用缓冲：一行的字节数，取自一个整帧，保证按帧边界切 */
#define REC_DUMP_RECS_PER_CHUNK  256

static int cmd_rec(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "status";

    /* ---------- start ---------- */
    if (strcmp(sub, "start") == 0) {
        if (!imu_logger_available()) {
            printf("存储没挂载，无法录制（看启动日志里 SPIFFS 的错误）\n");
            return 1;
        }
        esp_err_t err = imu_logger_start();
        if (err != ESP_OK) {
            printf("开始录制失败: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("开始录制。走完记得先执行 `rec stop` 再拔线，"
               "否则最后约 7 秒数据会丢。\n");
        return 0;
    }

    /* ---------- stop ---------- */
    if (strcmp(sub, "stop") == 0) {
        esp_err_t err = imu_logger_stop();
        if (err != ESP_OK) {
            printf("停止录制失败: %s\n", esp_err_to_name(err));
            return 1;
        }
        imu_logger_status_t st;
        imu_logger_get_status(&st);
        printf("已停止。本次 %" PRIu32 " 帧 / %" PRIu32 " 秒 / %" PRIu32 " 字节\n",
               st.samples, st.seconds, st.bytes);
        printf("导出: rec get %s\n", st.current[0] ? st.current : "<文件>");
        return 0;
    }

    /* ---------- status ---------- */
    if (strcmp(sub, "status") == 0) {
        imu_logger_status_t st;
        imu_logger_get_status(&st);

        printf("\n  存储     : %s\n", st.mounted ? "已挂载" : "**未挂载**");
        printf("  录制     : %s\n", st.recording ? "**进行中**" : "空闲");
        if (st.recording || st.samples) {
            printf("  本次     : %" PRIu32 " 帧 / %" PRIu32 " 秒 / %" PRIu32 " 字节\n",
                   st.samples, st.seconds, st.bytes);
            printf("  当前文件 : %s\n", st.current[0] ? st.current : "-");
        }
        printf("  文件数   : %" PRIu32 " 个\n", st.files);
        printf("  容量     : 已用 %" PRIu32 " KB / 共 %" PRIu32 " KB",
               st.used_bytes / 1024, st.total_bytes / 1024);
        if (st.total_bytes > st.used_bytes) {
            const uint32_t free_b = st.total_bytes - st.used_bytes;
            /* 50Hz × 12 字节 = 600 B/s */
            printf("（还能录约 %" PRIu32 " 分钟）", free_b / 600 / 60);
        }
        printf("\n\n");
        return 0;
    }

    /* ---------- list ---------- */
    if (strcmp(sub, "list") == 0) {
        char names[64][32];
        const int n = imu_logger_list(names, 64);
        if (n == 0) {
            printf("没有日志文件\n");
            return 0;
        }
        printf("\n发现 %d 个日志文件：\n", n);
        for (int i = 0; i < n; i++) {
            const uint32_t sz = imu_logger_file_size(names[i]);
            printf("  %-16s %8" PRIu32 " 字节  %6" PRIu32 " 帧  %5" PRIu32 " 秒\n",
                   names[i], sz, sz / 12, sz / 12 / 50);
        }
        printf("\n导出方法: rec get %s\n\n", names[0]);
        return 0;
    }

    /* ---------- erase ---------- */
    if (strcmp(sub, "erase") == 0) {
        if (imu_logger_erase_all() != ESP_OK) {
            printf("擦除失败\n");
            return 1;
        }
        printf("已删除所有日志文件\n");
        return 0;
    }

    /* ---------- get ---------- */
    if (strcmp(sub, "get") == 0) {
        if (argc < 3) {
            printf("用法: rec get <文件>    例如 rec get /spiffs/imu_0001.bin\n");
            printf("      rec list 可以看有哪些文件\n");
            return 1;
        }

        const char *name = argv[2];

        const uint32_t size = imu_logger_file_size(name);
        if (size == 0) {
            printf("文件 %s 不存在或为空（用 rec list 看看）\n", name);
            return 1;
        }

        printf("===== BEGIN %s %" PRIu32 " bytes =====\n", name, size);

        /*
         * ============================================================
         * 带校验的帧流格式（每行）
         * ============================================================
         *
         *     <帧序号> <12 字节十六进制> <校验和>
         *     000123 01b7affe0000fe13 5c
         *
         * ## 为什么要这么麻烦（纯 CSV / 纯 hex 为什么不行）
         *
         * 实测这块板的 USB-Serial-JTAG 在**持续高速输出**时会丢字节，
         * 而且丢得很隐蔽：行数是对的（换行符没丢、行没粘连），
         * 但行**内部**的字段会错位。第一版我用 `%u,%d,%d,%d` 输出 CSV，
         * 导出结果前半段正常（t=7034,ax=-32），后半段变成 t=13634,ax=0
         * 这种乱值 —— 而且**不带任何校验，根本发现不了**，
         * 拿来调参会把算法调到沟里去。
         *
         * ## 校验和怎么算
         *
         * 从**帧序号第一个字节**到**帧数据最后一个字节**逐字节异或，
         * 不包含空格和校验和字段自身。所以丢字节、粘行、字符畸变
         * 都会让校验失败。
         *
         * ## 帧序号不是多余的
         *
         * 它让 PC 端能算出"缺了哪几帧"，从而精确重传那几帧，
         * 不用整段重来。（导 1 小时数据要 20 多分钟，整段重来代价太大。）
         */
        static imu_log_rec_t chunk[REC_DUMP_RECS_PER_CHUNK];
        uint32_t offset   = 0;
        uint32_t frame_no = 0;

        while (offset < size) {
            uint32_t got = 0;
            const uint32_t want = sizeof(chunk);
            if (imu_logger_read(name, offset, (uint8_t *)chunk, want, &got) != ESP_OK) {
                printf("!! 读取失败于偏移 %" PRIu32 "\n", offset);
                break;
            }
            if (got == 0) {
                break;
            }

            const uint32_t recs = got / sizeof(imu_log_rec_t);

            for (uint32_t i = 0; i < recs; i++) {
                const uint8_t *raw = (const uint8_t *)&chunk[i];

                /* 组行：帧序号 + 12 字节 hex */
                char line[48];
                int n = snprintf(line, sizeof(line), "%06" PRIu32 " ", frame_no);
                for (int b = 0; b < (int)sizeof(imu_log_rec_t); b++) {
                    n += snprintf(line + n, sizeof(line) - n, "%02x", raw[b]);
                }

                /* 校验和：序号 + 数据的全部字符（跳过分隔空格） */
                uint8_t sum = 0;
                for (const char *p = line; *p; p++) {
                    if (*p != ' ') {
                        sum ^= (uint8_t)*p;
                    }
                }
                printf("%s %02x\n", line, sum);

                frame_no++;

                /*
                 * 每 16 帧让一次 CPU，并给串口 TX 缓冲留出排空时间。
                 * 两件事都必要：
                 *   - 让出 CPU：否则 LVGL 和 IMU 采样任务会被饿着
                 *   - 留出间隔：实测能显著降低 TX FIFO 溢出丢字节的概率
                 */
                if ((i & 0x0F) == 0x0F) {
                    vTaskDelay(1);
                }
            }

            offset += recs * sizeof(imu_log_rec_t);
        }

        printf("===== END %s =====\n", name);
        return 0;
    }

    printf("未知子命令 '%s'。可用: start / stop / status / list / get / erase\n", sub);
    return 1;
}

/* ==================================================================
 * 命令：imu_raw —— 打印原始码值（验证量程换算用）
 * ================================================================== */
static int cmd_imu_raw(int argc, char **argv)
{
    const int n = (argc > 1) ? atoi(argv[1]) : 10;
    if (n <= 0 || n > 200) {
        printf("用法: imu_raw [次数 1-200]\n");
        return 1;
    }

    /*
     * 换算函数自检：把同一个原始值喂给四个量程，打印结果。
     * 期望（输入 -4046，正确公式 lsb = 1 << (15 - fs)）：
     *     fs=0 (+/-2g, 16384) -> -246      fs=1 (+/-4g, 8192) -> -493
     *     fs=2 (+/-8g, 4096)  -> -987      fs=3 (+/-16g,2048) -> -1975
     * 若 fs=2 打出 -493，说明跑的还是旧公式（32768>>fs）。
     */
    printf("\n  [换算函数自检] raw = -4046\n");
    printf("    期望: -246 / -493 / -987 / -1975\n");
    /* 把公式中间量也打出来：这样能立刻分清"公式错"还是"编译到旧代码" */
    printf("    实测 LSB/g: %" PRId32 " / %" PRId32 " / %" PRId32 " / %" PRId32 "\n",
           qmi8658_afs_lsb_per_g(QMI8658_AFS_2G),
           qmi8658_afs_lsb_per_g(QMI8658_AFS_4G),
           qmi8658_afs_lsb_per_g(QMI8658_AFS_8G),
           qmi8658_afs_lsb_per_g(QMI8658_AFS_16G));
    printf("    期望 LSB/g: 16384 / 8192 / 4096 / 2048\n");
    printf("    实测: %" PRId32 " / %" PRId32 " / %" PRId32 " / %" PRId32 "\n\n",
           qmi8658_accel_raw_to_mg(-4046, QMI8658_AFS_2G),
           qmi8658_accel_raw_to_mg(-4046, QMI8658_AFS_4G),
           qmi8658_accel_raw_to_mg(-4046, QMI8658_AFS_8G),
           qmi8658_accel_raw_to_mg(-4046, QMI8658_AFS_16G));

    qmi8658_sample_t s;
    printf("\n  #   ax_raw   ay_raw   az_raw      |a|_raw   |a|_mg\n");
    printf("     (原始码值 0x8000=-32768)      (4096 = 1g @ +/-8g)\n");
    for (int i = 0; i < n; i++) {
        if (qmi8658_read(&s) != ESP_OK) {
            printf("  读失败\n");
            break;
        }
        /* 合矢量：用 mg 反推等价码值，便于和 4096 对比 */
        const int32_t ax = qmi8658_accel_raw_to_mg(s.ax, QMI8658_AFS_8G);
        const int32_t ay = qmi8658_accel_raw_to_mg(s.ay, QMI8658_AFS_8G);
        const int32_t az = qmi8658_accel_raw_to_mg(s.az, QMI8658_AFS_8G);
        const int64_t s2 = (int64_t)ax * ax + (int64_t)ay * ay + (int64_t)az * az;
        int32_t r = (int32_t)s2, x = r, y = 1;
        while (x > y) { x = (x + y) / 2; y = r / x; }   /* 整数开方 */

        printf("  %2d  %7d  %7d  %7d     %8" PRId32 "   %6" PRId32 "\n",
               /*
                * x 是整数开方后的 **mg** 值，不要再乘 4096/1000 ——
                * 我第一版多乘了一次，把 988mg 显示成 4045，
                * 看着像"量程错了一倍"，白查了半天。
                * 第五列给等价原始码值（4096 = 1g @ +/-8g）方便对照。
                */
               i + 1, s.ax, s.ay, s.az, x, (int32_t)((int64_t)x * 4096 / 1000));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    printf("\n  判读（先看上面两行 LSB/g 和 期望值是否一致）：\n");
    printf("    LSB/g  必须是 16384 / 8192 / 4096 / 2048\n");
    printf("            （写成 32768/16384/8192/4096 就是公式错了一倍）\n");
    printf("    |a|_mg 静止平放时应约 1000（1g）\n");
    printf("            明显偏离就先修公式，调计步参数是白费劲\n");
    printf("    用宿主 gcc 单独跑公式验证比烧板子快，见 imu 组件头文件注释\n\n");
    return 0;
}

/** @brief 翻页（调试用；实机上靠滑动） */
static int cmd_next(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = watch_ui_next();
    /* 等换页动画跑完再报状态 */
    vTaskDelay(pdMS_TO_TICKS(350));
    printf("已翻页 -> [%d]  可纵向滚 %d px\n",
           watch_ui_current_index(), watch_ui_debug_page_scroll());
    return err == ESP_OK ? 0 : 1;
}

/** @brief 看当前页的布局/滚动情况（排查"按钮够不着"用） */
static int cmd_pages(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("\n");
    printf("  当前页          : [%d]\n", watch_ui_current_index());
    printf("  可纵向滚动      : %d px（0 = 内容不满一屏）\n",
           watch_ui_debug_page_scroll());
    printf("  水平偏移        : %d（静止时应为 0）\n", watch_ui_debug_scroll_x());
    printf("\n");
    watch_ui_debug_dump();
    printf("\n");
    return 0;
}

/* ==================================================================
 * 命令组：wifi / power / time set —— v0.4.0 新增
 *
 * 命名沿用"短命令"习惯（不加 at 前缀）：用户在手表上点了什么，
 * 这里就有对应的命令，方便脚本化和排错。
 * ================================================================== */

static int cmd_wifi(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "status";

    /* wifi on —— 启动协议栈（开机时故意不启动，省 50KB RAM） */
    if (strcmp(sub, "on") == 0) {
        esp_err_t err = wifi_service_init();
        printf("%s\n", err == ESP_OK ? "WiFi 已启动" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "status") == 0) {
        printf("\n");
        wifi_service_dump();
        printf("\n");
        return 0;
    }
    if (strcmp(sub, "scan") == 0) {
        const wifi_hooks_t *h = wifi_service_get_hooks();
        if (h->scan_start() != 0) {
            printf("扫描发起失败（WiFi 没启动？先执行 wifi on）\n");
            return 1;
        }
        printf("正在扫描…\n");
        for (int i = 0; i < 60 && h->scan_busy(); i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        wifi_ap_t aps[WIFI_SCAN_MAX];
        const int n = h->get_scan_result(aps, WIFI_SCAN_MAX);
        printf("发现 %d 个网络：\n", n);
        for (int i = 0; i < n; i++) {
            printf("  %-28s %4d dBm  %s\n", aps[i].ssid, aps[i].rssi,
                   aps[i].secure ? "[加密]" : "[开放]");
        }
        return 0;
    }
    if (strcmp(sub, "connect") == 0) {
        if (argc < 3) {
            printf("用法: wifi connect <SSID> [密码]\n");
            return 1;
        }
        if (!wifi_service_started()) {
            wifi_service_init();
        }
        const wifi_hooks_t *h = wifi_service_get_hooks();
        const char *pwd = (argc > 3) ? argv[3] : "";
        if (h->connect(argv[2], pwd) != 0) {
            printf("连接发起失败\n");
            return 1;
        }
        printf("正在连接 '%s'…（用 wifi status 看结果）\n", argv[2]);
        return 0;
    }
    if (strcmp(sub, "disconnect") == 0) {
        const wifi_hooks_t *h = wifi_service_get_hooks();
        h->disconnect();
        printf("已断开（保存的密码仍留着，下次还能自动重连）\n");
        return 0;
    }
    /*
     * ★ v0.7.0：补上 forget。
     *
     *   这个子命令以前只写在 help 串里、**没有实现**（交接文档 §10 #4）：
     *   底下 `hook_forget()` 早就写好了、也挂进了 hooks，只是命令行没接线。
     *   它和 disconnect 是**两个不同的意图**（源码注释里写着）：
     *     disconnect = 先不连了，密码留着
     *     forget     = 彻底忘掉这个网，下次要重新输密码
     */
    if (strcmp(sub, "forget") == 0) {
        const wifi_hooks_t *h = wifi_service_get_hooks();
        if (!h->forget) {
            printf("本版本不支持 forget\n");
            return 1;
        }
        if (h->forget() != 0) {
            printf("WiFi 没启动（先执行 wifi on）\n");
            return 1;
        }
        printf("已忘掉该网络（保存的凭据已清除，下次要重新输密码）\n");
        return 0;
    }
    if (strcmp(sub, "reset") == 0) {
        esp_err_t err = wifi_service_reset();
        printf("%s\n", err == ESP_OK ? "WiFi 已重置（凭据已清除）"
                                      : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "sync") == 0) {
        const wifi_hooks_t *h = wifi_service_get_hooks();
        if (h->sync_time() != 0) {
            printf("对时失败（没连上 WiFi？）\n");
            return 1;
        }
        printf("已发起 SNTP 对时，几秒后 time 命令可看结果\n");
        return 0;
    }

    printf("可用: wifi on|status|scan|connect <SSID> [密码]|disconnect|forget|reset|sync\n");
    return 1;
}

static int cmd_power(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "status";

    if (strcmp(sub, "off") == 0) {
        power_screen_off();
        printf("已息屏（只关背光，CPU 照跑）\n");
        return 0;
    }
    if (strcmp(sub, "on") == 0) {
        power_screen_on();
        printf("已亮屏\n");
        return 0;
    }
    if (strcmp(sub, "status") == 0) {
        printf("\n");
        power_dump();
        printf("\n");
        return 0;
    }
    printf("可用: power on|off|status\n");
    return 1;
}

/**
 * @brief time set <YYYY-MM-DD> <HH:MM:SS> —— 按 **UTC** 校时
 *
 * @note 和老的 `set_time` 区别：
 *       `set_time` 按**本地时间**（会按时区换算成 UTC 再写 RTC），
 *       `time set` 按 **UTC** 直接写。
 *       两个都留着：看你手上拿的是哪种时间。
 */
static int cmd_time_set(int argc, char **argv)
{
    /*
     * ⚠️ 参数下标要看清：esp_console 把
     *      "time set 2026-10-06 12:00:00"
     *    切成 argv[0]="time" argv[1]="set" argv[2]=日期 argv[3]=时间，
     *    也就是**下标是 2 和 3**，不是 1 和 2。
     *    我第一版按 1/2 取，结果把字符串 "set" 当日期去 sscanf，
     *    报"格式不对"——踩过一次，别再改回去。
     */
    if (argc < 4) {
        printf("用法: time set <YYYY-MM-DD> <HH:MM:SS>\n");
        printf("      填的是 **UTC**（set_time 那个按本地时间）\n");
        return 1;
    }

    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(argv[2], "%d-%d-%d", &y, &mo, &d) != 3 ||
        sscanf(argv[3], "%d:%d:%d", &hh, &mm, &ss) != 3) {
        printf("格式不对，应为 YYYY-MM-DD HH:MM:SS\n");
        return 1;
    }
    if (y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) {
        printf("数值超出范围\n");
        return 1;
    }

    /*
     * 走"日历 -> RTC 驱动 -> 时间戳"这条路，而不是自己算 epoch。
     *
     * 为什么：`set_time utc` 早就验证过这条路是通的；
     * 而我自己写的 timegm 等价物虽然数学上没问题（用 Python 对过），
     * 但**自己实现一份日期换算等于多一个出错点**。能复用就别重写。
     */
    rtc_pcf_time_t rt = { .year = y, .month = mo, .day = d,
                          .hour = hh, .minute = mm, .second = ss, .weekday = 1 };
    if (!rtc_pcf_time_is_valid(&rt)) {
        printf("时间字段非法: %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, hh, mm, ss);
        return 1;
    }
    rtc_pcf_set_time(&rt);              /* 借 RTC 驱动算时间戳 */
    const uint32_t epoch = rtc_pcf_get_epoch();

    esp_err_t err = data_hub_set_epoch(epoch);
    if (err == ESP_OK) {
        settings_set(SETTINGS_TIME_VALID, 1);
        printf("已按 UTC 校时: %04d-%02d-%02d %02d:%02d:%02d (epoch %u)\n",
               y, mo, d, hh, mm, ss, (unsigned)epoch);
    } else {
        printf("校时失败: %s\n", esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

/* ==================================================================
 * 注册与启动
 * ================================================================== */
/* ==================================================================
 * 命令：taskmem / stats / bt —— 运行时观测（v0.6.1 新增）
 *
 * 为什么加这三个（对标 Watch-Player_IDF 的串口控制台）：
 *   `taskmem` 是**最值钱**的一个 —— 它把每个任务的**栈峰值余量**打出来，
 *   于是"这个任务的栈该给多大"不再靠猜和试（本工程 data_hub 的采样任务
 *   上还跑着整条 UI 刷新链，V0.6.1 就是把它的栈从 3072 提到 4096 的，
 *   最终值应当由这里的实测数据决定）。
 *
 *   依赖 sdkconfig 的三项（见 sdkconfig.defaults 里的说明）：
 *     CONFIG_FREERTOS_USE_TRACE_FACILITY / GENERATE_RUN_TIME_STATS /
 *     VTASKLIST_INCLUDE_COREID
 * ================================================================== */

/** @brief 任务状态的中文名 */
static const char *task_state_str(eTaskState st)
{
    switch (st) {
    case eRunning:   return "运行";
    case eReady:     return "就绪";
    case eBlocked:   return "阻塞";
    case eSuspended: return "挂起";
    case eDeleted:   return "删";
    default:         return "?";
    }
}

/**
 * @brief taskmem [任务名] —— 各任务的栈余量
 *
 * @note 栈余量用 `uxTaskGetStackHighWaterMark()` 取，**单位是字节**
 *       （ESP-IDF 的文档明确：FreeRTOS 原生是字，IDF 改成了字节）。
 *       `TaskStatus_t.usStackHighWaterMark` 字段的单位没有明确说明，
 *       所以这里显式调 API，避免误读。
 */
static int cmd_taskmem(int argc, char **argv)
{
    const char *want = (argc > 1) ? argv[1] : NULL;

    const UBaseType_t cap = uxTaskGetNumberOfTasks() + 2;
    TaskStatus_t *arr = malloc(cap * sizeof(TaskStatus_t));
    if (!arr) {
        printf("内存不足\n");
        return 1;
    }

    uint32_t total = 0;
    const UBaseType_t n = uxTaskGetSystemState(arr, cap, &total);

    printf("任务              状态  优先 核    栈余量(字节)\n");
    printf("------------------------------------------------\n");
    int shown = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        if (want && strcmp(arr[i].pcTaskName, want) != 0) {
            continue;
        }
        const uint32_t hw = (uint32_t)uxTaskGetStackHighWaterMark(
                                (TaskHandle_t)arr[i].xHandle);
        /* 缓冲给足 12 字节：xCoreID 是 int，-O2 下 GCC 会按"最多 11 位数字"算，
         * 给 5 字节会被判成 -Werror=format-truncation（真踩过） */
        char core[12];
        if (arr[i].xCoreID == tskNO_AFFINITY) {
            snprintf(core, sizeof(core), "any");
        } else {
            snprintf(core, sizeof(core), "%d", (int)arr[i].xCoreID);
        }
        printf("%-16s  %-4s  %-4u %-4s  %u\n",
               arr[i].pcTaskName,
               task_state_str(arr[i].eCurrentState),
               (unsigned)arr[i].uxCurrentPriority,
               core, (unsigned)hw);
        shown++;
    }
    free(arr);

    if (want && shown == 0) {
        printf("没有叫 '%s' 的任务（不带参数看全部）\n", want);
        return 1;
    }
    printf("\n说明：栈余量是【历史最小值】，越小越危险；低于 512 字节就该给它加栈了。\n");
    return 0;
}

/** @brief 在两次采样之间按句柄配对（任务顺序可能变） */
static const TaskStatus_t *find_task(const TaskStatus_t *arr, UBaseType_t n,
                                     TaskHandle_t h)
{
    for (UBaseType_t i = 0; i < n; i++) {
        if (arr[i].xHandle == h) {
            return &arr[i];
        }
    }
    return NULL;
}

/**
 * @brief stats —— 各任务 CPU 占用（间隔 1 秒采两次算增量）
 *
 * @note 归一化方式：**按核**把该核上所有任务的运行时间增量加起来当分母，
 *       这样"当前正在跑的任务"不会算出 >100%（踩过这个坑的写法见参考工程）。
 *       代价：没有亲和性的任务（xCoreID = tskNO_AFFINITY）统一算进核 0，
 *       所以核 0 的总和会偏高一点 —— 看**相对大小**就好。
 */
static int cmd_stats(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const UBaseType_t cap = uxTaskGetNumberOfTasks() + 2;
    TaskStatus_t *a = malloc(cap * sizeof(TaskStatus_t));
    TaskStatus_t *b = malloc(cap * sizeof(TaskStatus_t));
    if (!a || !b) {
        free(a);
        free(b);
        printf("内存不足\n");
        return 1;
    }

    uint32_t t1 = 0, t2 = 0;
    const UBaseType_t n1 = uxTaskGetSystemState(a, cap, &t1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    const UBaseType_t n2 = uxTaskGetSystemState(b, cap, &t2);

    uint64_t core_sum[2] = { 0, 0 };
    for (UBaseType_t i = 0; i < n2; i++) {
        const TaskStatus_t *p = find_task(a, n1, b[i].xHandle);
        if (!p) {
            continue;
        }
        const uint32_t d = b[i].ulRunTimeCounter - p->ulRunTimeCounter;
        const int c = (b[i].xCoreID == tskNO_AFFINITY) ? 0 : (int)b[i].xCoreID;
        if (c >= 0 && c < 2) {
            core_sum[c] += d;
        }
    }

    printf("任务              CPU%%   优先 核\n");
    printf("-----------------------------------\n");
    for (UBaseType_t i = 0; i < n2; i++) {
        const TaskStatus_t *p = find_task(a, n1, b[i].xHandle);
        if (!p) {
            continue;                       /* 这 1 秒内新建的任务，跳过 */
        }
        const uint32_t d = b[i].ulRunTimeCounter - p->ulRunTimeCounter;
        const int c = (b[i].xCoreID == tskNO_AFFINITY) ? 0 : (int)b[i].xCoreID;
        const int cpu = (c >= 0 && c < 2 && core_sum[c])
                            ? (int)((uint64_t)d * 100 / core_sum[c]) : 0;
        printf("%-16s  %4d%%  %-4u %s\n", b[i].pcTaskName, cpu,
               (unsigned)b[i].uxCurrentPriority,
               (b[i].xCoreID == tskNO_AFFINITY) ? "any" : "");
    }
    free(a);
    free(b);

    printf("\n总运行时间计数 %u -> %u（单位 us；无亲和性的任务算进核 0）\n",
           (unsigned)t1, (unsigned)t2);
    return 0;
}

/**
 * @brief bt —— 打印所有任务的调用栈
 *
 * @note 用途：设备卡死/看门狗复位之后，先看这里"卡在哪个函数"。
 *       它内部会短暂挂起调度器取快照，属于**诊断**手段，别在正常运行时频繁调。
 *       （只支持"全部任务"，不支持按名字过滤：单个任务的栈回溯要自己走 TCB 取帧，
 *        收益不大、风险不小，暂不做。）
 */
static int cmd_bt(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        printf("本命令不支持指定任务名（直接敲 bt 会打印所有任务）\n");
        return 1;
    }
    printf("=== 所有任务的调用栈（depth=50）===\n");
    const esp_err_t err = esp_backtrace_print_all_tasks(50);
    if (err != ESP_OK) {
        printf("打印失败: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

/**
 * @brief crash —— **故意崩溃**，用来验证 coredump 通路
 *
 * @warning 会让设备立刻复位。只在验证 coredump 时用。
 *
 * 用法（v0.6.1 加的，为的是"以后崩了能查到现场"这件事本身可验证）：
 *   1. 串口敲 `crash`
 *   2. 设备打印回溯后复位
 *   3. 在 PC 工程目录跑 `idf.py coredump-info`
 *      → 应当能看到出错任务和调用栈
 */
static int cmd_crash(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("即将故意触发一次 panic（用于验证 coredump），设备会复位...\n");
    printf("复位后在 PC 上跑: idf.py coredump-info\n");
    fflush(stdout);

    esp_system_abort("crash 命令：故意触发的崩溃，用于验证 coredump 通路");
    return 0;   /* 不会走到这里 */
}

static void register_commands(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "time",           .help = "时间: time（看状态）/ time set <YYYY-MM-DD> <HH:MM:SS>（UTC校时）", .func = cmd_time },
        { .command = "set_time",       .help = "校时: set_time <YYYY-MM-DD> <HH:MM:SS> [utc]", .func = cmd_set_time },
        { .command = "battery",        .help = "打印电池电压（对照万用表）", .func = cmd_battery },
        { .command = "next",           .help = "翻到下一页（调试用）", .func = cmd_next },
        { .command = "pages",          .help = "当前页布局/滚动情况", .func = cmd_pages },
        { .command = "wifi",           .help = "WiFi: wifi on|status|scan|connect <SSID> [密码]|disconnect|forget|reset|sync", .func = cmd_wifi },
        { .command = "power",          .help = "屏幕: power on|off|status", .func = cmd_power },
        { .command = "rec",            .help = "IMU 录制: rec start|stop|status|list|get <文件>|erase", .func = cmd_rec },
        { .command = "imu_raw",        .help = "打印 IMU 原始码值: imu_raw [次数]", .func = cmd_imu_raw },
        { .command = "motion",         .help = "计步状态（步数/步频/信号强度）", .func = cmd_motion },
        { .command = "motion_watch",   .help = "连续打印计步遥测: motion_watch <秒>", .func = cmd_motion_watch },
        { .command = "steps_reset",    .help = "步数清零", .func = cmd_steps_reset },
        { .command = "brightness",     .help = "亮度: brightness <5-100>", .func = cmd_brightness },
        { .command = "settings",       .help = "打印所有设置项", .func = cmd_settings },
        { .command = "reset_settings", .help = "恢复出厂设置", .func = cmd_reset_settings },
        { .command = "status",         .help = "硬件自检 + 状态快照", .func = cmd_status },
        /* v0.6.1 新增：运行时观测（taskmem 用来定各任务的栈大小） */
        { .command = "taskmem",        .help = "任务栈余量: taskmem [任务名]", .func = cmd_taskmem },
        { .command = "stats",          .help = "各任务 CPU 占用（采两次，间隔 1 秒）", .func = cmd_stats },
        { .command = "bt",             .help = "打印所有任务的调用栈（卡死时用）", .func = cmd_bt },
        /* ⚠️ 会让你重启，仅调试用 */
        { .command = "crash",          .help = "故意崩溃（验证 coredump 用，会复位）", .func = cmd_crash },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "注册命令 '%s' 失败: %s", cmds[i].command,
                     esp_err_to_name(err));
        }
    }
}

esp_err_t app_console_start(void)
{
    /*
     * ⚠️ 这些字段一个都不能漏：
     *    max_history_len  = 0 -> IDF 内部 linenoiseHistorySetMaxLen(0) 失败
     *    task_stack_size  = 0 -> REPL 任务建成 0 栈，起来就崩
     *    task_priority    = 0 -> 优先级 0 的任务永远排不上
 *    task_stack_size  = 4096 也偏紧 -> 带 printf 的命令（rec status）会栈溢出重启
     * 所以下面显式给全，别依赖 memset(0) 的默认值。
     * （等价于 IDF 的 ESP_CONSOLE_REPL_CONFIG_DEFAULT()）
     */
    const esp_console_repl_config_t repl_cfg = {
        .max_history_len    = 16,
        .task_stack_size    = 6144,   /* 4096 对带 printf 的命令偏紧，见下方说明 */
        .task_priority      = 2,
        .max_cmdline_length = 128,
        .prompt             = "watch> ",
        .history_save_path  = NULL,     /* 不落盘，省 NVS 擦写 */
    };
    const esp_console_dev_usb_serial_jtag_config_t usb_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_console_repl_t *repl = NULL;
    /*
     * 走 USB-Serial-JTAG，不走 UART：
     * 这块板子的 Type-C 直接接在 ESP32-S3 原生 USB 上（Windows 认成
     * "USB JTAG/serial debug unit"），UART0 只引到扩展口、没接出来。
     * 详见 sdkconfig.defaults 里 Console 那一节的说明。
     */
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&usb_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建串口 REPL 失败: %s", esp_err_to_name(err));
        return err;
    }

    register_commands();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_register_help_command());

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动串口 REPL 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "命令行就绪（输入 help 看命令；校时用 set_time）");
    return ESP_OK;
}
