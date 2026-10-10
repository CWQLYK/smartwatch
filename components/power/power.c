/**
 * @file power.c
 * @brief 屏幕/背光/省电管理实现
 *
 * ## 息屏到底"关"了什么（★ v0.7.1 起变成真省电）
 *
 * | 动作 | v0.7.0 | v0.7.1 |
 * |---|---|---|
 * | 背光 | `bsp_backlight_set(0)` | 同左 |
 * | 面板显示输出 | 不管（仍在扫描） | **`bsp_lcd_display(false)`（DISPOFF）** |
 * | LVGL 刷新 | 照跑 100Hz | **放慢到 100ms**（不是暂停！原因见 refr_apply） |
 * | 电源轮询 | 50ms | **200ms** |
 * | 触摸 I2C | 息屏也 100Hz 轮询 | **INT 门控 → 空闲时 0 次**（在 bsp.c 里） |
 *
 * ⚠️ **仍然不做的事**（见 `doc/智能手表-低功耗实施文档.md`）：
 *    不冻结整个 `lv_timer_handler()`（闹钟/倒计时/提醒层全靠它）、
 *    不动 CPU 频率、不开 `CONFIG_PM_ENABLE`/light sleep（属第二档）、
 *    不降 IMU 采样率（会破坏按 50Hz 标定的计步滤波器）。
 *
 * ## 息屏后第一次触摸怎么"吃掉"
 *
 * 机制在 `watch_platform.c` + `bsp.c`，**不在本文件**：
 *   bsp 的触摸读回调 → `w_screen_notify_activity()`（发现屏是灭的 → 置"吞掉"标志）
 *   → 同一次回调里问 `w_screen_should_swallow_touch()` → 是则上报 RELEASED。
 *
 * ⚠️ 本文件历史上写着"用 `lv_indev_enable(false)` 临时禁用输入设备"——
 *    那是**错的**，全工程从没调过 false，而且 LVGL 8.4 是在**调用读回调之前**
 *    检查 disabled 的（`lv_indev.c:78`），事后禁用根本来不及。
 *    那句残留的 `lv_indev_enable(indev, true)` 已在本版删除。
 *
 * ⚠️ 唤醒**必须**仍然经过 bsp 的触摸读回调（那是唯一设置"吞掉"标志的地方）——
 *    所以不要在别处（比如轮询里读 INT 脚）抢先亮屏，否则 `was_on` 已是 true，
 *    那一下就不吞了 → 手指下面的控件会被误触发（v0.6.1 修过的老 bug 会复发）。
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

/*
 * ★ v0.7.1 省电：**息屏期间**把轮询放慢到 200ms。
 *
 * 息屏状态下 poll_cb 几乎什么都不做（只剩"屏蔽期结束"和下面的兜底），
 * 20Hz 轮询纯属白醒。放慢到 5Hz 后，每秒少醒 15 次。
 * 亮屏时立刻恢复 50ms —— 活动检测的精度只在有人操作时才重要。
 */
#define POWER_POLL_OFF_MS    200

/* 亮屏后屏蔽输入的时长（ms）。太短会漏吃掉那一下，太长会觉得"点不动" */
#define W_IGNORE_TOUCH_MS    350

/*
 * ★ v0.7.1 省电：**息屏期间**把 LVGL 显示刷新放慢到这个周期。
 *
 * ⚠️ 为什么是"放慢"而不是"暂停"：见 refr_apply() 上面那段长注释
 *    （暂停会让 esp_lvgl_port 的任务睡到 500ms，闹钟/倒计时/亮屏全会迟到）。
 */
#define POWER_REFR_OFF_MS    100

/* ==================== 状态 ==================== */
static bool     s_screen_on   = true;
static int32_t  s_hold_count  = 0;       /* "别息屏"引用计数 */
static int64_t  s_last_active = 0;       /* 上次用户活动时刻（us） */
static int64_t  s_wake_at     = 0;       /* 本次亮屏时刻（us）；0 = 没在屏蔽期 */
static esp_timer_handle_t s_poll = NULL;
static uint32_t s_poll_ms     = POWER_POLL_MS;   /* 当前轮询周期（息屏会放慢） */

