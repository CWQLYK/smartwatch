/**
 * @file watch_widgets.h
 * @brief 手表界面公共控件 —— 表盘/健康/运动/设置四个页面共用
 *
 * 为什么要抽这一层：
 *   第一版 main.c 里每个页面都重写了一遍"造卡片 + 放标题 + 放数值"，
 *   结果四个页面的圆角、内边距、字色全都略有差异。
 *   把重复的 5-15 行收敛成一次函数调用，页面代码就能一眼看清结构。
 *
 * 所有控件都**不处理业务**（不读传感器、不加锁），只负责画。
 */
#pragma once

#include "lvgl.h"
#include "watch_style.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、容器
 * ================================================================== */

/**
 * @brief 造一个页面根对象（深色背景 + 统一内边距 + 不可滚动）
 *
 * @param parent 页面容器（由滑动轮播提供，**不能传 NULL**）
 *
 * @note ⚠️ v0.3 起页面根是**普通对象**，不再是 screen。
 *       原因：跟手滑动要求"相邻页并排放在同一个可滚动容器里"，
 *       而 LVGL 的 screen 挂不进容器 —— `lv_obj_set_parent()` 会直接拒绝
 *       （`lv_obj_tree.c:132`），且 8.4 没有 `lv_obj_remove_from_parent()`。
 *       详见 watch_pages.h 的文件头说明。
 *
 *       老的 `w_screen_create()` 已删除：它的返回值挂不进容器，
 *       症状是"页面一片空白"，很难查。
 */
lv_obj_t *w_page_create(lv_obj_t *parent);

/**
 * @brief 造一张卡片
 *
 * @param parent 父对象（一般传页面根对象）
 * @param x,y    相对父对象的位置；传 W_AUTO_POS 表示交给调用者自己 align
 * @param w,h    尺寸；传 W_AUTO_SIZE 表示自适应内容
 */
#define W_AUTO_POS   (-32768)
#define W_AUTO_SIZE  (-32768)

lv_obj_t *w_card_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h);

/**
 * @brief 造一张可点击的卡片（带按下反馈）
 */
lv_obj_t *w_card_create_clickable(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                  lv_coord_t w, lv_coord_t h);

/* ==================================================================
 * 二、文字
 * ================================================================== */

/**
 * @brief 判断字符串里有没有非 ASCII 字符
 *
 * 用途：**自动挑字体**。界面里很多值是"有时是数字、有时是中文占位符"
 * （比如步数可能是 "1235" 也可能是 "--未接入"）。
 * 数字用 Montserrat 更好看，中文必须用 watch_font_cn()，
 * 让工厂函数按内容自动选，就不用每处都人肉判断了。
 */
bool w_text_has_non_ascii(const char *text);

/**
 * @brief 造一个标签
 * @param font  传 NULL 用默认字体（W_FONT_SMALL）
 * @param color 字色；传 W_COLOR_AUTO 用默认字色（W_COLOR_TEXT）
 *
 * @note 颜色是**值传递**，因为 W_COLOR_xxx 是函数式宏、不能取地址。
 */
lv_obj_t *w_label_create(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, lv_color_t color);

/**
 * @brief 造一个"按内容自动选字体"的标签
 *
 * @param ascii_font 内容是纯 ASCII 时用的字体（如 W_FONT_MEDIUM，数字好看）
 * @param cjk_size   内容含中文时用的中文字号（如 16）
 *
 * @note ⚠️ 这是**防豆腐块**的推荐做法。
 *       豆腐块这个坑本项目踩过三次，全部是"标签设了 ASCII 字体但内容是中文"。
 *       凡是"值可能是数字也可能是中文"的地方，一律用本函数。
 */
lv_obj_t *w_label_create_auto(lv_obj_t *parent, const char *text,
                              const lv_font_t *ascii_font, int cjk_size,
                              lv_color_t color);

/**
 * @brief 给标签记住"纯 ASCII 时该用哪个字体"，之后换文案时自动切换
 *
 * @param label        w_label_create() 造的标签
 * @param ascii_font   内容变成纯 ASCII 时切回这个字体（传 NULL = W_FONT_SMALL）
 * @param cjk_size     内容含中文时用的中文字号
 *
 * @note 为什么要记住：数值标签的字体是**创建时**设好的，
 *       而内容会在 refresh 里反复变（"1235" ↔ "--"）。
 *       只按创建时那一瞬间的内容选字体，之后变中文就又成豆腐块了。
 */
void w_label_set_auto_font(lv_obj_t *label, const lv_font_t *ascii_font, int cjk_size);

/**
 * @brief 更新标签文案，并按内容自动切换字体（推荐所有"值"都用它）
 */
void w_label_set_text_auto(lv_obj_t *label, const char *text);

/**
 * @brief printf 版 w_label_set_text_auto()
 */
void w_label_set_text_fmt_auto(lv_obj_t *label, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/** @brief 造"标题 + 数值"组合：标题小字在左上，数值大字居中偏下 */
lv_obj_t *w_label_value_create(lv_obj_t *parent, const char *title,
                               const char *value);

/**
 * @brief 造一个卡片：标题 + 数值（表盘底部三个小卡片就是这个）
 *
 * @param title 标题（中文，用 watch_font_cn）
 * @param value 初始数值（英文数字，用 Montserrat）
 * @param[out] out_value 返回数值标签，方便以后 `lv_label_set_text_fmt()` 更新
 * @return 卡片对象
 */
lv_obj_t *w_metric_card_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                               lv_coord_t w, lv_coord_t h,
                               const char *title, const char *value,
                               lv_obj_t **out_value);

/* ==================================================================
 * 三、专用控件
 * ================================================================== */

/**
 * @brief 造电池图标（外框 + 电量条 + 正极小凸起）
 *
 * @param w,h    图标尺寸（建议 22x11 左右）
 * @param[out] out_fill 返回内部电量条对象，用 w_battery_set() 更新
 */
lv_obj_t *w_battery_icon_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                lv_obj_t **out_fill);

/**
 * @brief 更新电池图标
 * @param fill    w_battery_icon_create() 返回的电量条
 * @param percent 0-100；负数表示未知（画成灰色空条）
 * @param charging 是否充电中（充电时电量条用主色）
 */
void w_battery_set(lv_obj_t *fill, int percent, bool charging);

/**
 * @brief 造一个圆形图标按钮（直径 d），文字用 LV_SYMBOL_xxx
 */
lv_obj_t *w_icon_button_create(lv_obj_t *parent, lv_coord_t d,
                               const char *symbol, lv_event_cb_t cb, void *user_data);

/**
 * @brief 造一个横向进度条（用于设置页的亮度等）
 */
lv_obj_t *w_slider_create(lv_obj_t *parent, lv_coord_t w, int min, int max, int value);

/**
 * @brief 造一条细分隔线
 */
lv_obj_t *w_divider_create(lv_obj_t *parent, lv_coord_t w);

/**
 * @brief 把对象设为"占位/未知"样式（灰色 + 灰字），用于显示 "--"
 */
void w_set_placeholder(lv_obj_t *label, bool placeholder);

#ifdef __cplusplus
}
#endif
