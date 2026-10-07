/**
 * @file qmi8658c.c
 * @brief QMI8658C 驱动 + 腕戴计步算法（全定点实现）
 *
 * ── 信号链（采样率 50Hz）──────────────────────────────────────────
 *   |a| ──► 重力 LPF ──► 带通 0.5–3.5Hz（2 级 biquad，Q15）
 *        ──► EMA 均值/方差 ──► 自适应门限 + 波峰因数门
 *        ──► 上升沿峰值跟踪（回落确认，能处理连峰） ──► 不应期 + 节律门
 *        ──► 确认一步
 *
 * ── 带通滤波器设计（Q15 量化后实测响应，已数值验证）──────────────
 *   0.5Hz -2.97dB | 1.0Hz -0.24dB | 1.32Hz 0.00dB | 2.0Hz -0.70dB
 *   3.0Hz -2.91dB | 3.5Hz -4.34dB | 5.0Hz -9.56dB | 8.0Hz -20.9dB
 *   12Hz -34.4dB  | 20Hz -65.2dB
 *   累加器最大幅值 2.9e8（int32 内，±8g 不溢出）
 *
 * ⚠️ 已知的实测经验（不是理论值，必须上板确认）：
 *   - 用户静止不动 15 秒内 step_counter 不应计步；
 *   - 走路时误差应有 ±10% 以内；跑步/极慢走需重新调参；
 *   - 打字/挥手可能误计，需调 crest_min_x100 与 rhythm_cv_x1000。
 */
#include "qmi8658c.h"
#include <string.h>
#ifndef IMU_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#else
#include "esp_log.h"
/* host build: no RTOS / timer needed for the algorithm half */
#endif

static const char *TAG = "qmi8658c";
/* TAG is only referenced by the register-access half and by ESP_LOGD, which
 * compiles to nothing in release builds; keep the warning away. */
#define IMU_UNUSED(x) ((void)(x))

/* ==================================================================
 * 一、计步算法内部常量
 * ================================================================== */

/* 带通系数 {b0,b1,b2,a1,a2}，Q15；反馈符号约定：
 * y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2                            */
static const int32_t k_bp[2][5] = {
    { 5237,    0, -5237, -54370, 22362 },   /* RBJ BPF f0=1.323Hz, BW=2.807oct */
    { 1850, 3699,  1850, -40129, 14760 },   /* RBJ LPF 4.5Hz, Q=0.7071        */
};

#define Q_SIG           10                  /* 信号小数位（Q10 g） */
#define Q_COEF          15                  /* 系数小数位 */
#define MG_ONE_G        1000

/* 重力 LPF：τ = 3s @50Hz → alpha = dt/(τ+dt) = 0.00662 → 7/1024
 * 必须远慢于最慢步周期（30spm 时 2s）。若 τ 取 0.6s，跟踪器会跟着步伐
 * 本身摆动，不但去不掉重力，反而把步态信号放大 —— 这是常见踩坑点。 */
#define GRAV_ALPHA_Q10   7
/* 统计 EMA：τ = 1.6s @50Hz → 13/1024 */
#define STAT_ALPHA_Q10  13
/* 方差 EMA 的 alpha，Q20 表示：13/1024 * 2^10 = 13312 */
#define STAT_ALPHA_Q20  13312
/* 峰值门限参考衰减：τ ≈ 3s @50Hz → 7/1024 */
#define SHADOW_DECAY_Q10 7
/* 显示用峰值参考衰减：τ ≈ 30s @50Hz → 1/1024 */
#define SHADOW_SLOW_Q10  1

#define Q10(g_milli)    ((g_milli) * 1024 / MG_ONE_G)   /* mg → Q10 g */

/* ------------------------------------------------------------------ */

/** 整数平方根：uint64 → int32（逐位法，不碰 FPU） */
static int32_t isqrt64(uint64_t v)
{
    uint64_t rem = 0, root = 0;
    int i;
    for (i = 0; i < 32; i++) {
        root <<= 1;
        rem = (rem << 2) | (v >> 62);
        v <<= 2;
        if (root < rem) { rem -= root + 1; root += 2; }
    }
    return (int32_t)(root >> 1);
}

/**
 * 四舍五入的定点 EMA 步进。
 *
 * ⚠️ 注意：对负数乘积直接 `>>` 是向 -∞ 取整。用它做重力跟踪会引入
 *    持续向下的偏置（50Hz 下约 1.5mg/s），该偏置随后漏进带通，表现为
 *    缓慢斜坡，最终把检测器带偏。必须四舍五入。
 */
