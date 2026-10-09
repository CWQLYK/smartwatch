/**
 * @file watch_toast.h
 * @brief 通用顶部提示条（toast）—— 一两秒后自动消失的短反馈
 *
 * ## 为什么要有它
 *
 * 以前每个功能各自的反馈方式都不一样：WiFi 页用页面内的 popup、录制完成只打串口日志、
 * 设置保存成功什么都不显示。用户经常"点了没反应"。
 * 这个组件提供**唯一**的轻量反馈通道：`watch_toast_show("已连接")`。
 *
 * ## 挂在哪一层（★ 一个曾经被记错的事实）
 *
 * 挂 `lv_layer_top()`，**不是**挂在轮播 screen 上（模态栈/提醒层/游戏层都在那儿）。
 *
 * 项目里曾有一条注释说"`lv_layer_top()` 一旦可点击就会吞掉整屏触摸"
 * （见 `watch_overlay.c` 的文件头）。**那句归因是错的**，事实是：
 *
 *   - `lv_hal_disp.c:225-230` 在创建 top_layer 时就**显式清掉了 CLICKABLE**
 *     （`lv_obj_clear_flag(disp->top_layer, LV_OBJ_FLAG_CLICKABLE)`）→ 层本身不吞触摸；
 *   - 真正吞触摸的是**建在它上面的默认尺寸子容器** —— `lv_obj_create(NULL)`
 *     默认就是"满屏 + 可点击"。
 *
 * 所以本组件建完容器后**第一件事**就是清掉自己的 `CLICKABLE`/`CLICK_FOCUSABLE`（见 .c）。
 * 挂 layer_top 的额外好处：它天然高于挂在 screen 上的模态栈，
 * 不需要像 `watch_alert.c` 那样每次 poll 都 `lv_obj_move_foreground()` 抢 z-order。
 *
 * ## 与闹钟/倒计时提醒层（watch_alert）的关系
 *
 * `watch_alert` 是"必须盖住一切"的模态级提醒（上面有【停止】按钮）。
 * toast 若显示在它之上会挡住按钮，所以**提醒层可见时 toast 直接丢弃**（不排队）：
 * 闹钟正在响的时候，多一条"已连接"本来也没有价值。
 *
 * ## 线程约定
 *
 * 所有函数都会碰 `lv_*`，**必须在持有 `ui_port_lock()` 的 LVGL 任务里调用**。
 * 服务层（如天气任务）不要直接调，应由页面在自己的 refresh 回调里转达。
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 建好提示条（只建一次，之后只做显示/隐藏）
 *
 * @note 幂等：重复调用直接返回。必须在 `lv_layer_top()` 可用之后调用
 *       （即界面初始化阶段，`watch_pages_init()` / `watch_ui_init()` 里）。
 * @note 调用者要持 UI 锁。
 */
void watch_toast_init(void);

/**
 * @brief 弹一条提示
 *
 * @param text  UTF-8 文案。内部会拷贝，调用方可以用栈上的临时缓冲。
 *              中文必须已经登记在 `watch_strings.h`（否则是豆腐块）。
 *
 * ## 幂等语义（三种情况）
 *
 * | 当前状态 | 行为 |
 * |---|---|
 * | 空闲     | 从屏幕外滑入 → 保持 → 滑出 |
 * | 保持中   | **只换文案 + 重新计时**（不重播滑入动画，视觉上不闪） |
 * | 滑出中   | 反向滑回 + 重建计时（参考工程漏了这一步，导致这类提示一闪即逝） |
 *
 * @note 调用者要持 UI 锁。
 */
void watch_toast_show(const char *text);

/**
 * @brief printf 版 watch_toast_show()（带格式化的短文案）
 */
void watch_toast_show_fmt(const char *fmt, ...);

/** @brief 提示条是否正在显示（含滑入/滑出过程） */
bool watch_toast_visible(void);

/**
 * @brief 当前状态名（`"空闲"` / `"滑入中"` / `"保持中"` / `"滑出中"`）
 *
 * @note 给自检和日志用。自检要断言"**滑出中途**再来一条提示能反向滑回"，
 *       而滑出只有 200ms —— 靠 `sim_run_ms()` 估算时间撞这个窗口很脆
 *       （PC 模拟器的 Sleep 精度不够）。所以把状态名暴露出来，
 *       自检改为**轮询状态**而不是猜时间。
 */
const char *watch_toast_state_name(void);

/**
 * @brief 自检用：拿到提示条的根对象（在 `lv_layer_top()` 上）
 *
 * @return 根对象；没初始化时返回 NULL
 *
 * @note 只给 PC 模拟器自检截图用。它挂在 layer_top 上，
 *       而 `sim_shot()` 截的是 `lv_scr_act()`，所以自检需要自己把根对象
 *       临时挪到屏幕上才能截到（见 sim_main.c 的提示条自检）。
 */
lv_obj_t *watch_toast_root_for_test(void);

/** @brief 自检用：把状态清回空闲，让下一次 show 走"从零开始"的路径 */
void watch_toast_deinit_for_test(void);

#ifdef __cplusplus
}
#endif
