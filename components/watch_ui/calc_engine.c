/**
 * @file calc_engine.c
 * @brief 计算器求值引擎实现 —— 见 calc_engine.h 的语法与取舍说明
 *
 * 实现方式：**双栈调度场**（运算符栈 + 数值栈）。
 * 与教科书版本相比多了两处项目特有的处理：
 *   1. 后缀 `%` 的"商用计算器语义"（`100-10%` = 90）
 *   2. 一元负号
 * 前者靠"在处理 `%` 时回看**运算符栈顶**"来判断语义
 * （不能用"上一个已入栈数值的前导运算符"，那个值不对，见下面 % 分支的注释）。
 */
#include "calc_engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>     /* ★ strtod —— 漏了这个头文件会静默退化成返回 int，
                         *   把 double 的位模式截断成垃圾值（本文件真踩过：
                         *   编译只有一条 warning，运行结果全是天文数字）*/
#include <string.h>

/* 表达式限长 40，运算符最多也就十几个；给 24 留足余量又不用动态分配 */
#define CALC_STK_MAX   24

/** @brief 运算符优先级：+ - = 1，* / = 2，其它（含"无"）= 0 */
static int op_prio(char op)
{
    switch (op) {
    case '+':
    case '-': return 1;
    case '*':
    case '/': return 2;
    default:  return 0;
    }
}

/** @brief 是不是"数字或小数点"（token 内的字符）*/
static bool is_num_char(char c)
{
    return (c >= '0' && c <= '9') || c == '.';
}

/**
 * @brief 校验一个数字 token
 *
 * ★ 这是与参考工程最重要的一处分歧：它把 `1..2` 直接交给 `stod`，
 *   而 `stod` 做的是**前缀解析** —— 于是 `1..2` 静默变成 `1`（`.2` 被丢掉），
 *   用户看到一个错的答案却毫无提示。我们在这里显式挡掉。
 *
 * @param s    token 起点
 * @param len  token 长度
 */
static bool num_token_valid(const char *s, int len)
{
    if (len <= 0) {
        return false;
    }
    int dots = 0;
    int digits = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '.') {
            if (++dots > 1) {
                return false;       /* 两个小数点：`1..2` / `1.2.3` */
            }
        } else if (s[i] >= '0' && s[i] <= '9') {
            digits++;
        } else {
            return false;
        }
    }
    return digits > 0;              /* 单独的 "." 不算数字 */
}