static inline int32_t ema_step(int32_t cur, int32_t target, int32_t alpha_q10)
{
    int64_t d = ((int64_t)target - cur) * alpha_q10;
    return cur + (int32_t)((d >= 0 ? (d + (1 << (Q_SIG - 1)))
                                   : (d - (1 << (Q_SIG - 1)))) >> Q_SIG);
}

/** 64 位 EMA，alpha 用 Q20（给 Q20 方差用） */
static inline int64_t ema_step64(int64_t cur, int64_t target, int64_t alpha_q20)
{
    int64_t d = (target - cur) * alpha_q20;
    return cur + ((d >= 0 ? (d + (1 << 19)) : (d - (1 << 19))) >> 20);
}

/** 单级 biquad，Direct Form I，Q15 系数，Q10 信号进出 */
static inline int32_t biquad(const int32_t c[5], int32_t *x1, int32_t *x2,
                             int32_t *y1, int32_t *y2, int32_t x)
{
    /* |c| 之和 < 2^17，|x| 正常 < 2^15，累加器 < 2^32；
     * 用 int64 以防瞬态冲击溢出。 */
    int64_t acc = (int64_t)c[0] * x   + (int64_t)c[1] * (*x1) + (int64_t)c[2] * (*x2)
                - (int64_t)c[3] * (*y1) - (int64_t)c[4] * (*y2);
    int32_t y = (int32_t)(acc >> Q_COEF);
    *x2 = *x1; *x1 = x;
    *y2 = *y1; *y1 = y;
    return y;
}

/** 步间隔的变异系数，放大 1000 倍返回（260 == CV 0.26） */
static int32_t rhythm_cv(const uint16_t *v, uint8_t n)
{
    uint32_t sum = 0, mean;
    uint64_t sq = 0;
    uint8_t i;
    if (n < 2) return 1000;
    for (i = 0; i < n; i++) sum += v[i];
    mean = sum / n;
    if (mean == 0) return 1000;
    for (i = 0; i < n; i++) {
        int32_t d = (int32_t)v[i] - (int32_t)mean;
        sq += (int64_t)d * d;
    }
    return (int32_t)(isqrt64(sq / n) * 1000u / mean);
}

/**
 * @brief 步长（cm）—— **固定值**，不再按身高推算
 *
 * ★ v0.5.0 改动（用户明确要求"默认一步 0.5m"）：
 *     走路 = `cfg.stride_walk_cm`（默认 50cm）
 *     跑步 = `cfg.stride_run_cm` （默认 90cm）
 *
 * ## 为什么废掉原来那套
 *
 * 原来用的是 `0.43 × 身高 + 步频微调`（跑步 `0.95 × 身高`）的通用经验式，
 * 实测**明显偏大** —— 180cm 身高算出 180cm 步长（等于身高），
 * 于是"距离"这个指标基本不可信（见交接文档 §10「步长估算偏大」）。
 *
 * 固定 50cm 对普通成年人走路是个偏保守但不会离谱的值，
 * 而且**所见即所得**：距离 = 步数 × 0.5m，用户一眼能验算。
 *
 * ⚠️ 以后要做个体标定（走一段量过的路反算真实步长），
 *    改 `cfg.stride_walk_cm` / `cfg.stride_run_cm` 即可，
 *    **不要再把"身高 × 系数"那套加回来**。
 */
static int32_t step_length_cm(const step_config_t *c, int32_t cadence_spm,
                              uint8_t activity)
{
    /* cadence 保留在签名里：以后要做"步长随步频变化"时可以从此处接 */
    (void)cadence_spm;

    if (activity == 2) {
        return c->stride_run_cm ? c->stride_run_cm : 90;
    }
    return c->stride_walk_cm ? c->stride_walk_cm : 50;
}

/* ==================================================================
 * 二、计步器实现
 * ================================================================== */

