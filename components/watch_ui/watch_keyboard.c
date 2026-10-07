/**
 * @file watch_keyboard.c
 * @brief 屏幕小键盘实现
 *
 * 布局（240×280）：
 *
 *     ┌────────────────────────────┐
 *     │ 标题                        │
 *     │ ┌────────────────────────┐ │
 *     │ │ 输入内容…            _ │ │  输入框（显示尾部）
 *     │ └────────────────────────┘ │
 *     │  [⇧/层] [空格] [⌫] [完成]  │  功能行 4 键
 *     │ ┌──┬──┬──┬──┬──┬──┐        │
 *     │ │  │  │  │  │  │  │        │  6×4 字符区
 *     │ ├──┼──┼──┼──┼──┼──┤        │
 *     │ │  │  │  │  │  │  │        │
 *     │ ├──┼──┼──┼──┼──┼──┤        │
 *     │ │  │  │  │  │  │  │        │
 *     │ ├──┼──┼──┼──┼──┼──┤        │
 *     │ │  │  │  │  │  │  │        │
 *     │ └──┴──┴──┴──┴──┴──┘        │
 *     └────────────────────────────┘
 *
 * 键宽 = (240 - 2*8(边距) - 5*4(间隙)) / 6 = 34px，键高 40px。
 * 34×40 对指尖来说偏小，但这是 240 宽屏的物理上限；
 * 实测比"6 列更宽但放不下功能键"要好用。
 */
#include "watch_keyboard.h"
#include "watch_style.h"
#include "watch_widgets.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "KEYBOARD";

/* ==================== 尺寸 ==================== */
#define KB_PAD        8
#define KB_KEY_W      34
#define KB_KEY_H      40
#define KB_GAP        4
#define KB_COLS       6
#define KB_ROWS       4

#define KB_WIDTH      (W_SCREEN_W)
#define KB_GRID_W     (KB_COLS * KB_KEY_W + (KB_COLS - 1) * KB_GAP)
#define KB_GRID_H     (KB_ROWS * KB_KEY_H + (KB_ROWS - 1) * KB_GAP)

/* 输入框只显示尾部这么多字符（屏太窄，全显示会被截断且看不出光标） */
#define KB_TAIL_CHARS 9

/* ==================== 字符层 ==================== */
typedef enum {
    LAY_LOWER = 0,      /* 小写 */
    LAY_UPPER,          /* 大写 */
    LAY_DIGIT,          /* 数字 */
    LAY_SYMBOL,         /* 符号 */
    LAY_COUNT
} kb_layer_t;

static const char *const s_layer_name[LAY_COUNT] = {
    "abc", "ABC", "123", "#+="
};

/*
 * 每层 4 行 × 6 列，共 24 个键。
 * 空字符串表示空位。
 *
 * 符号层的选字原则：只放 WiFi 密码/SSID 里**真的会用到**的。
 * 常见的是 - _ . @ # ! $ % & * + / = ? ~ ^
 */
static const char *const s_layer[LAY_COUNT][KB_ROWS][KB_COLS] = {
    /* ---- 小写 ---- */
    {
        { "q","w","e","r","t","y" },
        { "a","s","d","f","g","h" },
        { "z","x","c","v","b","n" },
        { "1","2","3","4","5","6" },   /* 顺手放一行数字，省得切层 */
    },
    /* ---- 大写 ---- */
    {
        { "Q","W","E","R","T","Y" },
        { "A","S","D","F","G","H" },
        { "Z","X","C","V","B","N" },
        { "7","8","9","0","-","_" },   /* 大写层顺带补上剩余数字和常用符号 */
    },
    /* ---- 数字 ---- */
    {
        { "1","2","3","4","5","6" },
        { "7","8","9","0",".","-" },
        { "_","/",":","@","#","!" },
        { "$","%","&","*","+","=" },
    },
    /* ---- 符号 ---- */
    {
        { "~","^","?","'","\"","\\" },
        { "(",")","[","]","{","}" },
        { "<",">",",",";",":","|" },
        { ".","_","-","/","@","#" },
    },
};

