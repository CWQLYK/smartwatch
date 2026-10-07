/**
 * @file dino_game.c
 * @brief 谷歌小恐龙（Chrome Dino）—— 竖屏手表版
 *
 * ## 和原版的三个差别（都是这块表逼出来的，不是偷懒）
 *
 * 1. **没有下蹲**。原版是空格跳、下箭头蹲，两个键互不干扰；
 *    这块表 `board.h` 里**一个实体按键都没有**，只有一块触摸屏，
 *    "点任意处跳"和"按住下半屏蹲"会抢同一块区域 ——
 *    想蹲的人会先跳起来撞死。所以用户拍板：**只保留跳跃**，
 *    翼龙改成在高空飞过、不需要躲（而且不给它碰撞盒，双保险）。
 *
 * 2. **竖屏**。手表戴在手上没法横过来，所以是"天在地上、障碍从右往左来"。
 *    镜头宽度只有 240px（原版 600px），预警距离天然短得多，
 *    所以速度曲线整体压到原版的 1/3 左右（见 kSpeedStart/kSpeedMax）。
 *
 * 3. **深色主题**。原版白底黑恐龙，这里反过来：近黑底 + 浅色恐龙，
 *    和表盘其它页面一致（调色板见 watch_game.c 的 canvas_alloc）。
 *
 * ## 坐标约定
 *
 * 画布就是整屏 240x280，y 向下：
 *
 *     y=0    ┌──────────────┐  ← 宿主画的 HUD（暂停键 + 分数），不归本文件管
 *     y=30   ├──────────────┤
 *            │   云 / 翼龙    │     云在 108~152，翼龙在 62~96
 *     y=248  ├──────────────┤  ← 地面线（kGroundTop）
 *     y=258  │   地面条 10px  │
 *     y=280  └──────────────┘
 */
#include "dino_game.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "watch_gfx.h"
#include "watch_pages.h"      /* W_SETTING_DINO_BEST */
#include "watch_strings.h"
#include "dino_sprites.h"

static const char *TAG = "DINO";

/* ==================================================================
 * 一、布局与物理参数
 * ================================================================== */
#define SCR_W            240
#define SCR_H            280

#define GROUND_TOP       248      /* 地面条顶边（恐龙脚底就站在这里） */
#define DINO_X           20
#define DINO_W           40
#define DINO_H           43

/** @brief 跳跃初速度与重力
 *
 * 这两个数**不能随便调**，它们和"速度曲线 / 障碍宽度"是一条约束链上的：
 *
 *     能从障碍上跳过去的条件：
 *         脚底高于障碍的时长  ≥  障碍从"碰到恐龙"到"离开恐龙"的时长
 *
 *   左边 = 2*sqrt(v0² - 2gh)/g   （抛物线高于 h 的那一段）
 *   右边 = (障碍宽 + 恐龙碰撞盒宽) / 速度
 *
 * 当前参数：v0=530、g=1250
 *   · 最高点 = v0²/(2g) ≈ 112px，滞空 0.85s
 *   · 高于 46px（大仙人掌）的时长 ≈ 0.66s，高于 33px（小仙人掌）≈ 0.72s
 *   · 起始 190px/s 时，两株大仙人掌（46px）通过恐龙身位要 0.41s，余量 60%；
 *     三株（69px）要 0.51s，余量 29% —— 所以 spawn_obstacle() 的闸门
 *     在起始速度下只放两株，三株要等速度涨上去。
 *
 * ⚠️ 第一版这里写的是 v0=520 / g=2000 / 起始速度 110px/s。
 *    结果：高于仙人掌只有 0.32s，而单株大仙人掌通过恐龙身位要 0.5s，
 *    **数学上就不可能跳过去**。是自检里的机器玩家 3 秒撞死才把它揪出来的 ——
 *    实机上人肉试玩只会觉得"这游戏好难"，根本定位不到参数关系。
 */
#define GRAVITY          1250.0f
#define JUMP_V0          530.0f