void step_counter_init(step_counter_t *sc, const step_config_t *cfg)
{
    if (!sc) return;
    memset(sc, 0, sizeof(*sc));
    if (cfg) {
        sc->cfg = *cfg;
    } else {
        /* ---------------- 起始参数，不是标定值 ----------------
         * 这些值用合成腕部运动模型扫参得到：对正常走路步频
         * （约 90–115 步/分）表现尚可，但**未经真实腕部数据验证**。
         * 必须上板按下述优先级实测调整：
         *   1. peak_thresh_mg / start_thresh_mg  你的腕表噪声地板
         *   2. crest_min_x100                    冲击型 vs 平滑型运动
         *   3. refractory_ms / refractory_pct     重复计步 vs 漏步
         *   4. cadence band + rhythm_cv_x1000     误计抑制
         * ---------------------------------------------------- */
        sc->cfg.sample_rate_hz     = 50;
        /*
         * ============================================================
         * 下面这组值是**用真实腕部数据**重新标定的（v0.4.1）
         * ============================================================
         *
         * 原来的值是用"合成腕部运动模型"扫出来的，作者当时就标注了
         * "上板前不要信"。第一份真实数据（50 步 / 74 步每分）一跑，
         * 输出 **0 步** —— 完全不能用。排查结论：
         *
         *   1) **波峰因数门（crest）是主要元凶**。
         *      它假设步态是"脉冲型冲击"，靠"峰值 ≥ 2σ"筛掉平滑摆动。
         *      但真实腕部在 74 步每分（1.23Hz，正好落在带通中心）时，
         *      信号是一个很强但**平滑的正弦**：
         *          |bp| 均值 120 Q10、p95 = 300、峰值 545
         *      信噪比其实很好，问题是 σ≈146 和 |bp| 均值同量级，
         *      于是 peak/σ 只有 1.5~3.7，**永远过不了 2.0 这一关**，
         *      444/475 个真实峰被拒。所以这里把它**关掉**（=0），
         *      改用"不应期 + 步频范围 + 节律一致性"来压误计。
         *
         *   2) **不应期必须加大到 400ms**。
         *      原始 250ms 太小：平滑信号配合贴着信号走的自适应门限，
         *      会在一步内反复穿越、报出多个峰。实测原始 92 个峰里，
         *      有 57 个间隔落在 100~400ms（一步双检），
         *      而真实步间隔在 700~1200ms。400ms 正好把双检挡掉、
         *      又留给快步（150 步每分 = 400ms 周期）。
         *
         *   3) **步频下限放宽到 50**：用户实测 74 步每分，
         *      原来的 60 下限对慢走太苛刻。
         *
         * ## 标定结果（tools/step_eval.py）
         *
         *   真实数据：50 步 -> **51 步（+2%）**
         *   合成交叉检查（60~120 步每分）：最差 **2.1%**（原来 100%）
         *
         * ⚠️ 只有一份真实数据，仍有过度拟合风险。
         *    再录几段不同步频/不同握姿的数据，用
         *    tools/step_search.py 多文件联合评估会更可靠。
         */
        sc->cfg.peak_thresh_mg     = 50;
        sc->cfg.start_thresh_mg    = 60;    /* 进入门限（>= 地板） */
        sc->cfg.end_thresh_mg      = 25;    /* 退出门限（滞回下沿） */
        sc->cfg.sigma_mul_x100     = 30;    /* thr = 包络 + 0.30σ */
        /*
         * 波峰因数门：0 = 关闭。
         * 真实腕部信号是平滑正弦而非脉冲，这个门会把真实步全拒掉。
         * 详见上面第 1 条。
         */
        sc->cfg.crest_min_x100     = 0;
        sc->cfg.peak_rel_pct       = 0;     /* 关闭（需真实数据定标） */
        /* 不应期：400ms 挡住"一步双检"，又留给 150 步每分的快步 */
        sc->cfg.refractory_pct     = 45;
        sc->cfg.refractory_ms      = 400;
        sc->cfg.strong_step_ms     = 0;     /* 关闭（与不应期重叠） */
        sc->cfg.strong_peak_mg     = 100000;
        sc->cfg.reversal_pct       = 35;    /* 回落 35% 确认峰 */
        sc->cfg.adaptive_reset_ms  = 1800;
        sc->cfg.rhythm_window      = 5;
        sc->cfg.rhythm_cv_x1000    = 700;   /* CV <= 0.70（真实数据标定，见上方说明） */
        /* commit_after 决定“起步要丢掉几步才确认节律”。取 4 时每段走路
         * 开头会丢约 5 步，多段短走（走走停停）会明显少计；取 3 兼顾
         * 误计抑制与漏计，实测 60s 走路误差约 -6%。 */
        sc->cfg.commit_after       = 3;
        sc->cfg.cadence_min_spm    = 50;   /* 放宽到 50：实测 74 步每分，原来的 60 对慢走太苛刻 */
        sc->cfg.cadence_max_spm    = 230;
        /* ---- 步长：固定值（v0.5.0，用户要求"默认一步 0.5m"）---- */
        sc->cfg.stride_walk_cm     = 50;
        sc->cfg.stride_run_cm      = 90;
        /* 体重只用于卡路里。设置里已删掉体重项，这里用固定常量；
         * 以后"卡路里"功能开发回来时，由 app 层把真实体重填进来。 */
        sc->cfg.user_weight_kg     = 65;
    }
    if (sc->cfg.rhythm_window < 2) sc->cfg.rhythm_window = 2;
    if (sc->cfg.rhythm_window > STEP_MAX_INTERVALS)
        sc->cfg.rhythm_window = STEP_MAX_INTERVALS;
    if (sc->cfg.commit_after > sc->cfg.rhythm_window)
        sc->cfg.commit_after = sc->cfg.rhythm_window;

    sc->grav_lp = Q10(MG_ONE_G);   /* 先假设合矢量 1g */
    sc->fresh = 1;
    IMU_UNUSED(TAG);
}

