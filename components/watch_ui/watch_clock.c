/**
 * @file watch_clock.c
 * @brief 时钟服务实现（秒表 / 倒计时 / 闹钟）
 *
 * 三块逻辑都不碰 LVGL 控件，只维护状态 + 判断要不要提醒。
 * 界面由 watch_page_clock.c 渲染，提醒层由 watch_alert.c 弹。
 */
#include "watch_clock.h"

#include <stdio.h>
#include "lvgl.h"
#include "esp_log.h"
#include "watch_platform.h"
#include "watch_alert.h"
#include "watch_pages.h"    /* watch_ui_setting_get/set + W_SETTING_* 键名 */
#include "ui_port.h"        /* ui_port_critical_enter/exit：跨任务短临界区（v0.6.1） */

static const char *TAG = "CLOCK";

/* 蜂鸣器频率：板上蜂鸣器的谐振点，最响 */
#define CLOCK_BEEP_HZ        2700
/* 响铃节奏：每 500ms 响 300ms（用户指定） */
#define CLOCK_RING_PERIOD_MS 500
#define CLOCK_RING_ON_MS     300

/* ==================================================================
 * 状态
 * ================================================================== */

/* ---------- 秒表 ---------- */
static bool     s_sw_run    = false;
static uint32_t s_sw_start  = 0;      /* 本次启动时的 lv_tick */
static uint32_t s_sw_accum  = 0;      /* 之前累计的毫秒 */

/* ---------- 倒计时 ---------- */
static int  s_cd_set_sec    = 300;    /* 设定时长 */
static bool s_cd_run        = false;
static uint32_t s_cd_start  = 0;      /* 本次启动时的 lv_tick */
static uint32_t s_cd_accum  = 0;      /* 之前已经倒数掉的毫秒 */
static int  s_cd_ring_sec   = 10;

/* ---------- 闹钟 ---------- */
static int  s_al_hour       = 7;
static int  s_al_min        = 0;
static bool s_al_en         = false;
/*
 * 防重复触发：记录"上一次已经触发过的 hh:mm"。
 * 每秒都会被喂一次时间，同一分钟内会进来 60 次，不去重就会响 60 遍。
 */
static int  s_al_fired_hhmm = -1;

/* ---------- 提醒 ---------- */
static w_clock_alert_t s_alert      = W_CLOCK_ALERT_NONE;
static uint32_t        s_alert_ms   = 0;   /* 响铃已持续毫秒（倒计时用来自动停） */
static bool            s_alert_beep = false;  /* 本周期蜂鸣器是否为"响"状态 */

/* ---------- 时间有效性 ---------- */
static bool s_time_ok   = false;
static int  s_last_hour = 0;
static int  s_last_min  = 0;

/* ---------- 全局定时器与界面刷新回调 ---------- */
static lv_timer_t *s_timer      = NULL;
static uint32_t    s_timer_period = 0;   /* 自己记周期：LVGL 8.4 没有 lv_timer_get_period */
static void      (*s_ui_refresh)(void) = NULL;

static bool s_inited = false;

/* ==================================================================
 * 全局定时器
 *
 * ★ 为什么必须放在服务里、而且必须是全局的：
 *   用户完全可能"起了倒计时就翻到别的页面去了"。如果定时器挂在
 *   时钟页上，页面一关倒计时就永远不触发 —— 那闹钟/倒计时就是废的。
 *
 * ★ 周期自适应：只有"秒表在跑 / 倒计时在跑 / 正在提醒"时才需要
 *   100ms 的精度；空闲时降到 1000ms（少 9 倍的唤醒，省电）。
 *   周期自己用 s_timer_period 记 —— LVGL 8.4 只有 lv_timer_set_period，
 *   **没有** lv_timer_get_period（查过 lv_timer.h）。
 * ================================================================== */
