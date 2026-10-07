/**
 * @file watch_game.c
 * @brief 游戏宿主层实现（设计说明见 watch_game.h）
 *
 * ## 中文为什么是字面量
 *
 * 本文件在 watch_ui 里，`watch_strings.h` 在 watch_ui_pages 里，依赖方向是反的。
 * 按项目既有约定（watch_modal.c 的"返回"、watch_alert.c 的"停止"、
 * watch_wheel.c 的"取消/确定"），这一层的中文直接写字面量。
 * 字库生成脚本扫的是所有源码的字符串字面量，照样进字库。
 *
 * ## 为什么没有"poll"接口
 *
 * 提醒层到点、屏幕熄灭、这一局结束 —— 这三件事全都在**帧定时器**里查。
 * 定时器每 33ms 必然跑一次，再另开一条轮询路径只会多一处状态不同步的机会。
 */
#include "watch_game.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "watch_alert.h"      /* 闹钟/倒计时到点要自动暂停 */
#include "watch_gfx.h"
#include "watch_modal.h"      /* 退出时刷新底下的游戏列表（更新最高分） */
#include "watch_pages.h"      /* watch_ui_setting_get/set */
#include "watch_platform.h"   /* 防息屏 */
#include "watch_style.h"
#include "watch_widgets.h"

static const char *TAG = "GAME";

/* ==================================================================
 * 参数
 * ================================================================== */

/** @brief 帧周期。33ms ≈ 30FPS。
 *
 * 为什么不是 60：LCD 走 SPI 40MHz，16bpp 下有效带宽约 5MB/s。
 * 一帧只重画下面那条带子（约 240x140 = 67KB）就要 ~13ms，
 * 再加 LVGL 的格式转换和绘制，33ms 是稳妥值。
 * 实机嫌不够跟手可以往 25ms（40FPS）调 —— 脏区机制已经留了余量。 */
#define GAME_FRAME_MS     33

/** @brief 单帧最大 dt。
 *
 * ⚠️ 必须有上限：息屏、切页、系统卡一下，dt 可能是几百毫秒，
 *    障碍物会"瞬移"穿过恐龙 —— 玩家明明跳了却撞死，而且完全不可复现。
 *    夹住之后最坏情况只是游戏变慢，不会出现穿越。 */
#define GAME_DT_MAX_MS    80

#define PAD               12

/** @brief 顶部 HUD（暂停键 + 分数）下面才开始算脏区 */
#define HUD_H             30

/* ==================================================================
 * 内部状态
 * ================================================================== */
typedef enum {
    GS_NONE = -1,     /* 没开 */
    GS_READY = 0,     /* 开始面板 */
    GS_PLAY,          /* 游戏中 */
    GS_PAUSE,         /* 暂停面板 */
    GS_OVER,          /* 结束面板 */
} game_state_t;

static lv_obj_t   *s_host      = NULL;
static lv_obj_t   *s_root      = NULL;   /* 全屏层 */
static lv_obj_t   *s_canvas    = NULL;
static lv_obj_t   *s_panel     = NULL;   /* 面板（开始/暂停/结束共用） */
static lv_obj_t   *s_primary   = NULL;   /* 面板上的主按钮（开始/继续/再来一次） */
static lv_obj_t   *s_score_lb  = NULL;
static lv_obj_t   *s_pause_btn = NULL;
static lv_timer_t *s_timer     = NULL;

static const w_game_t *s_game  = NULL;
static game_state_t    s_state = GS_NONE;

static uint8_t   *s_buf      = NULL;     /* 画布缓冲区（含 16 字节调色板） */
static size_t     s_buf_size = 0;
static lv_coord_t s_cw = 0, s_ch = 0;

static uint32_t s_last_tick = 0;
static int      s_frames    = 0;
static int      s_best      = 0;
static int      s_score_last = -1;
static bool     s_debug_hold_timer = false;   /* 自检手动推帧时压住定时器 */
static const char *s_pause_reason = NULL;     /* 为什么自动暂停（写在面板上） */

/* 面板构建函数定义在下面，但几个按钮回调要用，先声明 */
static void ui_show_pause_panel(void);
static void ui_show_over_panel(void);

/* ==================================================================
 * 最高分
 * ================================================================== */
static int best_load(void)
{
    if (!s_game || !s_game->best_key) {
        return 0;
    }
    return watch_ui_setting_get(s_game->best_key, 0);
}

static void best_store(int v)
{
    if (!s_game || !s_game->best_key) {
        return;
    }
    watch_ui_setting_set(s_game->best_key, v);
    ESP_LOGI(TAG, "最高分写入 %s = %d", s_game->best_key, v);
}

