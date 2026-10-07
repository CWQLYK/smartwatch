/**
 * @file watch_widgets.c
 * @brief 手表界面公共控件实现
 */
#include "watch_widgets.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "WIDGET";

/* ==================== 容器 ==================== */
lv_obj_t *w_page_create(lv_obj_t *parent)
{
    /*
     * ⚠️ 必须是 lv_obj_create(parent) —— 不能是 lv_obj_create(NULL)。
     *    后者造出来的是 **screen**，而 screen 挂不进滑动轮播的容器里
     *    （lv_obj_set_parent 会拒绝），症状是页面全白。
     */
    lv_obj_t *root = lv_obj_create(parent);

    /* 占满整个槽（槽本身已经是 240x280） */
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);

    lv_obj_set_style_bg_color(root, W_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, W_PAD_PAGE, 0);

    /*
     * 页面根**不可滚动**（SCROLLABLE 清掉、scroll_dir = NONE）。
     *
     * ⚠️ 这个决定和两个之前踩过的坑直接相关，改之前先读完：
     *
     * 1. **为什么不让它自己滚**：一旦页面根是 SCROLLABLE，
     *    LVGL 的 find_scroll_obj() 就会把它当成滚动候选 —— **哪怕内容
     *    根本没超出**（那是为了做弹性回弹效果）。后果是
     *    `indev_gesture()` 开头的 `if(scroll_obj) return;` 会直接返回，
     *    **"上滑打开控制中心"在内容不满一屏的页面上就失效了**。
     *
     * 2. **为什么纵向滚动由外层轮播代管**：页面内容可能超过一屏
     *    （设置页就有 450px，屏幕只有 280px），必须能滚。
     *    但横向已经由轮播手动接管，如果让页面根可滚，两条路径会打架。
     *    所以统一在 watch_pages.c 的 on_pressing() 里判断：
     *    主轴是纵向且内容溢出 → 用 lv_obj_scroll_by_bounded() 滚当前页；
     *    否则纵向留给手势层（上滑开控制中心）。
     *
     * 注意 SCROLLABLE 和 SCROLL_DIR 是两个东西，都要设：
     * LVGL 的 find_scroll_obj() 判的是 scroll_dir。
     */
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(root, LV_DIR_NONE);

    /* 保持可点击：事件要从子控件沿 EVENT_BUBBLE 链爬到 viewport */
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);

    return root;
}

lv_obj_t *w_card_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);

    if (x != W_AUTO_POS || y != W_AUTO_POS) {
        lv_obj_set_pos(card, x, y);
    }
    if (w != W_AUTO_SIZE && h != W_AUTO_SIZE) {
        lv_obj_set_size(card, w, h);
    } else {
        lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    }

    /*
     * 卡片不可滚动 —— 这一条对"跟手滑动切页"是**必需**的：
     * LVGL 的 find_scroll_obj() 是判 scroll_dir 而不是 SCROLLABLE 标志，
     * 只要卡片 scroll_dir 不是 NONE，它就会把拖动当成"滚这张卡片"吃掉，
     * 外层 viewport 就收不到滚动 → 表现为"划不动"。
     */
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_NONE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);   /* 默认不抢事件 */
    lv_obj_set_style_bg_color(card, W_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, W_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card, W_PAD_CARD, 0);

    return card;
}

lv_obj_t *w_card_create_clickable(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                  lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = w_card_create(parent, x, y, w, h);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    /* 按下变亮一点，手指有反馈 */
    lv_obj_set_style_bg_color(card, W_COLOR_CARD_HI, LV_STATE_PRESSED);

    return card;
}

/* ==================== 文字 ==================== */
bool w_text_has_non_ascii(const char *text)
{
    if (!text) {
        return false;
    }
    /* UTF-8 多字节字符的首字节都 >= 0x80 */
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p >= 0x80) {
            return true;
        }
    }
    return false;
}

