/**
 * @file buzzer.c
 * @brief 蜂鸣器实现 —— LEDC 出方波 + esp_timer 做"非阻塞音序器"
 *
 * 设计要点：
 *   ⚠️ **绝对不能在调用者任务里 vTaskDelay 等音播完** ——
 *      调用者常常是 LVGL 任务（触摸反馈音），卡住它界面就顿了。
 *   所以用一个 esp_timer 单次定时器驱动的两相状态机：
 *
 *        PHASE_ON --(on_ms)--> PHASE_OFF --(off_ms)--> 下一步 / 本步重复 / 结束
 *                                                        ↑ s_gap_pending 区分
 *
 *   整个过程零阻塞、零额外任务。
 */
#include "buzzer.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/ledc.h"
#include "board.h"

static const char *TAG = "BUZZER";

/* ==================== 硬件资源（别和背光撞） ==================== */
#define BZ_LEDC_TIMER      LEDC_TIMER_1
#define BZ_LEDC_CHANNEL    LEDC_CHANNEL_1
#define BZ_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define BZ_LEDC_RES        LEDC_TIMER_10_BIT     /* 1023 级：约 78Hz~40kHz */
#define BZ_DUTY_50PCT      512                   /* 方波 50% 占空比最响 */
#define BZ_DEFAULT_FREQ    2700                  /* 板上蜂鸣器谐振点附近 */

/* ==================== 预设音序表 ====================
 * 一声 = { 频率, 响多久, 之后静多久 }；repeat = 这一声重复几遍。
 * 表尾用 freq_hz == 0 作为结束哨兵。 */
typedef struct {
    uint32_t freq_hz;
    uint16_t on_ms;
    uint16_t off_ms;
    uint8_t  repeat;
} buzzer_step_t;

#define BZ_MAX_STEPS  3

static const buzzer_step_t s_patterns[BUZZER_BEEP_COUNT][BZ_MAX_STEPS] = {
    /* TICK   */ { { 2700,  20,   0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },
    /* OK     */ { { 2700,  60,   0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },
    /* DOUBLE */ { { 2700,  60,  70, 2 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },
    /* LONG   */ { { 2700, 500,   0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },
    /* ERROR  */ { { 3000,  70,  80, 3 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },
};

/* ==================== 内部状态 ==================== */
typedef enum { PHASE_OFF = 0, PHASE_ON } tone_phase_t;

static bool               s_ready = false;
static esp_timer_handle_t s_timer = NULL;

/* 音序器状态 */
static const buzzer_step_t *s_seq         = NULL;   /* NULL = 手动单音（无音序）模式 */
static int                  s_seq_len     = 0;
static int                  s_step        = 0;      /* 当前第几步（0-based） */
static int                  s_repeat_left = 0;      /* 本步还要响几声 */
static tone_phase_t         s_phase       = PHASE_OFF;
static bool                 s_gap_pending = false;  /* true = 前端间隔结束后**进入下一步** */
static bool                 s_playing     = false;

/* ==================== 底层：起音 / 停音 ==================== */
static void hw_tone_on(uint32_t freq_hz)
{
    esp_err_t err = ledc_set_freq(BZ_LEDC_MODE, BZ_LEDC_TIMER, freq_hz);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "频率 %" PRIu32 "Hz 设置失败(%s)，回退到 %dHz",
                 freq_hz, esp_err_to_name(err), BZ_DEFAULT_FREQ);
        ledc_set_freq(BZ_LEDC_MODE, BZ_LEDC_TIMER, BZ_DEFAULT_FREQ);
    }
    ledc_set_duty(BZ_LEDC_MODE, BZ_LEDC_CHANNEL, BZ_DUTY_50PCT);
    ledc_update_duty(BZ_LEDC_MODE, BZ_LEDC_CHANNEL);
}

static void hw_tone_off(void)
{
    ledc_set_duty(BZ_LEDC_MODE, BZ_LEDC_CHANNEL, 0);
    ledc_update_duty(BZ_LEDC_MODE, BZ_LEDC_CHANNEL);
}

/* ==================== 音序器 ==================== */
static void seq_stop(void)
{
    if (s_timer) {
        esp_timer_stop(s_timer);
    }
    hw_tone_off();
    s_seq         = NULL;
    s_seq_len     = 0;
    s_step        = 0;
    s_repeat_left = 0;
    s_phase       = PHASE_OFF;
    s_gap_pending = false;
    s_playing     = false;
}

/** @brief 启动单次定时器；ms 兜底到 1（esp_timer 不接受 0） */
static void seq_schedule(uint32_t ms)
{
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, (uint64_t)(ms ? ms : 1) * 1000ULL);
}

/** @brief 起第 step 步的第一声 */
static void seq_start_step(int step)
{
    s_step        = step;
    s_repeat_left = s_seq[step].repeat;
    s_phase       = PHASE_ON;

    hw_tone_on(s_seq[step].freq_hz);
    seq_schedule(s_seq[step].on_ms);
}

/** @brief 进入下一步；没有下一步就收工 */
static void seq_next_step(void)
{
    const int next = s_step + 1;
    if (!s_seq || next >= s_seq_len) {
        seq_stop();
        return;
    }
    seq_start_step(next);
}

/**
 * @brief 定时器回调：两相状态机
 */
static void timer_cb(void *arg)
{
    (void)arg;

    /* ---- 手动单音模式（没有音序）：到点直接关音 ---- */
    if (!s_seq) {
        if (s_phase == PHASE_ON) {
            hw_tone_off();
            s_phase   = PHASE_OFF;
            s_playing = false;
        }
        return;
    }

    if (s_phase == PHASE_ON) {
        /* 一声响完 → 关音，然后安排间隔 */
        hw_tone_off();
        s_phase = PHASE_OFF;

        if (s_repeat_left > 1) {
            /* 本步还要再响：间隔后**重复本步** */
            s_repeat_left--;
            s_gap_pending = false;
        } else {
            /* 本步最后一声：间隔后**进入下一步** */
            s_repeat_left = 0;
            s_gap_pending = true;
        }

        const uint16_t gap = s_seq[s_step].off_ms;
        if (gap) {
            seq_schedule(gap);
        } else if (s_gap_pending) {
            seq_next_step();
        } else {
            seq_start_step(s_step);         /* 无间隔的连续重复（预设里不会出现） */
        }
        return;
    }

    /* ---- PHASE_OFF：间隔结束 ---- */
    if (s_gap_pending) {
        seq_next_step();
    } else if (s_repeat_left > 0) {
        /* 回到本步的发声相（不计入 repeat，因为上面已经减过） */
        s_phase = PHASE_ON;
        hw_tone_on(s_seq[s_step].freq_hz);
        seq_schedule(s_seq[s_step].on_ms);
        /* 这次还要按"最后一声"逻辑收尾：再减一次 */
        s_repeat_left--;
        if (s_repeat_left == 0) {
            s_gap_pending = true;
        }
    } else {
        seq_next_step();
    }
}

/* ==================== 对外接口 ==================== */
esp_err_t buzzer_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const ledc_timer_config_t tcfg = {
        .speed_mode      = BZ_LEDC_MODE,
        .timer_num       = BZ_LEDC_TIMER,
        .duty_resolution = BZ_LEDC_RES,
        .freq_hz         = BZ_DEFAULT_FREQ,
        /* ★ 第二档 DFS：同背光 —— 用晶振做时钟源，音调不受 CPU 降频影响
         *   （`ledc_set_freq()` 是按"当前时钟"重算分频的，APB 一变音调就跑）*/
        .clk_cfg         = LEDC_USE_XTAL_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "LEDC 定时器配置失败");

    const ledc_channel_config_t ccfg = {
        .gpio_num   = BOARD_BUZZER_GPIO,
        .speed_mode = BZ_LEDC_MODE,
        .channel    = BZ_LEDC_CHANNEL,
        .timer_sel  = BZ_LEDC_TIMER,
        .duty       = 0,                    /* 上电先静音 */
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "LEDC 通道配置失败");

    const esp_timer_create_args_t targs = {
        .callback = timer_cb,
        .arg      = NULL,
        .name     = "buzzer",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_timer), TAG, "蜂鸣器定时器创建失败");

    s_ready = true;
    ESP_LOGI(TAG, "蜂鸣器就绪（GPIO%d，LEDC TIMER_%d/CH%d，默认 %dHz）",
             BOARD_BUZZER_GPIO, BZ_LEDC_TIMER, BZ_LEDC_CHANNEL, BZ_DEFAULT_FREQ);
    return ESP_OK;
}

