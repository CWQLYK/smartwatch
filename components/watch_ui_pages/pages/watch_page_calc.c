/**
 * @file watch_page_calc.c
 * @brief 计算器（模态子页）—— v0.7.0
 *
 * ## 分层
 *
 * 求值全部交给 `calc_engine.c`（纯逻辑，自检里有 34 条测试向量锁死）。
 * 本文件只负责三件事：
 *   1. 搭界面（显示区 + 3×7 键盘）
 *   2. **按键守卫**：把非法序列挡在门外，别让引擎去报泛化的语法错误
 *   3. 按键 → 拼表达式 → 更新显示
 *
 * ## 为什么把校验放在按键层（参考工程的教训）
 *
 * 参考工程 `fs_calculator.cpp` 几乎不设输入守卫，什么都交给引擎，
 * 于是用户能按出 `5+*3` 这种串，只能得到一句泛化的 `Error`，
 * 而且它"Error 之后按运算符会得到前导运算符、下次按 = 必然又报错"——卡死。
 * 我们把守卫前置，体验差别很大。
 *
 * ## 键盘布局（240×280 屏，内容区宽 216）
 *
 * ```
 *   ┌────────────────────┐
 *   │  显示区 (216×70)    │  表达式 + 实时结果
 *   ├────────────────────┤
 *   │  C    ÷    ×       │
 *   │  7    8    9       │  3 列 × 6 行（150px 高）
 *   │  4    5    6       │  键约 67×21
 *   │  1    2    3       │
 *   │  0    .    ⌫       │
 *   │  =    %    +       │
 *   └────────────────────┘
 * ```
 *
 * 减号不加单独键位 —— 需要时先按 `=` 得到一个负数，或者用 `0-3` 这种写法。
 * （参考工程也没有一元负号；我们的**引擎**支持，只是键盘上没给专门键位。）
 *
 * ## 与参考工程的两处界面取舍
 *
 * - 它用 `lv_textarea` 当纯显示（内部还会再建一个 label 子对象）；
 *   我们用 `lv_label`，少一层对象、也不用管光标/滚动。
 * - 它的"退格"是独立 `lv_btn` 放在键盘外面（和第一行有约 19px 的压叠，
 *   因为它忘了清主题默认 pad）；我们把 ⌫ 直接放进键盘里，布局干净。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_strings.h"
#include "watch_modal.h"
#include "watch_toast.h"
#include "calc_engine.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "CALC";

/* 显示区高度 */
#define CALC_DISP_H     70
/*
 * 键盘高度：**必须给一个明确的像素值**。
 *
 * ⚠️ 这里踩过（截图才发现的）：模态的内容容器（`watch_modal.c` 的 body）
 *    高度是 `LV_SIZE_CONTENT` —— 也就是**不确定高度**。
 *    在里面用 `LV_PCT(100)` 或 `lv_obj_set_flex_grow()` 都算不出高度，
 *    结果键盘被渲染成 **0 像素高**：屏幕上只剩标题和显示区，键盘整块消失。
 *    （同理，显示区的 `LV_PCT(100)` **宽度**没问题，因为父容器宽度是确定的。）
 *
 * 可用高度：页面内容区 280-2×12=256，减模态标题行 TITLE_H(26) → 230。
 *   70(显示区) + 6(间隙) + 150(键盘) = 226 ≤ 230 ✅
 *   6 行 → 每行 (150 - 2×3 - 5×4)/6 ≈ 20.7px；键宽 (216 - 2×3 - 2×4)/3 ≈ 67px。
 *   键偏矮但够点（参考工程那套也是 30px 高、240 宽 4 列）。
 */
#define CALC_KB_H       150
/* 键盘行数 / 列数 */
#define CALC_ROWS       6
#define CALC_COLS       3