lv_obj_t *w_label_create(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);

    lv_obj_set_style_text_font(lbl, font ? font : W_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl,
                                (color.full == W_COLOR_AUTO.full) ? W_COLOR_TEXT : color, 0);
    lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, 0);
    lv_label_set_text(lbl, text ? text : "");

    return lbl;
}

lv_obj_t *w_label_create_auto(lv_obj_t *parent, const char *text,
                              const lv_font_t *ascii_font, int cjk_size,
                              lv_color_t color)
{
    lv_obj_t *lbl = w_label_create(parent, text,
                                   w_text_has_non_ascii(text)
                                       ? watch_font_cn(cjk_size)
                                       : (ascii_font ? ascii_font : W_FONT_SMALL),
                                   color);
    /* 记住偏好，之后 w_label_set_text_auto() 换文案时才能切回来 */
    w_label_set_auto_font(lbl, ascii_font, cjk_size);
    return lbl;
}

/*
 * ==================== 自动字体（防豆腐块） ====================
 *
 * 用一张静态表记住每个标签的"ASCII 字体偏好"：
 * 内容是数字/英文 → 用 Montserrat（好看）；出现中文 → 自动切中文字库。
 *
 * ## ★ v0.5.0 修掉的一个严重 bug（用户实测：部分中文变豆腐块）
 *
 * 原来这张表**只增不减**：页面重建（模态 refresh、轮播换页）会把标签删掉，
 * 但表里的记录还在。于是：
 *   1. **表很快被占满**（原来只有 24 格）。占满之后 `w_label_set_auto_font()`
 *      直接 `return` —— 什么都不做（注释还写着"降级为只设中文字体"，是错的）。
 *      之后新建的标签查不到记录 → `w_label_set_text_auto()` 提前返回
 *      → 标签保持创建时的 ASCII 字体 → **中文全变豆腐块**。
 *   2. 更隐蔽：删掉的标签地址会被新标签复用，`find_auto()` 命中**残留记录**，
 *      于是拿到别的标签的字体偏好。
 *
 * 症状之所以是"**部分**中文出问题"，就是因为只有最早那二十几个标签
 * 抢到了格子。v0.5.0 新增的时钟页每次切标签都重建一整屏控件，
 * 24 格瞬间见底，问题才暴露出来。
 *
 * 三条修法（缺一不可）：
 *   - 标签被删除时**释放格子**（挂 LV_EVENT_DELETE）
 *   - 格子数放宽到 64（一条记录才 12 字节，代价可忽略）
 *   - 查不到记录时**兜底**：内容含中文就直接上中文字体 ——
 *     宁可字号不理想，也绝不留豆腐块
 */
#define AUTO_FONT_MAX       64
/* 查不到记录时兜底用的中文字号（和 watch_font_cn(14) 一致） */
#define AUTO_CJK_FALLBACK   14

typedef struct {
    lv_obj_t       *label;
    const lv_font_t *ascii_font;
    int              cjk_size;
} auto_font_t;

static auto_font_t s_auto[AUTO_FONT_MAX];

/** @brief 标签被删除时释放它的格子（否则表会被重建出来的标签撑满） */
static void auto_font_on_delete(lv_event_t *e)
{
    lv_obj_t *lbl = lv_event_get_target(e);
    for (int i = 0; i < AUTO_FONT_MAX; i++) {
        if (s_auto[i].label == lbl) {
            s_auto[i].label      = NULL;
            s_auto[i].ascii_font = NULL;
            s_auto[i].cjk_size   = 0;
            return;
        }
    }
}

