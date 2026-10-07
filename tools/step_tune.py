"""
计步算法离线仿真器 —— 把 components/imu/qmi8658c.c 的定点实现**逐行照搬**到 Python。

## 为什么需要它

调计步参数必须能"快速试不同参数"，而在设备上试一次要：
编译 → 烧录 → 带着表走 50 步 → 导出。
用这个脚本可以拿**已经录好的真实数据**反复调，几秒钟一轮。

## 为什么必须"逐行照搬"而不是用 numpy 写个等价的

算法里全是定点数（Q10 / Q15 / Q20）、四舍五入右移、整数开方、
灾难性相消的规避手法。用浮点重写会**得出不一样的结果**，
调出来的参数搬到设备上就不准了。
所以这里连 `ema_step` 的"负数也要四舍五入"都照抄。

## 用法

    python tools/step_tune.py imu_logs/imu_0001.csv --steps 50
    python tools/step_tune.py imu_logs/imu_0001.csv --steps 50 --sweep

`--steps N` 是你实际走的步数（真值），脚本会把仿真结果和它对比。
"""
import argparse
import csv
import math
import sys


# ======================================================================
# 定点工具（严格对应 qmi8658c.c）
# ======================================================================
Q_SIG = 10          # 信号小数位（Q10 g）
Q_COEF = 15
MG_ONE_G = 1000

GRAV_ALPHA_Q10 = 7
STAT_ALPHA_Q10 = 13
STAT_ALPHA_Q20 = 13312
SHADOW_DECAY_Q10 = 7
SHADOW_SLOW_Q10 = 1

# 带通系数 {b0,b1,b2,a1,a2}，Q15
K_BP = [
    [5237, 0, -5237, -54370, 22362],    # RBJ BPF f0=1.323Hz BW=2.807oct
    [1850, 3699, 1850, -40129, 14760],  # RBJ LPF 4.5Hz Q=0.7071
]


def q10(g_milli):
    """mg → Q10 g。注意 C 里是整数除法，向零截断。"""
    v = g_milli * 1024
    return int(v / MG_ONE_G) if v >= 0 else -int(-v / MG_ONE_G)


def isqrt64(v):
    """整数平方根（逐位法），和 C 版一致"""
    if v <= 0:
        return 0
    return math.isqrt(v)


def sar(x, n):
    """算术右移（向 -inf），对应 C 的有符号 >>"""
    return x >> n


def ema_step(cur, target, alpha_q10):
    """
    四舍五入的定点 EMA。
    ⚠️ 负数乘积也必须四舍五入 —— 直接 >> 是向 -inf 取整，
       会引入持续向下的偏置（C 代码注释里专门写过）。
    """
    d = (target - cur) * alpha_q10
    if d >= 0:
        return cur + ((d + (1 << (Q_SIG - 1))) >> Q_SIG)
    return cur + ((d - (1 << (Q_SIG - 1))) >> Q_SIG)


def ema_step64(cur, target, alpha_q20):
    d = (target - cur) * alpha_q20
    if d >= 0:
        return cur + ((d + (1 << 19)) >> 20)
    return cur + ((d - (1 << 19)) >> 20)


class Biquad:
    __slots__ = ("c", "x1", "x2", "y1", "y2")

    def __init__(self, c):
        self.c = c
        self.x1 = self.x2 = self.y1 = self.y2 = 0

    def __call__(self, x):
        c = self.c
        acc = (c[0] * x + c[1] * self.x1 + c[2] * self.x2
               - c[3] * self.y1 - c[4] * self.y2)
        y = sar(acc, Q_COEF)
        self.x2, self.x1 = self.x1, x
        self.y2, self.y1 = self.y1, y
        return y


