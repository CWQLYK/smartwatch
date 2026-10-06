/**
 * @file app.h
 * @brief 应用装配层 —— main.c 只需要调这一个函数
 *
 * 分层到此为止：
 *
 *     main.c            （3 行：调 app_start() 然后挂起）
 *        │
 *     app               ← 本组件：把各模块接起来（装配），不含业务/界面
 *        │
 *     ├─ data_hub ─ rtc / battery          （数据和服务）
 *     ├─ watch_ui ─ watch_ui_pages         （界面，零 ESP-IDF 依赖）
 *     ├─ event_bus / settings              （基础设施）
 *     └─ bsp / board / buzzer              （板级与驱动）
 *
 * 这样做的直接好处：
 *   - 想换成 PC 模拟器跑界面：换掉 app 里注入的 data provider 即可
 *   - 想加 BLE（P2）：新写一个组件，在 app 里多接一根线
 *   - 想加 AI 语音（P3）：同上，界面和驱动一行都不用改
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动整个手表应用
 *
 * 内部顺序（每一步失败都不会让整机死掉，而是降级）：
 *   1. 板级初始化（屏幕 / 触摸 / 背光 / LVGL 任务）
 *   2. 事件总线 + 设置服务（NVS）
 *   3. 数据中枢（RTC + 电池采样任务）
 *   4. 应用设置里的亮度到背光
 *   5. 装配界面（注入数据源 + 亮度钩子），建首页
 *   6. 启动串口命令行（校时 / 查状态）
 *
 * @note 本函数**会返回**（不阻塞）。返回后 LVGL 在自己的任务里跑，
 *       主任务可以挂起。
 */
esp_err_t app_start(void);

/**
 * @brief 取固件版本字符串
 */
const char *app_version(void);

#ifdef __cplusplus
}
#endif
