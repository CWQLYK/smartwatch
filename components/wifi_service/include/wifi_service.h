/**
 * @file wifi_service.h
 * @brief WiFi 连接 + SNTP 校时服务
 *
 * ## 它解决什么问题
 *
 * 板上的 PCF85063 RTC **没有备份电池**（8-pin 封装连 VBAT 都没有），
 * 所以每次断电上电时间都归零，界面显示"未校准"。
 * 最省事的补救就是连上路由器用 SNTP 对一次时 —— 硬件零成本。
 *
 * ## 设计：能力收在服务里，界面只拿钩子
 *
 * `watch_ui_pages` 不许 include ESP-IDF 头文件（否则 PC 模拟器编不过），
 * 所以这里提供一组**函数指针钩子**（和 brightness / settings 同样的套路）：
 *
 *     界面层：wifi_hooks_t *h = wifi_service_get_hooks();  h->connect(ssid, pwd);
 *     实机  ：钩子指向本文件的实现
 *     模拟器：不注册钩子，界面显示"WiFi 不可用"，但照样能打开、不崩
 *
 * ## 连接策略（按需求定）
 *
 * - 密码**存 NVS**，开机自动重连（省得每次敲）
 * - 连上后**保持连接**，但提供手动断开
 * - 连上就自动 SNTP 对时，对完时继续连着（用户要求）
 * - 界面要弹提示告诉用户"WiFi 耗电，不用时请断开"
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ==================================================================
 * 类型定义：**复用界面层的那一份**（watch_platform.h）
 * ==================================================================
 *
 * 一开始我在本文件和 watch_platform.h 里各写了一份同构的
 * wifi_ap_t / wifi_status_t / wifi_hooks_t，靠"字段对齐"的约定维护。
 * 那是个坏设计：两边一旦改不同步，赋值会**静默错位**（结构体尺寸一样时
 * 编译器不会报错），排查起来极痛苦。
 *
 * 现在改成：watch_platform.h 里定义 w_* 类型（那份不依赖 ESP-IDF，
 * 界面层能用），本文件**直接别名过来**：
 *
 *     typedef w_wifi_ap_t     wifi_ap_t;
 *     typedef w_wifi_status_t wifi_status_t;
 *     typedef w_wifi_state_t  wifi_state_t;
 *     typedef w_wifi_hooks_t  wifi_hooks_t;
 *
 * 只有一份定义，不可能对不上。
 */
#include "watch_platform.h"

typedef w_wifi_ap_t     wifi_ap_t;
typedef w_wifi_status_t wifi_status_t;
typedef w_wifi_state_t  wifi_state_t;
typedef w_wifi_hooks_t  wifi_hooks_t;

#define WIFI_SCAN_MAX   W_WIFI_SCAN_MAX
#define WIFI_SSID_MAX   W_WIFI_SSID_MAX
#define WIFI_PASS_MAX   W_WIFI_PASS_MAX

/* ==================================================================
 * 界面层用的钩子（这些函数**不依赖 ESP-IDF**，可以放进界面代码）
 * ================================================================== */

/** @brief 取钩子表（永远是同一个静态实例，不会返回 NULL） */
const wifi_hooks_t *wifi_service_get_hooks(void);

/* ==================================================================
 * 实机 API（只有 app 层该调）
 * ================================================================== */

/**
 * @brief 初始化 WiFi + 注册钩子 + 自动重连
 *
 * @note 幂等。失败不致命：手表其余功能照常，只是 WiFi 页会提示不可用。
 *       注意它会占用较多 RAM（WiFi 协议栈约 50KB+），
 *       所以**不要**在 app_start 里默认初始化 —— 等用户点进 WiFi 页再初始化，
 *       或者由 app 决定。这里也提供 wifi_service_init_deferred() 用于按需初始化。
 */
esp_err_t wifi_service_init(void);

/**
 * @brief 注册"SNTP 对时成功"回调
 *
 * @param cb  收到 UTC 时间戳时调用（在 SNTP 任务上下文里，别做耗时操作）
 *
 * @note 用回调而不是让 wifi_service 直接写 RTC，是为了保持依赖方向：
 *       wifi_service 不该知道 RTC 的存在。"对到时间之后干什么"是 app 的事。
 */
void wifi_service_set_time_cb(void (*cb)(uint32_t utc_epoch));

/**
 * @brief 彻底重置 WiFi 驱动并清除保存的凭据
 *
 * @note 用于"连接失败几次后一直连不上"的情况 —— 驱动内部认证状态会变脏，
 *       只有 deinit + init 才能回到干净状态（断电重启也有效，但太重）。
 */
esp_err_t wifi_service_reset(void);

/**
 * @brief 取钩子表（界面用；服务没起来时钩子仍返回，内部会安全失败）
 *
 * @note 和 wifi_service_get_hooks() 是同一个东西，名字区分用途：
 *       界面代码统一用这个，语义更清楚。
 */
const wifi_hooks_t *wifi_service_hooks(void);

/** @brief 只注册钩子，不初始化 WiFi 协议栈（省 RAM；等真正要用时再 init） */
void wifi_service_register_hooks(void);

/** @brief WiFi 协议栈是否已经初始化 */
bool wifi_service_started(void);

/** @brief 打印状态（串口命令用） */
void wifi_service_dump(void);

#ifdef __cplusplus
}
#endif