/* ==================================================================
 * 画布
 * ================================================================== */
static bool canvas_alloc(lv_coord_t w, lv_coord_t h)
{
    s_buf_size = LV_IMG_BUF_SIZE_INDEXED_2BIT(w, h);
    s_buf = (uint8_t *)lv_mem_alloc(s_buf_size);
    if (!s_buf) {
        ESP_LOGE(TAG, "画布缓冲区申请失败（要 %u 字节）", (unsigned)s_buf_size);
        return false;
    }
    memset(s_buf, 0, s_buf_size);

    s_canvas = lv_canvas_create(s_root);
    lv_canvas_set_buffer(s_canvas, s_buf, w, h, LV_IMG_CF_INDEXED_2BIT);
    lv_obj_set_size(s_canvas, w, h);
    lv_obj_set_pos(s_canvas, 0, 0);
    /* 画布不接收点击：手指事件要落到游戏根上（根上才挂着手势）。
     * 否则点在画布上时命中测试停在画布，根收不到 PRESSED。 */
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_canvas, LV_DIR_NONE);

    /*
     * ★ 调色板必须在 set_buffer **之后**设：
     *   调色板就是写进那块缓冲区头 16 字节的，顺序反了就白设（且不报错）。
     * 4 个索引：底色 / 前景墨色 / 次要（地面云）/ 强调色。
     * 表盘主题是深色，所以恐龙是**浅色**的 —— 和 Chrome 的白底黑恐龙正好反过来。
     */
    lv_canvas_set_palette(s_canvas, W_GFX_IDX_BG,  W_COLOR_BG);
    lv_canvas_set_palette(s_canvas, W_GFX_IDX_INK, W_COLOR_TEXT);
    lv_canvas_set_palette(s_canvas, W_GFX_IDX_DIM, W_COLOR_TEXT_DIM);
    lv_canvas_set_palette(s_canvas, W_GFX_IDX_HI,  W_COLOR_ACCENT);

    s_cw = w;
    s_ch = h;

    ESP_LOGI(TAG, "画布 %dx%d，缓冲 %u 字节（2bpp + 16 字节调色板）",
             (int)w, (int)h, (unsigned)s_buf_size);
    return true;
}

static void canvas_free(void)
{
    if (s_canvas) {
        lv_obj_del(s_canvas);
        s_canvas = NULL;
    }
    /*
     * 把图像缓存整个作废再释放缓冲区。
     * 我们这个板子 LV_IMG_CACHE_DEF_SIZE=0（缓存其实是关的），
     * 但模拟器/以后改配置时不一定 —— 缓存里留着已释放的指针会变成
     * "玩几次之后随机崩"，而且崩在别的代码里，极难查。
     */
    lv_img_cache_invalidate_src(NULL);
    if (s_buf) {
        lv_mem_free(s_buf);
        s_buf = NULL;
    }
    s_cw = s_ch = 0;
}

/* ==================================================================
 * 面板（开始 / 暂停 / 结束）
 * ================================================================== */
static void panel_clear(void)
{
    if (s_panel) {
        lv_obj_del(s_panel);         /* 连子对象一起删，回调里的指针不会残留 */
        s_panel = NULL;
    }
    s_primary = NULL;
}

static lv_obj_t *panel_begin(const char *title, const char *sub)
{
    panel_clear();

    s_panel = lv_obj_create(s_root);
    lv_obj_set_size(s_panel, s_cw ? s_cw : W_SCREEN_W, s_ch ? s_ch : W_SCREEN_H);
    lv_obj_set_pos(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, PAD, 0);
    /* 半透明：底下的画面（恐龙/地面）还看得见，不像"死机了" */
    lv_obj_set_style_bg_color(s_panel, W_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_80, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_panel, LV_DIR_NONE);
    /* 吃掉点击：面板上的空白处不该被当成"跳"（没有 EVENT_BUBBLE，冒不到根上） */
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 8, 0);

    if (title) {
        w_label_create_auto(s_panel, title, W_FONT_MEDIUM, 20, W_COLOR_ACCENT);
    }
    if (sub) {
        w_label_create_auto(s_panel, sub, W_FONT_SMALL, 14, W_COLOR_TEXT_DIM);
    }
    return s_panel;
}

