/**
 * @file watch_clock.h
 * @brief 时钟服务（秒表 / 倒计时 / 闹钟）—— v0.5.0 新增
 *
 * ## 为什么单独抽一个常驻服务，而不是写在时钟页里
 *
 * 用户明确要求："秒表正在跑时切到别的标签页再切回来，应该**继续**计时"。
 * 而且闹钟/倒计时**必须能跨页面触发**（用户多半不在时钟页上）。
 *
 * 如果把状态放在页面里，页面一重建（模态每次 refresh 都会 `lv_obj_clean`
 * 重跑 build）状态就没了。所以状态和计时逻辑放这里，页面只负责"渲染 + 按钮"。
 *
 * ## 计时源为什么用 lv_tick_get()
 *
 * 界面层**不许 include ESP-IDF 头文件**（否则 PC 模拟器编不过），
 * 而 LVGL 是允许的依赖，`lv_tick_get()` 正好是毫秒级单调计数
 * （底层就是 esp_timer，精度足够秒表用）。
 * 用它就不用为"取时间"再加一组钩子。
 *
 * ## 闹钟的时间口径（★ 容易搞错）
 *
 * 直接用**墙上时间**的 hour/minute 比较（`watch_state_t.time`），
 * 不碰 epoch、不做时区换算 —— 和表盘显示的口径完全一致。
 * 理由见 data_hub.c 顶部："RTC 芯片里存的就是墙上时间"。
 *
 * ⚠️ **RTC 没有备份电池**：掉电后时间是 `--:--`（`time.valid == false`），
 *    此时闹钟**不会生效**，界面要明确提示（W_STR_ALARM_NO_TIME）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "watch_pages.h"    /* watch_time_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、生命周期
 * ================================================================== */

/**
 * @brief 初始化时钟服务
 *
 * @note 幂等。只建内部状态，**不建 LVGL 定时器** ——
 *       定时器由各页面按需创建（见下面 tick 函数），
 *       这样秒表不跑时就不占 CPU。
 */
void watch_clock_init(void);

/**
 * @brief 每秒喂一次当前**墙上时间**（由 watch_pages.c 的数据回调调用）
 *
 * @param t 当前时间快照；`t->valid == false` 表示未校准
 *
 * @note 闹钟就是在这里比较并触发的。
 */
void watch_clock_on_time(const watch_time_t *t);

/**
 * @brief 周期 tick（由页面的 lv_timer 调用，建议 100ms 一次）
 *
 * @note 只做两件事：推进倒计时、判断要不要触发提醒。
 *       秒表不需要 tick（elapsed 是按需算的）。
 */
void watch_clock_tick(void);

/* ==================================================================
 * 二、秒表
 * ================================================================== */

/** @brief 开始 / 暂停（按当前状态切换） */
void     watch_clock_sw_toggle(void);
/** @brief 归零（会顺带停表） */
void     watch_clock_sw_reset(void);
/** @brief 是否在走 */
bool     watch_clock_sw_running(void);
/** @brief 已计时毫秒数（暂停时返回累计值） */
uint32_t watch_clock_sw_elapsed_ms(void);

/* ==================================================================
 * 三、倒计时
 * ================================================================== */

/** @brief 设定倒计时时长（秒），会写 NVS 记住 */
void watch_clock_cd_set_sec(int sec);
/** @brief 取设定的时长（秒） */
int  watch_clock_cd_sec(void);

/** @brief 开始 / 暂停 */
void watch_clock_cd_toggle(void);
/** @brief 复位到设定时长（会停） */
void watch_clock_cd_reset(void);
/** @brief 是否在倒数 */
bool watch_clock_cd_running(void);
/** @brief 剩余秒数（向上取整；不在倒数时返回设定时长） */
int  watch_clock_cd_remaining_sec(void);

/** @brief 设定到点后的响铃时长（秒），会写 NVS */
void watch_clock_cd_set_ring_sec(int sec);
/** @brief 取响铃时长（秒），默认 10 */
int  watch_clock_cd_ring_sec(void);