/*
 * ★ 按键表必须是 **static 常驻**：`lv_btnmatrix` 只存指针
 *   （`lv_btnmatrix.c:102` 的 `btnm->map_p = map;`），
 *   写成栈上临时数组会悬空 —— 项目早期踩过这个坑。
 *
 * ★ 行分隔符是**单独的 `"\n"` 元素**（`lv_btnmatrix.c:130`：
 *   它靠 `strcmp(map_row[i], "\n") == 0` 判断一行结束）。
 *   ⚠️ 这里踩过：第一版按"每 3 个一格、用空串结尾"写，
 *      结果整张表被当成**一行 18 键** —— 屏幕上文字连成一排、
 *      18 根竖条，完全没有网格（截图才发现）。
 *      空串元素只表示"这一行到此为止"，并不等于换行。
 *
 * ★ 乘除用 **ASCII 的 `*` `/`**，不用 `×` `÷`。
 *   我们只生成"数字+字母"的 ASCII 字体（`tools/gen_font.ps1`），
 *   没有 `×`(U+00D7) / `÷`(U+00F7) / `⌫`(U+232B) 这几个字形 ——
 *   用它们会显示成**豆腐块**（截图当场发现）。
 *   退格用 LVGL 自带图标，项目里已经用过、证明能渲染。
 */
static const char *s_map[] = {
    "C",  "/",  "*",  "\n",
    "7",  "8",  "9",  "\n",
    "4",  "5",  "6",  "\n",
    "1",  "2",  "3",  "\n",
    "0",  ".",  LV_SYMBOL_BACKSPACE,  "\n",
    "=",  "%",  "+",  "",
};

/*
 * 按钮下标：**不含换行符**，按行的顺序连续编号
 * （`lv_btnmatrix.c:174` 的 `btn_tot_i` 只对真实按钮自增）。
 *
 * ⚠️ 这里踩过一次：原来用 `(CALC_ROWS - 2) * CALC_COLS + 2` 从末尾倒数，
 *    算出来是 **17**，而那正好是 `+` 键 —— 于是**按 + 变成退格**。
 *    教训：别靠"从末尾倒数"，直接按按钮在网格里的实际行列写死。
 */
#define BTN_ID(r, c)    ((r) * CALC_COLS + (c))
/* ⌫ 在第 5 行（r=4）第 3 列（c=2） */
#define KEY_BACKSPACE   BTN_ID(4, 2)   /* = 14 */
/* = 在第 6 行（r=5）第 1 列（c=0） */
#define KEY_EQUAL       BTN_ID(5, 0)   /* = 15 */

/* ==================== 内部状态 ==================== */
static struct {
    lv_obj_t   *disp;       /* 显示区（表达式 + 结果）*/
    lv_obj_t   *lb_expr;    /* 第一行：表达式 */
    lv_obj_t   *lb_res;     /* 第二行：实时结果 / 错误说明 */
    lv_obj_t   *btnm;       /* 键盘 */
    char        expr[CALC_EXPR_MAX];
    uint8_t     len;
    bool        just_evaluated;   /* 刚刚按过 = ：下一个数字键开新算式 */
} ui;

/* ==================== 显示刷新 ==================== */

/** @brief 把表达式与"实时结果"画到显示区 */
static void refresh_display(void)
{
    if (!ui.lb_expr || !ui.lb_res) {
        return;
    }

    w_label_set_text_auto(ui.lb_expr, ui.expr[0] ? ui.expr : "0");

    if (ui.len == 0) {
        w_label_set_text_auto(ui.lb_res, "");
        return;
    }

    const calc_eval_t r = calc_evaluate(ui.expr);
    if (r.type == CALC_OK) {
        char buf[40];
        calc_format(r.value, buf, sizeof(buf));
        w_label_set_text_auto(ui.lb_res, buf);
        lv_obj_set_style_text_color(ui.lb_res, W_COLOR_ACCENT, 0);
    } else {
        /*
         * 输到一半的不完整算式（比如刚按完 `+`）报"算式不完整"太吵，
         * 那种情况先不显示结果；只有真的按了 `=` 才把错误说出来。
         */
        w_label_set_text_auto(ui.lb_res, "");
    }
}