calc_eval_t calc_evaluate(const char *expr)
{
    calc_eval_t out = { 0.0, CALC_ERR_SYNTAX };

    if (!expr) {
        return out;
    }

    double vstk[CALC_STK_MAX];      /* 数值栈 */
    char   ostk[CALC_STK_MAX];      /* 运算符栈 */
    int    vsp = 0, osp = 0;

    /* 丢掉空格，把界面上的 × ÷（UTF-8 两字节）归一化成 * / */
    char clean[CALC_EXPR_MAX * 2];
    int  n = 0;
    for (const char *p = expr; *p && n < (int)sizeof(clean) - 1; p++) {
        if (*p == ' ' || *p == '\t') {
            continue;
        }
        if ((unsigned char)p[0] == 0xC3 && (unsigned char)p[1] == 0x97) {  /* × */
            clean[n++] = '*';
            p++;
            continue;
        }
        if ((unsigned char)p[0] == 0xC3 && (unsigned char)p[1] == 0xB7) {  /* ÷ */
            clean[n++] = '/';
            p++;
            continue;
        }
        clean[n++] = *p;
    }
    clean[n] = '\0';

    if (n == 0) {
        return out;                 /* 空串：语法错误 */
    }

    int i = 0;
    while (i < n) {
        const char c = clean[i];

        /* ---------- 一元负号 ---------- */
        /*
         * 允许出现在：表达式开头，或任意运算符之后。
         * 参考工程不支持一元负号（`-5+3` / `5*-3` / `5--3` 全报语法错误），
         * 而"上一步算出的负数没法继续算"是很常见的操作，所以补上。
         */
        if (c == '-' && (i == 0 || op_prio(clean[i - 1]) > 0)) {
            if (vsp >= CALC_STK_MAX) {
                return out;
            }
            /* 不建栈项，只记一个待生效的负号 */
            int j = i + 1;
            int len = 0;
            while (j + len < n && is_num_char(clean[j + len])) {
                len++;
            }
            if (!num_token_valid(clean + j, len)) {
                return out;
            }
            char numbuf[32];
            const int copy = (len < (int)sizeof(numbuf) - 1) ? len : (int)sizeof(numbuf) - 1;
            memcpy(numbuf, clean + j, (size_t)copy);
            numbuf[copy] = '\0';
            const double v = strtod(numbuf, NULL);
            vstk[vsp] = -v;
            vsp++;
            i = j + len;
            continue;
        }

        /* ---------- 数字 ---------- */
        if (is_num_char(c)) {
            int len = 0;
            while (i + len < n && is_num_char(clean[i + len])) {
                len++;
            }
            if (!num_token_valid(clean + i, len)) {
                return out;         /* 含 `1..2` 这类非法数字 */
            }
            char numbuf[32];
            const int copy = (len < (int)sizeof(numbuf) - 1) ? len : (int)sizeof(numbuf) - 1;
            memcpy(numbuf, clean + i, (size_t)copy);
            numbuf[copy] = '\0';
            double v = strtod(numbuf, NULL);

            i += len;

            /* ---------- 紧跟的后缀 % ---------- */
            if (i < n && clean[i] == '%') {
                i++;
                if (vsp >= CALC_STK_MAX) {
                    return out;
                }
                /*
                 * ★ 前导运算符要从**运算符栈顶**取，不能从 val_op[vsp-1] 取
                 *   （本文件真踩过，两次）：
                 *     处理 `100-10%` 时，`10` **还没入栈**，此刻正在生效的
                 *     运算符就是 `ostk[osp-1]`（`-`）。
                 *     而 `val_op[vsp-1]` 是**上一个已入栈的值**(100) 的前导，
                 *     那是更早的运算符（这里是"无"）→ 于是走进了"除以 100"
                 *     分支，算出 100-0.1 = 99.9 而不是 90。
                 */
                const char lead = (osp > 0) ? ostk[osp - 1] : 0;
                if ((lead == '+' || lead == '-') && vsp > 0) {
                    /*
                     * 商用计算器语义：`100-10%` = 100 - (100 的 10%) = 90。
                     * 这里直接把"百分比"换算成**实际增减量**，
                     * 于是上层照常做二元运算即可。
                     */
                    v = vstk[vsp - 1] * v / 100.0;
                } else {
                    /* 乘除 / 开头位置：`50%` = 0.5、`200*10%` = 20 */
                    v = v / 100.0;
                }
            }

            if (vsp >= CALC_STK_MAX) {
                return out;
            }
            vstk[vsp] = v;
            vsp++;

            /*
             * ★ 这里必须显式前进一格（本文件真踩过）：
             *   上面 `i += len;` 之后 i 正好停在 `%` 上，而循环末尾是 `continue`
             *   ——**没有 i++**。不补这一下的话 `%` 会在下一轮被重新当成
             *   "孤立百分号"处理，于是：
             *     · 它被当运算符压进 ostk，把 val_op 的语义搅乱；
             *     · `100-10%` 实际算成 100-0.1 = 99.9（而不是 90）。
             *   trace 出来是：`% seen: vsp=1 lead=0x00`（lead 该是 '-'）。
             */
            if (i < n && clean[i] == '%') {
                i++;
            }
            continue;
        }

        /* ---------- 二元运算符 ---------- */
        if (op_prio(c) > 0) {
            /* 一个数值都没有就出现运算符 → 语法错误（一元负号已在上面处理）*/
            if (vsp == 0) {
                return out;
            }
            while (osp > 0 && op_prio(ostk[osp - 1]) >= op_prio(c)) {
                const char op = ostk[--osp];
                if (vsp < 2) {
                    return out;
                }
                const double b = vstk[--vsp];
                const double a = vstk[vsp - 1];
                double r = 0.0;
                if (op == '/' && b == 0.0) {
                    out.type = CALC_ERR_DIV_ZERO;   /* ★ 严格判 == 0，不用 |b|<eps */
                    return out;
                }
                switch (op) {
                case '+': r = a + b; break;
                case '-': r = a - b; break;
                case '*': r = a * b; break;
                case '/': r = a / b; break;
                default:  return out;
                }
                vstk[vsp - 1] = r;
            }
            if (osp >= CALC_STK_MAX) {
                return out;
            }
            ostk[osp++] = c;
            i++;
            continue;
        }

        /* ---------- 其它字符 ---------- */
        if (c == '%') {
            return out;             /* 孤立的 %（前面不是数字）→ 语法错误 */
        }
        return out;                 /* 不认识的字符：参考工程是静默丢弃，我们报错 */
    }

    if (vsp == 0) {
        return out;
    }

    /* ---------- 收尾：清空运算符栈 ---------- */
    while (osp > 0) {
        const char op = ostk[--osp];
        if (vsp < 2) {
            return out;
        }
        const double b = vstk[--vsp];
        const double a = vstk[vsp - 1];
        double r = 0.0;
        if (op == '/' && b == 0.0) {
            out.type = CALC_ERR_DIV_ZERO;
            return out;
        }
        switch (op) {
        case '+': r = a + b; break;
        case '-': r = a - b; break;
        case '*': r = a * b; break;
        case '/': r = a / b; break;
        default:  return out;
        }
        vstk[vsp - 1] = r;
    }

    if (vsp != 1) {
        return out;                 /* 剩多个数值 = 表达式不完整（如 "1 2"）*/
    }

    const double v = vstk[0];
    /*
     * ★ 参考工程只查 isinf 不查 isnan，于是可能显示小写 "nan"，
     *   而它的"自动清屏名单"里写的是大写 "NaN" —— 匹配不上，用户就卡住了。
     *   我们两个都查，并且用同一个错误码。
     */
    if (isnan(v) || isinf(v)) {
        out.type = CALC_ERR_OVERFLOW;
        return out;
    }

    out.value = v;
    out.type  = CALC_OK;
    return out;
}

