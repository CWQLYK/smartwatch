/**
 * @file watch_widgets.c
 * @brief 手表界面公共控件实现
 */
#include "watch_widgets.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
     * 页面根**不可滚动**：跟手滑动靠外层 viewport 来滚，
     * 页面自己一滚就把拖动吃掉了（又是一次"滑不动"）。
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
 * 用一张静态表记住每个标签的"ASCII 字体偏好"。
 * 标签数量是有限且固定的（一个界面几十个），表够用；
 * 表满时降级为"只设中文字体"，不会出错，只是数字不那么好看。
 */
#define AUTO_FONT_MAX   24

typedef struct {
    lv_obj_t       *label;
    const lv_font_t *ascii_font;
    int              cjk_size;
} auto_font_t;

static auto_font_t s_auto[AUTO_FONT_MAX];

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
            return;
        }
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
        return;                     /* 没登记过就别乱动字体 */
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

    lv_obj_t *val_lbl = w_label_create(parent, value, W_FONT_MEDIUM, W_COLOR_AUTO);
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
