"""
从串口导出手表上的 IMU 日志（带校验），存成干净的 CSV。

用法:
    python tools/imu_dump.py COM24 --list              # 看有哪些日志
    python tools/imu_dump.py COM24                     # 导出最新的一个
    python tools/imu_dump.py COM24 /spiffs/imu_0002.bin
    python tools/imu_dump.py COM24 --plot               # 导出后画波形

## 为什么要专门写脚本，不直接重定向串口输出（两个坑）

1. **换行会翻倍**：这块板的 USB-Serial-JTAG 把每个 "\\n" 发成 "\\r\\r\\n"，
   直接重定向得到的 CSV 全是空行，pandas/numpy 读起来全是 NaN。

2. **持续高速输出会丢字节，而且丢得隐蔽**（更严重）：
   实测行数是对的（换行没丢、行没粘连），但行**内部**的字段会错位。
   纯 CSV 输出过一段就变成乱值，而且**不带校验根本发现不了**。
   所以固件改成输出 `<帧序号> <12字节hex> <校验和>`，
   本脚本负责逐行验校验和 —— 发现坏行就报警并统计，而不是默默写进 CSV。

## 数据格式（固件 imu_logger 写的原始帧，12 字节小端）

    offset  type     字段
    0       uint32   时间戳 ms（⚠️ 会 uint32 回绕，本脚本自动还原）
    4       int16    ax (mg)
    6       int16    ay (mg)
    8       int16    az (mg)
    10      uint16   保留
"""
import argparse
import os
import re
import struct
import sys
import time

import serial

BOOT_WAIT_S = 5.0
PROMPT = b"watch>"
OUT_DIR = "imu_logs"

# <6位帧序号> <24位十六进制> <2位校验和>
FRAME_RE = re.compile(r"^(\d{6})\s+([0-9a-fA-F]{24})\s+([0-9a-fA-F]{2})$")

FRAME_BYTES = 12
UINT32_SPAN = 1 << 32
WRAP_THRESHOLD = 1 << 31


def connect(port, retries=6):
    """
    连上串口并等设备启动完。

    ⚠️ 必须带重试：设备复位时 USB 会**重新枚举**，这段时间打开串口会报
    `ClearCommError failed (PermissionError(13, '设备不识别此命令。'))`。
    实测断开重连后立刻打开就会撞上，等一两秒重试即可。
    """
    last_err = None
    for attempt in range(retries):
        try:
            s = serial.Serial(port, 115200, timeout=0.3)
        except serial.SerialException as e:
            last_err = e
            time.sleep(1.5)
            continue

        # USB-Serial-JTAG: DTR/RTS 不会复位，直接收发即可
        s.setDTR(False)
        s.setRTS(False)
        s.reset_input_buffer()

        # 打开串口会触发一次芯片复位，等它启动完
        t = time.time()
        while time.time() - t < BOOT_WAIT_S:
            try:
                s.read(65536)
            except serial.SerialException as e:
                # 中途掉线（正在复位）→ 关掉重来
                last_err = e
                s.close()
                s = None
                break
            time.sleep(0.05)

        if s is not None:
            s.reset_input_buffer()
            return s

        time.sleep(1.5)

    raise SystemExit(f"连不上 {port}：{last_err}\n"
                     f"（设备正在复位/重新枚举时会这样，重跑一次通常就好）")


def send(s, cmd, timeout=60.0, quiet_for=1.5):
    """发命令并回读。结束判据：见到提示符，或收到内容后静默 quiet_for 秒。"""
    s.write((cmd + "\r\n").encode())
    s.flush()

    t = time.time()
    last_rx = t
    got = b""
    while time.time() - t < timeout:
        chunk = s.read(8192)
        if chunk:
            got += chunk
            last_rx = time.time()
            if PROMPT in chunk:
                break
        elif got and (time.time() - last_rx) > quiet_for:
            break
    return got.decode("utf-8", errors="replace")


def list_files(s):
    out = send(s, "rec list")
    files = re.findall(r"(/spiffs/imu_\d+\.bin)\s+(\d+)\s+字节\s+(\d+)\s+帧", out)
    print("\n手表上的 IMU 日志：")
    if not files:
        print("  （没有。先在手表上或串口执行 rec start / rec stop 录一段）")
        return []
    for name, size, frames in files:
        print(f"  {name}   {int(size):>9,} 字节  {int(frames):>7,} 帧  "
              f"约 {int(frames)//50:>5} 秒")
    return [f[0] for f in files]


def xor_checksum(frame_no: str, hexdata: str) -> int:
    """
    复刻固件的校验和算法。

    ⚠️ 必须和 app_console.c 里的实现完全一致：
       校验对象是"帧序号 + 12 字节 hex"的**全部 ASCII 字符**（跳过分隔空格），
       不包含校验和字段自身。
       固件那边是先 snprintf 出 `"%06u %02x%02x..."` 再逐字符异或。
    """
    s = 0
    for ch in (frame_no + hexdata):
        s ^= ord(ch)
    return s


