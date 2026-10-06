/**
 * @file motion.h
 * @brief 运动服务 —— IMU 采样 + 计步/距离/卡路里（P1 功能 6、7）
 *
 * ## 职责
 *
 * ```
 *   QMI8658C ──50Hz──▶ motion 任务 ──▶ 计步算法 ──▶ data_hub_set_steps()
 *                                                          │
 *                                            （快照 + 事件 + 界面刷新）
 * ```
 *
 * ## 为什么单独一层
 *
 * `data_hub` 的职责是"存设备状态 + 广播变化"，它不该知道 IMU 怎么采样。
 * 所以采样和算法都放这里，算完把**结果**交给 data_hub。
 * 以后换 IMU、换算法、加"抬腕检测"，都只动这个组件。
 *
 * ## 数据流与刷新率
 *
 * - 采样 50Hz（走路信号的主要能量在 0.5–3.5Hz，50Hz 足够且留了余量）
 * - **推给 data_hub 的节流到 1Hz**：步数每秒变一次就够界面用了，
 *   50Hz 推会让 UI 每秒重画 50 次，纯浪费电。
 *
 * ## ⚠️ 关于精度的实话
 *
 * 算法默认参数是**用合成腕部运动模型扫参得到的，没有用真实腕部数据标定过**。
 * 参考公开评测（Koffman 2024 复现 7 种开源计步算法）：
 * 最好的纯算法平均绝对误差约 8.6%，纯阈值法普遍**系统性少计**。
 * 所以：
 *   - 别指望 5% 以内；
 *   - 上板后必须按 tools 里的方法实测调参（见 doc/版本记录 与 imu 组件注释）；
 *   - 界面上的"步数"只作参考，不要拿来做健康结论。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 运动服务状态（给串口命令查用） */
typedef struct {
    bool     imu_ok;         ///< IMU 是否初始化成功（不成功时步数恒为 --）
    bool     running;        ///< 采样任务是否在跑
    uint32_t steps;          ///< 已确认步数
    uint32_t steps_pending;  ///< 待确认步数（节律门还没放行）
    int32_t  cadence_spm;    ///< 当前步频（步/分）
    uint8_t  activity;       ///< 0 静止 / 1 走路 / 2 跑步
    int32_t  stride_cm;      ///< 当前步长估计（cm）
    uint32_t distance_m;     ///< 累计距离（米）
    uint32_t kcal;           ///< 累计活动热量（千卡）
    /* 遥测（调参用，见 motion_dump_telemetry()） */
    int32_t  sig_mg;         ///< 去重力后的信号（mg）
    int32_t  thr_mg;         ///< 当前门限（mg）
} motion_status_t;

/**
 * @brief 初始化运动服务并启动采样任务
 *
 * @note 幂等。IMU 不在线时不会失败——只是步数一直显示 "--"，
 *       手表其余功能照常。
 */
esp_err_t motion_init(void);

/**
 * @brief 取当前状态（线程安全）
 */
void motion_get_status(motion_status_t *out);

/**
 * @brief 把用户身高体重同步给计步算法（影响步长和卡路里估算）
 *
 * @note 设置页改身高体重后调一次；motion_init() 里也会自动读一次 settings。
 */
void motion_apply_user_params(void);

/**
 * @brief 清零步数（"今日步数"手动重置 / 跨天自动重置用）
 *
 * @note P1 未做持久化，重启本来就归零；这个接口是给以后加"跨天归零"准备的。
 */
void motion_reset_steps(void);

/**
 * @brief 把一行遥测打到日志（调参用）
 *
 * 格式（逗号分隔，方便直接粘进表格）：
 *     ms,活动状态,信号mg,门限mg,步频spm,确认步数,待确认步数,距离m,千卡
 */
void motion_dump_telemetry(void);

#ifdef __cplusplus
}
#endif
