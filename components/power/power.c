/**
 * @file power.c
 * @brief 屏幕/背光管理实现
 *
 * ## 息屏到底"关"了什么
 *
 * 只调 `bsp_backlight_set(0)`。CPU、LVGL、IMU 采样任务全在跑。
 * 这是本版本的**刻意取舍**（用户要求先不做低功耗）：
 *   - 好处：没有"睡醒"相关的坑（外设复位、任务挂起、唤醒源配置）
 *   - 代价：静态功耗没降，但背光本来是耗电大头，关掉它已经省了大部分
 *
 * ## 息屏后第一次触摸怎么"吃掉"
 *
 * LVGL 的输入事件不好在中间拦（读点在 `lv_timer_handler` 里）。
 * 所以走**轮询判断**：定时器里检查
 *
 *     屏幕是黑的？ && 触摸控制器报告有按下？ -> 亮屏 + 记录时间戳
 *
 * 然后在 `W_IGNORE_TOUCH_MS` 这段时间里，把 LVGL 的输入设备**临时禁用**
 * （`lv_indev_enable(false)`），这一下就不会传到界面。
 * 抬手后再启用 —— 用户第二下就能正常操作。
 */
#include "power.h"

#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "bsp.h"
#include "board.h"
#include "settings.h"
#include "watch_platform.h"     /* 注册界面钩子 */
#include "watch_pages.h"        /* watch_ui_set_setting_hooks 的同族 */

static const char *TAG = "POWER";

/* 检测周期：50ms 够灵敏，又不会太占 CPU */
#define POWER_POLL_MS        50

/* 亮屏后屏蔽输入的时长（ms）。太短会漏吃掉那一下，太长会觉得"点不动" */
#define W_IGNORE_TOUCH_MS    350

/* ==================== 状态 ==================== */
static bool     s_screen_on   = true;
static int32_t  s_hold_count  = 0;       /* "别息屏"引用计数 */
static int64_t  s_last_active = 0;       /* 上次用户活动时刻（us） */
static int64_t  s_wake_at     = 0;       /* 本次亮屏时刻（us）；0 = 没在屏蔽期 */
static esp_timer_handle_t s_poll = NULL;

/* ==================== 内部 ==================== */
static inline int64_t now_us(void)
{
    return esp_timer_get_time();
}

/** @brief 取自动息屏秒数（0 = 常亮） */
static int timeout_sec(void)
{
    return (int)settings_get_or_default(SETTINGS_SCREEN_TIMEOUT);
}

static void backlight(bool on)
{
    if (on) {
        const int bright = (int)settings_get_or_default(SETTINGS_BRIGHTNESS);
        bsp_backlight_set(bright > 5 ? bright : 5);
    } else {
        bsp_backlight_set(0);
    }
}

void power_screen_on(void)
{
    s_last_active = now_us();
    if (s_screen_on) {
        return;
    }
    backlight(true);
    s_screen_on = true;
    s_wake_at   = now_us();     /* 进入"吃掉第一下"的屏蔽期 */
    ESP_LOGI(TAG, "亮屏");
}

void power_screen_off(void)
{
    if (!s_screen_on) {
        return;
    }
    backlight(false);
    s_screen_on = false;
    ESP_LOGI(TAG, "息屏（只关背光，CPU 照跑）");
}

bool power_screen_is_on(void)
{
    return s_screen_on;
}

void power_hold_awake(bool hold)
{
    if (hold) {
        s_hold_count++;
    } else if (s_hold_count > 0) {
        s_hold_count--;
        /* 计数归零后重新计时，而不是立刻黑屏 */
        s_last_active = now_us();
    }
    ESP_LOGI(TAG, "别息屏计数 = %" PRId32, s_hold_count);
}

int power_hold_count(void)
{
    return (int)s_hold_count;
}

void power_notify_activity(void)
{
    s_last_active = now_us();
}

/* ==================== 轮询 ==================== */
/**
 * @brief 检查触摸控制器是否有按下
 *
 * @note 这里**直接读 CST816T 的中断引脚**（GPIO14，低有效），
 *       而不是走 LVGL 的输入设备 —— 因为我们要在 LVGL 拿到之前就拦下来。
 *       读引脚比读 I2C 快得多，也不会和 LVGL 抢 I2C 总线。
 */
static bool touch_pressed(void)
{
    return gpio_get_level(BOARD_TOUCH_GPIO_INT) == 0;
}