static void clock_timer_cb(lv_timer_t *t)
{
    const bool busy = s_sw_run || s_cd_run || watch_clock_alerting();
    const uint32_t want = busy ? 100 : 1000;
    if (s_timer_period != want) {
        s_timer_period = want;
        lv_timer_set_period(t, want);
    }

    watch_clock_tick();

    /* 让时钟页重画数字（页面不存在时回调为空，安全） */
    if (s_ui_refresh) {
        s_ui_refresh();
    }

    /* 提醒层的显隐/文案跟着服务状态走 */
    watch_alert_poll();
}

void watch_clock_set_ui_refresh(void (*cb)(void))
{
    s_ui_refresh = cb;
}

/* ==================================================================
 * 初始化
 * ================================================================== */
void watch_clock_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;

    /* 从 NVS 恢复（走设置钩子；模拟器上钩子没接，直接拿默认值） */
    s_al_hour     = watch_ui_setting_get(W_SETTING_ALARM_HOUR, 7);
    s_al_min      = watch_ui_setting_get(W_SETTING_ALARM_MIN, 0);
    s_al_en       = watch_ui_setting_get(W_SETTING_ALARM_EN, 0) != 0;
    s_cd_set_sec  = watch_ui_setting_get(W_SETTING_CD_PRESET, 300);
    s_cd_ring_sec = watch_ui_setting_get(W_SETTING_CD_RING, 10);

    if (s_cd_set_sec < 1)   s_cd_set_sec = 300;
    if (s_cd_ring_sec < 1)  s_cd_ring_sec = 10;

    /* 开机时闹钟处于"已触发过"状态：这样"刚开机正好是闹钟那一分钟"
     * 不会立刻吵起来，等这一分钟过去后自然解除。
     * （用户改了时间/开关时也会重新置位，见 alarm_arm()） */
    s_al_fired_hhmm = -1;

    if (!s_timer) {
        s_timer_period = 1000;
        s_timer = lv_timer_create(clock_timer_cb, s_timer_period, NULL);
    }

    ESP_LOGI(TAG, "时钟服务就绪：闹钟 %02d:%02d(%s)，倒计时 %d 秒，响铃 %d 秒",
             s_al_hour, s_al_min, s_al_en ? "开" : "关",
             s_cd_set_sec, s_cd_ring_sec);
}

/** @brief 重新武装防重复：让闹钟要等"下一分钟"才会响 */
static void alarm_arm(void)
{
    s_al_fired_hhmm = -1;
}

/* ==================================================================
 * 秒表
 * ================================================================== */
void watch_clock_sw_toggle(void)
{
    if (s_sw_run) {
        /* 暂停：把这一段累计进去 */
        s_sw_accum += (uint32_t)(lv_tick_get() - s_sw_start);
        s_sw_run = false;
    } else {
        s_sw_start = (uint32_t)lv_tick_get();
        s_sw_run   = true;
    }
}

void watch_clock_sw_reset(void)
{
    s_sw_run   = false;
    s_sw_accum = 0;
    s_sw_start = 0;
}

bool watch_clock_sw_running(void)
{
    return s_sw_run;
}

uint32_t watch_clock_sw_elapsed_ms(void)
{
    if (!s_sw_run) {
        return s_sw_accum;
    }
    /* 无符号减法天然处理 lv_tick 回绕 */
    return s_sw_accum + (uint32_t)(lv_tick_get() - s_sw_start);
}

/* ==================================================================
 * 倒计时
 * ================================================================== */
void watch_clock_cd_set_sec(int sec)
{
    if (sec < 1)     sec = 1;
    if (sec > 86399) sec = 86399;

    s_cd_set_sec = sec;

    /* 设定新时长就复位（正在倒数也要停 —— 语义最不容易误解） */
    s_cd_run   = false;
    s_cd_accum = 0;

    watch_ui_setting_set(W_SETTING_CD_PRESET, sec);
    ESP_LOGI(TAG, "倒计时时长设为 %d 秒", sec);
}

int watch_clock_cd_sec(void)
{
    return s_cd_set_sec;
}

