/**
 * @file watch_keyboard.h
 * @brief 屏幕小键盘 —— 给手表上"需要输文字"的场景用（目前是 WiFi 密码）
 *
 * ## 为什么需要它
 *
 * 手表只有一块 240×280 的触摸屏，没有实体键盘。要连 WiFi 就必须能输
 * SSID/密码，所以必须自带一个软键盘。
 *
 * ## 设计取舍（都踩过或权衡过）
 *
 * 1. **紧凑 6 列 × 4 行**，键宽约 37px。再小手指点不准，
 *    再大就放不下"输入框 + 功能键"。
 *
 * 2. **4 层字符，用一个切换键循环**
 *    小写 → 大写 → 数字 → 符号 → 小写 …
 *    没做"长按切符号"这类手势：小屏上手势容易误触，
 *    而且开发者很难向用户解释。一个明确的键最省事。
 *
 * 3. **Shift 只影响下一个字母**（输完自动回层），和手机习惯一致。
 *
 * 4. **密码显示明文**（用户明确要求）。好处是这么小的屏上能自己检查
 *    有没有敲错 —— 只显示 `••••` 时敲错了根本看不出来，
 *    而 WiFi 密码敲错是最高频的失败原因。
 *
 * 5. **输入框只显示尾部**：密码可能 30+ 字符，240px 宽放不下，
 *    所以超过 8 个字符就只显示最后 8 个并加前缀提示。
 *
 * ## 用法
 *
 * ```c
 * w_keyboard_open(parent, "输入密码", false, my_on_done, NULL);
 * ```
 * 键盘自己是一个模态面板，盖在 parent 上；Done 回调里拿走文本。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 最大输入长度（WiFi 密码上限 63，留点余量） */
#define W_KB_TEXT_MAX   64

/** @brief 键盘容器对象（用来关掉它） */
typedef struct w_keyboard_t w_keyboard_t;

/**
 * @brief 完成回调
 *
 * @param text  输入的文本
 * @param ok    true = 点了"完成"，false = 点了"取消"
 * @param user  透传的用户指针
 */
typedef void (*w_keyboard_done_cb_t)(const char *text, bool ok, void *user);

/**
 * @brief 在 parent 上弹出一个键盘面板
 *
 * @param parent    挂载父对象（通常传页面根）
 * @param title     标题，如 "输入密码"（可为 NULL）
 * @param password  true = 用密码字符集（其实只是提示用途，显示仍是明文）
 * @param initial   初始文本（可为 NULL）
 * @param on_done   完成/取消回调（必须给）
 * @param user      透传给回调
 *
 * @return 键盘对象；失败返回 NULL
 *
 * @note 回调触发后键盘会**自动删除自己**，不要在回调里再调 w_keyboard_close()。
 */
w_keyboard_t *w_keyboard_open(lv_obj_t *parent, const char *title,
                              bool password, const char *initial,
                              w_keyboard_done_cb_t on_done, void *user);

/**
 * @brief 主动关掉键盘（不触发回调）
 */
void w_keyboard_close(w_keyboard_t *kb);

/**
 * @brief 取当前输入文本（回调之外想主动读时用）
 */
const char *w_keyboard_text(const w_keyboard_t *kb);

#ifdef __cplusplus
}
#endif