/* ==================== 按键守卫 ==================== */

/** @brief 表达式最后一个字符（'\0' 表示空）*/
static char expr_last(void)
{
    return ui.len ? ui.expr[ui.len - 1] : '\0';
}

static bool last_is_digit(void)
{
    const char c = expr_last();
    return (c >= '0' && c <= '9');
}

static bool last_is_op(void)
{
    const char c = expr_last();
    return (c == '+' || c == '-' || c == '*' || c == '/');
}

/** @brief 追加一个字符（自动限长）*/
static bool expr_push(char c)
{
    if (ui.len + 1 >= CALC_EXPR_MAX) {
        return false;               /* 到上限了：静默忽略，别让它溢出 */
    }
    ui.expr[ui.len++] = c;
    ui.expr[ui.len]   = '\0';
    return true;
}

/** @brief 处理一个数字/小数点 */
static void key_digit(char c)
{
    /* 按过 = 之后再按数字 → 开始一个新算式（参考工程这里会卡在前一个结果上）*/
    if (ui.just_evaluated) {
        ui.len   = 0;
        ui.expr[0] = '\0';
        ui.just_evaluated = false;
    }

    if (c == '.') {
        /*
         * 守卫：一个数里只能有一个小数点。
         *
         * ⚠️ 这段"回扫当前数字起点"的逻辑连着踩了三次，最后这个写法才对：
         *    1. 边界写成 `i > 0` → 把索引 0 的数字漏掉；
         *    2. 扫描范围少算一格 → `1.5` 的 `.` 落在范围外；
         *    3. **回扫遇到 `.` 就停** → `1.5` 只回扫到 `5`（索引 2），
         *       索引 1 的 `.` 永远扫不到，于是 `1.5` 后面还能再按一个点。
         *    正确的"当前数字"边界是"最近一个运算符（或表达式开头）之后"，
         *    所以回扫要**把 `.` 当成数字的一部分一起跳过**。
         *
         * 引擎其实也会挡（`1.5.2` 报语法错误，见 num_token_valid），
         * 但那会显示成一句错误提示；在按键层直接忽略，用户什么都看不到、更顺。
         */
        int i = ui.len;
        while (i > 0) {
            const char p = ui.expr[i - 1];
            if ((p >= '0' && p <= '9') || p == '.') {
                i--;                /* 数字与小数点都算"当前这个数"的一部分 */
            } else {
                break;              /* 撞到运算符 → 起点就在这里 */
            }
        }
        for (int k = i; k < ui.len; k++) {
            if (ui.expr[k] == '.') {
                return;             /* 这个数已经有小数点了 */
            }
        }
        /* 起始位置直接按 "." → 补一个前导 0，避免出现 "." 这种非法 token */
        if (ui.len == 0 || last_is_op()) {
            expr_push('0');
        }
        expr_push('.');
        return;
    }

    expr_push(c);
}

/** @brief 处理一个运算符 */
static void key_op(char c)
{
    ui.just_evaluated = false;

    if (ui.len == 0) {
        /*
         * 空表达式时只有 `-` 有意义（负号），其余忽略。
         * 参考工程这里会直接把运算符塞进去，下一步必然报错。
         */
        if (c == '-') {
            expr_push('-');
        }
        return;
    }

    if (last_is_op()) {
        const char prev = expr_last();
        /*
         * `-` 后面允许再跟一个 `-`：`5--3` = 8（引擎支持一元负号）。
         * 其它"运算符接运算符"一律用新按键**替换**掉旧的那个，
         * 而不是追加 —— 这是所有计算器的通用手感。
         */
        if (c == '-' && prev != '-') {
            expr_push('-');
        } else {
            ui.expr[ui.len - 1] = c;
        }
        return;
    }

    expr_push(c);
}