void step_counter_settle(step_counter_t *sc, uint32_t now_ms)
{
    sc->ready_ms = now_ms + 600;
}

/**
 * @brief 设置卡路里用的体重（v0.5.0）
 *
 * @note 原来叫 `step_counter_set_user(height, weight, is_male)`，
 *       身高用于步长、性别用于热量系数。v0.5.0 起：
 *         · 步长改为固定 50/90cm → 身高不再需要
 *         · 性别系数**本来就没被读过**（`sc_commit()` 里只按 activity 取 0.5/1.0）
 *           —— 属于"设了但没用"的死参数，一并删掉
 *       所以现在只剩体重一个参数（卡路里公式要用）。
 */
void step_counter_set_weight(step_counter_t *sc, uint16_t weight_kg)
{
    if (weight_kg > 20 && weight_kg < 300) {
        sc->cfg.user_weight_kg = weight_kg;
    }
}

/** 把攒下的 pending 步数落账（步数 / 距离 / 卡路里） */
static void sc_commit(step_counter_t *sc)
{
    uint32_t n = sc->pending;
    int32_t coef10, cals;
    if (n == 0 || sc->stride_den == 0) return;

    sc->st.steps += n;

    /* 距离：Σ(步长)/n，用 <<8 保留小数避免逐步截断累积误差 */
    sc->st.distance_cm += (uint32_t)(sc->stride_num / sc->stride_den);

    /* 卡路里：kcal = 步长cm × 步数 × 体重kg × 系数 / 1e5
     *   系数 0.5（走路）/ 1.0（跑步），单位 kcal/(kg·km)
     * 出处：ACSM 代谢方程在平地上化简后的近似
     *       （走路 ≈ 0.5 kcal/kg/km，跑步 ≈ 1.0 kcal/kg/km）；
     *       等价于常见的 0.0005 kcal/(kg·m) 经验式。
     * ⚠️ 这是**活动净消耗**，不含基础代谢。 */
    coef10 = (sc->st.activity == 2) ? 10 : 5;
    cals = (int32_t)(((int64_t)sc->stride_num * sc->cfg.user_weight_kg * coef10)
                     / ((int64_t)sc->stride_den * 10000));
    sc->st.calories_mkcal += (uint32_t)cals;

    sc->pending = 0;
    sc->stride_num = 0;
    sc->stride_den = 0;
    sc->fresh = 0;
}

static void sc_add_interval(step_counter_t *sc, uint32_t dt_ms)
{
    int32_t inst;
    if (dt_ms == 0 || dt_ms > 5000) {
        sc->n_intervals = 0; sc->head = 0; return;
    }
    sc->intervals[sc->head] = (uint16_t)dt_ms;
    sc->head = (uint8_t)((sc->head + 1) % sc->cfg.rhythm_window);
    if (sc->n_intervals < sc->cfg.rhythm_window) sc->n_intervals++;

    inst = (int32_t)(60000u / dt_ms);
    if (sc->cadence_lp == 0) sc->cadence_lp = inst;
    else sc->cadence_lp = (sc->cadence_lp * 3 + inst) / 4;   /* EMA(0.25) */
}

