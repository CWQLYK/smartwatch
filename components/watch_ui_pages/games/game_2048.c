/**
 * @file game_2048.c
 * @brief 2048 游戏本体 —— v0.7.0（游戏列表第 2 款）
 *
 * ## 加一款游戏要动的地方（对照 watch_page_games.c 的文件头）
 *
 *   1. 本文件：实现 `w_game_t` 那几个回调          ← 就这一个文件
 *   2. `watch_page_games.c` 的 `k_games[]` 加一行
 *   3. `W_SETTING_2048_BEST` + app.c 一行映射（最高分存档）
 *
 * 全屏外壳、33ms 帧定时器、暂停/结算面板、防息屏、闹钟到点自动暂停
 * **全部由宿主白送**（watch_ui/watch_game.c），本文件一行都不用管。
 *
 * ## 与参考工程（Watch-Player_IDF 的 `fs_game.cpp`）的关键差别
 *
 * | 它的做法 | 问题 | 我们的做法 |
 * |---|---|---|
 * | 动画用 **16 个格子的 LVGL 对象**（`lv_anim` + 移动/缩放） | 对象多、动画和"手动改位置"会打架；退出时必须保证父容器被删才能回收 anim | **动画当数据**：格子状态存结构体，`step()` 插值、`render()` 一次画完。**没有任何 anim/定时器需要清理** |
 * | 动画期间**不屏蔽输入** | 连划会重入、位置跳变 | `busy` 标志：动画没播完不接受新滑动 |
 * | **没有胜负判定** | 无限局，没有结算面板可用 | 判负（无空格且无相邻等值）+ 达成 2048 判胜 |
 * | 用 `pow(2, val)` 还原数值 | 每格每帧一次浮点调用 | **查表** |
 * | 手势用 `LV_EVENT_GESTURE`（阈值 50px） | 240 宽屏上 50px 偏重 | 宿主转发坐标，自己算 Δ，**阈值 28px** |
 *
 * ## 数字怎么画
 *
 * 画布是 2bpp 四色调色板（见 watch_gfx.h），**没有字体**，所以数字要自己画。
 * 用一个 3x5 的点阵字模（`s_digits`），按格子宽度自动居中。
 * 2048 最大到 4 位数（131072 就 6 位了，实际玩不到），
 * 3px 宽 + 1px 间距 = 4px/位，4 位共 15px，44px 的格子里很宽松。
 */
#include "watch_pages.h"    /* watch_ui_setting_get/set + W_SETTING_* 键名 */
#include "watch_game.h"
#include "watch_gfx.h"
#include "watch_style.h"
#include "game_2048.h"

#include <stdio.h>
#include <stdlib.h>     /* rand() —— 漏了会拿到隐式声明的 int rand(void)，
                         * 在这个构建里能编过但语义不保证 */
#include <string.h>
#include "esp_log.h"

static const char *TAG = "2048";

/* ==================== 布局 ==================== */
#define N            4                     /* 4x4 棋盘 */
#define TILE         44                    /* 格子边长 */
#define GAP          6                     /* 格子间距 */
#define BOARD_W      (N * TILE + (N + 1) * GAP)    /* = 4*44 + 5*6 = 206 */
#define BOARD_X      ((W_SCREEN_W - BOARD_W) / 2)  /* = (240-206)/2 = 17 */
#define BOARD_Y      38                    /* 让开宿主的 HUD（暂停键/分数占顶部 32）*/

#define MOVE_MS      110                   /* 移动动画时长 */
#define SWIPE_MIN    28                    /* 滑动判定阈值（像素）*/

/* ==================== 数字点阵（3x5，高位在左）====================
 *
 * 每一位用 5 行 x 3 列；行内 bit2..bit0 对应左中右三列。
 * 例如 '0' = 111 / 101 / 101 / 101 / 111。
 */
