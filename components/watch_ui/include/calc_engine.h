/**
 * @file calc_engine.h
 * @brief 计算器求值引擎（**纯逻辑，不碰 LVGL、不碰任何硬件**）
 *
 * ## 为什么单独拆出来
 *
 * 拆开之后：
 *   1. 求值逻辑可以被**模拟器自检直接断言**（见 lvgl_sim 的计算器自检那一节），
 *      UI 只是它的一个壳 —— 项目里"能自动验的就别留给人眼"这条规则。
 *   2. 它属于"纯 C 算法"，不需要 ui_port_lock、也不需要中文字库。
 *
 * ## 支持的语法
 *
 * - 二元运算符：`+ - * /`（界面上的 `×` `÷` 会先被归一化成 `*` `/`）
 * - **后缀百分号 `%`**，语义按商用计算器：
 *     · `50%`     → 0.5
 *     · `100-10%` → 90     （"减去 100 的 10%"，不是减去 0.1）
 *     · `100+10%` → 110
 *     · `200*10%` → 20     （乘除场景就是普通的"除以 100"）
 * - 一元负号：`-5+3` → -2、`5*-3` → -15（参考工程这三条全部报语法错误，我们补上）
 *
 * **不支持**括号（屏幕太小，键位放不下）。
 *
 * ## 与参考工程（Watch-Player_IDF 的 fs_calculator.cpp）的取舍
 *
 * 它的调度场双栈思路我们沿用，但下面这些**明确不抄**（都是它的真缺陷）：
 *
 * | 它的做法 | 问题 | 我们的做法 |
 * |---|---|---|
 * | `1..2` 交给 `stod` 前缀解析 | **静默变成 1**（丢了 `.2`） | 数字 token 里自己校验：至少 1 位数字、最多 1 个 `.` |
 * | 除零判据 `|b| < 1e-9` | `1/0.0000000001` 也被判成除零 | 严格用 `b == 0.0` |
 * | 只查 `isinf`、不查 `NaN` | 可能显示小写 `nan`，而清屏名单写的是 `NaN`（匹配不上） | 两个都查 |
 * | 输入长度**完全无上限** | 用 `std::string` 无限增长 | `CALC_EXPR_MAX` 限长 |
 * | 无一元负号 | `-5+3` / `5*-3` / `5--3` 全部 Error | 支持 |
 * | 链式运算**复用显示文本**再解析 | 结果用 `%.10g` 输出成科学计数法后，清洗时把 `e` 丢掉 → **静默算错** | 引擎只接受表达式，**链式续算由 UI 负责把结果重新拼进表达式**，不反向解析显示串 |
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>     /* size_t —— calc_format() 用；不显式包含的话
                         * 在设备端（-Werror=all）会报 unknown type name */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 表达式缓冲区建议长度（含结尾 '\0'） */
#define CALC_EXPR_MAX   40

/** @brief 求值结果类型 */
typedef enum {
    CALC_OK = 0,        ///< 正常
    CALC_ERR_SYNTAX,    ///< 语法错误（含"数字里有两个小数点"）
    CALC_ERR_DIV_ZERO,  ///< 除零
    CALC_ERR_OVERFLOW,  ///< 溢出 / 结果不是有限数
} calc_result_t;

/** @brief 求值结果 */
typedef struct {
    double       value;     ///< CALC_OK 时有效；出错时为 0
    calc_result_t type;     ///< 结果类型
} calc_eval_t;

/**
 * @brief 求值一个表达式
 *
 * @param expr 以 '\0' 结尾的表达式；空格与 `×` `÷`（UTF-8）会被忽略/归一化
 * @return 结果。**任何输入都不会崩**：空串/纯运算符/超长都会返回具体的错误码。
 *
 * @note 线程安全：无全局状态，可任意任务调用。
 */
calc_eval_t calc_evaluate(const char *expr);

/**
 * @brief 把结果格式化成人类可读的字符串
 *
 * @param v      值
 * @param buf    输出缓冲
 * @param buflen 缓冲长度（建议 >= 24）
 *
 * @note 整数结果不带小数点（`7` 而不是 `7.000000`）；
 *       过大/过小的数退回科学计数法。
 *       ⚠️ 显示的串**不要**再喂回 calc_evaluate()（见文件头"链式运算"那条）。
 */
void calc_format(double v, char *buf, size_t buflen);

/**
 * @brief 把结果压成"可以继续参与运算的表达式片段"
 *
 * @note 与 calc_format() 的区别：这个只输出**能被引擎重新解析**的字符
 *       （纯十进制，绝不输出 `e` 科学计数法）。链式续算时用这个，
 *       这样就绕开了参考工程"显示串回灌导致静默算错"的坑。
 */
void calc_format_reusable(double v, char *buf, size_t buflen);

/** @brief 结果类型的中文说明（用于界面提示） */
const char *calc_result_text(calc_result_t t);

#ifdef __cplusplus
}
#endif