/* ---------- 按钮回调 ---------- */
static void enter_play(void)
{
    s_state = GS_PLAY;
    s_pause_reason = NULL;
    s_score_last = -1;
    s_last_tick = lv_tick_get();
    panel_clear();
    if (s_pause_btn) {
        lv_obj_clear_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cb_start(lv_event_t *e)
{
    (void)e;
    if (s_game && s_game->reset) {
        s_game->reset();
    }
    enter_play();
    ESP_LOGI(TAG, "开始游戏 '%s'", s_game && s_game->id ? s_game->id : "?");
}

static void cb_resume(lv_event_t *e)
{
    (void)e;
    enter_play();
}

static void cb_pause(lv_event_t *e)
{
    (void)e;
    if (s_state != GS_PLAY) {
        return;
    }
    s_state = GS_PAUSE;
    s_pause_reason = NULL;
    ui_show_pause_panel();
    ESP_LOGI(TAG, "用户暂停");
}

static void cb_restart(lv_event_t *e)
{
    (void)e;
    cb_start(e);
}

static void cb_quit(lv_event_t *e)
{
    (void)e;
    w_game_close();
}

static void ui_show_pause_panel(void)
{
    lv_obj_t *p = panel_begin("已暂停", s_pause_reason);
    if (s_pause_btn) {
        lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    }
    s_primary = w_btn_create(p, "继续", 180, 44, W_COLOR_ACCENT, W_COLOR_BG,
                             watch_font_cn(18), cb_resume);
    w_btn_create(p, "重新开始", 180, 44, W_COLOR_CARD, W_COLOR_TEXT, watch_font_cn(18), cb_restart);
    w_btn_create(p, "退出",     180, 44, W_COLOR_CARD, W_COLOR_TEXT, watch_font_cn(18), cb_quit);
}

static void ui_show_over_panel(void)
{
    const int sc = s_game && s_game->score ? s_game->score() : 0;

    if (sc > s_best) {
        s_best = sc;
        best_store(s_best);
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "本局得分 %d", sc);
    lv_obj_t *p = panel_begin("游戏结束", buf);

    snprintf(buf, sizeof(buf), "最高分 %d", s_best);
    w_label_create_auto(p, buf, W_FONT_SMALL, 14, W_COLOR_TEXT);

    s_primary = w_btn_create(p, "再来一次", 180, 44, W_COLOR_ACCENT, W_COLOR_BG,
                             watch_font_cn(18), cb_restart);
    w_btn_create(p, "退出",     180, 44, W_COLOR_CARD, W_COLOR_TEXT, watch_font_cn(18), cb_quit);
}

static void ui_show_ready_panel(void)
{
    lv_obj_t *p = panel_begin(s_game ? s_game->title : "游戏",
                              s_game ? s_game->hint : NULL);

    char buf[48];
    snprintf(buf, sizeof(buf), "最高分 %d", s_best);
    w_label_create_auto(p, buf, W_FONT_SMALL, 14, W_COLOR_TEXT);

    s_primary = w_btn_create(p, "开始", 180, 48, W_COLOR_ACCENT, W_COLOR_BG,
                             watch_font_cn(20), cb_start);
    w_btn_create(p, "返回", 180, 40, W_COLOR_CARD, W_COLOR_TEXT, watch_font_cn(16), cb_quit);
}

/* ==================================================================
 * 输入
 * ================================================================== */
static void on_pressed(lv_event_t *e)
{
    (void)e;
    if (s_state != GS_PLAY || !s_game || !s_game->input) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    s_game->input(pt.x, pt.y, true);
}

static void on_released(lv_event_t *e)
{
    (void)e;
    if (s_state != GS_PLAY || !s_game || !s_game->input) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t pt = { 0, 0 };
    if (indev) {
        lv_indev_get_point(indev, &pt);
    }
    s_game->input(pt.x, pt.y, false);
}

/* ==================================================================
 * 帧循环
 * ================================================================== */
static void frame_run(uint32_t dt_ms)
{
    if (!s_game || !s_canvas) {
        return;
    }
    if (dt_ms > GAME_DT_MAX_MS) {
        dt_ms = GAME_DT_MAX_MS;
    }

    if (s_game->step) {
        s_game->step(dt_ms);
    }
    if (s_game->render) {
        s_game->render(s_canvas);
    }

    /* 只重画下面那条会动的带子；上面的天空、分数、暂停键都不动 */
    lv_coord_t top = HUD_H;
    if (s_game->dirty_top) {
        top = s_game->dirty_top();
    }
    if (top < 0) {
        top = 0;
    }
    if (top < s_ch) {
        lv_area_t a = { 0, top, (lv_coord_t)(s_cw - 1), (lv_coord_t)(s_ch - 1) };
        lv_obj_invalidate_area(s_canvas, &a);
    }

    /* 分数：每帧比一下，变了才改标签（改文本本身会触发那小块重绘） */
    const int sc = s_game->score ? s_game->score() : 0;
    if (sc != s_score_last && s_score_lb) {
        s_score_last = sc;
        char buf[16];
        snprintf(buf, sizeof(buf), "%05d", sc);
        w_label_set_text_auto(s_score_lb, buf);
    }

    s_frames++;
}

/** @brief 这一局是否已经结束 -> 显示结束面板（只做一次） */
static void check_over(void)
{
    if (s_state != GS_PLAY || !s_game || !s_game->over) {
        return;
    }
    if (!s_game->over()) {
        return;
    }
    s_state = GS_OVER;
    ESP_LOGI(TAG, "游戏结束，得分 %d", s_game->score ? s_game->score() : 0);
    ui_show_over_panel();
}

/** @brief 提醒弹出 / 屏幕熄灭 -> 自动暂停（只做一次） */
static void auto_pause_check(void)
{
    if (s_state != GS_PLAY) {
        return;
    }
    if (watch_alert_visible()) {
        s_state = GS_PAUSE;
        s_pause_reason = "到点提醒弹出，游戏已暂停";
        ESP_LOGI(TAG, "提醒层弹出 -> 自动暂停");
        ui_show_pause_panel();
        return;
    }
    /* 正常玩的时候有 hold_awake 挡着，这里是兜底 */
    if (!w_screen_is_on()) {
        s_state = GS_PAUSE;
        s_pause_reason = "屏幕已息屏，游戏已暂停";
        ESP_LOGI(TAG, "屏幕熄灭 -> 自动暂停");
        ui_show_pause_panel();
    }
}

static void frame_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_debug_hold_timer || s_state == GS_NONE) {
        return;
    }

    if (s_state == GS_PLAY) {
        auto_pause_check();
    }
    if (s_state == GS_PLAY) {
        const uint32_t now = lv_tick_get();
        const uint32_t dt = now - s_last_tick;
        frame_run(dt);
        check_over();
    }
    /* 非游戏中也要刷，免得恢复时 dt 累积成一个巨大的值 */
    s_last_tick = lv_tick_get();
}