def dump(s, remote):
    print(f"正在导出 {remote} ...（大文件要几分钟，别拔线）")
    t0 = time.time()
    out = send(s, f"rec get {remote}", timeout=1800.0, quiet_for=3.0)
    dt = time.time() - t0

    frames = {}          # frame_no -> 12 字节
    bad_checksum = 0
    skipped = 0
    dup = 0

    for line in out.splitlines():
        line = line.replace("\x1b", "").strip()
        m = FRAME_RE.match(line)
        if not m:
            if line and not line.startswith(("rec get", "=====", "每行", "校验和")):
                skipped += 1
            continue

        no_s, hexdata, sum_s = m.group(1), m.group(2), m.group(3)
        if xor_checksum(no_s, hexdata) != int(sum_s, 16):
            bad_checksum += 1
            continue

        no = int(no_s)
        if no in frames:
            dup += 1
            continue
        frames[no] = bytes.fromhex(hexdata)

    if not frames:
        print("!! 一帧都没解析到。原始输出前 600 字：")
        print(out[:600])
        return None

    # ---- 解析成 (t_ms, ax, ay, az) ----
    nos = sorted(frames)
    recs = []
    for no in nos:
        t_ms, ax, ay, az, _ = struct.unpack("<IhhhH", frames[no])
        recs.append((t_ms, ax, ay, az))

    # uint32 时间戳回绕还原
    ts = []
    offset = 0
    prev = None
    wrapped = False
    for r in recs:
        v = r[0]
        if prev is not None and v < prev and (prev - v) > WRAP_THRESHOLD:
            offset += UINT32_SPAN
            wrapped = True
        ts.append(v + offset)
        prev = v

    recs = [(ts[i], r[1], r[2], r[3]) for i, r in enumerate(recs)]

    # ---- 完整性报告 ----
    expect = list(range(nos[0], nos[-1] + 1))
    missing = sorted(set(expect) - set(nos))

    os.makedirs(OUT_DIR, exist_ok=True)
    base = os.path.basename(remote).replace(".bin", ".csv")
    path = os.path.join(OUT_DIR, base)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write("t_ms,ax,ay,az\n")
        for r in recs:
            f.write(f"{r[0]},{r[1]},{r[2]},{r[3]}\n")

    print(f"\n===== 导出完成 =====")
    print(f"  文件      : {path}")
    print(f"  有效帧    : {len(recs):,}")
    print(f"  校验失败行: {bad_checksum:,}")
    print(f"  重复帧    : {dup:,}")
    print(f"  其他忽略行: {skipped:,}")
    if missing:
        # 缺帧只可能是真的丢了（校验失败的行已被统计在上面）
        print(f"  ⚠️ 缺帧    : {len(missing):,} 帧"
              f"（序号 {missing[0]}..{missing[-1]}）")
    else:
        print(f"  缺帧      : 无 ✓")

    if len(recs) > 1:
        tss = [r[0] for r in recs]
        dur = (tss[-1] - tss[0]) / 1000.0
        dts = [tss[i + 1] - tss[i] for i in range(len(tss) - 1)]
        avg = sum(dts) / len(dts)
        print(f"  时长      : {dur:.1f} 秒")
        print(f"  实际采样率: {1000.0/avg:.1f} Hz（标称 50 Hz）")
        print(f"  帧间隔    : {min(dts)} / 中位 {sorted(dts)[len(dts)//2]} / {max(dts)} ms")

        def rng(i):
            v = [r[i] for r in recs]
            return min(v), max(v)

        print(f"  ax 范围   : {rng(1)[0]} .. {rng(1)[1]} mg")
        print(f"  ay 范围   : {rng(2)[0]} .. {rng(2)[1]} mg")
        print(f"  az 范围   : {rng(3)[0]} .. {rng(3)[1]} mg")

        if wrapped:
            print("  （时间戳有 uint32 回绕，已自动还原为单调递增）")

        # 采样间隔抖动大 = 数据不可信
        if max(dts) > 3 * avg:
            print(f"\n  ⚠️ 最大帧间隔 {max(dts)} ms（平均 {avg:.0f} ms），采样有卡顿。")
            print("     计步算法假设采样均匀，调参时要留意这些位置。")

    print(f"  导出耗时  : {dt:.1f} 秒\n")
    return path, recs


def plot(path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("没装 matplotlib，跳过画图（pip install matplotlib 后可画）")
        return

    import csv
    ts, ax, ay, az = [], [], [], []
    with open(path, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            ts.append(int(row["t_ms"]) / 1000.0)
            ax.append(int(row["ax"]))
            ay.append(int(row["ay"]))
            az.append(int(row["az"]))

    fig, axes = plt.subplots(3, 1, figsize=(13, 8), sharex=True)
    for a, v, name in zip(axes, (ax, ay, az), ("ax", "ay", "az")):
        a.plot(ts, v, linewidth=0.6)
        a.set_ylabel(f"{name} (mg)")
        a.grid(alpha=0.3)
    axes[-1].set_xlabel("时间 (秒)")
    axes[0].set_title(os.path.basename(path))
    fig.tight_layout()
    png = path.replace(".csv", ".png")
    fig.savefig(png, dpi=110)
    print(f"  波形图    : {png}")


def main():
    ap = argparse.ArgumentParser(
        description="从串口导出手表上的 IMU 日志（带校验）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("port", help="串口号，如 COM24")
    ap.add_argument("remote", nargs="?",
                    help="手表上的文件，如 /spiffs/imu_0001.bin；省略则导最新的一个")
    ap.add_argument("--list", action="store_true", help="只列出有哪些日志")
    ap.add_argument("--plot", action="store_true", help="导出后画波形图")
    args = ap.parse_args()

    s = connect(args.port)
    try:
        target = args.remote
        if args.list or not target:
            files = list_files(s)
            if not target:
                if not files:
                    return 1
                target = files[-1]           # 文件名递增，最后一个是最近的
                print(f"\n未指定文件，导出最新的：{target}")
        res = dump(s, target)
        if res and args.plot:
            plot(res[0])
    finally:
        s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