/** 处理一个候选峰；返回 true 表示被接受为一步 */
static bool sc_accept_peak(step_counter_t *sc, int32_t peak_q10,
                           uint32_t peak_ms, int32_t sigma_q10)
{
    uint32_t gap = sc->last_step_ms ? (peak_ms - sc->last_step_ms) : 0;
    int32_t cadence, amp_floor;

    /* ---- 门 A：波峰因数。步态冲击是脉冲型，平滑摆动不是 ---- */
    if (peak_q10 < (sigma_q10 * sc->cfg.crest_min_x100) / 100) {
        ESP_LOGD(TAG, "rej crest peak=%d sig=%d", peak_q10, sigma_q10);
        return false;
    }

    /* ---- 门 B：相对近期峰值最大值的幅度门 ---- */
    {
        int32_t ref = sc->peak_shadow > sc->peak_shadow_disp
                      ? sc->peak_shadow : sc->peak_shadow_disp;
        amp_floor = Q10(sc->cfg.peak_thresh_mg);
        if (ref > 0 && sc->cfg.peak_rel_pct > 0) {
            int32_t rel = (ref * sc->cfg.peak_rel_pct) / 100;
            if (rel > amp_floor) amp_floor = rel;
        }
    }
    if (peak_q10 < amp_floor) {
        ESP_LOGD(TAG, "rej amp peak=%d floor=%d", peak_q10, amp_floor);
        return false;
    }

    /* ---- 门 C：学习到的不应期 ---- */
    if (gap && sc->cadence_period_ms) {
        uint32_t min_gap = (uint32_t)((sc->cadence_period_ms *
                                       sc->cfg.refractory_pct) / 100u);
        if (min_gap < sc->cfg.refractory_ms) min_gap = sc->cfg.refractory_ms;
        if (gap < min_gap) {
            ESP_LOGD(TAG, "rej near gap=%u < %u", gap, min_gap);
            return false;
        }
    } else if (gap && gap < sc->cfg.refractory_ms) {
        return false;
    }

    sc->last_step_ms = peak_ms;
    if (gap) sc_add_interval(sc, gap);

    /* 峰值参考跟踪：快攻慢放 */
    if (peak_q10 > sc->peak_shadow) sc->peak_shadow = peak_q10;
    else sc->peak_shadow = ema_step(sc->peak_shadow, peak_q10, SHADOW_DECAY_Q10);
    if (peak_q10 > sc->peak_shadow_disp) sc->peak_shadow_disp = peak_q10;
    else sc->peak_shadow_disp = ema_step(sc->peak_shadow_disp, peak_q10,
                                         SHADOW_SLOW_Q10);

    cadence = sc->cadence_lp ? sc->cadence_lp : 100;

    if (cadence < sc->cfg.cadence_min_spm || cadence > sc->cfg.cadence_max_spm) {
        /* 步频不合理：清掉节律历史，本步不计 */
        sc->n_intervals = 0;
        sc->head = 0;
        sc->cadence_lp = 0;
        sc->cadence_period_ms = 0;
        sc->pending = 0;
        sc->stride_num = 0;
        sc->stride_den = 0;
        return false;
    }

    sc->cadence_period_ms = (uint16_t)(60000 / cadence);

    sc->pending++;
    sc->st.stride_cm = step_length_cm(&sc->cfg, cadence,
                                      (cadence >= 140) ? 2 : 1);
    sc->stride_num += sc->st.stride_cm * 256;
    sc->stride_den += 256;

    if (sc->n_intervals >= sc->cfg.commit_after) {
        int32_t cv = rhythm_cv(sc->intervals, sc->n_intervals);
        if (cv <= sc->cfg.rhythm_cv_x1000) {
            sc->st.cadence_spm = cadence;
            sc->st.activity = (cadence >= 140) ? 2 : 1;
            sc_commit(sc);
        } else {
            ESP_LOGD(TAG, "rej rhythm cv=%d", cv);
        }
    }
    return true;
}