void watch_clock_cd_toggle(void)
{
    if (s_cd_run) {
        s_cd_accum += (uint32_t)(lv_tick_get() - s_cd_start);
        s_cd_run = false;
    } else {
        /* 已经走完了？从满值重新开始，避免"点开始立刻结束" */
        if (s_cd_accum >= (uint32_t)s_cd_set_sec * 1000u) {
            s_cd_accum = 0;
        }
        s_cd_start = (uint32_t)lv_tick_get();
        s_cd_run   = true;
    }
}

void watch_clock_cd_reset(void)
{
    s_cd_run   = false;
    s_cd_accum = 0;
    s_cd_start = 0;
}

bool watch_clock_cd_running(void)
{
    return s_cd_run;
}

/** @brief 已倒数掉的毫秒 */
static uint32_t cd_elapsed_ms(void)
{
    if (!s_cd_run) {
        return s_cd_accum;
    }
    return s_cd_accum + (uint32_t)(lv_tick_get() - s_cd_start);
}

int watch_clock_cd_remaining_sec(void)
{
    const int32_t total = s_cd_set_sec * 1000;
    int32_t left = total - (int32_t)cd_elapsed_ms();
    if (left <= 0) {
        return 0;
    }
    /* 向上取整：4.2 秒显示 "5" 更符合倒数的手感 */
    return (left + 999) / 1000;
}

void watch_clock_cd_set_ring_sec(int sec)
{
    if (sec < 1)   sec = 1;
    if (sec > 300) sec = 300;
    s_cd_ring_sec = sec;
    watch_ui_setting_set(W_SETTING_CD_RING, sec);
}

int watch_clock_cd_ring_sec(void)
{
    return s_cd_ring_sec;
}

/* ==================================================================
 * 闹钟
 * ================================================================== */
void watch_clock_alarm_set(int hour, int minute)
{
    if (hour < 0)   hour = 0;
    if (hour > 23)  hour = 23;
    if (minute < 0) minute = 0;
    if (minute > 59) minute = 59;

    s_al_hour = hour;
    s_al_min  = minute;

    watch_ui_setting_set(W_SETTING_ALARM_HOUR, hour);
    watch_ui_setting_set(W_SETTING_ALARM_MIN, minute);

    /* 刚设完不要立刻响：等这一分钟过去 */
    alarm_arm();
    ESP_LOGI(TAG, "闹钟设为 %02d:%02d", hour, minute);
}

int watch_clock_alarm_hour(void) { return s_al_hour; }
int watch_clock_alarm_min(void)  { return s_al_min; }

void watch_clock_alarm_enable(bool on)
{
    s_al_en = on;
    watch_ui_setting_set(W_SETTING_ALARM_EN, on ? 1 : 0);
    alarm_arm();        /* 启用时同样等下一分钟 */
    ESP_LOGI(TAG, "闹钟%s", on ? "启用" : "停用");
}

bool watch_clock_alarm_enabled(void) { return s_al_en; }
bool watch_clock_time_ok(void)       { return s_time_ok; }

bool watch_clock_last_time(int *hour, int *minute)
{
    /*
     * ★ v0.6.1：这几个变量是**跨任务**的 ——
     *   `watch_clock_on_time()` 跑在 data_hub 采样任务上（见 watch_pages.c
     *   里"必须在拿 LVGL 锁之前喂时间"的说明），而这里被 LVGL 任务调用。
     *   所以读要包在临界区里，读到的一组值才是同一时刻的。
     */
    ui_port_critical_enter();
    const int  h  = s_last_hour;
    const int  m  = s_last_min;
    const bool ok = s_time_ok;
    ui_port_critical_exit();

    if (hour)   *hour   = h;
    if (minute) *minute = m;
    return ok;
}

/* ==================================================================
 * 提醒
 * ================================================================== */
bool watch_clock_alerting(void)
{
    ui_port_critical_enter();
    const bool on = (s_alert != W_CLOCK_ALERT_NONE);
    ui_port_critical_exit();
    return on;
}

w_clock_alert_t watch_clock_alert_kind(void)
{
    ui_port_critical_enter();
    const w_clock_alert_t k = s_alert;
    ui_port_critical_exit();
    return k;
}

