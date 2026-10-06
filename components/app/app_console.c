/**
 * @file app_console.c
 * @brief 串口命令行实现（esp_console + linenoise，UART0 115200）
 *
 * 实现说明：
 *   - 用 `esp_console_new_repl_uart()` 一把梭：它会装 UART 驱动、
 *     把 stdin/stdout 接到驱动、起一个 REPL 任务，然后立刻返回。
 *     所以**不会阻塞 app_start()**。
 *   - 命令解析用 sscanf 手工做，比 argtable 直观，
 *     而且时间格式（YYYY-MM-DD HH:MM:SS）用 sscanf 一行就能拆。
 *   - 所有命令都在 REPL 自己的任务里跑，**不要在里面碰 lv_xxx**。
 */
#include "app_console.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "bsp.h"
#include "settings.h"
#include "data_hub.h"
#include "battery.h"
#include <inttypes.h>
#include "rtc_pcf85063.h"
#include "motion.h"

static const char *TAG = "CONSOLE";

/* ==================================================================
 * 命令：time —— 看当前状态
 * ================================================================== */
static int cmd_time(int argc, char **argv)
{
    (void)argc; (void)argv;

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
 * 注册与启动
 * ================================================================== */
static void register_commands(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "time",           .help = "显示时间/电量/运行状态", .func = cmd_time },
        { .command = "set_time",       .help = "校时: set_time <YYYY-MM-DD> <HH:MM:SS> [utc]", .func = cmd_set_time },
        { .command = "battery",        .help = "打印电池电压（对照万用表）", .func = cmd_battery },
        { .command = "motion",         .help = "计步状态（步数/步频/信号强度）", .func = cmd_motion },
        { .command = "motion_watch",   .help = "连续打印计步遥测: motion_watch <秒>", .func = cmd_motion_watch },
        { .command = "steps_reset",    .help = "步数清零", .func = cmd_steps_reset },
        { .command = "brightness",     .help = "亮度: brightness <5-100>", .func = cmd_brightness },
        { .command = "settings",       .help = "打印所有设置项", .func = cmd_settings },
        { .command = "reset_settings", .help = "恢复出厂设置", .func = cmd_reset_settings },
        { .command = "status",         .help = "硬件自检 + 状态快照", .func = cmd_status },
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
     * 所以下面显式给全，别依赖 memset(0) 的默认值。
     * （等价于 IDF 的 ESP_CONSOLE_REPL_CONFIG_DEFAULT()）
     */
    const esp_console_repl_config_t repl_cfg = {
        .max_history_len    = 16,
        .task_stack_size    = 4096,
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
