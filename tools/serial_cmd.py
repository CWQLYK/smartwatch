"""
给板子发串口命令并回读结果（验证命令行 / 校时 / 导出 IMU 日志用）。

用法:
    python tools/serial_cmd.py COM24 "set_time 2026-10-06 14:30:00" "time"
    python tools/serial_cmd.py COM24 "rec status"
    python tools/serial_cmd.py COM24 --raw "rec get /spiffs/imu_0001.bin" > imu.csv

## 为什么不用"等提示符 watch>"来判断就绪（踩过的坑）

我第一版是靠 `watch>` 提示符同步的，结果**极不稳定**，两个原因：

1. **打开串口会触发一次芯片复位**（日志里是 `rst:0x15 (USB_UART_CHIP_RESET)`），
   所以刚连上时设备正在重新启动，而启动日志里**也有** `watch>` 字样 ——
   等提示符会立刻被启动日志满足，命令其实还没发出去。
2. 这块板的 USB-Serial-JTAG 在命令行**刚起来的瞬间会吐乱码**
   （能看到 `[5n` 之类），紧接着才是正常输出。按行匹配提示符很不可靠。

## 改成"定时 + 全量回读"

ESP32-S3 的 USB-Serial-JTAG 收到数据会先**缓存在接收 FIFO**里，
而 REPL 起来之后会把这批积压的输入一次性处理掉。所以：

    连上 -> 等固定时间（够它复位+启动+REPL 就绪）-> 直接发命令 -> 回读

不依赖任何提示符，稳定得多。代价是每次连接固定多等几秒。
"""
import sys
import time

import serial

# 复位 + 启动 + REPL 就绪实测约 2.7 秒，取 5 秒留足余量
BOOT_WAIT_S = 5.0

PROMPT = b"watch>"


def main():
    argv = sys.argv[1:]
    raw = "--raw" in argv
    if raw:
        argv.remove("--raw")

    port = argv[0] if argv else "COM24"
    cmds = argv[1:] or ["time"]

    s = serial.Serial(port, 115200, timeout=0.3)
    # USB-Serial-JTAG: DTR/RTS 不会复位，直接收发即可
    s.setDTR(False)
    s.setRTS(False)

    # 1) 等设备复位 + 启动 + REPL 就绪。期间把启动日志读掉。
    s.reset_input_buffer()
    t = time.time()
    while time.time() - t < BOOT_WAIT_S:
        s.read(65536)
        time.sleep(0.05)
    s.reset_input_buffer()

    for cmd in cmds:
        s.write((cmd + "\r\n").encode())
        s.flush()

        # 2) 回读直到看到提示符（或超时）。
        #    导出大文件要很久，所以 rec get 给 5 分钟。
        limit = 300 if cmd.startswith("rec get") else 15
        t = time.time()
        got = b""
        while time.time() - t < limit:
            chunk = s.read(8192)
            if chunk:
                got += chunk
                if PROMPT in chunk:
                    break
            else:
                # 已经收到过内容、且静默超过 1.5 秒 -> 认定输出结束
                if got and (time.time() - t) > 1.5:
                    break

        if raw:
            sys.stdout.write(got.decode("utf-8", errors="replace"))
            sys.stdout.flush()
            continue

        text = got.decode("utf-8", errors="replace").replace("\x1b", "")
        print(f"\n===== $ {cmd} =====")
        for line in text.splitlines():
            stripped = line.strip()
            if not stripped or stripped == cmd or stripped.endswith("watch>"):
                continue
            print("  " + stripped)

    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