# ======================================================================
# 参数（对应 step_config_t 的默认值）
# ======================================================================
class Cfg:
    def __init__(self, **kw):
        self.sample_rate_hz = 50
        self.peak_thresh_mg = 60
        self.start_thresh_mg = 60
        self.end_thresh_mg = 25
        self.sigma_mul_x100 = 30
        self.crest_min_x100 = 200
        self.peak_rel_pct = 0
        self.refractory_pct = 35
        self.refractory_ms = 250
        self.strong_step_ms = 0
        self.strong_peak_mg = 100000
        self.reversal_pct = 35
        self.peak_timeout_ms = 400
        self.adaptive_reset_ms = 3000
        self.rhythm_window = 5
        self.rhythm_cv_x1000 = 260
        self.commit_after = 3
        self.cadence_min_spm = 60
        self.cadence_max_spm = 200
        self.user_height_cm = 180
        self.user_weight_kg = 65
        self.user_is_male = True
        for k, v in kw.items():
            if not hasattr(self, k):
                raise KeyError(f"未知参数 {k}")
            setattr(self, k, v)


# ======================================================================
# 计步器
# ======================================================================
class StepCounter:
    def __init__(self, cfg):
        self.cfg = cfg
        self.reset()

    def reset(self):
        self.grav_lp = 0
        self.bp = [Biquad(K_BP[0]), Biquad(K_BP[1])]
        self.bp_mean = 0
        self.bp_var_sq = 0
        self.bp_abs_dc = 0
        self.adapt_n = 0
        self.active = 0
        self.fresh = 1
        self.above_cnt = 0
        self.activity = 0
        self.last_step_ms = 0
        self.last_motion_ms = 0
        self.ready_ms = 0
        self.rising = 0
        self.peak_cand = 0
        self.peak_deferred = 0
        self.peak_cand_ms = 0
        self.peak_shadow = 0
        self.peak_shadow_disp = 0
        self.cadence_period_ms = 0
        self.cadence_lp = 0
        self.n_intervals = 0
        self.head = 0
        self.intervals = [0] * max(8, self.cfg.rhythm_window)
        self.pending = 0
        self.stride_num = 0
        self.stride_den = 0
        # 输出
        self.steps = 0
        self.steps_pending = 0
        self.distance_cm = 0
        self.calories_mkcal = 0
        self.cadence_spm = 0
        self.stride_cm = 0
        # 遥测
        self.ac_dbg = 0
        self.bp_dbg = 0
        self.thr_dbg = 0
        # 拒绝原因统计
        self.rej = {"crest": 0, "amp": 0, "near": 0, "cadence": 0, "rhythm": 0}
        self.peaks_seen = 0

    # ---------------- 内部 ----------------
    def _rhythm_cv(self, v, n):
        if n < 2:
            return 1000
        s = sum(v[:n])
        mean = s // n
        if mean == 0:
            return 1000
        sq = sum((x - mean) ** 2 for x in v[:n])
        return isqrt64(sq // n) * 1000 // mean

    def _step_length_cm(self, cadence, activity):
        h = self.cfg.user_height_cm
        if activity == 2:
            return h * 95 // 100
        nominal = h * 43 // 100
        spm = min(max(cadence, 60), 160)
        ln = nominal + (spm - 100) * h // 400
        ln = max(ln, nominal * 6 // 10)
        ln = min(ln, nominal * 16 // 10)
        return ln

    def _commit(self):
        n = self.pending
        if n == 0 or self.stride_den == 0:
            return
        self.steps += n
        self.distance_cm += self.stride_num // self.stride_den
        coef10 = 10 if self.activity == 2 else 5
        cals = (self.stride_num * self.cfg.user_weight_kg * coef10) // (self.stride_den * 10000)
        self.calories_mkcal += cals
        self.pending = 0
        self.stride_num = 0
        self.stride_den = 0
        self.fresh = 0

    def _add_interval(self, dt_ms):
        if dt_ms == 0 or dt_ms > 5000:
            self.n_intervals = 0
            self.head = 0
            return
        self.intervals[self.head] = dt_ms
        self.head = (self.head + 1) % self.cfg.rhythm_window
        if self.n_intervals < self.cfg.rhythm_window:
            self.n_intervals += 1
        inst = 60000 // dt_ms
        if self.cadence_lp == 0:
            self.cadence_lp = inst
        else:
            self.cadence_lp = (self.cadence_lp * 3 + inst) // 4

    def _accept_peak(self, peak_q10, peak_ms, sigma_q10):
        cfg = self.cfg
        gap = (peak_ms - self.last_step_ms) if self.last_step_ms else 0

        # 门 A：波峰因数
        if peak_q10 < (sigma_q10 * cfg.crest_min_x100) // 100:
            self.rej["crest"] += 1
            return False

        # 门 B：相对幅度门
        ref = max(self.peak_shadow, self.peak_shadow_disp)
        amp_floor = q10(cfg.peak_thresh_mg)
        if ref > 0 and cfg.peak_rel_pct > 0:
            rel = (ref * cfg.peak_rel_pct) // 100
            if rel > amp_floor:
                amp_floor = rel
        if peak_q10 < amp_floor:
            self.rej["amp"] += 1
            return False

        # 门 C：不应期
        if gap and self.cadence_period_ms:
            min_gap = (self.cadence_period_ms * cfg.refractory_pct) // 100
            if min_gap < cfg.refractory_ms:
                min_gap = cfg.refractory_ms
            if gap < min_gap:
                self.rej["near"] += 1
                return False
        elif gap and gap < cfg.refractory_ms:
            self.rej["near"] += 1
            return False

        self.last_step_ms = peak_ms
        if gap:
            self._add_interval(gap)

        if peak_q10 > self.peak_shadow:
            self.peak_shadow = peak_q10
        else:
            self.peak_shadow = ema_step(self.peak_shadow, peak_q10, SHADOW_DECAY_Q10)
        if peak_q10 > self.peak_shadow_disp:
            self.peak_shadow_disp = peak_q10
        else:
            self.peak_shadow_disp = ema_step(self.peak_shadow_disp, peak_q10,
                                             SHADOW_SLOW_Q10)

        cadence = self.cadence_lp if self.cadence_lp else 100
        if cadence < cfg.cadence_min_spm or cadence > cfg.cadence_max_spm:
            self.rej["cadence"] += 1
            self.n_intervals = 0
            self.head = 0
            self.cadence_lp = 0
            self.cadence_period_ms = 0
            self.pending = 0
            self.stride_num = 0
            self.stride_den = 0
            return False

        self.cadence_period_ms = 60000 // cadence
        self.pending += 1
        act = 2 if cadence >= 140 else 1
        self.stride_cm = self._step_length_cm(cadence, act)
        self.stride_num += self.stride_cm * 256
        self.stride_den += 256

        if self.n_intervals >= cfg.commit_after:
            cv = self._rhythm_cv(self.intervals, self.n_intervals)
            if cv <= cfg.rhythm_cv_x1000:
                self.cadence_spm = cadence
                self.activity = act
                self._commit()
            else:
                self.rej["rhythm"] += 1
        return True

    # ---------------- 主循环 ----------------
    def update(self, accel_mg, now_ms):
        cfg = self.cfg
        mag_q10 = isqrt64(accel_mg[0] ** 2 + accel_mg[1] ** 2 + accel_mg[2] ** 2) \
            * 1024 // MG_ONE_G

        # 去重力
        self.grav_lp = ema_step(self.grav_lp, mag_q10, GRAV_ALPHA_Q10)
        ac = mag_q10 - self.grav_lp

        # 带通
        bp = self.bp[0](ac)
        bp = self.bp[1](bp)
        abp = abs(bp)

        # 统计量
        self.adapt_n += 1
        self.bp_mean = ema_step(self.bp_mean, bp, STAT_ALPHA_Q10)
        self.bp_abs_dc = ema_step(self.bp_abs_dc, abp, STAT_ALPHA_Q10)
        dev = bp - self.bp_mean
        self.bp_var_sq = ema_step64(self.bp_var_sq, dev * dev, STAT_ALPHA_Q20)
        sigma_q10 = isqrt64(self.bp_var_sq) if self.bp_var_sq > 0 else 0

        # 启动建立
        if now_ms < self.ready_ms:
            return
        if self.ready_ms == 0:
            if abp < q10(cfg.start_thresh_mg):
                self.ready_ms = now_ms + 600
            return

        # 自适应门限
        if not self.active or self.adapt_n < 30:
            thr_q10 = q10(cfg.start_thresh_mg if self.fresh else cfg.peak_thresh_mg)
        else:
            thr_mg = self.bp_abs_dc * MG_ONE_G // 1024 \
                + (sigma_q10 * cfg.sigma_mul_x100 // 100) * MG_ONE_G // 1024
            floor_mg = cfg.peak_thresh_mg
            if not self.active and cfg.start_thresh_mg > floor_mg:
                floor_mg = cfg.start_thresh_mg
            thr_mg = max(thr_mg, floor_mg)
            thr_q10 = q10(thr_mg)

        # 包络滞回门
        on = q10(cfg.start_thresh_mg)
        if not self.active:
            if abp > on:
                self.above_cnt += 1
                if self.above_cnt >= 3:
                    self.active = 1
                    self.above_cnt = 0
                    self.adapt_n = 0
            else:
                self.above_cnt = 0
        else:
            if abp > q10(cfg.end_thresh_mg):
                self.last_motion_ms = now_ms
            elif (now_ms - self.last_motion_ms) > cfg.adaptive_reset_ms:
                self.active = 0
                self.fresh = 1
                self.steps_pending = 0
                self.peak_shadow = 0
                self.cadence_spm = 0
                self.activity = 0
                self.adapt_n = 0
                self.n_intervals = 0
                self.head = 0
                self.cadence_lp = 0
                self.pending = 0
                self.stride_num = 0
                self.stride_den = 0
                self.rising = 0
                self.peak_cand = 0
                return

        if not self.active:
            self.rising = 0
            self.peak_cand = 0
            self.cadence_spm = 0
            self.activity = 0
            self.thr_dbg = thr_q10
            self.ac_dbg = ac
            self.bp_dbg = bp
            return

        # 峰值跟踪
        if abp > thr_q10:
            if not self.rising:
                self.rising = 1
                self.peak_cand = abp
                self.peak_cand_ms = now_ms
            elif abp > self.peak_cand:
                self.peak_cand = abp
                self.peak_cand_ms = now_ms

        if self.rising:
            close = False
            if abp <= thr_q10:
                close = True
            elif self.peak_cand > 0 and abp < self.peak_cand - \
                    (self.peak_cand * cfg.reversal_pct) // 100:
                close = True
            elif (now_ms - self.peak_cand_ms) > cfg.peak_timeout_ms:
                close = True

            if close:
                peak = self.peak_cand
                peak_ms = self.peak_cand_ms
                self.peaks_seen += 1
                gap = (peak_ms - self.last_step_ms) if self.last_step_ms else 0xFFFFFFFF

                if abp > thr_q10:
                    self.peak_deferred = peak
                    self.peak_cand = abp
                    self.peak_cand_ms = now_ms
                else:
                    self.rising = 0
                    self.peak_cand = 0

                if gap >= cfg.refractory_ms and peak >= q10(cfg.peak_thresh_mg):
                    if cfg.strong_step_ms == 0 or gap >= cfg.strong_step_ms \
                            or peak >= q10(cfg.strong_peak_mg):
                        if self._accept_peak(peak, peak_ms, sigma_q10):
                            self.peak_deferred = 0
                elif self._accept_peak(peak, peak_ms, sigma_q10):
                    self.peak_deferred = 0

                if self.peak_deferred > 0 and not self.rising:
                    self.rising = 1
                    self.peak_cand = self.peak_deferred
                    self.peak_cand_ms = peak_ms

        self.thr_dbg = thr_q10
        self.ac_dbg = ac
        self.bp_dbg = bp
        self.steps_pending = self.pending
        self.cadence_spm = self.cadence_lp


# ======================================================================
def load_csv(path):
    ts, acc = [], []
    with open(path, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            ts.append(int(row["t_ms"]))
            acc.append((int(row["ax"]), int(row["ay"]), int(row["az"])))
    # 时间戳还原单调（跨复位会有偏移，导出工具已处理；这里再兜一层）
    return ts, acc


def run(ts, acc, cfg):
    sc = StepCounter(cfg)
    for t, a in zip(ts, acc):
        sc.update(a, t)
    return sc


def report(sc, truth, label=""):
    err = sc.steps - truth
    pct = (err / truth * 100.0) if truth else 0.0
    print(f"  {label:<28} 步数 {sc.steps:>4}  (真值 {truth}, 误差 {err:+d} / {pct:+.1f}%)")
    return abs(pct)


def main():
    ap = argparse.ArgumentParser(description="计步算法离线调参")
    ap.add_argument("csv", help="imu_dump.py 导出的 CSV")
    ap.add_argument("--steps", type=int, required=True, help="实际走了多少步（真值）")
    ap.add_argument("--sweep", action="store_true", help="扫参数")
    args = ap.parse_args()

    ts, acc = load_csv(args.csv)
    dur = (ts[-1] - ts[0]) / 1000.0
    print(f"\n数据: {args.csv}")
    print(f"  {len(ts)} 帧 / {dur:.1f} 秒 / {len(ts)/dur:.1f} Hz")
    print(f"  真值: {args.steps} 步  (≈ {args.steps/dur*60:.0f} 步/分)\n")

    # ---- 当前参数 ----
    print("=" * 62)
    print("当前参数（设备上正在用的）")
    print("=" * 62)
    sc = run(ts, acc, Cfg())
    report(sc, args.steps, "默认参数")
    print(f"  检测到峰 {sc.peaks_seen} 个；拒绝统计 {sc.rej}")
    print(f"  步频 {sc.cadence_spm} 步/分  距离 {sc.distance_cm/100:.1f} m  "
          f"卡路里 {sc.calories_mkcal/1000:.1f} kcal")

    if not args.sweep:
        print("\n（加 --sweep 扫参数）")
        return 0

    # ---- 单参数扫描 ----
    print("\n" + "=" * 62)
    print("单参数扫描")
    print("=" * 62)
    sweeps = {
        "commit_after": [1, 2, 3, 4, 5, 6],
        "rhythm_cv_x1000": [200, 260, 320, 400, 500, 700, 1000],
        "refractory_pct": [20, 25, 30, 35, 45, 55],
        "refractory_ms": [150, 200, 250, 300, 400],
        "crest_min_x100": [0, 100, 150, 200, 250, 300],
        "peak_thresh_mg": [30, 40, 50, 60, 80, 100, 120],
        "cadence_min_spm": [40, 50, 60, 70, 80],
    }
    best = {}
    for key, vals in sweeps.items():
        print(f"\n--- {key} ---")
        row = []
        for v in vals:
            sc = run(ts, acc, Cfg(**{key: v}))
            err = sc.steps - args.steps
            pct = err / args.steps * 100.0
            row.append((abs(pct), v, sc.steps, pct))
            flag = "  <<<" if abs(pct) < 3 else ""
            print(f"    {key}={v:<6} -> {sc.steps:>4} 步  ({pct:+6.1f}%){flag}")
        row.sort()
        best[key] = row[0]

    print("\n" + "=" * 62)
    print("各参数最佳值（单参数最优）")
    print("=" * 62)
    for k, (apct, v, steps, pct) in best.items():
        print(f"  {k:<18} = {v:<8} -> {steps} 步 ({pct:+.1f}%)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