void step_counter_update(step_counter_t *sc, const int32_t accel_mg[3],
                         uint32_t now_ms)
{
    int64_t mag_q10;
    int32_t ac, bp, abp, thr_q10, sigma_q10;

    /* ---------- 1. 合矢量模长（Q10 g） ---------- */
    {
        uint64_t s = (uint64_t)((int64_t)accel_mg[0] * accel_mg[0]
                              + (int64_t)accel_mg[1] * accel_mg[1]
                              + (int64_t)accel_mg[2] * accel_mg[2]);
        mag_q10 = ((int64_t)isqrt64(s) << Q_SIG) / MG_ONE_G;
    }

    /* ---------- 2. 去重力 ---------- */
    sc->grav_lp = ema_step(sc->grav_lp, (int32_t)mag_q10, GRAV_ALPHA_Q10);
    ac = (int32_t)mag_q10 - sc->grav_lp;

    /* ---------- 3. 带通 0.5–3.5Hz ---------- */
    bp = biquad(k_bp[0], &sc->bp_x1[0], &sc->bp_x2[0],
                &sc->bp_y1[0], &sc->bp_y2[0], ac);
    bp = biquad(k_bp[1], &sc->bp_x1[1], &sc->bp_x2[1],
                &sc->bp_y1[1], &sc->bp_y2[1], bp);
    abp = bp < 0 ? -bp : bp;

    /* ---------- 4. 统计量 EMA ---------- */
    if (sc->adapt_n < 0xFFFFFFFFu) sc->adapt_n++;
    sc->bp_mean  = ema_step(sc->bp_mean, bp, STAT_ALPHA_Q10);
    sc->bp_abs_dc = ema_step(sc->bp_abs_dc, abp, STAT_ALPHA_Q10);

    /* σ² = Var(bp)。两个精度陷阱：
     *  1) 用 E[x²]-E[x]² 计算在 Q10 下会灾难性相消；
     *  2) 平方偏差用 Q10 存会把典型值（100mg 偏差 → 0.0095）舍成 0。
     * 因此方差 EMA 用 Q20 保存，只在开方时降位。 */
    {
        int32_t dev = bp - sc->bp_mean;
        int64_t sq = (int64_t)dev * dev;              /* Q20 g² */
        sc->bp_var_sq = ema_step64(sc->bp_var_sq, sq, STAT_ALPHA_Q20);
        sigma_q10 = sc->bp_var_sq > 0
                    ? isqrt64((uint64_t)sc->bp_var_sq) : 0;
    }

    /* ---------- 5. 启动建立 ---------- */
    /* 使能瞬间输入是阶跃，带通会振铃约 0.5s；吞掉这段，否则会误计。
     * 等输入安静下来再从那一刻起算 600ms。 */
    if (now_ms < sc->ready_ms) return;
    if (sc->ready_ms == 0) {
        if (abp < Q10(sc->cfg.start_thresh_mg)) sc->ready_ms = now_ms + 600;
        return;
    }

    /* ---------- 6. 自适应门限 ---------- */
    if (!sc->active || sc->adapt_n < 30) {
        thr_q10 = Q10(sc->fresh ? sc->cfg.start_thresh_mg
                                : sc->cfg.peak_thresh_mg);
    } else {
        int32_t thr_mg = sc->bp_abs_dc * MG_ONE_G / 1024
                       + (sigma_q10 * sc->cfg.sigma_mul_x100 / 100)
                         * MG_ONE_G / 1024;
        int32_t floor_mg = sc->cfg.peak_thresh_mg;
        if (!sc->active && sc->cfg.start_thresh_mg > floor_mg)
            floor_mg = sc->cfg.start_thresh_mg;
        if (thr_mg < floor_mg) thr_mg = floor_mg;
        thr_q10 = Q10(thr_mg);
    }

    /* ---------- 7. 包络滞回门 ---------- */
    {
        int32_t on = Q10(sc->cfg.start_thresh_mg);
        if (!sc->active) {
            if (abp > on) {
                if (++sc->above_cnt >= 3) {
                    sc->active = 1;
                    sc->above_cnt = 0;
                    sc->adapt_n = 0;
                }
            } else {
                sc->above_cnt = 0;
            }
        } else {
            if (abp > Q10(sc->cfg.end_thresh_mg)) {
                sc->last_motion_ms = now_ms;
            } else if ((now_ms - sc->last_motion_ms) > sc->cfg.adaptive_reset_ms) {
                /* 静默超时 → 回静止态，清掉未确认的步 */
                sc->active = 0;
                sc->fresh = 1;
                sc->st.steps_pending = 0;
                sc->peak_shadow = 0;
                sc->st.cadence_spm = 0;
                sc->st.activity = 0;
                sc->adapt_n = 0;
                sc->n_intervals = 0;
                sc->head = 0;
                sc->cadence_lp = 0;
                sc->pending = 0;
                sc->stride_num = 0;
                sc->stride_den = 0;
                sc->rising = 0;
                sc->peak_cand = 0;
                return;
            }
        }
    }
    if (!sc->active) {
        sc->rising = 0;
        sc->peak_cand = 0;
        sc->st.cadence_spm = 0;
        sc->st.activity = 0;
        sc->thr_dbg = thr_q10;
        sc->ac_dbg = ac;
        sc->bp_dbg = bp;
        return;
    }

    /* ---------- 8. 峰值跟踪 ---------- */
    /* 峰的两个确认条件：从运行最大值回落 reversal_pct%，或超时。
     * 用“回落”而不是“跌破门限”，是为了让连续两步融合成一个骑跨信号
     * （慢跑/跑步常见）时仍能分辨出峰。 */
    if (abp > thr_q10) {
        if (!sc->rising) {
            sc->rising = 1;
            sc->peak_cand = abp;
            sc->peak_cand_ms = now_ms;
        } else if (abp > sc->peak_cand) {
            sc->peak_cand = abp;
            sc->peak_cand_ms = now_ms;
        }
    }

    if (sc->rising) {
        bool close = false;
        if (abp <= thr_q10) {
            close = true;
        } else if (sc->peak_cand > 0 &&
                   abp < sc->peak_cand -
                         (sc->peak_cand * sc->cfg.reversal_pct) / 100) {
            close = true;
        } else if ((now_ms - sc->peak_cand_ms) > sc->cfg.peak_timeout_ms) {
            close = true;
        }

        if (close) {
            int32_t peak = sc->peak_cand;
            uint32_t peak_ms = sc->peak_cand_ms;
            uint32_t gap = sc->last_step_ms ? (peak_ms - sc->last_step_ms)
                                            : 0xFFFFFFFFu;

            /* 仍在门限之上 → 骑跨基线上的“双峰”，直接接到下一个候选 */
            if (abp > thr_q10) {
                sc->peak_deferred = peak;
                sc->peak_cand = abp;
                sc->peak_cand_ms = now_ms;
            } else {
                sc->rising = 0;
                sc->peak_cand = 0;
            }

            if (gap >= sc->cfg.refractory_ms &&
                peak >= Q10(sc->cfg.peak_thresh_mg)) {
                if (sc->cfg.strong_step_ms == 0 ||
                    gap >= sc->cfg.strong_step_ms ||
                    peak >= Q10(sc->cfg.strong_peak_mg)) {
                    if (sc_accept_peak(sc, peak, peak_ms, sigma_q10))
                        sc->peak_deferred = 0;
                }
            } else if (sc_accept_peak(sc, peak, peak_ms, sigma_q10)) {
                sc->peak_deferred = 0;
            }

            /* 重新武装，让被推迟的那个峰去闭合下一个峰 */
            if (sc->peak_deferred > 0 && !sc->rising) {
                sc->rising = 1;
                sc->peak_cand = sc->peak_deferred;
                sc->peak_cand_ms = peak_ms;
            }
        }
    }

    sc->thr_dbg = thr_q10;
    sc->ac_dbg = ac;
    sc->bp_dbg = bp;
    sc->st.steps_pending = sc->pending;
    sc->st.cadence_spm = sc->cadence_lp;
}

