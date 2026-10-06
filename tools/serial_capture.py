"""
临时工具：抓一段串口日志（验证烧录结果用）

用法:
    python tools/serial_capture.py COM24 12            # 复位并抓 12 秒
    python tools/serial_capture.py COM24 12 --nors     # 不复位，直接抓
    python tools/serial_capture.py COM24 12 --panic    # 只抓到第一次 panic 就停
"""
import re
import sys
import time

import serial  # pyserial

port = sys.argv[1] if len(sys.argv) > 1 else "COM24"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
do_reset = "--nors" not in sys.argv
stop_at_panic = "--panic" in sys.argv

s = serial.Serial(port, 115200, timeout=0.2)

# 关键：读日志时必须把 DTR/RTS 都放开。
# 板上的 USB 转串口有自动复位电路，DTR/RTS 停在某个组合会把芯片按在复位/下载模式，
# 现象是"串口能打开但一个字符都没有"。
s.setDTR(False)
s.setRTS(False)
time.sleep(0.05)

if do_reset:
    # 复位开发板（和 esptool hard_reset 一样的时序）
    s.setRTS(True)
    time.sleep(0.1)
    s.setRTS(False)
    time.sleep(0.05)

s.reset_input_buffer()

deadline = time.time() + secs
buf = b""
panicked = False

while time.time() < deadline:
    chunk = s.read(4096)
    if not chunk:
        continue
    buf += chunk

    if stop_at_panic and not panicked and b"Backtrace:" in buf:
        # 已经拿到 panic + Backtrace 了，再读一点点就够
        panicked = True
        deadline = min(deadline, time.time() + 1.0)

s.close()

text = buf.decode("utf-8", errors="replace")

# 去掉 ANSI 颜色码，方便贴日志
text = re.sub(r"\x1b\[[0-9;]*m", "", text)

sys.stdout.write(text)