/* ==================== 对象结构 ==================== */
struct w_keyboard_t {
    lv_obj_t *panel;        /* 整个面板 */
    lv_obj_t *input_lbl;    /* 输入内容显示 */
    lv_obj_t *layer_btn;    /* 层级切换键（文字随层变化） */
    lv_obj_t *btn[KB_ROWS][KB_COLS];

    char      text[W_KB_TEXT_MAX];
    size_t    len;
    kb_layer_t layer;
    w_keyboard_done_cb_t on_done;
    void     *user;
    bool      closed;
};

/* ==================== 输入框刷新 ==================== */
/**
 * @brief 刷新输入框显示
 *
 * @note 只显示**尾部**若干字符：密码可能 30+ 位，240px 放不下，
 *       而输密码时用户最关心的是"刚敲进去的那几个对不对"。
 *       被截断时前面加 "…" 提示还有内容。
 */
static void input_refresh(w_keyboard_t *kb)
{
    if (!kb->input_lbl) {
        return;
    }

    char shown[W_KB_TEXT_MAX + 4];
    if (kb->len <= KB_TAIL_CHARS) {
        snprintf(shown, sizeof(shown), "%s", kb->text);
    } else {
        snprintf(shown, sizeof(shown), "…%s", kb->text + (kb->len - KB_TAIL_CHARS));
    }

    /* 末尾加个光标，让人知道还会继续往后接 */
    lv_label_set_text_fmt(kb->input_lbl, "%s_", shown);

    if (kb->len == 0) {
        lv_obj_set_style_text_color(kb->input_lbl, W_COLOR_TEXT_DIM, 0);
    } else {
        lv_obj_set_style_text_color(kb->input_lbl, W_COLOR_TEXT, 0);
    }
}

/* ==================== 键盘操作 ==================== */
static void append_char(w_keyboard_t *kb, const char *ch)
{
    if (!ch || !ch[0]) {
        return;
    }
    if (kb->len + 1 >= W_KB_TEXT_MAX) {
        ESP_LOGW(TAG, "输入已达上限 %d，忽略", W_KB_TEXT_MAX - 1);
        return;
    }
    kb->text[kb->len++] = ch[0];
    kb->text[kb->len]   = '\0';
    input_refresh(kb);
}

static void backspace(w_keyboard_t *kb)
{
    if (kb->len == 0) {
        return;
    }
    kb->text[--kb->len] = '\0';
    input_refresh(kb);
}

/** @brief 刷新层级切换键的文字和字符区 */
static void layer_refresh(w_keyboard_t *kb)
{
    if (kb->layer_btn) {
        lv_label_set_text(lv_obj_get_child(kb->layer_btn, 0), s_layer_name[kb->layer]);
    }
    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            lv_obj_t *b = kb->btn[r][c];
            if (!b) {
                continue;
            }
            lv_obj_t *lbl = lv_obj_get_child(b, 0);
            if (lbl) {
                lv_label_set_text(lbl, s_layer[kb->layer][r][c]);
            }
        }
    }
}

static void cycle_layer(w_keyboard_t *kb)
{
    kb->layer = (kb_layer_t)((kb->layer + 1) % LAY_COUNT);
    layer_refresh(kb);
}

/* ==================== 事件回调 ==================== */
/**
 * @brief 字符键回调：**按当前层实时查字符**，不能用创建时存下的指针
 *
 * ⚠️ 这里修过一个很隐蔽的 bug（用户实测报的"点数字键打出字母"）：
 *
 *   原来是在创建键时把 `s_layer[初始层][r][c]` 的指针存进 user_data，
 *   回调里直接用。但 `layer_refresh()` 切层时**只改了标签文字，
 *   没有更新那个指针** —— 于是切到数字层后，键上显示 "1"，
 *   按下去却仍然返回创建时那一层的 "q"。
 *
 *   现在改成：user_data 只存 **(行,列) 的编号**，回调里用
 *   `s_layer[kb->layer][r][c]` 现查。层变了自然就查对了。
 *   （编号 +1 存，因为 0 转换到 void* 是 NULL，会被当成"没设置"。）
 */