void step_counter_get(const step_counter_t *sc, step_status_t *out)
{
    if (sc && out) *out = sc->st;
}

/* ==================================================================
 * 三、I2C / 寄存器访问
 * ================================================================== */
#ifdef IMU_HOST_TEST
/* Host build: the register half is not compiled. */
#else
static i2c_master_dev_handle_t s_dev;
static bool    s_ready = false;
static qmi8658_afs_t s_afs = QMI8658_AFS_8G;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    return board_i2c_write_reg(s_dev, reg, &val, 1);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return board_i2c_read_reg(s_dev, reg, buf, len);
}

/** 读-改-写单个寄存器（仅用于 CTRL5 这类无竞争寄存器） */
static esp_err_t reg_update(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t v;
    esp_err_t e = reg_read(reg, &v, 1);
    if (e != ESP_OK) return e;
    v = (uint8_t)((v & ~mask) | (val & mask));
    return reg_write(reg, v);
}

esp_err_t qmi8658_reset(void)
{
    uint8_t v = 0;
    int i;

    /* Table 27：向 RESET(0x60) 写 0xB0 触发软复位。
     * 手册 §3.3：“复位过程最长 15ms”，成功后 0x4D 读回 0x80。
     * 注：手册 §3.3 正文里写成“写 0x0B”，与 Table 27 的 0xB0 冲突 ——
     *     以寄存器表 0xB0 为准（SensorLib / 各家驱动也都用 0xB0）。 */
    esp_err_t e = reg_write(QMI8658_REG_RESET, QMI8658_RESET_MAGIC);
    if (e != ESP_OK) return e;

    for (i = 0; i < 15; i++) {
        vTaskDelay(pdMS_TO_TICKS(2));           /* 30ms 预算 > 15ms */
        if (reg_read(QMI8658_REG_RST_RESULT, &v, 1) == ESP_OK &&
            v == QMI8658_RST_RESULT_OK) break;
    }
    if (v != QMI8658_RST_RESULT_OK) {
        ESP_LOGE(TAG, "复位失败，0x4D = 0x%02X", v);
        return ESP_ERR_TIMEOUT;
    }

    /* CTRL1：ADDR_AI=1 开地址自增（0x35..0x40 的 12 字节突发读必需），
     * BE=0 → 小端，SIM=0 → I2C。手册默认是 0，必须显式设置，
     * 否则多字节读会一直读到同一个寄存器。 */
    return reg_write(QMI8658_REG_CTRL1, QMI8658_CTRL1_ADDR_AI);
}

esp_err_t qmi8658_config_accel(qmi8658_afs_t fs, qmi8658_aodr_t odr,
                               bool lpf_en, qmi8658_lpf_t lpf)
{
    esp_err_t e;

    /* CTRL2 必须**一次整字节写完**（aST(7)=0 | aFS(6:4) | aODR(3:0)）。
     * 分两次读-改-写中间会经过一个非法/中间配置。 */
    e = reg_write(QMI8658_REG_CTRL2,
                  (uint8_t)(((uint8_t)fs << 4) | ((uint8_t)odr & 0x0F)));
    if (e != ESP_OK) return e;
    s_afs = fs;

    /* CTRL5：aLPF_EN(bit0)，aLPF_MODE(bits2:1) */
    return reg_update(QMI8658_REG_CTRL5, 0x07,
                      (uint8_t)((lpf_en ? QMI8658_CTRL5_aLPF_EN : 0) |
                                ((uint8_t)lpf << QMI8658_CTRL5_aLPF_MODE_S)));
}