static const uint8_t s_digits[10][5] = {
    { 0x7, 0x5, 0x5, 0x5, 0x7 },   /* 0 */
    { 0x2, 0x6, 0x2, 0x2, 0x7 },   /* 1 */
    { 0x7, 0x1, 0x7, 0x4, 0x7 },   /* 2 */
    { 0x7, 0x1, 0x7, 0x1, 0x7 },   /* 3 */
    { 0x5, 0x5, 0x7, 0x1, 0x1 },   /* 4 */
    { 0x7, 0x4, 0x7, 0x1, 0x7 },   /* 5 */
    { 0x7, 0x4, 0x7, 0x5, 0x7 },   /* 6 */
    { 0x7, 0x1, 0x1, 0x1, 0x1 },   /* 7 */
    { 0x7, 0x5, 0x7, 0x5, 0x7 },   /* 8 */
    { 0x7, 0x5, 0x7, 0x1, 0x7 },   /* 9 */
};
#define DIGIT_W   3
#define DIGIT_H   5
#define DIGIT_GAP 1

/** @brief 指数 → 数值（查表，避免每格每帧调 pow）*/
static const uint32_t s_tile_val[16] = {
    0u, 2u, 4u, 8u, 16u, 32u, 64u, 128u,
    256u, 512u, 1024u, 2048u, 4096u, 8192u, 16384u, 32768u,
};

/* ==================== 状态 ==================== */

/** @brief 一格在移动动画里的原位置 */
typedef struct {
    uint8_t from_r, from_c;     /* 从哪来 */
    bool    moving;             /* 这一格是否正在移动 */
} move_t;

static struct {
    uint8_t grid[N][N];         /* 指数：0 = 空，1 = 2，2 = 4 … */
    uint8_t prev[N][N];         /* 移动前的一帧（用于插值）*/
    move_t  mv[N][N];           /* 目标格 ← 来源格 */
    uint32_t score;
    uint32_t best;
    bool    over;
    bool    won;                /* 是否已经达成 2048 */
    bool    busy;               /* 动画进行中：不接受新滑动 */
    uint32_t anim_ms;           /* 已播放的动画时长 */
    /* 触摸 */
    int     press_x, press_y;
    bool    pressed;
    /* 上一次渲染用的画布（render 每次都会刷新，这里只用于自检统计）*/
    uint32_t last_px;
} s;

/* ==================== 绘图工具 ==================== */

/** @brief 把一个数字（3x5 点阵）画到指定位置 */
static void draw_number(w_gfx_t *g, uint32_t v, int x, int y, int idx)
{
    char buf[12];
    const int n = snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
    if (n <= 0) {
        return;
    }
    for (int i = 0; i < n; i++) {
        const int d = buf[i] - '0';
        if (d < 0 || d > 9) {
            continue;
        }
        const int ox = x + i * (DIGIT_W + DIGIT_GAP);
        for (int r = 0; r < DIGIT_H; r++) {
            const uint8_t row = s_digits[d][r];
            for (int c = 0; c < DIGIT_W; c++) {
                /* 高位在左：bit2 = 最左列 */
                if (row & (1u << (DIGIT_W - 1 - c))) {
                    w_gfx_px(g, ox + c, y + r, idx);
                }
            }
        }
    }
}

/** @brief 数字占多宽（居中用）*/
static int number_w(uint32_t v)
{
    char buf[12];
    const int n = snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
    if (n <= 0) {
        return 0;
    }
    return n * DIGIT_W + (n - 1) * DIGIT_GAP;
}

/** @brief 第 r 行第 c 列格子的左上角像素坐标 */
static void cell_xy(int r, int c, int *x, int *y)
{
    *x = BOARD_X + GAP + c * (TILE + GAP);
    *y = BOARD_Y + GAP + r * (TILE + GAP);
}

/** @brief 画一格（值 = 指数；0 = 空）*/
static void draw_cell(w_gfx_t *g, int px, int py, uint8_t exp)
{
    if (exp == 0) {
        /* 空格：只画一个淡边框，看得见格子但很轻 */
        w_gfx_frame(g, px, py, TILE, TILE, W_GFX_IDX_DIM);
        return;
    }

    /* 有值：实心底 + 深色数字 */
    w_gfx_rect(g, px, py, TILE, TILE, W_GFX_IDX_DIM);
    /* 高值给个强调色边框，一眼能看出"这格很大" */
    if (exp >= 8) {                 /* >= 256 */
        w_gfx_frame(g, px, py, TILE, TILE, W_GFX_IDX_HI);
    }

    const uint32_t v = (exp < 16) ? s_tile_val[exp] : 0;
    const int tw = number_w(v);
    draw_number(g, v,
                px + (TILE - tw) / 2,
                py + (TILE - DIGIT_H) / 2,
                W_GFX_IDX_BG);      /* 数字用背景色 → 在 DIM 底上最清楚 */
}