void calc_format(double v, char *buf, size_t buflen)
{
    if (!buf || buflen == 0) {
        return;
    }
    /* 整数就不显示小数点，省得 "7.000000" 又丑又占宽度 */
    if (v == (double)(long long)v && v < 1e15 && v > -1e15) {
        snprintf(buf, buflen, "%lld", (long long)v);
    } else {
        snprintf(buf, buflen, "%.10g", v);
    }
}

void calc_format_reusable(double v, char *buf, size_t buflen)
{
    if (!buf || buflen == 0) {
        return;
    }
    /*
     * 关键：**绝不输出科学计数法**。
     *
     * 参考工程把显示串（`%.10g`）直接回灌给引擎做链式运算，而它的清洗步骤
     * 会把字母全部丢掉 —— 于是 "1e+15" 变成 "1+15"，**静默算错**。
     * 这里用 %.10f 再手工去掉末尾的 0，保证输出永远是纯十进制。
     */
    if (v == (double)(long long)v && v < 1e15 && v > -1e15) {
        snprintf(buf, buflen, "%lld", (long long)v);
        return;
    }

    char tmp[40];
    snprintf(tmp, sizeof(tmp), "%.10f", v);

    /* 去掉末尾多余的 0，再去掉可能剩下的小数点 */
    size_t len = strlen(tmp);
    while (len > 0 && tmp[len - 1] == '0') {
        tmp[--len] = '\0';
    }
    if (len > 0 && tmp[len - 1] == '.') {
        tmp[--len] = '\0';
    }
    if (len == 0) {
        snprintf(tmp, sizeof(tmp), "0");
    }
    snprintf(buf, buflen, "%s", tmp);
}

const char *calc_result_text(calc_result_t t)
{
    switch (t) {
    case CALC_OK:            return "";
    case CALC_ERR_SYNTAX:    return "算式不完整";
    case CALC_ERR_DIV_ZERO:  return "不能除以零";
    case CALC_ERR_OVERFLOW:  return "结果太大";
    default:                 return "错误";
    }
}