/*
 * ★ v0.7.1 省电：LVGL 刷新是否处于"息屏放慢"状态。
 *
 * 为什么需要"期望状态 + 补做"而不是直接调用：
 *   `power_screen_off()` 是从 **esp_timer 任务**（poll_cb）里调用的，
 *   而改 LVGL 定时器必须先拿 LVGL 锁；esp_timer 回调**绝对不能阻塞**等锁，
 *   所以拿不到锁时只记下"想要的状态"，下一拍再补做（见 refr_apply）。
 */
static bool     s_refr_slow    = false;  /* true = 息屏，刷新已放慢到 POWER_REFR_OFF_MS */
static bool     s_refr_pending = false;  /* true = 有一次没拿上锁，需要补做 */

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

/* ==================== v0.7.1 省电：LVGL 刷新 + 轮询周期 ==================== */

/**
 * @brief 把"息屏时放慢刷新"应用到 LVGL 的 display refresh timer
 *
 * ## ⚠️ 为什么是"放慢"而不是"暂停"（这条踩过，别改回去）
 *
 * `lv_timer_handler()` 的返回值是"距下一个**未暂停**定时器还有多久"：
 *
 * ```c
 * lv_timer.c:121  uint32_t time_till_next = LV_NO_TIMER_READY;
 * lv_timer.c:124      if(!next->paused) { ...取最小 delay... }
 * lv_timer.c:145  return time_till_next;
 * ```
 *
 * 而 `esp_lvgl_port` 的任务**直接拿这个返回值当自己的睡眠时间**
 * （`esp_lvgl_port.c:198-206`，上限 `task_max_sleep_ms`=500ms）。
 *
 * 所以一旦把 refresh timer **暂停**，息屏时屏幕上就只剩 `watch_clock` 的
 * 1 秒定时器 → 任务变成**每 500ms 才醒一次** → 三个真实后果：
 *
 *   1. **闹钟响铃迟到**：`alert_pump()`（"每 500ms 响 300ms"）跑在 LVGL 定时器上，
 *      最长迟 500ms 才开始响，而且节奏会被量化成 500ms 粒度；
 *   2. **倒计时到点迟到**：到点判定在 `clock_timer_cb()` 里，最长迟 500ms；
 *   3. **亮屏重画迟到**：任务睡着时没人处理 invalidate，最长 500ms 才出画面。
 *
 * 放慢到 100ms 就全避开：延迟 ≤100ms（无感），而刷新相关的唤醒从 **100Hz 降到 10Hz**
 * —— 省掉的正是那 90 次/秒的空转检查；剩下 10 次/秒几乎没有绝对开销。
 *
 * ## 拿不到锁怎么办
 * 置 `s_refr_pending`，由下一次 `poll_cb` 补做 ——
 * esp_timer 回调里**不许阻塞等锁**。
 */
static void refr_apply(void)
{
    if (!bsp_display_lock(0)) {
        s_refr_pending = true;      /* LVGL 正忙，下一拍再来 */
        return;
    }

    lv_disp_t *disp = bsp_display_get();
    lv_timer_t *t   = disp ? _lv_disp_get_refr_timer(disp) : NULL;
    if (t) {
        /* 用宏而不是写死 10：模拟器那份 lv_conf.h 是 30，两边都能编 */
        lv_timer_set_period(t, s_refr_slow ? POWER_REFR_OFF_MS
                                           : (uint32_t)LV_DISP_DEF_REFR_PERIOD);
    }
    s_refr_pending = false;
    bsp_display_unlock();
}

/** @brief 请求"息屏放慢刷新 / 亮屏恢复刷新"（拿不到锁会自动补做） */
static void refr_slow_request(bool slow)
{
    if (s_refr_slow == slow && !s_refr_pending) {
        return;                     /* 已经是想要的状态 */
    }
    s_refr_slow = slow;
    refr_apply();
}