esp_err_t buzzer_tone(uint32_t freq_hz)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "蜂鸣器未初始化");
    ESP_RETURN_ON_FALSE(freq_hz >= 100 && freq_hz <= 20000, ESP_ERR_INVALID_ARG, TAG,
                        "频率 %" PRIu32 " 超出 100~20000Hz", freq_hz);

    seq_stop();                         /* 取消正在进行的音序 */
    hw_tone_on(freq_hz);
    s_phase   = PHASE_ON;
    s_playing = true;
    return ESP_OK;
}

esp_err_t buzzer_stop(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    seq_stop();
    return ESP_OK;
}

esp_err_t buzzer_tone_ms(uint32_t freq_hz, uint32_t duration_ms)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "蜂鸣器未初始化");
    ESP_RETURN_ON_FALSE(freq_hz >= 100 && freq_hz <= 20000, ESP_ERR_INVALID_ARG, TAG,
                        "频率 %" PRIu32 " 超出 100~20000Hz", freq_hz);
    ESP_RETURN_ON_FALSE(duration_ms > 0, ESP_ERR_INVALID_ARG, TAG, "时长必须 > 0");

    seq_stop();
    hw_tone_on(freq_hz);
    s_phase   = PHASE_ON;               /* s_seq 保持 NULL → 回调到点关音 */
    s_playing = true;
    seq_schedule(duration_ms);
    return ESP_OK;
}

esp_err_t buzzer_beep(buzzer_beep_t kind)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "蜂鸣器未初始化");
    ESP_RETURN_ON_FALSE(kind >= 0 && kind < BUZZER_BEEP_COUNT, ESP_ERR_INVALID_ARG, TAG,
                        "提示音编号越界: %d", (int)kind);

    /* 数一下这个预设里有几步（表尾 freq_hz == 0 作为哨兵） */
    int len = 0;
    while (len < BZ_MAX_STEPS && s_patterns[kind][len].freq_hz != 0) {
        len++;
    }
    ESP_RETURN_ON_FALSE(len > 0, ESP_FAIL, TAG, "预设 %d 是空音序", (int)kind);

    seq_stop();

    s_seq         = s_patterns[kind];
    s_seq_len     = len;
    s_repeat_left = 0;
    s_step        = 0;
    s_gap_pending = false;
    s_playing     = true;

    seq_start_step(0);
    return ESP_OK;
}

bool buzzer_is_playing(void)
{
    return s_playing;
}
