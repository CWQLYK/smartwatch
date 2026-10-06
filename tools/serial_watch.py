"""
临时工具：实时抓串口日志（边收边打印，能看出"卡住"还是"跑着"）。

用法:
    python tools/serial_watch.py COM24 20
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM24"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

s = serial.Serial(port, 115200, timeout=0.1)
s.setDTR(False)
s.setRTS(False)
time.sleep(0.05)

# 复位一次
s.setRTS(True)
time.sleep(0.15)
s.setRTS(False)
time.sleep(0.05)

s.reset_input_buffer()

t0 = time.time()
pending = b""

def flush_lines(data, final=False):
    """按行输出；最后一行如果不完整，非 final 时留到下次"""
    global pending
    pending += data
    while b"\n" in pending:
        line, pending = pending.split(b"\n", 1)
        emit(line)
    if final and pending:
        emit(pending)
        pending = b""

def emit(line):
    text = line.decode("utf-8", errors="replace").rstrip("\r")
    if text.strip():
        # 实时输出，带相对时间；用 flush 保证顺序
        print(f"{time.time() - t0:6.2f}s | {text}", flush=True)

while time.time() - t0 < secs:
    chunk = s.read(4096)
    if chunk:
        flush_lines(chunk)

flush_lines(b"", final=True)
s.close()