/** @brief 处理 `%` */
static void key_percent(void)
{
    ui.just_evaluated = false;
    /*
     * 守卫：`%` 只能跟在一个数后面。
     * 照抄参考工程的话这里会放进一个孤立的 `%`，引擎只能报语法错误；
     * 它源码里那个 `last_char == '%'` 的判断其实是死代码（`%` 不是数字）。
     */
    if (!last_is_digit()) {
        return;
    }
    expr_push('%');
}

/** @brief 处理 `=` */
static void key_equal(void)
{
    if (ui.len == 0) {
        return;
    }

    const calc_eval_t r = calc_evaluate(ui.expr);
    if (r.type != CALC_OK) {
        /* 把原因明说出来（参考工程只说一句 Error）*/
        w_label_set_text_auto(ui.lb_res, calc_result_text(r.type));
        lv_obj_set_style_text_color(ui.lb_res, W_COLOR_WARN, 0);
        return;
    }

    /*
     * ★ 关键：用 calc_format_reusable() 把结果写回表达式，
     *   而不是拿显示用的文本回灌（参考工程就是回灌 `%.10g` 的显示串，
     *   一旦出现科学计数法 `1e+15`，它的清洗步骤会把 `e` 丢掉 →
     *   变成 `1+15`，**静默算错**）。这样链式运算才安全。
     */
    char buf[CALC_EXPR_MAX];
    calc_format_reusable(r.value, buf, sizeof(buf));
    snprintf(ui.expr, sizeof(ui.expr), "%s", buf);
    ui.len = (uint8_t)strlen(ui.expr);
    ui.just_evaluated = true;

    refresh_display();
    /* 结果已经显示在下面了，上面那行同步成结果本身，方便接着算 */
    w_label_set_text_auto(ui.lb_expr, ui.expr);
}

/** @brief 键盘事件 */
static void btnm_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    lv_obj_t *bm = lv_event_get_target(e);
    const uint32_t id = lv_btnmatrix_get_selected_btn(bm);
    const char *txt = lv_btnmatrix_get_btn_text(bm, id);
    if (!txt || !txt[0]) {
        return;                     /* 空占位键 */
    }

    ESP_LOGD(TAG, "按键 '%s'（id=%u）", txt, (unsigned)id);

    /* ---- 清空 ---- */
    if (strcmp(txt, "C") == 0) {
        ui.len = 0;
        ui.expr[0] = '\0';
        ui.just_evaluated = false;
        refresh_display();
        return;
    }

    /* ---- 退格（图标键，用下标认，不能比文案）---- */
    if (id == KEY_BACKSPACE) {
        if (ui.len > 0) {
            ui.expr[--ui.len] = '\0';
        }
        ui.just_evaluated = false;
        refresh_display();
        return;
    }

    /* ---- 等于 ---- */
    if (id == KEY_EQUAL || strcmp(txt, "=") == 0) {
        key_equal();
        return;
    }

    /* ---- 百分号 ---- */
    if (strcmp(txt, "%") == 0) {
        key_percent();
        refresh_display();
        return;
    }

    /* ---- 四则运算符（键盘上就是 ASCII 的 + - * /，引擎直接认）---- */
    if (txt[1] == '\0' && (txt[0] == '+' || txt[0] == '-' ||
                           txt[0] == '*' || txt[0] == '/')) {
        key_op(txt[0]);
        refresh_display();
        return;
    }

    /* ---- 数字 / 小数点 ---- */
    if ((txt[0] >= '0' && txt[0] <= '9') || txt[0] == '.') {
        key_digit(txt[0]);
        refresh_display();
        return;
    }
}

/* ==================== 建界面 ==================== */