static void key_cb(lv_event_t *e)
{
    w_keyboard_t *kb = (w_keyboard_t *)lv_event_get_user_data(e);
    if (!kb) {
        return;
    }
    lv_obj_t *btn = lv_event_get_target(e);
    const intptr_t code = (intptr_t)lv_obj_get_user_data(btn);
    if (code <= 0) {
        return;                     /* 没设过（空位） */
    }
    const int r = (int)((code - 1) / KB_COLS);
    const int c = (int)((code - 1) % KB_COLS);
    if (r < 0 || r >= KB_ROWS || c < 0 || c >= KB_COLS) {
        return;
    }
    append_char(kb, s_layer[kb->layer][r][c]);
}

static void layer_cb(lv_event_t *e)
{
    w_keyboard_t *kb = (w_keyboard_t *)lv_event_get_user_data(e);
    if (kb) {
        cycle_layer(kb);
    }
}

static void space_cb(lv_event_t *e)
{
    w_keyboard_t *kb = (w_keyboard_t *)lv_event_get_user_data(e);
    if (kb) {
        append_char(kb, " ");
    }
}

static void back_cb(lv_event_t *e)
{
    w_keyboard_t *kb = (w_keyboard_t *)lv_event_get_user_data(e);
    if (kb) {
        backspace(kb);
    }
}

/**
 * @brief 结束：回调 + 删自己
 *
 * ⚠️ 必须用 lv_obj_del_async 删面板：
 *    这个回调是在 LVGL 事件派发**过程中**触发的，直接 lv_obj_del()
 *    会把正在被遍历的对象删掉（LVGL 会崩或行为未定义）。
 */
static void finish(w_keyboard_t *kb, bool ok)
{
    if (kb->closed) {
        return;                     /* 防重入：连点两下"完成" */
    }
    kb->closed = true;

    char snapshot[W_KB_TEXT_MAX];
    memcpy(snapshot, kb->text, sizeof(snapshot));

    lv_obj_t *panel = kb->panel;
    w_keyboard_done_cb_t cb = kb->on_done;
    void *user = kb->user;

    /* 先删界面，再回调：回调里往往要建新界面，界面还盖着会很难看 */
    if (panel) {
        lv_obj_del_async(panel);
    }

    if (cb) {
        cb(snapshot, ok, user);
    }
    /* 注意：kb 本身由 panel 的删除回调释放（见 panel_deleted_cb） */
}

static void done_cb(lv_event_t *e)
{
    w_keyboard_t *kb = (w_keyboard_t *)lv_event_get_user_data(e);
    if (kb) {
        finish(kb, true);
    }
}

static void cancel_cb(lv_event_t *e)
{
    w_keyboard_t *kb = (w_keyboard_t *)lv_event_get_user_data(e);
    if (kb) {
        finish(kb, false);
    }
}

/**
 * @brief "返回"按钮：关闭键盘回到上一页（等同取消）
 *
 * @note 和 cancel_cb 行为一致，但**单独一个回调**是为了以后好区分：
 *       如果将来"返回"要"回到扫描列表"而"取消"要"回到已连接页"，
 *       两者就会分叉。现在只是语义命名更清楚。
 */
static void back_to_list_cb(lv_event_t *e)
{
    w_keyboard_t *kb = (w_keyboard_t *)lv_event_get_user_data(e);
    if (kb) {
        finish(kb, false);
    }
}

/** @brief 面板被删时释放结构体，避免泄漏 */
static void panel_deleted_cb(lv_event_t *e)
{
    w_keyboard_t *kb = (w_keyboard_t *)lv_event_get_user_data(e);
    if (kb) {
        lv_mem_free(kb);
    }
}