void w_label_set_auto_font(lv_obj_t *label, const lv_font_t *ascii_font, int cjk_size)
{
    if (!label) {
        return;
    }
    /* 已有记录 → 更新 */
    for (int i = 0; i < AUTO_FONT_MAX; i++) {
        if (s_auto[i].label == label) {
            s_auto[i].ascii_font = ascii_font;
            s_auto[i].cjk_size   = cjk_size;
            return;
        }
    }
    /* 新记录 */
    for (int i = 0; i < AUTO_FONT_MAX; i++) {
        if (s_auto[i].label == NULL) {
            s_auto[i].label      = label;
            s_auto[i].ascii_font = ascii_font;
            s_auto[i].cjk_size   = cjk_size;
            /* 删除时回收格子 —— 这一行是防"表被用满"的关键，别删 */
            lv_obj_add_event_cb(label, auto_font_on_delete, LV_EVENT_DELETE, NULL);
            return;
        }
    }

    /*
     * 表满（正常不会再发生，因为格子会回收）。
     * 兜底：内容有中文就直接上中文字体，**绝不静默什么都不做**。
     */
    ESP_LOGW(TAG, "自动字体表已满（%d 格），本标签改用即时判断", AUTO_FONT_MAX);
    if (w_text_has_non_ascii(lv_label_get_text(label))) {
        lv_obj_set_style_text_font(label, watch_font_cn(cjk_size ? cjk_size
                                                                : AUTO_CJK_FALLBACK), 0);
    }
}

static const auto_font_t *find_auto(lv_obj_t *label)
{
    for (int i = 0; i < AUTO_FONT_MAX; i++) {
        if (s_auto[i].label == label) {
            return &s_auto[i];
        }
    }
    return NULL;
}

void w_label_set_text_auto(lv_obj_t *label, const char *text)
{
    if (!label) {
        return;
    }
    lv_label_set_text(label, text ? text : "");

    const auto_font_t *af = find_auto(label);
    if (!af) {
        /*
         * 没登记过（不是 w_label_create_auto 造的，或表满降级）。
         *
         * ⚠️ 这里**不能直接 return**：那样含中文的文案会保持 ASCII 字体
         *    → 豆腐块（v0.5.0 用户实测到的就是这个）。
         *    但也**不能无脑覆盖字体** —— 纯 ASCII 时可能是调用方
         *    故意设的大号数字字体（如 W_FONT_CLOCK），覆盖了会变小。
         *    所以只在"内容含中文"时兜底。
         */
        if (w_text_has_non_ascii(text)) {
            lv_obj_set_style_text_font(label, watch_font_cn(AUTO_CJK_FALLBACK), 0);
        }
        return;
    }
    lv_obj_set_style_text_font(label,
                               w_text_has_non_ascii(text)
                                   ? watch_font_cn(af->cjk_size)
                                   : (af->ascii_font ? af->ascii_font : W_FONT_SMALL),
                               0);
}

void w_label_set_text_fmt_auto(lv_obj_t *label, const char *fmt, ...)
{
    if (!label || !fmt) {
        return;
    }
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    w_label_set_text_auto(label, buf);
}

lv_obj_t *w_label_value_create(lv_obj_t *parent, const char *title,
                               const char *value)
{
    lv_obj_t *title_lbl = w_label_create(parent, title, watch_font_cn(14),
                                         W_COLOR_TEXT_DIM);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    /*
     * ★ v0.5.0：这里原来是 `w_label_create(parent, value, W_FONT_MEDIUM, ...)` ——
     *   把字体写死成 Montserrat（只有 ASCII）。而 `value` 完全可能是中文
     *   （"--未接入"/"开"/"关"），那就是豆腐块。
     *
     *   这正是本项目踩过四次的坑（日期格式串 / 设置页值列 / 分隔符 /
     *   键盘功能键），`tools/check_fonts.py` 也把它报了出来。
     *   改用 `w_label_create_auto()`：按内容自动选字体，从根上杜绝。
     */
    lv_obj_t *val_lbl = w_label_create_auto(parent, value, W_FONT_MEDIUM, 16,
                                            W_COLOR_AUTO);
    lv_obj_align(val_lbl, LV_ALIGN_LEFT_MID, 0, 8);

    return val_lbl;
}