/* ==================== 棋盘逻辑 ==================== */

static int count_empty(void)
{
    int n = 0;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            if (s.grid[r][c] == 0) {
                n++;
            }
        }
    }
    return n;
}

/** @brief 在随机空格放一个新块（90% 出 2 / 10% 出 4，和原版一致）*/
static void spawn_tile(void)
{
    const int empty = count_empty();
    if (empty == 0) {
        return;
    }
    int pick = rand() % empty;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            if (s.grid[r][c] != 0) {
                continue;
            }
            if (pick-- == 0) {
                s.grid[r][c] = (rand() % 10 == 0) ? 2 : 1;   /* 指数 2 = 4 */
                return;
            }
        }
    }
}

/** @brief 判断是否已经无路可走 */
static bool no_moves_left(void)
{
    if (count_empty() > 0) {
        return false;
    }
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            if (c + 1 < N && s.grid[r][c] == s.grid[r][c + 1]) {
                return false;
            }
            if (r + 1 < N && s.grid[r][c] == s.grid[r + 1][c]) {
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief 朝一个方向移动一格棋盘
 *
 * @param dr,dc  方向（例如向左 = dr 0, dc -1）
 * @return true = 这一下真的动了（要补新块）
 *
 * ## 实现要点（照参考工程那套，但用位移直走）
 *
 * - 朝目标边**反序遍历**：靠边的先动，否则后面的块会把前面的挡住。
 * - `merged[][]` 记账：保证**一次移动里同一格只合并一次**
 *   （否则 `2 2 2 2` 往左会合并成 8 而不是 4 4）。
 * - 不搞"旋转棋盘"那套取巧，直走 + 记账更好读也更好调。
 */
static bool move_dir(int dr, int dc)
{
    bool moved = false;

    memcpy(s.prev, s.grid, sizeof(s.grid));
    memset(s.mv, 0, sizeof(s.mv));

    uint8_t merged[N][N];
    memset(merged, 0, sizeof(merged));

    /* 反序：从目标边那一侧开始处理 */
    const int r0 = (dr > 0) ? N - 1 : 0;
    const int r1 = (dr > 0) ? -1 : N;
    const int rs = (dr > 0) ? -1 : 1;
    const int c0 = (dc > 0) ? N - 1 : 0;
    const int c1 = (dc > 0) ? -1 : N;
    const int cs = (dc > 0) ? -1 : 1;

    for (int r = r0; r != r1; r += rs) {
        for (int c = c0; c != c1; c += cs) {
            const uint8_t v = s.grid[r][c];
            if (v == 0) {
                continue;
            }

            int cr = r, cc = c;
            /* 一路往前推 */
            for (;;) {
                const int nr = cr + dr;
                const int nc = cc + dc;
                if (nr < 0 || nr >= N || nc < 0 || nc >= N) {
                    break;                      /* 撞墙 */
                }
                if (s.grid[nr][nc] == 0) {
                    /* 下一格是空的 → 搬过去，继续往前看 */
                    s.grid[nr][nc] = v;
                    s.grid[cr][cc] = 0;
                    cr = nr;
                    cc = nc;
                    moved = true;
                    continue;
                }
                if (s.grid[nr][nc] == v && !merged[nr][nc]) {
                    /* 等值且这一格本回合还没合并过 → 合并 */
                    s.grid[nr][nc] = (uint8_t)(v + 1);
                    s.grid[cr][cc] = 0;
                    merged[nr][nc] = 1;
                    s.score += (v + 1 < 16) ? s_tile_val[v + 1] : 0;
                    if (s.grid[nr][nc] >= 11) {     /* 2048 = 2^11 */
                        s.won = true;
                    }
                    /* 记下"这一格是从 (r,c) 来的"（动画用）*/
                    s.mv[nr][nc].from_r = (uint8_t)r;
                    s.mv[nr][nc].from_c = (uint8_t)c;
                    s.mv[nr][nc].moving = true;
                    moved = true;
                    break;
                }
                break;                          /* 前面挡着且不能合并 */
            }

            /* 只是滑动（没合并）也要记来源，动画才连贯 */
            if ((cr != r || cc != c) && !s.mv[cr][cc].moving) {
                s.mv[cr][cc].from_r = (uint8_t)r;
                s.mv[cr][cc].from_c = (uint8_t)c;
                s.mv[cr][cc].moving = true;
            }
        }
    }

    /* 有格子动过 → 播一段动画、期间不接受新输入 */
    if (moved) {
        s.busy = true;
        s.anim_ms = 0;
    }
    return moved;
}

/* ==================== w_game_t 回调 ==================== */

static int g2048_init(void)
{
    memset(&s, 0, sizeof(s));
    s.best = (uint32_t)watch_ui_setting_get(W_SETTING_2048_BEST, 0);
    ESP_LOGI(TAG, "2048 就绪（棋盘 %dx%d，格子 %dpx，最高分 %lu）",
             BOARD_W, BOARD_W, TILE, (unsigned long)s.best);
    return 0;
}

static void g2048_reset(void)
{
    const uint32_t best = s.best;       /* 最高分要留着 */
    memset(&s, 0, sizeof(s));
    s.best = best;
    /* 开局两个块 */
    spawn_tile();
    spawn_tile();
}

static void g2048_step(uint32_t dt_ms)
{
    if (s.busy) {
        s.anim_ms += dt_ms;
        if (s.anim_ms >= MOVE_MS) {
            /* 动画播完：把 mv 清掉（之后按最终位置画），补一个新块 */
            s.busy = false;
            s.anim_ms = 0;
            memset(s.mv, 0, sizeof(s.mv));
            spawn_tile();

            if (no_moves_left()) {
                s.over = true;
            }
            /* 最高分落盘（只在刷新时写，别每帧写 NVS）*/
            if (s.score > s.best) {
                s.best = s.score;
                watch_ui_setting_set(W_SETTING_2048_BEST, (int)s.best);
            }
        }
    }
}

static void g2048_render(lv_obj_t *canvas)
{
    if (!canvas) {
        return;
    }
    /* 每帧重新取缓冲区：退出时会被释放，缓存指针会在第二局画到已释放内存
     * （dino_game.c 里踩过同一个坑，注释写得很详细）。*/
    lv_img_dsc_t *dsc = lv_canvas_get_img(canvas);
    if (!dsc || !dsc->data) {
        return;
    }

    w_gfx_t g;
    w_gfx_init(&g, (void *)dsc->data, W_SCREEN_W, W_SCREEN_H);
    w_gfx_clear(&g, W_GFX_IDX_BG);

    /* 棋盘底板边框，让"棋盘"有个范围感 */
    w_gfx_frame(&g, BOARD_X + 1, BOARD_Y + 1, BOARD_W - 2, BOARD_W - 2,
                W_GFX_IDX_DIM);

    const bool animating = s.busy && (s.anim_ms < MOVE_MS);

    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            int px, py;
            cell_xy(r, c, &px, &py);

            const uint8_t v = s.grid[r][c];

            if (animating && s.mv[r][c].moving && v != 0) {
                /*
                 * 这一格正在移动：从来源格插值到本格。
                 * 用线性插值就够（110ms 很短，缓动看不出来），
                 * 而且**不需要任何 LVGL 动画对象** —— 这就是"动画当数据"。
                 */
                int sx, sy;
                cell_xy(s.mv[r][c].from_r, s.mv[r][c].from_c, &sx, &sy);
                const int t = (int)s.anim_ms;
                px = sx + (px - sx) * t / MOVE_MS;
                py = sy + (py - sy) * t / MOVE_MS;
                draw_cell(&g, px, py, v);
            } else {
                draw_cell(&g, px, py, v);
            }
        }
    }

    /* 分数（画布左下角小字，宿主右上角那个是"总分"显示，这里是同步用的）*/
    s.last_px = (uint32_t)w_gfx_count(&g, W_GFX_IDX_BG);
}