static void poll_cb(void *arg)
{
    (void)arg;

    /*
     * 轮询现在只做两件事，**不再承担"检测用户活动"的职责**：
     *
     *   1. 屏蔽期结束后恢复触摸输入
     *   2. 到点了就息屏
     *
     * 用户活动一律由 bsp 的触摸读取路径通过 hook_activity() 上报
     * （LVGL 每 5ms 读一次，一次不漏）。
     *
     * ⚠️ 曾经这里靠"每 50ms 读一次触摸中断脚(GPIO14)"来判断有没有人操作。
     *    那会漏掉快速点击 —— 用户明明在连续点屏幕，计时器却以为没人动，
     *    于是中途息屏。用户实测报的就是这个现象。
     *    现在只留一个兜底：万一中断脚还是低电平（手一直按着），
     *    也当作有活动，避免"手指按着不动反而息屏"。
     */

    /* ---- 1. 屏蔽期结束，恢复触摸输入 ---- */
    if (s_wake_at) {
        if (now_us() - s_wake_at < (int64_t)W_IGNORE_TOUCH_MS * 1000) {
            return;                     /* 还在屏蔽期 */
        }
        lv_indev_t *indev = lv_indev_get_next(NULL);
        if (indev) {
            lv_indev_enable(indev, true);
        }
        s_wake_at = 0;
        ESP_LOGD(TAG, "恢复触摸输入");
    }

    /* ---- 2. 兜底：手一直按着也算有活动 ---- */
    if (s_screen_on && touch_pressed()) {
        s_last_active = now_us();
    }

    /* ---- 3. 到点息屏 ---- */
    if (!s_screen_on) {
        return;                         /* 已经灭了 */
    }
    if (s_hold_count > 0) {
        return;                         /* 有东西要求别息屏（比如正在录 IMU） */
    }

    const int to = timeout_sec();
    if (to <= 0) {
        return;                         /* 0 = 常亮 */
    }

    if (now_us() - s_last_active >= (int64_t)to * 1000000) {
        power_screen_off();
    }
}

/* ==================== 界面钩子 ==================== */
static void hook_wake(void)
{
    power_screen_on();
}

static bool hook_is_on(void)
{
    return s_screen_on;
}

static void hook_hold_awake(bool hold)
{
    power_hold_awake(hold);
}

/**
 * @brief "用户正在操作"上报（由 bsp 的触摸读取路径每次按下时调用）
 *
 * 这是**活动计时的主来源**。屏幕灭着时顺手亮屏并吃掉这一次操作。
 */
static void hook_activity(void)
{
    if (!s_screen_on) {
        /* 息屏状态下第一次触摸：只亮屏，不把这次操作传给界面 */
        power_screen_on();
        return;
    }
    s_last_active = now_us();
}

static const w_screen_hooks_t s_hooks = {
    .wake            = hook_wake,
    .is_on           = hook_is_on,
    .hold_awake      = hook_hold_awake,
    .notify_activity = hook_activity,
};

/* ==================== 对外 ==================== */
esp_err_t power_init(void)
{
    if (s_poll) {
        return ESP_OK;          /* 幂等 */
    }

    /* 触摸中断脚：输入 + 上拉（CST816T 是开漏低有效） */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_GPIO_INT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "触摸中断脚配置失败（%s），自动息屏可能不工作",
                 esp_err_to_name(err));
    }

    s_last_active = now_us();
    s_screen_on   = true;

    const esp_timer_create_args_t targs = {
        .callback = poll_cb,
        .name     = "power_poll",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_poll), TAG, "定时器创建失败");

    /* 用周期性定时器而不是 gpio 中断：
     * 有"屏蔽期"这种时间相关状态要处理，定时器轮询更好写也更好调试。 */
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_poll, POWER_POLL_MS * 1000),
                        TAG, "定时器启动失败");

    w_platform_set_screen_hooks(&s_hooks);

    ESP_LOGI(TAG, "电源管理就绪（自动息屏 %d 秒，只关背光不做低功耗）",
             timeout_sec());
    return ESP_OK;
}

void power_dump(void)
{
    const int to = timeout_sec();
    const int64_t idle_ms = (now_us() - s_last_active) / 1000;

    ESP_LOGI(TAG, "屏幕=%s  自动息屏=%d 秒  已空闲=%" PRId64 " 秒  别息屏计数=%" PRId32,
             s_screen_on ? "亮" : "灭", to, idle_ms / 1000, s_hold_count);
    ESP_LOGI(TAG, "  说明：息屏只关背光，CPU/LVGL/IMU 仍在运行（本版本不做低功耗）");
}