/**
 * @brief 亮屏时的"整屏重画"
 *
 * 为什么必须做：息屏期间刷新被放慢到 100ms，且屏幕内容可能已经不新鲜；
 * 而闹钟到点时 `alert_start()` 会主动 `w_screen_wake()`（watch_clock.c:359），
 * 不重画就可能出现"闹钟响了但屏上还是旧画面"。
 *
 * 三处都要 invalidate：
 *   `lv_scr_act()`      当前活动 screen
 *   `watch_ui_screen()` 轮播 screen —— 模态子页与**到点提醒层**都挂在它上面
 *   `lv_layer_top()`    toast 挂在 top layer 上
 */
static void invalidate_all(void)
{
    lv_obj_t *act = lv_scr_act();
    if (act) {
        lv_obj_invalidate(act);
    }
    lv_obj_t *carousel = watch_ui_screen();
    if (carousel && carousel != act) {
        lv_obj_invalidate(carousel);
    }
    lv_obj_t *top = lv_layer_top();
    if (top) {
        lv_obj_invalidate(top);
    }
}

/** @brief 改变电源轮询周期（息屏放慢 / 亮屏恢复） */
static void poll_retime(bool screen_off)
{
    if (!s_poll) {
        return;
    }
    const uint32_t want = screen_off ? POWER_POLL_OFF_MS : POWER_POLL_MS;
    if (s_poll_ms == want) {
        return;
    }
    /*
     * 用 esp_timer_restart：对周期定时器来说它的 timeout_us 就是**新周期**。
     * 注意它是在本定时器**自己的回调链**里被调用的（息屏/亮屏都可能从 poll_cb 进来）——
     * IDF 支持这么做；万一失败只影响省电幅度，不影响功能，所以只告警不返回错误。
     */
    const esp_err_t err = esp_timer_restart(s_poll, (uint64_t)want * 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "轮询周期切换失败（%s），保持 %u ms",
                 esp_err_to_name(err), (unsigned)s_poll_ms);
        return;
    }
    s_poll_ms = want;
}

void power_screen_on(void)
{
    s_last_active = now_us();
    if (s_screen_on) {
        return;
    }

    /* ★ 顺序很重要：先让面板能显示 → 再恢复刷新速率并整屏重画 → 最后开背光 */
    bsp_lcd_display(true);

    if (bsp_display_lock(0)) {
        lv_disp_t *disp = bsp_display_get();
        lv_timer_t *t   = disp ? _lv_disp_get_refr_timer(disp) : NULL;
        if (t) {
            lv_timer_set_period(t, (uint32_t)LV_DISP_DEF_REFR_PERIOD);
        }
        s_refr_slow    = false;
        s_refr_pending = false;
        invalidate_all();
        bsp_display_unlock();
    } else {
        /* 拿不到锁：记下期望状态，poll_cb 下一拍补做（重画也留在那时一起做） */
        s_refr_slow    = false;
        s_refr_pending = true;
    }

    backlight(true);
    poll_retime(false);

    s_screen_on = true;
    s_wake_at   = now_us();     /* 进入"吃掉第一下"的屏蔽期 */
    ESP_LOGI(TAG, "亮屏（面板 DISPON + 刷新恢复 %dms + 整屏重画）",
             (int)LV_DISP_DEF_REFR_PERIOD);
}