static void g2048_input(int x, int y, bool down)
{
    if (s.over) {
        return;                     /* 结束了，交给宿主的结算面板 */
    }
    if (down) {
        s.press_x = x;
        s.press_y = y;
        s.pressed = true;
        return;
    }

    if (!s.pressed) {
        return;
    }
    s.pressed = false;

    /* 动画没播完不接受新滑动（参考工程漏了这一步，连划会跳变）*/
    if (s.busy) {
        return;
    }

    const int dx = x - s.press_x;
    const int dy = y - s.press_y;
    const int adx = (dx < 0) ? -dx : dx;
    const int ady = (dy < 0) ? -dy : dy;

    if (adx < SWIPE_MIN && ady < SWIPE_MIN) {
        return;                     /* 当成点击，不处理 */
    }

    /* 取主轴方向 */
    if (adx > ady) {
        move_dir(0, (dx > 0) ? 1 : -1);
    } else {
        move_dir((dy > 0) ? 1 : -1, 0);
    }
}

static int g2048_score(void)
{
    return (int)s.score;
}

static bool g2048_over(void)
{
    return s.over;
}

/** @brief 整块棋盘都可能变（有动画），所以整屏重画 */
static lv_coord_t g2048_dirty_top(void)
{
    return BOARD_Y;
}

