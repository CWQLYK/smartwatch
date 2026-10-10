/**
 * @file pwr_log.h
 * @brief 功耗日志 —— 把「电池电压 + 墙上时间」定期录进 Flash，供离线算真实省电
 *
 * ## 为什么需要它（测量问题）
 *
 * 想证明"这一版真的省电了"，正常情况下要么有 USB 电流表，要么拔线比续航。
 * 但本项目**两者都没有**，而且有个硬约束：
 *
 *   - 插着 USB 时，VBUS 直接给整机供电、同时给电池充电
 *     → 量到的是"充电器在干活"，跟手表自己的功耗无关；
 *   - 不插 USB 就没有串口，**没法一边待机一边看数据**。
 *
 * 所以反过来做：**让手表自己记**。
 * 拔线后电压下降的"速率"正比于电流，同电压区间内比速率就能比出省电；
 * 回来插上线打一条 `power log dump` 就能拿到整条曲线与趋势（mV/小时）。
 *
 * ## 用法（典型一次测量）
 *
 * ```
 * power log erase            # 清掉旧数据（可选）
 * power log start            # 开始记录（每 30 秒一条）
 * （拔掉 USB，静置 2 小时；屏幕会自己息掉）
 * （插回 USB —— 注意：插线会让芯片复位，记录随之停止，数据已落盘）
 * power log dump             # 看汇总 + 抽样行
 * power log dump all         # 打印全部样本
 * ```
 *
 * ## 设计取舍
 *
 * - **不自动续录**：插线会复位，若自动续录就会把"充电中"的样本混进来。复位后保持停止，
 *   数据留在文件里，用 `dump` 读。
 * - **每条都 `fflush`**：复位/掉电时数据必须还在（照 `imu_logger` 那次的教训 ——
 *   攒批 + 复位 = 静默错位的数据）。每 30 秒一次写入对 Flash 寿命毫无压力。
 * - **电压取自 `data_hub` 快照**，不自己调 `battery_read()`：
 *   ① data_hub 本身就每 30 秒采一次（正好同频）；
 *   ② `battery_read()` 内部有个**无锁的共享环形缓冲**，再从第三个任务并发调它只会加剧竞争。
 * - **SPIFFS 由 `imu_logger` 挂载**（分区 `storage` → `/spiffs`），本组件只使用、不挂载；
 *   未挂载时给出明确错误而不是崩。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 采样间隔（秒）。与 data_hub 的电量采样周期（30 秒）对齐 */
#define PWR_LOG_INTERVAL_S    30

/**
 * @brief 记录条数上限（≈256KB，30 秒一条时约 5.7 天）
 *
 * 到上限后**停止记录、保留已有数据**（不覆盖）—— 测完就来读，正常不会撞到。
 */
#define PWR_LOG_MAX_RECORDS   16384

/** @brief 一条采样记录（定长 16 字节，直接落盘） */
typedef struct {
    uint32_t uptime_s;                  /**< 开机秒数（单调）：算间隔用它，不依赖 RTC 准不准 */
    uint16_t year;                      /**< 墙上时间；未校时时全 0 */
    uint8_t  month, day;                /**< 1-12 / 1-31 */
    uint8_t  hour, minute, second;      /**< 0-23 / 0-59 / 0-59 */
    uint16_t mv;                        /**< 电池电压 mV；0 = 未知 */
    int8_t   pct;                       /**< 电量 %；-1 = 未知 */
    uint8_t  flags;                     /**< PWR_LOG_F_xxx */
} pwr_log_rec_t;

#define PWR_LOG_F_PRESENT     0x01u     /**< 检测到电池 */
#define PWR_LOG_F_CHARGING    0x02u     /**< 充电中（电压会上升，算趋势时排除） */
#define PWR_LOG_F_TIME_VALID  0x04u     /**< RTC 已校时（墙上时间可信） */

/**
 * @brief 开始记录（幂等；会先采一条，保证"start 后马上拔线"也有起点）
 *
 * @note 文件以**追加**方式打开，多次 start 不会覆盖旧数据；要清空用 `pwr_log_erase()`。
 * @return ESP_OK / ESP_ERR_INVALID_STATE（SPIFFS 未挂载）/ ESP_ERR_NO_MEM（已达条数上限）
 */
esp_err_t pwr_log_start(void);

/** @brief 停止记录并关闭文件（幂等） */
esp_err_t pwr_log_stop(void);