/** @brief 开始一次提醒
 *
 *  @note v0.6.1：状态改写包在临界区里（可能和 hub 任务的时间回调并发），
 *        但**亮屏和日志都放在临界区外** —— 它们会取别的锁。 */
static void alert_start(w_clock_alert_t kind)
{
    ui_port_critical_enter();
    const bool started = (s_alert == W_CLOCK_ALERT_NONE);
    if (started) {
        s_alert      = kind;
        s_alert_ms   = 0;
        s_alert_beep = false;
    }
    ui_port_critical_exit();

    if (!started) {
        return;                      /* 已经在提醒了，别叠加 */
    }

    /* 立刻亮屏：很可能屏幕已经息了，否则用户看不到提示 */
    w_screen_wake();

    ESP_LOGI(TAG, "开始提醒：%s", kind == W_CLOCK_ALERT_ALARM ? "闹钟" : "倒计时");
}

void watch_clock_alert_stop(void)
{
    /*
     * ★ v0.6.1：判断和清状态必须在**同一个**临界区里完成。
     *   原先写成"先判断、再清"，中间那一小段正是 hub 任务
     *   可能刚设好 s_alert 的窗口 —— 会把一次刚起来的提醒抹掉。
     */
    ui_port_critical_enter();
    const bool had = (s_alert != W_CLOCK_ALERT_NONE);
    s_alert      = W_CLOCK_ALERT_NONE;
    s_alert_beep = false;
    ui_port_critical_exit();

    if (!had) {
        return;
    }
    w_buzzer_stop();          /* 硬件调用在临界区外 */
    ESP_LOGI(TAG, "提醒已停止");
}

/**
 * @brief 推进响铃节奏：每 500ms 响 300ms
 *
 * 为什么不用持续单音：2.7kHz 一直响很刺耳，"嘀…嘀…" 更像闹钟，
 * 而且省电（蜂鸣器是耗电件）。节奏是用户指定的。
 */
static void alert_pump(uint32_t dt_ms)
{
    bool beep_on  = false;      /* 需要起音 */
    bool beep_off = false;      /* 需要停声 */
    bool run      = false;

    /*
     * ★ v0.6.1：这一整段状态推进放在临界区里（s_alert / s_alert_ms /
     *   s_alert_beep 会被 hub 任务的时间回调并发写），
     *   但蜂鸣器调用一律挪到临界区外 —— 它走平台钩子，可能在别的锁上等。
     */
    ui_port_critical_enter();
    if (s_alert != W_CLOCK_ALERT_NONE) {
        run = true;
        s_alert_ms += dt_ms;

        /* 倒计时：响够"响铃时长"就自动停（闹钟不自动停，等用户点停止） */
        if (s_alert == W_CLOCK_ALERT_COUNTDOWN &&
            s_alert_ms >= (uint32_t)s_cd_ring_sec * 1000u) {
            s_alert_beep = false;
            beep_off     = true;
            /*
             * 注意：**只停声音，不清 s_alert**。
             * 用户要求提示层继续留在屏幕上，等他点【停止】才关
             * （见 watch_alert.c）。所以这里用 s_alert_beep 表达"响完了"。
             */
        } else {
            const uint32_t phase = s_alert_ms % CLOCK_RING_PERIOD_MS;
            const bool want_beep = (phase < CLOCK_RING_ON_MS);
            if (want_beep != s_alert_beep) {
                s_alert_beep = want_beep;
                beep_on      = want_beep;
                beep_off     = !want_beep;
            }
        }
    }
    ui_port_critical_exit();

    if (!run) {
        return;
    }
    if (beep_on) {
        w_buzzer_tone(CLOCK_BEEP_HZ);
    }
    if (beep_off) {
        w_buzzer_stop();
    }
}

/** @brief 提醒用的响铃是否已经响完（倒计时会自动停；闹钟不会） */
bool watch_clock_alert_ring_done(void)
{
    ui_port_critical_enter();
    const bool done = (s_alert == W_CLOCK_ALERT_COUNTDOWN) &&
                      (s_alert_ms >= (uint32_t)s_cd_ring_sec * 1000u);
    ui_port_critical_exit();
    return done;
}