static void calc_build(lv_obj_t *parent, void *user)
{
    (void)user;

    /*
     * 关掉内容容器的滚动条。
     *
     * 模态的内容容器（`watch_modal.c` 的 body）是不带滚动条的，
     * 但它的**父**（模态页根 page）是可滚动的 —— body 恰好比可视区高一点点时，
     * 右边会出现一条灰滚条（截图里看到的）。
     * 计算器一共就"显示区 + 键盘"两块，本来就该一屏放下，不需要滚动。
     */
    if (parent) {
        lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
        lv_obj_t *root = lv_obj_get_parent(parent);
        if (root) {
            lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
        }
    }

    /* ---------- 显示区 ---------- */
    ui.disp = w_card_create(parent, W_AUTO_POS, W_AUTO_POS,
                            LV_PCT(100), CALC_DISP_H);

    ui.lb_expr = w_label_create_auto(ui.disp, "0", W_FONT_MEDIUM, 20,
                                     W_COLOR_TEXT);
    lv_obj_set_width(ui.lb_expr, LV_PCT(100));
    lv_obj_align(ui.lb_expr, LV_ALIGN_TOP_LEFT, 0, 0);
    /* 表达式长了就左侧截断（看到最近输入的才重要）*/
    lv_label_set_long_mode(ui.lb_expr, LV_LABEL_LONG_SCROLL_CIRCULAR);

    ui.lb_res = w_label_create_auto(ui.disp, "", W_FONT_LARGE, 20,
                                    W_COLOR_ACCENT);
    lv_obj_set_width(ui.lb_res, LV_PCT(100));
    lv_obj_align(ui.lb_res, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_text_align(ui.lb_res, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(ui.lb_res, LV_LABEL_LONG_SCROLL_CIRCULAR);

    /* ---------- 键盘 ---------- */
    ui.btnm = lv_btnmatrix_create(parent);
    lv_btnmatrix_set_map(ui.btnm, s_map);
    /*
     * 宽度可以用百分比（父容器宽度确定），**高度必须写死**
     * —— 父容器高度是 SIZE_CONTENT，百分比算不出来（见 CALC_KB_H 的说明）。
     */
    lv_obj_set_size(ui.btnm, LV_PCT(100), CALC_KB_H);
    lv_obj_set_style_pad_all(ui.btnm, 3, 0);
    lv_obj_set_style_pad_row(ui.btnm, 4, 0);
    lv_obj_set_style_pad_column(ui.btnm, 4, 0);
    lv_obj_set_style_bg_opa(ui.btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.btnm, 0, 0);
    lv_obj_set_style_radius(ui.btnm, W_RADIUS_BTN, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(ui.btnm, W_COLOR_CARD, LV_PART_ITEMS);
    lv_obj_set_style_text_color(ui.btnm, W_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(ui.btnm, W_FONT_MEDIUM, LV_PART_ITEMS);
    /* 按下的高亮，让触摸有反馈 */
    lv_obj_set_style_bg_color(ui.btnm, W_COLOR_CARD_HI, LV_PART_ITEMS | LV_STATE_PRESSED);
    /* 运算符/结果键用主色区分 */
    lv_obj_set_style_text_color(ui.btnm, W_COLOR_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_add_event_cb(ui.btnm, btnm_cb, LV_EVENT_VALUE_CHANGED, NULL);

    ESP_LOGI(TAG, "计算器就绪（键盘 %d 键，表达式上限 %d 字符）",
             CALC_ROWS * CALC_COLS, CALC_EXPR_MAX - 1);
}

void watch_calc_open(void)
{
    /* 每次进页面都从干净状态开始（参考工程也这么做，且它连分数都不留）*/
    memset(&ui, 0, sizeof(ui));
    w_modal_open(W_STR_CALC_TITLE, calc_build, NULL);
}

/* ==================== 自检接口 ==================== */

lv_obj_t *watch_calc_debug_btnmatrix(void)
{
    return ui.btnm;
}

const char *watch_calc_debug_expr_text(void)
{
    return ui.expr;
}
