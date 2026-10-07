/**
 * @file watch_page_games.c
 * @brief 游戏列表子页 —— 功能列表第 5 行「游戏」进来（v0.6.0 新增）
 *
 *     ┌──────────────────────────┐
 *     │ 游戏              [返回] │
 *     │ ┌──────────────────────┐ │
 *     │ │ 小恐龙            >  │ │  最高分 1234
 *     │ └──────────────────────┘ │
 *     │ 选一个游戏开始。玩的时候  │
 *     │ 点左上角可以暂停或退出。  │
 *     └──────────────────────────┘
 *
 * ## 加一款新游戏要改哪里（就这三处）
 *
 *   1. 写 `games/xxx_game.c`，实现 `w_game_t` 那几个回调
 *   2. 在下面的 `k_games[]` 里加一行
 *   3. 如果它要存最高分：watch_pages.h 加一个 `W_SETTING_XXX` 键名，
 *      app.c 里加一行到 SETTINGS_* 的映射
 *
 * 界面框架、暂停/结束面板、防息屏、闹钟自动暂停**全都是白送的**
 * （都在 watch_ui/watch_game.c 里）。
 */
#include "watch_pages.h"

#include <stdio.h>
#include "esp_log.h"
#include "watch_game.h"
#include "watch_modal.h"
#include "watch_style.h"
#include "watch_strings.h"
#include "watch_widgets.h"
#include "dino_game.h"

static const char *TAG = "GAMEPAGE";

/* ==================================================================
 * 游戏注册表
 * ================================================================== */
typedef struct {
    const w_game_t *game;
} game_entry_t;

static const game_entry_t k_games[] = {
    { &dino_game },
};

#define GAME_COUNT   ((int)(sizeof(k_games) / sizeof(k_games[0])))

/* ==================================================================
 * 点一行 -> 全屏开玩
 * ================================================================== */
static void game_row_cb(lv_event_t *e)
{
    /*
     * ⚠️ 别在这里直接 `lv_obj_add_event_cb(row, game_row_cb, …)` ——
     *    那样会**绕过 w_menu_row_create() 里的点击守卫**，
     *    用户在列表上滑动之后抬手就会误进游戏（v0.6.0 修的那个问题）。
     *
     * 改成"从行在父容器里的下标反查是哪款游戏"：
     * 行是按 k_games[] 的顺序建的，所以下标就是游戏下标。
     */
    lv_obj_t *row = lv_event_get_target(e);      /* 冒泡时也返回最初的目标对象 */
    const int idx = (int)lv_obj_get_index(row);
    if (idx < 0 || idx >= GAME_COUNT) {
        /* ⚠️ 日志用字也要在字库里（工具会扫字符串字面量），
         *    这里刻意只用已覆盖的字，免得为一条日志重生成三份字库。 */
        ESP_LOGW(TAG, "游戏列表行下标不对：%d", idx);
        return;
    }
    const w_game_t *g = k_games[idx].game;
    if (!g) {
        return;
    }
    if (w_game_open(g) != 0) {
        ESP_LOGW(TAG, "游戏 '%s' 打不开", g->id ? g->id : "?");
    }
}

/* ==================================================================
 * 建界面
 * ================================================================== */
static void games_build(lv_obj_t *parent, void *user)
{
    (void)user;

    /*
     * 行高 56（比功能列表的 52 略高）：这里一行要放"游戏名 + 最高分"，
     * 而且列表很短，不需要省这点高度。
     */
    for (int i = 0; i < GAME_COUNT; i++) {
        const w_game_t *g = k_games[i].game;

        char sub[48];
        int best = 0;
        if (g->best_key) {
            best = watch_ui_setting_get(g->best_key, 0);
        }
        snprintf(sub, sizeof(sub), W_STR_GAME_BEST_FMT, best);

        /*
         * 行按 k_games[] 顺序建（回调靠 lv_obj_get_index() 反查是哪一款），
         * 所以**这一层不要在建行之前插别的对象**，否则下标会错位。
         * 回调也直接在 w_menu_row_create() 里给 —— 它内部有"拖动后不响应点击"
         * 的守卫，另挂一个回调会绕过守卫。
         */
        lv_obj_t *row = w_menu_row_create(parent, g->title,
                                          sub[0] ? sub : NULL, game_row_cb);
        (void)row;
    }

    lv_obj_t *hint = w_label_create_auto(parent, W_STR_GAME_LIST_HINT,
                                         W_FONT_SMALL, 13, W_COLOR_TEXT_DIM);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    if (GAME_COUNT <= 1) {
        lv_obj_t *more = w_label_create_auto(parent, W_STR_GAME_NO_MORE,
                                            W_FONT_SMALL, 13, W_COLOR_TEXT_MUTE);
        lv_obj_set_width(more, LV_PCT(100));
        lv_label_set_long_mode(more, LV_LABEL_LONG_WRAP);
    }
}

/* ==================================================================
 * 对外入口
 * ================================================================== */
void watch_games_open(void)
{
    if (w_modal_open(W_STR_GAMES, games_build, NULL) != 0) {
        ESP_LOGW(TAG, "游戏列表打开失败（模态层数满了？）");
    }
}