/* ==================================================================
 * 每秒时间回调（闹钟比较）
 * ================================================================== */
void watch_clock_on_time(const watch_time_t *t)
{
    if (!t) {
        return;
    }

    bool fire = false;

    /*
     * ★ v0.6.1：整段判定放进临界区。
     *
     *   本函数跑在 **data_hub 采样任务**上（watch_pages.c 刻意把它放在
     *   拿 LVGL 锁之前 —— 闹钟按"时:分"比较，每秒必须喂一次、一次都不能漏），
     *   而 s_alert / s_alert_ms / s_alert_beep 这一组同时被 LVGL 任务的
     *   alert_pump() / watch_clock_alert_stop() 改。
     *
     *   临界区里只做赋值和比较：日志、亮屏、蜂鸣器全部在外面。
     *   另外 `alert_start()` 自己也要进临界区，而 portMUX **不是递归锁**，
     *   所以这里只记下"要触发"，出了临界区再调它。
     */
    ui_port_critical_enter();
    s_time_ok   = t->valid;
    s_last_hour = t->hour;
    s_last_min  = t->minute;

    if (t->valid && s_al_en) {          /* 没校时：闹钟不生效（界面会提示） */
        const int hhmm = t->hour * 60 + t->minute;

        /* 已经过了闹钟那一分钟 -> 重新武装，这样"每天重复"就成立了 */
        if (hhmm != s_al_hour * 60 + s_al_min) {
            s_al_fired_hhmm = -1;
        } else if (s_al_fired_hhmm != hhmm) {
            /* 正好落在闹钟那一分钟，且这一分钟还没响过 */
            s_al_fired_hhmm = hhmm;
            fire            = true;
        }
    }
    ui_port_critical_exit();

    if (fire) {
        ESP_LOGI(TAG, "闹钟触发 %02d:%02d", t->hour, t->minute);
        alert_start(W_CLOCK_ALERT_ALARM);
    }
}

/* ==================================================================
 * 周期 tick
 * ================================================================== */
void watch_clock_tick(void)
{
    static uint32_t s_last_ms = 0;
    const uint32_t now = (uint32_t)lv_tick_get();

    /* 第一次 tick：只记基准，不算 dt（否则会拿一个巨大的假 dt） */
    if (s_last_ms == 0) {
        s_last_ms = now;
        return;
    }
    const uint32_t dt = (uint32_t)(now - s_last_ms);
    s_last_ms = now;

    /* ---------- 倒计时到点？ ---------- */
    if (s_cd_run) {
        if (cd_elapsed_ms() >= (uint32_t)s_cd_set_sec * 1000u) {
            /* 走完就停住（累加值夹到满值，避免 remaining 变负） */
            s_cd_accum = (uint32_t)s_cd_set_sec * 1000u;
            s_cd_run   = false;
            ESP_LOGI(TAG, "倒计时结束");
            alert_start(W_CLOCK_ALERT_COUNTDOWN);
        }
    }

    /* ---------- 推进响铃节奏 ---------- */
    alert_pump(dt);
}

/* ==================================================================
 * 格式化小工具（页面共用）
 * ================================================================== */
void watch_clock_fmt_stopwatch(uint32_t ms, char *buf, int len)
{
    if (!buf || len < 9) {
        return;
    }
    const uint32_t tenths = (ms / 100u) % 10u;
    const uint32_t secs   = ms / 1000u;
    snprintf(buf, len, "%02u:%02u.%u",
             (unsigned)((secs / 60u) % 100u), (unsigned)(secs % 60u),
             (unsigned)tenths);
}

void watch_clock_fmt_mmss(int total_sec, char *buf, int len)
{
    if (!buf || len < 10) {
        return;
    }
    if (total_sec < 0) {
        total_sec = 0;
    }
    const int h = total_sec / 3600;
    const int m = (total_sec / 60) % 60;
    const int s = total_sec % 60;
    if (h > 0) {
        snprintf(buf, len, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf(buf, len, "%02d:%02d", m, s);
    }
}