/** @brief 松手截断系数（按住跳更高的实现）
 *
 * 点一下就松：上升速度掉到 80% → 最高点约 72px、高于 46px 仍有约 0.42s，
 * 足够跳过大仙人掌（需要约 0.27s）。
 * 一直按着：跳满 112px。
 *
 * ⚠️ 别把这个系数调太小。第一版用 0.40，点一下最高只有 16px，
 *    **连小仙人掌都过不去** —— "按住跳更高"变成了"不按住就必死"，
 *    这不是难度，这是坏操作。 */
#define JUMP_CUT         0.80f

/** @brief 速度曲线（像素/秒）
 *
 * 下限由上面的约束链反推：太慢则障碍"通过恐龙身位"要很久，宽障碍必死。
 *
 * 反应窗口 = 障碍从屏幕右缘到撞上恐龙的距离 ÷ 速度
 *          = (240-54) / 190 ≈ 0.98 秒（够新手）；封顶 320 时约 0.58 秒。 */
#define SPEED_START      190.0f
#define SPEED_MAX        320.0f
#define SPEED_PER_100    10.0f    /* 每 100 分加速多少 px/s */

/** @brief 分数 = 走过的距离 / 这个数 */
#define SCORE_DIV        9.0f

/** @brief 障碍间隔（秒 x 速度）
 *
 * 下限必须大于"一次跳跃的滞空时间"（0.85s）+ 落地后的反应时间，
 * 否则落地就来下一个，玩家来不及再起跳。1.45~2.55 秒，机器玩家验证过。 */
#define GAP_MIN_SEC      1.45f
#define GAP_RAND_SEC     1.10f

#define CLOUD_MIN_Y      120
#define CLOUD_MAX_Y      160

/** @brief 翼龙飞的高度带
 *
 * ⚠️ 必须**高于跳跃最高点**：最高点时恐龙头顶在 y = 248-112-43 = 93，
 *    所以翼龙底边（y+32）要小于 93，也就是 y ≤ 61。这里取 28~56，
 *    留了 5px 余量 —— 玩家跳到最高点也不会"穿过"翼龙。
 *    （翼龙本来就没有碰撞盒，这是第二道保险。） */
#define BIRD_MIN_Y       28
#define BIRD_MAX_Y       56

#define MAX_OBS          4
#define MAX_CLOUDS       3
#define MAX_BIRDS        2

/* ==================================================================
 * 二、状态
 * ================================================================== */
typedef struct {
    bool  active;
    float x;              /* 左边缘 */
    int   kind;           /* 0=小 1=大 */
    int   copies;         /* 并排几株（原版就是这么拼出"双仙人掌"的） */
    int   variant;        /* 用哪张图 */
    int   w, h;
} obstacle_t;

typedef struct {
    bool  active;
    float x;
    int   y;
} cloud_t;

typedef struct {
    bool  active;
    float x;
    int   y;
    int   frame;
    float anim;           /* 扇翅膀计时 */
} bird_t;

static w_gfx_t     s_g;                    /* 每帧从画布重新绑定 */

static float       s_dino_y;               /* 脚底 y（相对地面线为 0，向上为负） */
static float       s_vy;
static bool        s_on_ground;
static bool        s_holding;              /* 手指是否按着 */
static int         s_jumps;
static int         s_hits;
static bool        s_over;

static float       s_speed;
static float       s_score_f;
static int         s_score;
static float       s_ground_off;           /* 地面卷动偏移 */
static float       s_spawn_dist;           /* 还有多少像素生成下一个障碍 */

static obstacle_t  s_obs[MAX_OBS];
static cloud_t     s_clouds[MAX_CLOUDS];
static bird_t      s_birds[MAX_BIRDS];

static uint32_t    s_rng = 0x12345678u;
static bool        s_seeded = false;       /* 自检固定种子时为 true */

static uint32_t    s_run_ms;               /* 跑动动画计时 */
static int         s_run_frame;
static int         s_unfair_spawns;        /* 生成了"跳不过去"的障碍的次数（必须恒为 0） */

/* ==================================================================
 * 三、随机数（自己写，不用 rand() —— 要能确定性重放）
 * ================================================================== */