/** @brief 是否正在记录 */
bool pwr_log_running(void);

/**
 * @brief 文件里已有多少条记录
 * @return 条数；-1 = 文件系统未就绪
 */
int pwr_log_count(void);

/**
 * @brief 读取记录（从文件头开始）
 * @param out  输出缓冲
 * @param max  最多读多少条
 * @return 实际读到的条数；-1 = 出错（0 = 没有数据）
 */
int pwr_log_read(pwr_log_rec_t *out, int max);

/**
 * @brief 打印汇总 + 样本行（给串口命令用）
 *
 * @param all true = 打印全部样本；false = 只打印抽样（最多约 48 行）+ 汇总
 *
 * @note 汇总里的**趋势（mV/小时）只统计"有电池 + 没在充电"的样本** ——
 *       充电时电压是上升的，混进去算出来的斜率没有意义。
 *
 * @note 这里直接用 `printf`（而不是 `ESP_LOGI`）：输出目标是串口命令行，
 *       和 `app_console.c` 里其它命令的呈现方式保持一致。
 */
void pwr_log_dump(bool all);

/** @brief 删除日志文件并停止记录 */
esp_err_t pwr_log_erase(void);

/**
 * @brief **复位黑盒**：把"这次为什么重启 + 当时的电池电压"记进 Flash
 *
 * ## 为什么需要它
 *
 * 电池供电时**串口是不存在的**（USB 一插上就变成 USB 供电，故障往往就不复现了），
 * 而"每隔几秒重启一次"这类问题恰恰只在电池下出现。所以让固件把每次启动的
 * `esp_reset_reason()` 和当时读到的电池 mV 记下来，插回 USB 后用
 * `power log boot` 读 —— 就能区分：
 *
 *   - **掉电复位（BROWNOUT）** → 供电问题（电池容量/内阻、接线、LDO 跌落）
 *   - **崩溃（PANIC）/看门狗** → 软件问题（再用 `crash` 看 coredump）
 *
 * @note 由 `app_start()` 在 `imu_logger_init()`（挂载 SPIFFS）之后调用一次。
 * @note 记录写 `/spiffs/bootlog.bin`，每条 16 字节；满 64 条后**只保留最近一半**，
 *       这样长时间重启循环里看到的始终是最新的。
 */
void pwr_log_note_boot(void);

/** @brief 打印复位黑盒（给串口命令 `power log boot` 用） */
void pwr_log_dump_boots(void);

/* ==================================================================
 * 卡死哨兵（hang watcher）—— v0.7.1
 *
 * ## 为什么需要它
 *
 * 实机出现过**硬死机**：屏幕冻住、**连串口控制台都无响应**，而且
 *   - **不产生复位**（复位黑盒里没有新增记录），
 *   - **没有 panic、没有 coredump**（事后 `idf.py coredump-info` 读不到），
 *   - 用"不拉 DTR/RTS 的直连"（已做过阳性对照，方法有效）也读不到任何字节。
 *
 * 也就是说：**事后取证的手段全部失效**，必须在"还活着的时候"把现场写下来。
 *
 * ## 做法
 *
 * 建一个**独立任务**（优先级 18、**绑核 CPU 1**）每秒采样一次所有任务的运行计数：
 *   - 只要 `taskLVGL` 的运行计数**连续 5 秒没有推进** → 判定为卡死；
 *   - 立刻把**每个任务的名称/状态/优先级/运行计数/栈顶地址/栈余量**写进
 *     `/spiffs/hang.txt`（覆盖写，永远保留最近一次）。
 *
 * 栈顶地址可以离线翻译成函数名：
 * ```
 * xtensa-esp32s3-elf-addr2line -pfiaC -e build/smartwatch.elf 0x4201abcd 0x4038xxxx ...
 * ```
 * 这样就能看出"卡在哪个函数"、"谁在等锁"。
 *
 * @note 绑 **CPU 1** 是刻意的：真死机往往是**某个核**被占死（例如关中断自旋），
 *       哨兵在另一个核上才活得下来。同理，它不去依赖 esp_timer（那条路也可能一起死）。
 * @note 由 `app_start()` 启动一次。
 * ================================================================== */

/** @brief 启动卡死哨兵（幂等） */
void pwr_log_hang_watch_start(void);

/** @brief 打印卡死报告（给串口命令 `power log hang` 用） */
void pwr_log_dump_hang(void);

#ifdef __cplusplus
}
#endif