lv_obj_t *w_metric_card_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                               lv_coord_t w, lv_coord_t h,
                               const char *title, const char *value,
                               lv_obj_t **out_value)
{
    lv_obj_t *card = w_card_create(parent, x, y, w, h);

    lv_obj_t *t = w_label_create(card, title, watch_font_cn(14), W_COLOR_TEXT_DIM);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);

    /*
     * 数值用"自动字体"：数字时用 Montserrat（好看），
     * 一旦变成中文占位符自动切中文字体，避免豆腐块。
     */
    lv_obj_t *v = w_label_create_auto(card, value, W_FONT_MEDIUM, 16,
                                      W_COLOR_ACCENT);
    lv_obj_align(v, LV_ALIGN_CENTER, 0, 9);

    if (out_value) {
        *out_value = v;
    }
    return card;
}

/* ==================== 电池图标 ==================== */
/*
 * 画法：外壳（描边矩形）+ 内部电量条 + 右侧正极小凸起
 *
 *    ┌────────┐
 *    │████    │▮      ← 电量条颜色随电量变化
 *    └────────┘
 */
lv_obj_t *w_battery_icon_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                lv_obj_t **out_fill)
{
    /* 外壳：留 2px 给正极凸起 */
    lv_obj_t *shell = lv_obj_create(parent);
    lv_obj_set_size(shell, w - 2, h);
    lv_obj_clear_flag(shell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(shell, LV_DIR_NONE);      /* 别抢外层轮播的拖动 */
    lv_obj_clear_flag(shell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(shell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shell, 2, 0);
    lv_obj_set_style_border_color(shell, W_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_radius(shell, 3, 0);
    lv_obj_set_style_pad_all(shell, 1, 0);

    /* 内部电量条 */
    lv_obj_t *fill = lv_obj_create(shell);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(fill, LV_DIR_NONE);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, 1, 0);
    lv_obj_set_style_bg_color(fill, W_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_height(fill, LV_PCT(100));
    lv_obj_set_width(fill, LV_PCT(70));

    /* 正极凸起 */
    lv_obj_t *tip = lv_obj_create(parent);
    lv_obj_set_size(tip, 2, (h * 4) / 10);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tip, LV_DIR_NONE);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(tip, 0, 0);
    lv_obj_set_style_radius(tip, 1, 0);
    lv_obj_set_style_bg_color(tip, W_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
    lv_obj_align_to(tip, shell, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

    if (out_fill) {
        *out_fill = fill;
    }
    return shell;
}

void w_battery_set(lv_obj_t *fill, int percent, bool charging)
{
    if (!fill) return;

    if (percent < 0) {
        /* 未知：灰条 30%，一眼能看出"没数据"而不是"快没电了" */
        lv_obj_set_style_bg_color(fill, W_COLOR_TEXT_MUTE, 0);
        lv_obj_set_width(fill, LV_PCT(30));
        return;
    }

    if (percent > 100) percent = 100;
    if (percent < 0)   percent = 0;
    lv_color_t col;
    if (charging) {
        col = W_COLOR_ACCENT;               /* 充电中：主色 */
    } else if (percent <= 15) {
        col = W_COLOR_DANGER;               /* 低电：红 */
    } else if (percent <= 30) {
        col = W_COLOR_WARN;                 /* 偏低：琥珀 */
    } else {
        col = W_COLOR_TEXT;                 /* 正常：白 */
    }

    lv_obj_set_style_bg_color(fill, col, 0);

    /* 留 2% 底，别让 0% 时看起来像没画出来 */
    int w_pct = (percent < 5) ? 5 : percent;
    lv_obj_set_width(fill, LV_PCT(w_pct));
}

/* ==================== 按钮 / 滑块 / 分隔线 ==================== */
lv_obj_t *w_icon_button_create(lv_obj_t *parent, lv_coord_t d,
                               const char *symbol, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, d, d);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, W_COLOR_CARD, 0);
    lv_obj_set_style_bg_color(btn, W_COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(btn, LV_DIR_NONE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, W_FONT_NORMAL, 0);
    lv_obj_set_style_text_color(lbl, W_COLOR_TEXT, 0);
    lv_label_set_text(lbl, symbol ? symbol : "");
    lv_obj_center(lbl);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

lv_obj_t *w_slider_create(lv_obj_t *parent, lv_coord_t w, int min, int max, int value)
{
    lv_obj_t *sl = lv_slider_create(parent);
    lv_obj_set_width(sl, w);
    lv_obj_set_height(sl, 8);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, value, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(sl, W_COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, W_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, W_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 2, LV_PART_KNOB);

    return sl;
}

lv_obj_t *w_divider_create(lv_obj_t *parent, lv_coord_t w)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, w, 1);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(line, LV_DIR_NONE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_bg_color(line, W_COLOR_TRACK, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(line, 0, 0);

    return line;
}

void w_set_placeholder(lv_obj_t *label, bool placeholder)
{
    if (!label) return;
    lv_obj_set_style_text_color(label,
                               placeholder ? W_COLOR_TEXT_MUTE : W_COLOR_ACCENT, 0);
}

/* ==================================================================
 * 四、菜单行与大按钮（v0.5.0）
 * ================================================================== */

lv_obj_t *w_flex_col_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                            lv_coord_t w, lv_coord_t h, lv_coord_t gap)
{
    lv_obj_t *box = lv_obj_create(parent);

    if (x != W_AUTO_POS || y != W_AUTO_POS) {
        lv_obj_set_pos(box, x, y);
    }
    if (w == W_AUTO_SIZE || h == W_AUTO_SIZE) {
        lv_obj_set_size(box, (w == W_AUTO_SIZE) ? LV_SIZE_CONTENT : w,
                        (h == W_AUTO_SIZE) ? LV_SIZE_CONTENT : h);
    } else {
        lv_obj_set_size(box, w, h);
    }

    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_row(box, gap, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    /* 不滚动、不抢事件 —— 和 w_card_create 同样的理由：
     * 一旦 scroll_dir 不是 NONE，拖动会被它当成"滚这个容器"吃掉，
     * 外层轮播就收不到滚动（表现为"划不动"）。 */
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_NONE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    return box;
}

/* ==================================================================
 * 触摸"拖动 / 点击"判定
 *
 * 完整背景（LVGL 为什么拦不住、为什么不能在这里直接清标记）见 watch_widgets.h。
 * ================================================================== */
static bool s_touch_drag = false;

bool w_touch_was_drag(void)
{
    return s_touch_drag;
}

void w_touch_mark_drag(void)
{
    s_touch_drag = true;
}

void w_touch_clear_drag(void)
{
    s_touch_drag = false;
}

/**
 * @brief 菜单行的点击守卫
 *
 * @note 事件 user_data 里存的是**业务回调本身**（函数指针），不是业务参数 ——
 *       这样"守卫 + 业务回调"只占一次注册，行上不用挂第二个回调。
 *       函数指针与 void* 互转在 C 标准里是"实现定义"，但 GCC / POSIX 都保证可用，
 *       全工程只在这一处这么用。
 */
static void menu_row_click_cb(lv_event_t *e)
{
    if (w_touch_was_drag()) {
        /*
         * 这次按下滑动过，抬手不该算点击 ——
         * 走到这里说明用户是在列表上滑动，只是手指最后停在了这一行上
         * （用户实测报的："从上往下滑，手指触到时钟，滑动后就进了时钟页"）。
         */
        return;
    }
    lv_event_cb_t user_cb = (lv_event_cb_t)lv_event_get_user_data(e);
    if (user_cb) {
        user_cb(e);
    }
}

lv_obj_t *w_menu_row_create(lv_obj_t *parent, const char *title, const char *sub,
                            lv_event_cb_t cb)
{
    lv_obj_t *card = w_card_create_clickable(parent, W_AUTO_POS, W_AUTO_POS,
                                             LV_PCT(100), 52);

    lv_obj_t *t = w_label_create(card, title, watch_font_cn(16), W_COLOR_TEXT);
    if (sub) {
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 2);
        lv_obj_t *s = w_label_create(card, sub, watch_font_cn(14), W_COLOR_TEXT_DIM);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    } else {
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    }

    /* 右箭头提示"可以点进去" */
    lv_obj_t *a = w_label_create(card, ">", W_FONT_SMALL, W_COLOR_ACCENT);
    lv_obj_align(a, LV_ALIGN_RIGHT_MID, 0, 0);

    if (cb) {
        /*
         * ⚠️ 挂的是**守卫**，业务回调塞在 user_data 里 ——
         *    直接挂业务回调的话，拖动后的误点击拦不住（用户实测报的问题）。
         */
        lv_obj_add_event_cb(card, menu_row_click_cb, LV_EVENT_CLICKED, (void *)cb);
    }
    return card;
}

/**
 * @brief 通用按钮的点击守卫
 *
 * @note 和菜单行同一个道理：**轮播页上**的按钮如果不管，
 *       用户在页面上滑动（滚页面）之后抬手停在按钮上，就会误触发。
 *       模态子页里的按钮其实由 LVGL 自己管着（模态是原生滚动，
 *       scroll_obj 非空，CLICKED 本来就不会发），
 *       但这里统一走守卫更省心 —— 守卫只在"这次触摸被标记成拖动"时才拦，
 *       其余情况完全是透传，没有副作用。
 */
static void btn_click_cb(lv_event_t *e)
{
    if (w_touch_was_drag()) {
        return;
    }
    lv_event_cb_t user_cb = (lv_event_cb_t)lv_event_get_user_data(e);
    if (user_cb) {
        user_cb(e);
    }
}

lv_obj_t *w_btn_create(lv_obj_t *parent, const char *text,
                       lv_coord_t w, lv_coord_t h,
                       lv_color_t bg, lv_color_t fg,
                       const lv_font_t *font, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);

    lv_obj_set_size(b, (w == W_AUTO_SIZE) ? LV_PCT(100) : w, h ? h : 48);
    lv_obj_set_style_radius(b, W_RADIUS_BTN, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_color(b,
                              (bg.full == W_COLOR_AUTO.full) ? W_COLOR_CARD : bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    /* 按下时压暗一点，手指有反馈 */
    lv_obj_set_style_bg_color(b, W_COLOR_CARD_HI, LV_STATE_PRESSED);

    lv_obj_t *l = w_label_create(b, text, font ? font : watch_font_cn(16),
                                 (fg.full == W_COLOR_AUTO.full) ? W_COLOR_TEXT : fg);
    lv_obj_center(l);

    if (cb) {
        lv_obj_add_event_cb(b, btn_click_cb, LV_EVENT_CLICKED, (void *)cb);
    }
    return b;
}

/* ==================================================================
 * 标签栏（分段按钮）
 *
 * ## 为什么不用 lv_btnmatrix（v0.5.0 换掉的）
 *
 * 第一版用的是 `lv_btnmatrix`：它原生支持"同时只有一个选中"，
 * 看起来很合适。但实测（PC 模拟器截图）**4 个标签被画成了一个大按钮**，
 * 而且踩到两个坑：
 *
 *   1. `lv_btnmatrix_set_map()` **只存指针、不拷贝字符串**
 *      （lv_btnmatrix.c:102 `btnm->map_p = map;`）。
 *      用复合字面量 `(const char *[]){ labels, "" }` 传进去，
 *      函数返回后数组就没了 → 悬空指针 → 访问违例崩溃。
 *      （这个模拟器自检直接抓到了。）
 *   2. 靠 `\n` 分隔按钮、靠 ctrl 位图表示"可选中/已选中/宽度"，
 *      这些隐式约定很容易出错，而画出来的东西**不对也不报错**。
 *
 * 所以换成最直白的做法：**N 个普通按钮 + flex 等宽平分**，
 * 选中态自己维护（改背景色 + 字色）。
 * 代价是多十几行，换来的是"所见即所得、出错一眼能看出来"。
 *
 * ⚠️ 同一时刻只支持一个标签栏（全项目只有时钟页用）。
 *    要支持多个的话，得把状态挂到容器上而不是用这个静态实例。
 * ================================================================== */
#define W_TABBAR_MAX   4
#define W_TABBAR_GAP   4
#define W_TABBAR_H     32

typedef struct {
    lv_obj_t     *cont;
    lv_obj_t     *btn[W_TABBAR_MAX];
    lv_obj_t     *lbl[W_TABBAR_MAX];
    int           count;
    int           active;
    w_tabbar_cb_t cb;
    void         *user;
} w_tabbar_t;

static w_tabbar_t s_tabbar;

/** @brief 应用选中态（改颜色） */
static void tabbar_apply(int idx)
{
    if (idx < 0 || idx >= s_tabbar.count) {
        return;
    }
    s_tabbar.active = idx;
    for (int i = 0; i < s_tabbar.count; i++) {
        const bool on = (i == idx);
        lv_obj_set_style_bg_color(s_tabbar.btn[i],
                                  on ? W_COLOR_ACCENT : W_COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(s_tabbar.btn[i], LV_OPA_COVER, 0);
        /* 字色必须设在**标签**上：创建时给了显式颜色，
         * 显式样式会盖过从按钮继承来的颜色。 */
        lv_obj_set_style_text_color(s_tabbar.lbl[i],
                                    on ? W_COLOR_BG : W_COLOR_TEXT_DIM, 0);
    }
}

static void tabbar_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == s_tabbar.active) {
        return;                     /* 点的是当前页，不折腾 */
    }
    tabbar_apply(idx);

    if (s_tabbar.cb) {
        s_tabbar.cb(idx, s_tabbar.user);
    }
}

lv_obj_t *w_tabbar_create(lv_obj_t *parent, const char *labels, uint16_t active,
                          w_tabbar_cb_t cb, void *user)
{
    memset(&s_tabbar, 0, sizeof(s_tabbar));
    s_tabbar.cb   = cb;
    s_tabbar.user = user;
    s_tabbar.active = -1;

    /* 先按 '\n' 数一下有几项，才能算等宽 */
    char tmp[W_TABBAR_MAX][24];
    int n = 0;
    {
        const char *p = labels ? labels : "";
        while (*p && n < W_TABBAR_MAX) {
            int k = 0;
            while (*p && *p != '\n' && k < (int)sizeof(tmp[0]) - 1) {
                tmp[n][k++] = *p++;
            }
            tmp[n][k] = '\0';
            n++;
            if (*p == '\n') {
                p++;
            }
        }
    }
    if (n == 0) {
        return NULL;
    }

    lv_obj_t *row = w_flex_col_create(parent, W_AUTO_POS, W_AUTO_POS,
                                      LV_PCT(100), W_TABBAR_H, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, W_TABBAR_GAP, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_tabbar.cont = row;

    const lv_coord_t w = (216 - (n - 1) * W_TABBAR_GAP) / n;

    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_btn_create(row);
        lv_obj_set_size(b, w, W_TABBAR_H);
        lv_obj_set_style_radius(b, W_RADIUS_BTN, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, W_COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);

        lv_obj_t *l = w_label_create(b, tmp[i], watch_font_cn(14),
                                     W_COLOR_TEXT_DIM);
        lv_obj_center(l);

        lv_obj_add_event_cb(b, tabbar_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_tabbar.btn[i] = b;
        s_tabbar.lbl[i] = l;
    }
    s_tabbar.count = n;

    tabbar_apply((active < (uint16_t)n) ? (int)active : 0);
    return row;
}

int w_tabbar_set_active(uint16_t idx)
{
    if (s_tabbar.count <= 0 || idx >= (uint16_t)s_tabbar.count) {
        return -1;
    }
    if ((int)idx != s_tabbar.active) {
        tabbar_apply((int)idx);
    }
    return 0;
}

int w_tabbar_active(void)
{
    return s_tabbar.count > 0 ? s_tabbar.active : -1;
}