static void kb_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_KEY) {
        return;
    }
    /* 面板自己也要收到键盘事件（桌面模拟器用） */
    w_keyboard_t *kb = (w_keyboard_t *)lv_event_get_user_data(e);
    if (!kb) {
        return;
    }
    switch (lv_event_get_key(e)) {
    case LV_KEY_BACKSPACE: backspace(kb);     break;
    case LV_KEY_ENTER:     finish(kb, true);  break;
    case LV_KEY_ESC:       finish(kb, false); break;
    default: break;
    }
}

/* ==================== 造键 ==================== */
static lv_obj_t *make_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, const char *text,
                          lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, KB_KEY_H);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, W_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);

    lv_obj_t *lbl = lv_label_create(b);
    lv_obj_set_style_text_font(lbl, W_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, W_COLOR_TEXT, 0);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);

    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

/**
 * @brief 造一个功能键（标签是中文，必须用中文字体）
 *
 * ⚠️ 为什么单独一个函数：`make_btn()` 把字体写死成 `W_FONT_SMALL`
 *    （Montserrat，只有 ASCII）。用它造"退格""完成"就是**豆腐块**。
 *    这个坑本项目已经踩了**四次**（日期格式串、设置页值列、
 *    分隔符、以及这里），所以凡是中文标签一律走这个函数。
 */
static lv_obj_t *make_fn_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, const char *text,
                             lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, KB_KEY_H);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, W_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);

    /* ★ 中文字体 */
    lv_obj_t *lbl = w_label_create(b, text, watch_font_cn(14), W_COLOR_TEXT);
    lv_obj_center(lbl);

    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