static uint32_t rnd_u32(void)
{
    /* xorshift32：够随机，而且跨平台结果完全一样（自检要和实机对得上） */
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

/** @brief 0.0 ~ 1.0 */
static float rnd_f(void)
{
    return (float)(rnd_u32() >> 8) / 16777216.0f;
}

static int rnd_range(int lo, int hi)
{
    if (hi <= lo) {
        return lo;
    }
    return lo + (int)(rnd_u32() % (uint32_t)(hi - lo + 1));
}

/* ==================================================================
 * 四、障碍物表
 * ================================================================== */
static const w_gfx_sprite_t *const k_small[2] = {
    &dino_spr_cactus_small1, &dino_spr_cactus_small2,
};
static const w_gfx_sprite_t *const k_large[2] = {
    &dino_spr_cactus_large1, &dino_spr_cactus_large2,
};

/** @brief 分数到了才出翼龙（原版也是 300 分以后才有） */
#define BIRD_FROM_SCORE  300

/* ==================================================================
 * 五、公平性闸门（跳跃弧线 -> 允许的障碍宽度）
 * ================================================================== */
/** @brief 恐龙碰撞盒宽（dino_box() 里的 w） */
#define DINO_HIT_W       32

/**
 * @brief 脚底高于 h 的持续时间（秒）
 *
 * 抛物线 h(t) = v0*t - g*t²/2 ≥ h 的解区间长度 = 2*sqrt(v0² - 2gh)/g。
 * 只在"生成障碍"时调用，不在每帧路径上，所以 sqrtf 的开销无所谓。
 */
static float air_time_above(float h)
{
    const float d = JUMP_V0 * JUMP_V0 - 2.0f * GRAVITY * h;
    if (d <= 0.0f) {
        return 0.0f;                 /* 根本跳不到这个高度 */
    }
    return 2.0f * sqrtf(d) / GRAVITY;
}

/**
 * @brief 当前速度下，一次跳跃最多能清过多宽的障碍
 *
 * @param cactus_h 障碍高度
 * @return 允许的总宽度（像素）；<= 0 表示连一株都跳不过去
 *
 * @note 留 25% 余量：像素碰撞盒各缩了 2px、帧是离散步进的、
 *       而且玩家不可能每一跳都卡在理论最优点上。
 *       余量给足一点，"能不能跳过去"才不会变成"要卡到帧才跳得过去"。
 */
static int max_clearable_width(int cactus_h)
{
    const float t_above = air_time_above((float)cactus_h) * 0.75f;
    const int   w       = (int)(t_above * s_speed) - DINO_HIT_W;
    return (w > 0) ? w : 0;
}

/* ==================================================================
 * 五、重置 / 初始化
 * ================================================================== */
static void spawn_obstacle(void)
{
    for (int i = 0; i < MAX_OBS; i++) {
        obstacle_t *o = &s_obs[i];
        if (o->active) {
            continue;
        }

        const int kind    = (rnd_f() < 0.5f) ? 0 : 1;
        const int variant = rnd_range(0, 1);
        const w_gfx_sprite_t *spr = (kind == 0) ? k_small[variant] : k_large[variant];

        /*
         * ★ 并排几株，**由当前速度反推上限**（这是公平性的硬保证，不是调参）
         *
         * 判据：脚底高于这个障碍的时长 ≥ 障碍通过恐龙身位所需时长
         *        2*sqrt(v0²-2gh)/g * 0.85  ≥  (宽 + 碰撞盒宽) / 速度
         * 左边乘 0.85 是留安全余量（像素碰撞盒有 2px 收缩、帧是离散的）。
         *
         * 于是：低速时只会出单株/两株，高速时才可能出现三株 ——
         * 难度随速度自然长起来，而且**永远不可能生成一个跳不过去的组合**。
         */
        const int allowed = max_clearable_width(spr->h);
        int max_copies = allowed / (int)spr->w;
        const int hard_cap = 3;                     /* 三株 = 69px，屏宽的 29%，再多就挤了 */
        if (max_copies > hard_cap) {
            max_copies = hard_cap;
        }
        if (max_copies < 1) {
            /*
             * 理论不该发生（起始速度已经按这条约束挑过了）。
             * 真发生就说明有人把速度下调过头 —— 记一笔，
             * 自检会断言这个计数必须是 0（见 dino_debug_unfair_spawns）。
             */
            s_unfair_spawns++;
            max_copies = 1;
            ESP_LOGW(TAG, "速度为 %.0f 时 %dx%d 的障碍理论上跳不过去",
                     (double)s_speed, (int)spr->w, (int)spr->h);
        }

        o->copies  = rnd_range(1, max_copies);
        o->variant = variant;
        o->kind    = kind;
        o->w       = spr->w * o->copies;
        o->h       = spr->h;
        o->x       = (float)SCR_W;
        o->active  = true;

        ESP_LOGD(TAG, "生成障碍 kind=%d variant=%d copies=%d/%d w=%d 速度=%.0f",
                 kind, variant, o->copies, max_copies, o->w, (double)s_speed);
        return;
    }
}

static void spawn_cloud(void)
{
    for (int i = 0; i < MAX_CLOUDS; i++) {
        cloud_t *c = &s_clouds[i];
        if (c->active) {
            continue;
        }
        c->active = true;
        c->x = (float)SCR_W + rnd_f() * 60.0f;
        c->y = rnd_range(CLOUD_MIN_Y, CLOUD_MAX_Y);
        return;
    }
}

static void spawn_bird(void)
{
    for (int i = 0; i < MAX_BIRDS; i++) {
        bird_t *b = &s_birds[i];
        if (b->active) {
            continue;
        }
        b->active = true;
        b->x = (float)SCR_W + rnd_f() * 40.0f;
        b->y = rnd_range(BIRD_MIN_Y, BIRD_MAX_Y);
        b->frame = 0;
        b->anim = 0.0f;
        return;
    }
}

static void do_reset(void)
{
    s_dino_y    = 0.0f;
    s_vy        = 0.0f;
    s_on_ground = true;
    s_holding   = false;
    s_over      = false;
    s_hits      = 0;
    s_jumps     = 0;
    s_speed     = SPEED_START;
    s_score_f   = 0.0f;
    s_score     = 0;
    s_ground_off = 0.0f;
    s_spawn_dist = 200.0f;        /* 开局给玩家一点准备时间 */
    s_run_ms    = 0;
    s_run_frame = 0;

    memset(s_obs, 0, sizeof(s_obs));
    memset(s_clouds, 0, sizeof(s_clouds));
    memset(s_birds, 0, sizeof(s_birds));

    if (!s_seeded) {
        /* 实机：每局都不一样。自检会先调 dino_debug_seed() 把种子钉死。 */
        s_rng = lv_tick_get() | 1u;
    }
    spawn_cloud();
    spawn_cloud();
}

static int dino_init(void)
{
    s_seeded = false;
    do_reset();
    ESP_LOGI(TAG, "小恐龙就绪（画布 %dx%d，重力 %.0f，初速 %.0f，速度 %.0f~%.0f）",
             SCR_W, SCR_H, (double)GRAVITY, (double)JUMP_V0,
             (double)SPEED_START, (double)SPEED_MAX);
    return 0;
}

static void dino_destroy(void)
{
    /* 画布和缓冲区由宿主释放；这里只把自己的引用清掉，
     * 免得 dino_debug_ink_pixels() 之类去读已经还给系统的内存。 */
    memset(&s_g, 0, sizeof(s_g));
}

/* ==================================================================
 * 六、碰撞
 * ================================================================== */
/** @brief 恐龙的碰撞盒（比图形小一圈，手感更宽容） */
static void dino_box(int *x, int *y, int *w, int *h)
{
    /* 脚底在 GROUND_TOP + s_dino_y（s_dino_y 向上为负） */
    const int feet = GROUND_TOP + (int)s_dino_y;
    const int top  = feet - DINO_H;
    *x = DINO_X + 4;
    *y = top + 2;
    *w = DINO_W - 8;
    *h = DINO_H - 4;
}

static bool hit_any(void)
{
    int dx, dy, dw, dh;
    dino_box(&dx, &dy, &dw, &dh);

    for (int i = 0; i < MAX_OBS; i++) {
        const obstacle_t *o = &s_obs[i];
        if (!o->active) {
            continue;
        }
        /* 障碍盒也缩 2px：像素画边缘的"擦到一点点"不该算死 */
        const int ox = (int)o->x + 2;
        const int oy = GROUND_TOP - o->h + 2;
        const int ow = o->w - 4;
        const int oh = o->h - 2;

        if (dx < ox + ow && dx + dw > ox && dy < oy + oh && dy + dh > oy) {
            return true;
        }
    }
    return false;
}

/* ==================================================================
 * 七、每帧推进
 * ================================================================== */
static void step(float dt)
{
    /* ---- 速度与分数 ---- */
    s_speed = SPEED_START + (float)(s_score / 100) * SPEED_PER_100;
    if (s_speed > SPEED_MAX) {
        s_speed = SPEED_MAX;
    }

    const float dist = s_speed * dt;
    s_ground_off += dist;
    s_score_f += dist / SCORE_DIV;
    s_score = (int)s_score_f;

    /* ---- 跳跃物理 ---- */
    if (!s_on_ground) {
        s_vy += GRAVITY * dt;
        s_dino_y += s_vy * dt;
        if (s_dino_y >= 0.0f) {
            s_dino_y = 0.0f;
            s_vy = 0.0f;
            s_on_ground = true;
        }
    }

    /* ---- 跑动动画（按时间换帧，不按距离 —— 快慢都看得清腿在动） ---- */
    s_run_ms += (uint32_t)(dt * 1000.0f);
    if (s_run_ms >= 110) {
        s_run_ms = 0;
        s_run_frame ^= 1;
    }

    /* ---- 障碍 ---- */
    s_spawn_dist -= dist;
    if (s_spawn_dist <= 0.0f) {
        spawn_obstacle();
        s_spawn_dist = s_speed * (GAP_MIN_SEC + rnd_f() * GAP_RAND_SEC);
    }
    for (int i = 0; i < MAX_OBS; i++) {
        obstacle_t *o = &s_obs[i];
        if (!o->active) {
            continue;
        }
        o->x -= dist;
        if (o->x + (float)o->w < 0.0f) {
            o->active = false;
        }
    }

    /* ---- 云（比地面慢，做出视差） ---- */
    for (int i = 0; i < MAX_CLOUDS; i++) {
        cloud_t *c = &s_clouds[i];
        if (!c->active) {
            continue;
        }
        c->x -= dist * 0.35f;
        if (c->x + (float)dino_spr_cloud.w < 0.0f) {
            c->active = false;
        }
    }
    if (rnd_f() < 0.004f) {
        spawn_cloud();
    }

    /* ---- 翼龙（纯装饰：飞得比跳跃最高点还高，而且没有碰撞盒） ---- */
    for (int i = 0; i < MAX_BIRDS; i++) {
        bird_t *b = &s_birds[i];
        if (!b->active) {
            continue;
        }
        b->x -= dist * 0.80f;
        b->anim += dt;
        if (b->anim >= 0.18f) {
            b->anim = 0.0f;
            b->frame ^= 1;
        }
        if (b->x + (float)dino_spr_bird1.w < 0.0f) {
            b->active = false;
        }
    }
    if (s_score >= BIRD_FROM_SCORE && rnd_f() < 0.0025f) {
        spawn_bird();
    }

    /* ---- 碰撞 ---- */
    if (hit_any()) {
        s_hits++;
        s_over = true;
        ESP_LOGI(TAG, "撞到障碍，得分 %d，速度 %.0f", s_score, (double)s_speed);
    }
}

static void dino_step(uint32_t dt_ms)
{
    if (s_over) {
        return;
    }
    step((float)dt_ms / 1000.0f);
}

/* ==================================================================
 * 八、绘制
 * ================================================================== */
static const w_gfx_sprite_t *dino_sprite_now(void)
{
    if (s_over) {
        return &dino_spr_dino_dead;
    }
    if (!s_on_ground) {
        return &dino_spr_dino_jump;
    }
    /* 站着不动（刚开始、还没跳）用站立帧，跑起来才换腿 */
    if (s_score == 0 && s_jumps == 0) {
        return &dino_spr_dino_idle;
    }
    return s_run_frame ? &dino_spr_dino_run2 : &dino_spr_dino_run1;
}

static void draw_all(void)
{
    w_gfx_clear(&s_g, W_GFX_IDX_BG);

    /* ---- 云 ---- */
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (s_clouds[i].active) {
            w_gfx_blit(&s_g, (int)s_clouds[i].x, s_clouds[i].y, &dino_spr_cloud);
        }
    }

    /* ---- 翼龙 ---- */
    for (int i = 0; i < MAX_BIRDS; i++) {
        const bird_t *b = &s_birds[i];
        if (b->active) {
            w_gfx_blit(&s_g, (int)b->x, b->y,
                       b->frame ? &dino_spr_bird2 : &dino_spr_bird1);
        }
    }

    /* ---- 地面（1200px 的原版地面条循环铺，卷动看起来是连续地形） ---- */
    w_gfx_blit_tiled_x(&s_g, 0, GROUND_TOP, SCR_W,
                       (int)s_ground_off, &dino_spr_ground);

    /* ---- 障碍 ---- */
    for (int i = 0; i < MAX_OBS; i++) {
        const obstacle_t *o = &s_obs[i];
        if (!o->active) {
            continue;
        }
        const w_gfx_sprite_t *spr = (o->kind == 0)
            ? k_small[o->variant & 1]
            : k_large[o->variant & 1];
        const int y = GROUND_TOP - o->h;
        for (int c = 0; c < o->copies; c++) {
            w_gfx_blit(&s_g, (int)o->x + c * spr->w, y, spr);
        }
    }

    /* ---- 恐龙 ---- */
    const w_gfx_sprite_t *ds = dino_sprite_now();
    w_gfx_blit(&s_g, DINO_X, GROUND_TOP + (int)s_dino_y - DINO_H, ds);
}