esp_err_t qmi8658_config_gyro(qmi8658_gfs_t fs, qmi8658_godr_t odr,
                              bool lpf_en, qmi8658_lpf_t lpf)
{
    esp_err_t e;
    /* CTRL3：gST(7)=0 | gFS(6:4) | gODR(3:0) */
    e = reg_write(QMI8658_REG_CTRL3,
                  (uint8_t)(((uint8_t)fs << 4) | ((uint8_t)odr & 0x0F)));
    if (e != ESP_OK) return e;
    /* CTRL5：gLPF_EN(bit4)，gLPF_MODE(bits6:5) */
    return reg_update(QMI8658_REG_CTRL5, 0x70,
                      (uint8_t)((lpf_en ? QMI8658_CTRL5_gLPF_EN : 0) |
                                ((uint8_t)lpf << QMI8658_CTRL5_gLPF_MODE_S)));
}

esp_err_t qmi8658_enable_accel(bool en)
{
    return reg_update(QMI8658_REG_CTRL7, QMI8658_CTRL7_aEN,
                      en ? QMI8658_CTRL7_aEN : 0);
}

esp_err_t qmi8658_enable_gyro(bool en)
{
    return reg_update(QMI8658_REG_CTRL7, QMI8658_CTRL7_gEN,
                      en ? QMI8658_CTRL7_gEN : 0);
}

esp_err_t qmi8658_read_status0(uint8_t *status0)
{
    return reg_read(QMI8658_REG_STATUS0, status0, 1);
}

/**
 * @brief 直接读一个寄存器（调试/自检用，见头文件说明）
 */
esp_err_t qmi8658_read_reg_raw(uint8_t reg, uint8_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return reg_read(reg, out, 1);
}

esp_err_t qmi8658_read(qmi8658_sample_t *out)
{
    uint8_t b[12];
    esp_err_t e;
    if (!out || !s_ready) return ESP_ERR_INVALID_STATE;

    /* 0x35..0x40 连续 12 字节：AX_L AX_H AY_L AY_H AZ_L AZ_H
     *                          GX_L GX_H GY_L GY_H GZ_L GZ_H
     * 小端（CTRL1.BE = 0），所以先读到的是低字节。 */
    e = reg_read(QMI8658_REG_AX_L, b, sizeof(b));
    if (e != ESP_OK) return e;

    out->ax = (int16_t)((b[1]  << 8) | b[0]);
    out->ay = (int16_t)((b[3]  << 8) | b[2]);
    out->az = (int16_t)((b[5]  << 8) | b[4]);
    out->gx = (int16_t)((b[7]  << 8) | b[6]);
    out->gy = (int16_t)((b[9]  << 8) | b[8]);
    out->gz = (int16_t)((b[11] << 8) | b[10]);
    return ESP_OK;
}

esp_err_t qmi8658_init(void)
{
    uint8_t who = 0;
    esp_err_t e;

    if (s_ready) return ESP_OK;

    e = board_init();
    if (e != ESP_OK) return e;
    if (!board_i2c_bus()) {
        ESP_LOGE(TAG, "共享 I2C 总线未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    e = board_i2c_add_device(BOARD_IMU_I2C_ADDR, BOARD_I2C_FREQ_HZ, &s_dev);
    if (e != ESP_OK) return e;

    /* WHO_AM_I(0x00) == 0x05 表示是 QST 的传感器（Table 20） */
    e = reg_read(QMI8658_REG_WHO_AM_I, &who, 1);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "读 WHO_AM_I 失败: %s", esp_err_to_name(e));
        return e;
    }
    if (who != 0x05) {
        ESP_LOGE(TAG, "WHO_AM_I = 0x%02X，期望 0x05", who);
        return ESP_ERR_NOT_FOUND;
    }

    e = qmi8658_reset();
    if (e != ESP_OK) return e;

    /* CTRL8 bit7 = 1：用 STATUSINT.bit7 作为 CTRL9 命令握手。
     * 本项目不用 CTRL9 命令（片内计步器/Tap），但留着也无害。 */
    e = reg_write(QMI8658_REG_CTRL8, 0x80);
    if (e != ESP_OK) return e;

    s_ready = true;
    ESP_LOGI(TAG, "QMI8658C 就绪（addr=0x%02X, WHO_AM_I=0x%02X）",
             BOARD_IMU_I2C_ADDR, who);
    return ESP_OK;
}
#endif /* IMU_HOST_TEST */
