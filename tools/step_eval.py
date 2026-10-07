"""
在真实数据上评估一组候选参数，并做"跨步频"稳健性检查。

## 为什么不能只调出一组"刚好对上 50 步"的参数

只有一份真实数据时，网格搜索很容易**过拟合**：
搜出一组参数刚好把 50 步算对，但换个步频/换个握姿就不准了。
（我第一轮搜到 crest=110 时误差 2%，但 crest=80 就变 16%、140 变 32%
 —— 这种"一个参数动一点结果就崩"的解是不敢用的。）

所以这个脚本做两件事：
  1. 在**真实数据**上评估，要求误差小
  2. 用**合成信号在不同步频下**做交叉检查：参数应该对 60~130 步/分都合理，
     不能只在 74 步/分这一个点上碰巧对

合成信号只用来**防止过拟合**，不作为主要依据 —— 真实数据才是标准答案。

用法:
    python tools/step_eval.py imu_logs/imu_0001.csv --steps 50 --candidates
"""
import argparse
import math
import statistics
import sys

from step_tune import Cfg, StepCounter, load_csv


# ======================================================================
# 合成腕部信号（只用于稳健性交叉检查）
# ======================================================================
def synth_walk(spm, seconds=40, rate=50, seed=1):
    """
    生成一段"腕部"合矢量信号，模拟 N 步/分的走路。

    刻意**不做成尖峰**：真实数据表明，在带通中心频率附近，
    腕部信号是接近正弦的平滑波形（这正是不该用波峰因数判据的原因）。
    所以这里用"正弦主频 + 少量二次谐波 + 噪声"，比"脉冲串"更接近实测。
    """
    import random
    rng = random.Random(seed)
    f = spm / 60.0                      # 步频 Hz
    n = int(seconds * rate)
    amp_mg = 300.0 if spm < 100 else 500.0    # 走得快幅度更大
    out = []
    for i in range(n):
        t = i / rate
        # 主频 + 二次谐波（真实腕部有足跟冲击的二次成分，但比主频弱）
        s = math.sin(2 * math.pi * f * t) + 0.25 * math.sin(4 * math.pi * f * t + 0.7)
        noise = rng.gauss(0, 25)         # 25mg 噪声
        mag = 1000.0 + amp_mg * s + noise
        # 合成一个方向，让合矢量等于 mag（算法只用合矢量）
        out.append((int(mag * 0.9), int(mag * 0.2), int(-mag * 0.4)))
    return [int(i * 1000 / rate) for i in range(n)], out


def eval_synth(cfg, spms=(60, 75, 90, 105, 120)):
    """返回各步频下的绝对百分比误差列表"""
    errs = []
    for spm in spms:
        ts, acc = synth_walk(spm)
        sc = StepCounter(cfg)
        for t, a in zip(ts, acc):
            sc.update(a, t)
        truth = spm / 60.0 * (ts[-1] - ts[0]) / 1000.0
        errs.append(abs(sc.steps - truth) / truth * 100.0)
    return errs


# ======================================================================
def main():
    ap = argparse.ArgumentParser(description="评估计步参数 + 跨步频稳健性检查")
    ap.add_argument("csv", help="真实数据 CSV")
    ap.add_argument("--steps", type=int, required=True, help="真值步数")
    args = ap.parse_args()

    ts, acc = load_csv(args.csv)
    dur = (ts[-1] - ts[0]) / 1000.0
    spm_real = args.steps / dur * 60
    print(f"\n真实数据: {len(ts)} 帧 / {dur:.1f}s / 真值 {args.steps} 步 "
          f"({spm_real:.0f} 步/分)\n")

    # ---------------- 候选参数 ----------------
    # 现状（合成模型调出来的）
    cur = Cfg()
    # 候选 A：按真实数据特征调 —— 弱化波峰因数门、加大不应期
    cand_a = Cfg(crest_min_x100=80, peak_thresh_mg=40, start_thresh_mg=50,
                 refractory_ms=400, refractory_pct=45, rhythm_cv_x1000=500,
                 commit_after=3)
    # 候选 B：直接关掉波峰因数门，靠不应期 + 节律
    cand_b = Cfg(crest_min_x100=0, peak_thresh_mg=40, start_thresh_mg=50,
                 refractory_ms=400, refractory_pct=45, rhythm_cv_x1000=500,
                 commit_after=3)
    # 候选 C：B 的保守版（不应期更长）
    cand_c = Cfg(crest_min_x100=0, peak_thresh_mg=40, start_thresh_mg=50,
                 refractory_ms=450, refractory_pct=55, rhythm_cv_x1000=400,
                 commit_after=4)

    cands = [("现状(合成模型调的)", cur), ("A 弱化crest+长不应期", cand_a),
             ("B 关crest", cand_b), ("C B的保守版", cand_c)]

    print("=" * 86)
    print(f"{'候选':<24}{'真实步数':>9}{'误差':>9}   {'合成各步频误差 (60/75/90/105/120)':<40}{'合成最差':>9}")
    print("=" * 86)
    for name, cfg in cands:
        sc = StepCounter(cfg)
        for t, a in zip(ts, acc):
            sc.update(a, t)
        err = (sc.steps - args.steps) / args.steps * 100.0
        se = eval_synth(cfg)
        worst = max(se)
        flag = ""
        if abs(err) <= 6 and worst <= 25:
            flag = "  <<< 可用"
        print(f"{name:<24}{sc.steps:>9}{err:>8.1f}%   "
              f"{'/'.join(f'{e:.0f}' for e in se):<40}{worst:>8.1f}%{flag}")

    print("\n注：'合成最差'只是防止过拟合的交叉检查（合成信号不等于真人），")
    print("    判据应以真实数据误差为主，合成误差控制在 25% 以内即可。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
