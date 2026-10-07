"""
计步参数网格搜索 —— 用真实走路数据找一组合适的参数。

## 背景：为什么需要重新调

原始参数是**用合成腕部运动模型**扫出来的（见 doc/QMI8658C-计步方案.md），
作者自己也标注了"上板前不要信"。用第一份真实数据一跑就发现：

    真实数据 50 步 → 算法输出 0 步
    检测到 475 个候选峰，其中 444 个被"波峰因数门"拒绝

原因是**信号形态和合成模型不一致**：
  - 合成模型假设是"脉冲型步态冲击"（足跟落地那种尖峰）
  - 真实腕部在 74 步/分（1.23Hz，正好落在带通中心）时，
    信号是一个**很强但很平滑的正弦**：
        |bp| 均值 120 Q10，p95 = 300，峰值 545
    信噪比其实非常好，问题是 σ≈146 和 |bp| 均值同量级，
    所以 peak/σ 只有 1.5~3.7，永远过不了 "≥2.0σ" 这一关。

结论：**这个数据集上不该用波峰因数做主要判据**，应该靠
不应期 + 步频范围 + 节律一致性来压误计。

## 用法

    python tools/step_search.py imu_logs/imu_0001.csv --steps 50
    python tools/step_search.py imu_logs/*.csv --steps 50,62   # 多份数据一起评

脚本会跑粗网格，按"平均绝对百分比误差"排序，输出前若干组参数。
"""
import argparse
import glob
import itertools
import statistics
import sys

from step_tune import Cfg, StepCounter, load_csv


def evaluate(datasets, cfg):
    """对一组参数，在多个数据集上跑，返回 (平均绝对误差%, 各数据集步数)"""
    errs, outs = [], []
    for ts, acc, truth in datasets:
        sc = StepCounter(cfg)
        for t, a in zip(ts, acc):
            sc.update(a, t)
        outs.append(sc.steps)
        errs.append(abs(sc.steps - truth) / truth * 100.0)
    return statistics.mean(errs), outs


def main():
    ap = argparse.ArgumentParser(description="计步参数网格搜索")
    ap.add_argument("csvs", nargs="+", help="数据文件（可多个）")
    ap.add_argument("--steps", required=True,
                    help="真值步数，逗号分隔，和 csv 一一对应")
    ap.add_argument("--top", type=int, default=15, help="显示前几组")
    args = ap.parse_args()

    # 展开通配符
    files = []
    for p in args.csvs:
        files.extend(sorted(glob.glob(p)) or [p])
    truths = [int(x) for x in args.steps.split(",")]
    if len(truths) != len(files):
        sys.exit(f"真值个数({len(truths)}) 和文件个数({len(files)}) 不匹配")

    datasets = []
    for f, tr in zip(files, truths):
        ts, acc = load_csv(f)
        dur = (ts[-1] - ts[0]) / 1000.0
        spm = tr / dur * 60
        print(f"  {f}: {len(ts)} 帧 / {dur:.1f}s / {tr} 步 / {spm:.0f} 步/分")
        datasets.append((ts, acc, tr))

    # ------------------------------------------------------------------
    # 网格
    # ------------------------------------------------------------------
    grid = {
        # 波峰因数门：0 = 关闭。这个数据集上信号是平滑正弦，
        # 原始值 200（要求峰值≥2σ）会把真实步全拒掉，所以重点扫小值和关闭
        "crest_min_x100": [0, 50, 80, 110, 140, 170],
        # 峰值地板：真实峰值中位 226 Q10，地板不能太高
        "peak_thresh_mg": [30, 40, 50, 60, 80],
        # 启动门限（进入运动态）
        "start_thresh_mg": [40, 60, 80],
        # 不应期
        "refractory_ms": [150, 200, 250, 300],
        "refractory_pct": [30, 35, 45],
        # 节律门
        "rhythm_cv_x1000": [150, 200, 260, 350, 500, 1000],
        "commit_after": [2, 3, 4, 5],
    }

    keys = list(grid.keys())
    total = 1
    for k in keys:
        total *= len(grid[k])
    print(f"\n网格规模: {total} 组，开始搜索…\n")

    results = []
    for combo in itertools.product(*(grid[k] for k in keys)):
        cfg = Cfg(**dict(zip(keys, combo)))
        mean_err, outs = evaluate(datasets, cfg)
        results.append((mean_err, combo, outs))

    results.sort(key=lambda r: r[0])

    print("=" * 78)
    print(f"{'平均误差':>9}  参数组合")
    print("=" * 78)
    for mean_err, combo, outs in results[:args.top]:
        params = " ".join(f"{k}={v}" for k, v in zip(keys, combo))
        print(f"{mean_err:>8.1f}%  {params}")
        print(f"{'':>9}   -> 步数 {outs}   (真值 {truths})")

    # ------------------------------------------------------------------
    # 稳健性检查：看最佳解的邻居是否也好
    #   单点最优可能是过拟合。真实可用的参数应该在邻域里都还行。
    # ------------------------------------------------------------------
    print("\n" + "=" * 78)
    print("稳健性：最佳解附近扰动（好参数应该邻域也稳）")
    print("=" * 78)
    _, best_combo, _ = results[0]
    best = dict(zip(keys, best_combo))
    for key in ("crest_min_x100", "peak_thresh_mg", "rhythm_cv_x1000",
                "commit_after", "refractory_ms"):
        vals = grid[key]
        i = vals.index(best[key])
        lo, hi = max(0, i - 2), min(len(vals), i + 3)
        line = []
        for v in vals[lo:hi]:
            cfg = Cfg(**{**best, key: v})
            e, outs = evaluate(datasets, cfg)
            mark = "*" if v == best[key] else " "
            line.append(f"{v}:{e:5.1f}%{mark}")
        print(f"  {key:<18} " + "  ".join(line))

    print("\n推荐参数（可直接填进 step_config_t）：")
    for k, v in best.items():
        print(f"    .{k:<18} = {v},")
    return 0


if __name__ == "__main__":
    sys.exit(main())