static void g2048_destroy(void)
{
    /* 没有任何需要释放的资源：没有动画对象、没有定时器、没有动态内存。
     * 这正是"动画当数据"换来的好处。*/
    memset(&s, 0, sizeof(s));
}

const w_game_t game_2048 = {
    .id        = "2048",
    .title     = "2048",
    .hint      = "滑动合并相同数字",
    .best_key  = W_SETTING_2048_BEST,
    .canvas_w  = W_SCREEN_W,
    .canvas_h  = W_SCREEN_H,
    .init      = g2048_init,
    .reset     = g2048_reset,
    .step      = g2048_step,
    .render    = g2048_render,
    .input     = g2048_input,
    .score     = g2048_score,
    .over      = g2048_over,
    .dirty_top = g2048_dirty_top,
    .destroy   = g2048_destroy,
};

/* ==================================================================
 * 自检接口（PC 模拟器用）
 *
 * 为什么需要：正常开局的棋盘是**随机的**，靠滑方向去断言结果非常脆
 * （合并与否取决于那一格恰好是不是等值）。所以给自检一组
 * "注入已知棋盘 → 走一步 → 读回结果"的口子，
 * 这样移动/合并/计分/判负这些纯逻辑就能被确定性地锁死。
 *
 * 和 dino_game.c 的 `dino_debug_*` 是同一套做法。
 * ================================================================== */

void game2048_debug_set_grid(const uint8_t g[16])
{
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            s.grid[r][c] = g[r * N + c];
        }
    }
    memset(s.mv, 0, sizeof(s.mv));
    s.busy = false;
    s.anim_ms = 0;
}

uint8_t game2048_debug_get(int r, int c)
{
    if (r < 0 || r >= N || c < 0 || c >= N) {
        return 0;
    }
    return s.grid[r][c];
}

int game2048_debug_move(int dr, int dc)
{
    return move_dir(dr, dc) ? 1 : 0;
}

void game2048_debug_set_score(uint32_t v)
{
    s.score = v;
}

uint32_t game2048_debug_score(void)
{
    return s.score;
}

int game2048_debug_no_moves(void)
{
    return no_moves_left() ? 1 : 0;
}

/** @brief 把当前棋盘的数值形态打成字符串（"2,4,0,..."），自检打失败信息用 */
void game2048_debug_dump(char *buf, int len)
{
    if (!buf || len <= 0) {
        return;
    }
    int n = 0;
    for (int r = 0; r < N && n < len - 8; r++) {
        for (int c = 0; c < N && n < len - 8; c++) {
            const uint8_t e = s.grid[r][c];
            const uint32_t v = (e < 16) ? s_tile_val[e] : 0;
            n += snprintf(buf + n, (size_t)(len - n), "%lu%s",
                          (unsigned long)v,
                          (r == N - 1 && c == N - 1) ? "" : ",");
        }
    }
    buf[len - 1] = '\0';
}

/**
 * @brief 自检用：直接画一帧（跳过游戏状态机）
 *
 * 用来**看数字排版**：注入含大数值（1024/2048）的棋盘再截图，
 * 确认两位数/四位数在 44px 的格子里放得下、居中好看。
 * 这类"画得好不好看"的问题断言抓不到，只能看图 —— 项目一贯做法。
 */
void game2048_debug_render(lv_obj_t *canvas)
{
    g2048_render(canvas);
}