/* ==================================================================
 * 打开 / 关闭
 * ================================================================== */
void watch_game_init(lv_obj_t *host)
{
    s_host = host;
    ESP_LOGI(TAG, "游戏宿主就绪");
}

bool w_game_is_open(void)
{
    return s_state != GS_NONE;
}

int w_game_open(const w_game_t *g)
{
    if (!s_host) {
        ESP_LOGE(TAG, "宿主没初始化（watch_game_init 没被调用）");
        return -1;
    }
    if (s_state != GS_NONE) {
        ESP_LOGW(TAG, "已经有一个游戏开着");
        return -1;
    }
    if (!g || !g->render || !g->step) {
        ESP_LOGE(TAG, "游戏定义不完整（step/render 必须有）");
        return -1;
    }

    s_game = g;
    if (g->init && g->init() != 0) {
        ESP_LOGE(TAG, "游戏 '%s' 初始化失败", g->id ? g->id : "?");
        s_game = NULL;
        return -1;
    }

    s_best = best_load();

    /* ---------- 全屏层 ---------- */
    const lv_coord_t cw = g->canvas_w ? g->canvas_w : W_SCREEN_W;
    const lv_coord_t ch = g->canvas_h ? g->canvas_h : W_SCREEN_H;

    s_root = lv_obj_create(s_host);
    lv_obj_set_size(s_root, cw, ch);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, W_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_root, LV_DIR_NONE);
    /* ★ 可点击 + 置顶 = 吃掉底下轮播和模态的所有触摸（见 watch_game.h 说明） */
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, on_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_root, on_released, LV_EVENT_RELEASED, NULL);
    lv_obj_move_foreground(s_root);

    if (!canvas_alloc(cw, ch)) {
        lv_obj_del(s_root);
        s_root = NULL;
        if (g->destroy) {
            g->destroy();
        }
        s_game = NULL;
        return -1;
    }

    /* ---------- HUD：暂停键（左上）+ 分数（右上） ---------- */
    s_pause_btn = w_btn_create(s_root, "暂停", 52, 26, W_COLOR_CARD, W_COLOR_TEXT,
                               watch_font_cn(14), cb_pause);
    lv_obj_align(s_pause_btn, LV_ALIGN_TOP_LEFT, 2, 2);

    s_score_lb = w_label_create(s_root, "00000", W_FONT_MEDIUM, W_COLOR_TEXT);
    lv_obj_align(s_score_lb, LV_ALIGN_TOP_RIGHT, -2, 2);

    /* ---------- 开局 ---------- */
    s_state = GS_READY;
    s_pause_reason = NULL;
    s_frames = 0;
    s_score_last = -1;
    s_debug_hold_timer = false;

    if (g->reset) {
        g->reset();
    }
    if (g->render) {
        g->render(s_canvas);        /* 开始面板背后先画一帧静态画面 */
    }
    lv_obj_invalidate(s_canvas);

    s_last_tick = lv_tick_get();
    s_timer = lv_timer_create(frame_timer_cb, GAME_FRAME_MS, NULL);

    /*
     * ★ 玩游戏期间不许自动息屏。
     *   用引用计数（不是布尔），和"录 IMU 时别息屏"共用同一套机制，
     *   谁加的自己解，互不干扰。
     */
    w_screen_hold_awake(true);

    ui_show_ready_panel();

    ESP_LOGI(TAG, "打开游戏 '%s'（画布 %dx%d，最高分 %d）",
             g->id ? g->id : "?", (int)cw, (int)ch, s_best);
    return 0;
}