static void dino_render(lv_obj_t *canvas)
{
    if (!canvas) {
        return;
    }
    /*
     * 每帧都从画布重新取一次缓冲区指针：
     * 退出游戏时 canvas 和缓冲区都会被释放，下一次打开是**新的一块内存**。
     * 缓存下来的话，第二局就会画到已释放的内存上（画面不动 + 随机崩溃）。
     *
     * 强制去掉 const：画布的这块缓冲区本来就是给人写的
     * （lv_canvas_set_px 也是这么干的），LVGL 只是把 data 字段声明成了 const。
     */
    lv_img_dsc_t *dsc = lv_canvas_get_img(canvas);
    if (!dsc || !dsc->data) {
        return;
    }
    w_gfx_init(&s_g, (void *)dsc->data, SCR_W, SCR_H);
    draw_all();
}

/** @brief 本帧至少要重画到哪一行
 *
 * 取所有"会动的东西"里最高的那个顶边，再往上留 1px 余量。
 * 天空是纯底色，不在这条带子里的像素永远不会变 ——
 * 这就是把每帧 134KB 的刷新量压到 60KB 左右的办法。 */
static lv_coord_t dino_dirty_top(void)
{
    int top = GROUND_TOP;                     /* 地面永远要重画 */

    const int dino_top = GROUND_TOP + (int)s_dino_y - DINO_H;
    if (dino_top < top) {
        top = dino_top;
    }
    for (int i = 0; i < MAX_OBS; i++) {
        if (s_obs[i].active && GROUND_TOP - s_obs[i].h < top) {
            top = GROUND_TOP - s_obs[i].h;
        }
    }
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (s_clouds[i].active && s_clouds[i].y < top) {
            top = s_clouds[i].y;
        }
    }
    for (int i = 0; i < MAX_BIRDS; i++) {
        if (s_birds[i].active && s_birds[i].y < top) {
            top = s_birds[i].y;
        }
    }

    top -= 1;
    if (top < 0) {
        top = 0;
    }
    return (lv_coord_t)top;
}