/* ==================================================================
 * 四、闹钟
 * ================================================================== */

/** @brief 设定闹钟时间（每天重复），会写 NVS */
void watch_clock_alarm_set(int hour, int minute);
/** @brief 闹钟-时 */
int  watch_clock_alarm_hour(void);
/** @brief 闹钟-分 */
int  watch_clock_alarm_min(void);
/** @brief 启用 / 停用闹钟（会写 NVS） */
void watch_clock_alarm_enable(bool on);
/** @brief 闹钟是否启用 */
bool watch_clock_alarm_enabled(void);
/** @brief 当前时间是否可用于闹钟（RTC 校准过） */
bool watch_clock_time_ok(void);

/**
 * @brief 取最后一次喂进来的墙上时间
 * @return true = 时间有效，hour / minute 已填好
 *
 * @note 写这个函数的文档时**别**把两个指针参数用斜杠连起来写，
 *       那会凑出注释起始符，触发 `-Werror=comment`，整个组件编不过。
 *       （项目里踩过这个坑，见代码架构说明坑 10 与交接文档坑 10。
 *         本行原来就是那么写的，编译直接失败。）
 */
bool watch_clock_last_time(int *hour, int *minute);

/**
 * @brief 注册/注销"界面需要重画"的回调
 *
 * @note 时钟服务内部有一个**全局** lv_timer（周期自适应：有秒表/倒计时/
 *       提醒时 100ms，空闲时 1000ms），它会调这个回调让时钟页刷新数字。
 *       时钟页在 build 时注册，在页面被删除时（LV_EVENT_DELETE）自动注销。
 *
 *       为什么必须由服务侧驱动：用户可能"起了倒计时就翻到别的页面"，
 *       服务必须继续跑，而页面已经不存在了 —— 所以回调要能安全地为空。
 */
void watch_clock_set_ui_refresh(void (*cb)(void));

/* ==================================================================
 * 五、提醒（给 watch_alert 用）
 * ================================================================== */

/**
 * @brief 用户在提示层点了"停止"
 *
 * @note 停掉蜂鸣器、清掉"正在提醒"状态。**倒计时不自动复位** ——
 *       用户可能想看看刚才设的是多久。
 */
void watch_clock_alert_stop(void);

/** @brief 是否正在提醒（蜂鸣器该不该响） */
bool watch_clock_alerting(void);

/**
 * @brief 提醒类型
 */
typedef enum {
    W_CLOCK_ALERT_NONE = 0,
    W_CLOCK_ALERT_COUNTDOWN,   /**< 倒计时到点：响"响铃时长"后自动停 */
    W_CLOCK_ALERT_ALARM,       /**< 闹钟：一直响到用户点停止 */
} w_clock_alert_t;

/** @brief 当前提醒类型 */
w_clock_alert_t watch_clock_alert_kind(void);

/**
 * @brief 倒计时的响铃是否已经响完
 *
 * @note 倒计时按"响铃时长"自动停声，但**提示层继续留在屏幕上**
 *       （用户要求"响声停了仍然等你点停止"）。提示层据此换个措辞。
 *       闹钟永远返回 false（一直响到用户点停止）。
 */
bool watch_clock_alert_ring_done(void);

/* ==================================================================
 * 六、格式化小工具（页面共用）
 * ================================================================== */

/**
 * @brief 毫秒 -> 秒表字符串 "MM:SS.d"（十分之一秒）
 * @param buf 输出缓冲，**至少 9 字节**
 */
void watch_clock_fmt_stopwatch(uint32_t ms, char *buf, int len);

/**
 * @brief 秒 -> "MM:SS"（超过 1 小时则 "H:MM:SS"）
 * @param buf 输出缓冲，**至少 10 字节**
 */
void watch_clock_fmt_mmss(int total_sec, char *buf, int len);

#ifdef __cplusplus
}
#endif