void w_game_close(void)
{
    if (s_state == GS_NONE) {
        return;
    }

    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    w_screen_hold_awake(false);

    panel_clear();
    canvas_free();

    if (s_root) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    s_pause_btn = NULL;
    s_score_lb = NULL;

    const w_game_t *g = s_game;
    s_game  = NULL;
    s_state = GS_NONE;
    s_pause_reason = NULL;

    if (g && g->destroy) {
        g->destroy();
    }

    /*
     * 回到下层的模态子页（游戏列表）时刷新它 ——
     * 最高分刚变过，列表上显示的还是旧数字。
     */
    if (w_modal_is_open()) {
        w_modal_refresh();
    }

    ESP_LOGI(TAG, "游戏已退出（共 %d 帧）", s_frames);
}

/* ==================================================================
 * 自检钩子
 * ================================================================== */
void w_game_debug_step(int frames, int dt_ms)
{
    if (s_state == GS_NONE) {
        return;
    }
    /*
     * 压住内部定时器，改成手动推帧。
     * 同时把"面板/结束判定"这两件定时器里做的事也照做一遍 ——
     * 否则自检截出来的图和实机不一样（比如暂停面板根本没建）。
     */
    s_debug_hold_timer = true;
    for (int i = 0; i < frames; i++) {
        if (s_state != GS_PLAY) {
            break;
        }
        auto_pause_check();
        if (s_state != GS_PLAY) {
            break;
        }
        frame_run((uint32_t)dt_ms);
        check_over();
    }
    s_last_tick = lv_tick_get();
}

void w_game_debug_press(int x, int y)
{
    if (s_state == GS_PLAY && s_game && s_game->input) {
        s_game->input(x, y, true);
    }
}

void w_game_debug_release(void)
{
    if (s_state == GS_PLAY && s_game && s_game->input) {
        s_game->input(0, 0, false);
    }
}

void w_game_debug_click(int x, int y)
{
    w_game_debug_press(x, y);
    w_game_debug_release();
}

int w_game_debug_state(void)
{
    return (int)s_state;
}

const char *w_game_debug_state_name(void)
{
    switch (s_state) {
    case GS_READY: return "开始面板";
    case GS_PLAY:  return "游戏中";
    case GS_PAUSE: return "暂停";
    case GS_OVER:  return "结束";
    default:       return "未打开";
    }
}

int w_game_debug_frames(void)
{
    return s_frames;
}

const w_game_t *w_game_debug_game(void)
{
    return s_game;
}

int w_game_debug_fill_canvas(int idx)
{
    if (!s_buf || !s_canvas) {
        return 0;
    }
    w_gfx_t g;
    w_gfx_init(&g, s_buf, s_cw, s_ch);
    w_gfx_clear(&g, idx);
    lv_obj_invalidate(s_canvas);
    return s_cw * s_ch;
}

/* ==================================================================
 * 自检钩子：把"用户按了暂停键"这条路径也走通（截图暂停面板要用）
 * ================================================================== */
void w_game_debug_pause(void)
{
    cb_pause(NULL);
}

bool w_game_debug_click_primary(void)
{
    if (!s_primary || !lv_obj_is_valid(s_primary)) {
        return false;
    }
    /*
     * 发的是**真正的 CLICKED 事件**，走按钮自己的回调 ——
     * 不是直接把状态改成"游戏中"。
     * 这样"按钮回调有没有接上、接错了没有"也在自检覆盖范围内
     * （直接改状态的话，回调断链了自检照样全过）。
     */
    lv_event_send(s_primary, LV_EVENT_CLICKED, NULL);
    return true;
}
