/**
 * @file app_console.h
 * @brief 串口命令行 —— 让手表不开电脑也能校时、看状态
 *
 * 为什么 P1 就需要它：
 *   板上 RTC（PCF85063）**没有备份电池**（原理图只接了 32.768kHz 晶振），
 *   一断电时间就丢。而 P2 的 BLE 校时、WiFi SNTP 校时都还没做，
 *   所以 P1 阶段必须有个"手动把时间喂进去"的口子 —— 就是这个命令行。
 *
 * 命令一览（烧录后 `.\build.ps1 monitor`，直接敲）：
 *
 *     help                        列出所有命令
 *     time                        显示当前时间 / 电量 / 运行状态
 *     set_time 2026-10-06 14:30:00      按"墙上时间"校时（本地时区）
 *     set_time utc 2026-10-06 06:30:00  按 UTC 校时（BLE 同步用的就是这条路径）
 *     battery                     打印电池电压与电量（对照万用表验证精度）
 *     brightness 60               设置亮度 5-100
 *     settings                    打印所有设置项
 *     reset_settings              恢复出厂设置
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动串口命令行（UART0，115200）
 *
 * @note 失败不是致命错误：没有命令行时手表照常走，只是没法校时。
 */
esp_err_t app_console_start(void);

#ifdef __cplusplus
}
#endif