/* ==================== 对外接口 ==================== */
w_keyboard_t *w_keyboard_open(lv_obj_t *parent, const char *title,
                              bool password, const char *initial,
                              w_keyboard_done_cb_t on_done, void *user)
{
    (void)password;     /* 显示一律明文，见头文件说明 */

    if (!parent || !on_done) {
        ESP_LOGE(TAG, "参数为空");
        return NULL;
    }

    w_keyboard_t *kb = lv_mem_alloc(sizeof(w_keyboard_t));
    if (!kb) {
        ESP_LOGE(TAG, "内存不足");
        return NULL;
    }
    memset(kb, 0, sizeof(*kb));
    kb->layer   = LAY_LOWER;
    kb->on_done = on_done;
    kb->user    = user;

    if (initial) {
        strncpy(kb->text, initial, W_KB_TEXT_MAX - 1);
        kb->len = strlen(kb->text);
    }

    /* ---------- 面板（盖满屏幕，半透明遮罩） ---------- */
    lv_obj_t *panel = lv_obj_create(parent);
    kb->panel = panel;
    lv_obj_set_size(panel, W_SCREEN_W, W_SCREEN_H);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, KB_PAD, 0);
    lv_obj_set_style_bg_color(panel, W_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_NONE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);   /* 吃掉底下页面的点击 */
    lv_obj_add_event_cb(panel, panel_deleted_cb, LV_EVENT_DELETE, kb);
    lv_obj_add_event_cb(panel, kb_event_cb, LV_EVENT_KEY, kb);
    lv_group_t *grp = lv_group_get_default();
    if (grp) {
        lv_group_add_obj(grp, panel);
        lv_group_focus_obj(panel);
    }

    lv_coord_t y = 0;

    /* ---------- 标题 ---------- */
    if (title) {
        lv_obj_t *t = w_label_create(panel, title, watch_font_cn(16), W_COLOR_TEXT_DIM);
        lv_obj_set_pos(t, 0, y);
        y += 22;
    }

    /* ---------- 输入框 ---------- */
    lv_obj_t *box = lv_obj_create(panel);
    lv_obj_set_size(box, KB_WIDTH - 2 * KB_PAD, 34);
    lv_obj_set_pos(box, 0, y);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 6, 0);
    lv_obj_set_style_bg_color(box, W_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_NONE);

    kb->input_lbl = w_label_create(box, "", W_FONT_SMALL, W_COLOR_TEXT);
    lv_obj_align(kb->input_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    /* 输入框内容只显示尾部，长按拖动没意义，所以裁剪掉溢出 */
    lv_label_set_long_mode(kb->input_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(kb->input_lbl, KB_WIDTH - 2 * KB_PAD - 12);

    y += 34 + KB_GAP;

    /* ---------- 功能行：返回 / 层级 / 空格 / 退格 / 完成 ---------- */
    /*
     * ⚠️ 这一行原来只有 4 个键（层级/空格/退格/完成）。
     *    用户实测反馈"输入密码的页面缺少返回按钮"—— 底部虽然有个"取消"，
     *    但离"完成"远、语义也不直观，用户找不到。
     *    现在把"返回"放到最左边，"完成"在最右边，两端对称。
     *
     *    5 个键要挤进 KB_GRID_W(224px)，所以键宽从 52 收到 42、
     *    行内间隙从 KB_GAP 收到 2。空格宽度 sw 自动吃掉剩余，
     *    以后改 fw 不用手算位置。
     */
    const lv_coord_t fw = 42;
    const lv_coord_t g  = 2;
    const lv_coord_t sw = KB_GRID_W - 4 * fw - 4 * g;

    lv_coord_t fx = 0;
    /*
     * ⚠️ "返回"这里用中文字面量，而不是 watch_strings.h 里的 W_STR_KEY_BACK：
     *    watch_ui 组件**不能依赖 watch_ui_pages**（那是反方向的依赖），
     *    所以拿不到那个宏。
     *    本文件里"空格/退格/完成/取消"一直是直接写字面量的，保持一致。
     *    （字库生成脚本会扫描所有源码的字符串字面量，所以照样能进字库。）
     */
    (void)make_fn_btn(panel, fx, y, fw, "返回", back_to_list_cb, kb);
    fx += fw + g;
    kb->layer_btn = make_fn_btn(panel, fx, y, fw, s_layer_name[kb->layer],
                                layer_cb, kb);
    fx += fw + g;
    (void)make_fn_btn(panel, fx, y, sw, "空格", space_cb, kb);
    fx += sw + g;
    (void)make_fn_btn(panel, fx, y, fw, "退格", back_cb, kb);
    fx += fw + g;
    (void)make_fn_btn(panel, fx, y, fw, "完成", done_cb, kb);
    y += KB_KEY_H + KB_GAP;

    /* ---------- 字符区 ---------- */
    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            const char *ch = s_layer[kb->layer][r][c];
            lv_obj_t *b = make_btn(panel,
                                   c * (KB_KEY_W + KB_GAP),
                                   y + r * (KB_KEY_H + KB_GAP),
                                   KB_KEY_W, ch, key_cb, kb);
            /*
             * 只存 (行,列) 编号，**不要存字符指针** ——
             * 存指针的话，切层后标签变了但指针没变，
             * 按下去会返回旧层的字符（踩过，见 key_cb 的说明）。
             * +1 是为了避开 0（转成 void* 是 NULL，会被当成"没设置"）。
             */
            lv_obj_set_user_data(b, (void *)(intptr_t)(r * KB_COLS + c + 1));
            kb->btn[r][c] = b;
        }
    }
    y += KB_GRID_H + KB_GAP;

    /* ---------- 取消 ---------- */
    (void)make_fn_btn(panel, 0, y, KB_GRID_W, "取消", cancel_cb, kb);

    input_refresh(kb);
    return kb;
}

void w_keyboard_close(w_keyboard_t *kb)
{
    if (!kb || kb->closed) {
        return;
    }
    kb->closed = true;
    kb->on_done = NULL;             /* 不触发回调 */
    if (kb->panel) {
        lv_obj_del_async(kb->panel);
    }
}

const char *w_keyboard_text(const w_keyboard_t *kb)
{
    return kb ? kb->text : "";
}
