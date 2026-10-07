/**
 * @file watch_modal.c
 * @brief 子页面容器实现
 */
#include "watch_modal.h"

#include <string.h>
#include "esp_log.h"
#include "watch_style.h"
#include "watch_widgets.h"

static const char *TAG = "MODAL";

#define TITLE_H   34

typedef struct {
    lv_obj_t           *root;      /* 子页面根（盖满屏） */
    w_modal_build_cb_t  build;
    void               *user;
    char                title[32];
} modal_t;

static lv_obj_t *s_host  = NULL;
static modal_t   s_stack[W_MODAL_MAX_DEPTH];
static int       s_depth = 0;

/* ==================== 内部 ==================== */
/** @brief "返回"按钮回调 */
static void back_cb(lv_event_t *e)
{
    (void)e;
    w_modal_close();
}

static void build_page(int i)
{
    modal_t *m = &s_stack[i];
    if (!m->root) {
        return;
    }

    /* 清掉旧内容（refresh 时会重跑） */
    lv_obj_clean(m->root);

    lv_obj_t *page = lv_obj_create(m->root);
    lv_obj_set_size(page, W_SCREEN_W, W_SCREEN_H);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, W_PAD_PAGE, 0);
    lv_obj_set_style_bg_color(page, W_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    /* 自己可滚动：子页面内容可能比屏高（IMU 录制页就超了） */
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    if (m->title[0]) {
        lv_obj_t *t = w_label_create(page, m->title, watch_font_cn(20), W_COLOR_TEXT);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    /* 返回键（右上角，和标题同一行） */
    lv_obj_t *back = lv_btn_create(page);
    lv_obj_set_size(back, 52, 26);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_set_style_bg_color(back, W_COLOR_CARD, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_t *bl = w_label_create(back, "返回", watch_font_cn(14), W_COLOR_TEXT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    /* 内容区：从标题下方开始 */
    lv_obj_t *body = lv_obj_create(page);
    lv_obj_set_size(body, W_SCREEN_W - 2 * W_PAD_PAGE, LV_SIZE_CONTENT);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, TITLE_H);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_NONE);

    if (m->build) {
        m->build(body, m->user);
    }
}

static void destroy_top(void)
{
    if (s_depth <= 0) {
        return;
    }
    s_depth--;
    modal_t *m = &s_stack[s_depth];
    if (m->root) {
        lv_obj_del(m->root);
        m->root = NULL;
    }
    memset(m, 0, sizeof(*m));
}

/* ==================== 对外 ==================== */
void w_modal_init(lv_obj_t *host)
{
    s_host  = host;
    s_depth = 0;
    memset(s_stack, 0, sizeof(s_stack));
}

int w_modal_open(const char *title, w_modal_build_cb_t build, void *user)
{
    if (!s_host) {
        ESP_LOGE(TAG, "没初始化（w_modal_init 没被调用）");
        return -1;
    }
    if (!build) {
        return -1;
    }
    if (s_depth >= W_MODAL_MAX_DEPTH) {
        ESP_LOGW(TAG, "子页面层数已达上限 %d", W_MODAL_MAX_DEPTH);
        return -1;
    }

    modal_t *m = &s_stack[s_depth];
    memset(m, 0, sizeof(*m));
    m->build = build;
    m->user  = user;
    if (title) {
        strncpy(m->title, title, sizeof(m->title) - 1);
    }

    m->root = lv_obj_create(s_host);
    lv_obj_set_size(m->root, W_SCREEN_W, W_SCREEN_H);
    lv_obj_set_pos(m->root, 0, 0);
    lv_obj_set_style_radius(m->root, 0, 0);
    lv_obj_set_style_border_width(m->root, 0, 0);
    lv_obj_set_style_pad_all(m->root, 0, 0);
    lv_obj_set_style_bg_color(m->root, W_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(m->root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(m->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(m->root, LV_DIR_NONE);
    /* 吃掉底下轮播的点击，否则点子页面会穿透去翻页 */
    lv_obj_add_flag(m->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(m->root);

    s_depth++;
    build_page(s_depth - 1);

    ESP_LOGD(TAG, "打开子页面 '%s'（第 %d 层）", m->title, s_depth);
    return 0;
}

void w_modal_close(void)
{
    if (s_depth <= 0) {
        return;
    }
    destroy_top();
    ESP_LOGD(TAG, "关闭子页面，剩 %d 层", s_depth);

    /* 回到上一层时刷新它（上层的显示可能被下层改过，比如 WiFi 连上了） */
    if (s_depth > 0) {
        build_page(s_depth - 1);
    }
}

bool w_modal_is_open(void)
{
    return s_depth > 0;
}

int w_modal_depth(void)
{
    return s_depth;
}

void w_modal_refresh(void)
{
    if (s_depth <= 0) {
        return;
    }
    build_page(s_depth - 1);
}
