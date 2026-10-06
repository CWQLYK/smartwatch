"""
给板子发串口命令并回读结果（验证命令行 / 校时用）。

用法:
    python tools/serial_cmd.py COM24 "set_time 2026-10-06 14:30:00" "time"
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM24"
cmds = sys.argv[2:] or ["time"]

s = serial.Serial(port, 115200, timeout=0.3)
# USB-Serial-JTAG: DTR/RTS 不会复位，直接收发即可
s.setDTR(False)
s.setRTS(False)
time.sleep(0.3)
s.reset_input_buffer()

# 等 REPL 就绪（最多 5 秒）
deadline = time.time() + 5
buf = b""
while time.time() < deadline:
    chunk = s.read(2048)
    if chunk:
        buf += chunk
        if b"watch>" in buf:
            break

for cmd in cmds:
    s.reset_input_buffer()
    s.write((cmd + "\r\n").encode())
    s.flush()

    got = b""
    deadline = time.time() + 6
    while time.time() < deadline:
        chunk = s.read(2048)
        if chunk:
            got += chunk
            if b"watch>" in got:
                break

    text = got.decode("utf-8", errors="replace")
    # 去掉 ANSI 与回显，只留结果
    lines = [l.rstrip() for l in text.replace("\x1b", "").splitlines()]
    print(f"\n===== $ {cmd} =====")
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped == cmd or stripped.endswith("watch>"):
            continue
        print("  " + stripped)

s.close()