void power_screen_off(void)
{
    if (!s_screen_on) {
        return;
    }

    backlight(false);
    /* 面板停显示输出：省掉 GRAM 静态功耗；这一步**非阻塞**，可以在这里调 */
    bsp_lcd_display(false);
    /* 放慢 LVGL 刷新（不是暂停！原因见 refr_apply 的注释；拿不到锁会自动补做） */
    refr_slow_request(true);
    /* 息屏期间轮询放慢 */
    poll_retime(true);

    s_screen_on = false;
    ESP_LOGI(TAG, "息屏（背光关 + 面板 DISPOFF + 刷新放慢到 %dms + 轮询 %dms）",
             POWER_REFR_OFF_MS, POWER_POLL_OFF_MS);
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
     * ★ v0.7.1：轮询现在做三件事
     *
     *   1. **补做**上次没拿上锁的"放慢/恢复 LVGL 刷新"
     *      （息屏/亮屏是从这里触发的，而 esp_timer 回调不许阻塞等锁 → 见 refr_apply）
     *   2. 屏蔽期结束后清掉屏蔽标志
     *   3. 到点了就息屏
     *
     * 用户活动一律由 bsp 的触摸读取路径通过 hook_activity() 上报
     * （LVGL 每 LV_INDEV_DEF_READ_PERIOD 读一次，一次不漏）。
     *
     * ⚠️ 曾经这里靠"每 50ms 读一次触摸中断脚(GPIO14)"来判断有没有人操作。
     *    那会漏掉快速点击 —— 用户明明在连续点屏幕，计时器却以为没人动，
     *    于是中途息屏。用户实测报的就是这个现象。
     *    现在只留一个兜底：万一中断脚还是低电平（手一直按着），
     *    也当作有活动，避免"手指按着不动反而息屏"。
     *
     * ⚠️ 历史残留已清理：这里原本还有一句 `lv_indev_enable(indev, true)`，
     *    但全工程**没有任何地方调过 false**（"吃掉唤醒那一下"真正的机制是
     *    watch_platform 的 w_screen_should_swallow_touch()，在 bsp.c 里消费），
     *    所以那是一句无效调用；而且它在 esp_timer 回调里裸碰 lv_* 也不合铁律。
     */

    /* ---- 0. 补做上次没拿上锁的刷新速率切换 ---- */
    if (s_refr_pending) {
        const bool want_slow = s_refr_slow;
        refr_apply();
        /* 若是"亮屏"那一次没拿上锁，把整屏重画也补上 */
        if (!s_refr_pending && !want_slow && bsp_display_lock(0)) {
            invalidate_all();
            bsp_display_unlock();
        }
    }

    /* ---- 1. 屏蔽期结束 ---- */
    if (s_wake_at) {
        if (now_us() - s_wake_at < (int64_t)W_IGNORE_TOUCH_MS * 1000) {
            return;                     /* 还在屏蔽期 */
        }
        s_wake_at = 0;
        ESP_LOGD(TAG, "屏蔽期结束");
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

    ESP_LOGI(TAG, "电源管理就绪（自动息屏 %d 秒；息屏=关背光+面板DISPOFF+停刷新+轮询%dms）",
             timeout_sec(), POWER_POLL_OFF_MS);
    return ESP_OK;
}

void power_dump(void)
{
    const int to = timeout_sec();
    const int64_t idle_ms = (now_us() - s_last_active) / 1000;

    ESP_LOGI(TAG, "屏幕=%s  自动息屏=%d 秒  已空闲=%" PRId64 " 秒  别息屏计数=%" PRId32,
             s_screen_on ? "亮" : "灭", to, idle_ms / 1000, s_hold_count);
    ESP_LOGI(TAG, "  省电状态：LVGL刷新周期=%d ms%s  轮询周期=%u ms  面板显示=%s",
             s_refr_slow ? POWER_REFR_OFF_MS : (int)LV_DISP_DEF_REFR_PERIOD,
             s_refr_pending ? "（有一次待补做）" : "",
             (unsigned)s_poll_ms,
             s_screen_on ? "开" : "关(DISPOFF)");
    ESP_LOGI(TAG, "  说明：息屏不冻结 lv_timer_handler（闹钟/倒计时仍会响并自动亮屏）；"
                  "刷新是放慢到 %dms 而非暂停（暂停会让 LVGL 任务睡 500ms，见 refr_apply）；"
                  "CPU 频率与 light sleep 属第二档，未做",
             POWER_REFR_OFF_MS);
}