/* ==================================================================
 * 九、输入（只有跳跃，没有下蹲）
 * ================================================================== */
static void dino_input(int x, int y, bool down)
{
    (void)x;
    (void)y;
    if (s_over) {
        return;
    }

    if (down) {
        s_holding = true;
        if (s_on_ground) {
            s_vy = -JUMP_V0;
            s_on_ground = false;
            s_jumps++;
        }
        return;
    }

    /* 松手：如果还在上升，把上升速度砍掉 —— 这就是"按住跳更高" */
    s_holding = false;
    if (!s_on_ground && s_vy < 0.0f) {
        s_vy *= JUMP_CUT;
    }
}

/* ==================================================================
 * 十、对外定义
 *
 * ⚠️ 顺序很重要：`.score` / `.over` 这两个函数必须**先定义**，
 *    因为 `dino_game` 是 `const` 的 —— 结构体一旦定义好就没法再"补"
 *    成员了（我第一版就在下面写了句"下面补"，那样链接出来的
 *    score/over 都是 NULL，宿主直接跳过结束判定，游戏永远不会结束）。
 * ================================================================== */
static int  dino_score(void) { return s_score; }
static bool dino_over(void)  { return s_over; }

const w_game_t dino_game = {
    .id        = "dino",
    .title     = W_STR_GAME_DINO,
    .hint      = W_STR_GAME_DINO_HINT,
    .best_key  = W_SETTING_DINO_BEST,
    .canvas_w  = SCR_W,
    .canvas_h  = SCR_H,
    .init      = dino_init,
    .reset     = do_reset,
    .step      = dino_step,
    .render    = dino_render,
    .input     = dino_input,
    .score     = dino_score,
    .over      = dino_over,
    .dirty_top = dino_dirty_top,
    .destroy   = dino_destroy,
};

/* ==================================================================
 * 十一、自检钩子
 * ================================================================== */
void dino_debug_seed(uint32_t seed)
{
    s_rng = seed ? seed : 1u;
    s_seeded = true;
    do_reset();          /* 立刻按新种子重排一遍开局 */
}

float dino_debug_speed(void)          { return s_speed; }
bool  dino_debug_on_ground(void)      { return s_on_ground; }
int   dino_debug_jumps(void)          { return s_jumps; }
int   dino_debug_hits(void)           { return s_hits; }

/**
 * @brief 画布上有多少个墨色像素
 *
 * @note 这是**按需扫描整幅画布**（6.7 万次读），只在自检里调 ——
 *       绝不能放进每帧的绘制路径，那样一帧就多几毫秒。
 *       必须是 render() 跑过之后调，否则 s_g 还是空的。
 */
int dino_debug_ink_pixels(void)
{
    if (!s_g.px) {
        return 0;
    }
    return w_gfx_count(&s_g, W_GFX_IDX_INK);
}

float dino_debug_nearest_gap(void)
{
    /*
     * 最近障碍的**碰撞盒左边缘** 到 **恐龙碰撞盒右边缘** 的距离（像素）。
     *
     * 口径必须是碰撞盒而不是图形边缘：自检里的机器玩家靠这个数决定
     * 什么时候起跳，用图形边缘的话会差 6px —— 而 6px 在 33ms 一帧、
     * 300px/s 的速度下正好是半帧的误差，足够让它偶尔撞死，
     * 于是自检会给出"难度不公平"的假警报。
     *
     * 已经跑到恐龙身后（gap < -20）的不算：否则机器玩家会对着
     * 屁股后面的仙人掌起跳。
     */
    const float dino_right = (float)(DINO_X + 4 + DINO_HIT_W);
    float best = 9999.0f;
    for (int i = 0; i < MAX_OBS; i++) {
        const obstacle_t *o = &s_obs[i];
        if (!o->active) {
            continue;
        }
        const float gap = (o->x + 2.0f) - dino_right;
        if (gap > -20.0f && gap < best) {
            best = gap;
        }
    }
    return best;
}

int dino_debug_unfair_spawns(void)
{
    return s_unfair_spawns;
}

void dino_debug_hitbox(int *x, int *y, int *w, int *h)
{
    int bx, by, bw, bh;
    dino_box(&bx, &by, &bw, &bh);
    if (x) *x = bx;
    if (y) *y = by;
    if (w) *w = bw;
    if (h) *h = bh;
}

int dino_debug_obstacles(void)
{
    int n = 0;
    for (int i = 0; i < MAX_OBS; i++) {
        if (s_obs[i].active) {
            n++;
        }
    }
    return n;
}
