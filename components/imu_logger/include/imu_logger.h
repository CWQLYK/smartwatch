/**
 * @file imu_logger.h
 * @brief IMU 数据记录器 —— 把真实走路数据录进 Flash，回来离线调参
 *
 * ## 为什么需要它
 *
 * 调计步参数必须用**真实走路**的 IMU 数据。但串口线很短，只能坐在桌前甩手，
 * 而"甩手"和"走路"的信号特征完全不是一回事（幅度、频率、周期性都不同），
 * 用甩手数据调出来的参数上真走路大概率不准。
 *
 * 所以让手表自己把数据录进 Flash：
 *
 *     带表出去走路（录） → 回来插 USB（导出） → 电脑上离线跑算法调参
 *
 * ## 录什么
 *
 * **只录原始加速度**，每帧 12 字节：
 *
 *     offset  type     字段
 *     0       uint32   时间戳（ms，设备开机起算；两段之间有间隔说明中间重启过）
 *     4       int16    ax (mg)
 *     6       int16    ay (mg)
 *     8       int16    az (mg)
 *     10      uint16   保留（写 0）
 *
 * 不录算法中间量。理由：**原始数据可以反复用不同参数重算**，
 * 想试什么阈值/滤波器系数组都能在 PC 上离线跑，不用重新走路。
 *
 * ## 存哪、怎么存
 *
 * - 存 SPIFFS 分区（`partitions.csv` 里的 `storage`，11MB，之前一直空着）
 * - 文件名 `imu_0001.bin` 递增；**写满就环形覆盖最旧的**
 * - RAM 里攒一批再写 Flash（省擦写寿命），但**每次写完都更新元数据**，
 *   所以掉电最多丢最后一个缓冲块（约 2 秒）
 *
 * ## 容量
 *
 * 50Hz × 12B = 600 B/s ≈ 2.16 MB/小时 → 11MB 能录 **约 5 小时**。
 *
 * ## 线程模型
 *
 * `imu_logger_feed()` 由 **motion 采样任务**在 50Hz 下调用，
 * 必须是**非阻塞**的（不能因为写 Flash 而拖慢采样，否则采到的数据时间戳会抖）。
 * 所以：
 *   - 只往 RAM 缓冲里塞，满了就落盘（落盘发生在调用线程里，但一次几十 ms、
 *     每 2 秒才一次，对 50Hz 采样影响可忽略；且落盘期间的样本照样进缓冲）
 *   - 元数据用单独的小文件，避免每次落盘都重写整个头部
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 一帧记录（12 字节，和文件里的布局一致） */
typedef struct {
    uint32_t t_ms;      ///< 时间戳（ms）
    int16_t  ax;        ///< 加速度 X（mg）
    int16_t  ay;        ///< 加速度 Y（mg）
    int16_t  az;        ///< 加速度 Z（mg）
    uint16_t reserved;  ///< 保留，写 0（便于以后加字段/保持 12 字节对齐）
} imu_log_rec_t;

/** @brief 录制状态（界面和串口命令都用它） */
typedef struct {
    bool     mounted;      ///< 文件系统是否挂载成功
    bool     recording;    ///< 是否正在录
    uint32_t samples;      ///< 本次已录帧数（含还在 RAM 缓冲里的）
    uint32_t seconds;      ///< 本次录制时长（秒，按时间戳算）
    uint32_t bytes;        ///< 本次已写入的字节数
    uint32_t files;        ///< 当前日志文件个数
    uint32_t max_files;    ///< 环形上限（写满后覆盖最旧的）
    uint32_t total_bytes;  ///< 文件系统总容量（字节）
    uint32_t used_bytes;   ///< 已用容量（字节）
    char     current[32];  ///< 当前正在写的文件名
} imu_logger_status_t;

/**
 * @brief 挂载文件系统并初始化记录器
 *
 * @note 幂等。挂载失败不算致命错误——手表照常跑，只是录制功能不可用。
 *       调用者可以用 `imu_logger_available()` 查。
 */
esp_err_t imu_logger_init(void);

/** @brief 录制功能是否可用（文件系统挂上了） */
bool imu_logger_available(void);

/**
 * @brief 开始录制
 *
 * @note 会新建一个日志文件；如果空间不够会先删最旧的。
 *       已经在录时返回 ESP_OK（幂等）。
 */
esp_err_t imu_logger_start(void);

/**
 * @brief 停止录制并**把缓冲里的数据落盘**
 *
 * @note 停止后一定要调它（而不是直接断电），否则最后约 2 秒的数据会丢。
 */
esp_err_t imu_logger_stop(void);

/**
 * @brief 喂一帧数据（由 motion 采样任务 50Hz 调用）
 *
 * @note **非阻塞**。没在录制时是空操作（一次 bool 判断）。
 *       buffer 是 NULL 时直接用 t_ms/accel_mg 组帧。
 */
void imu_logger_feed(uint32_t t_ms, const int32_t accel_mg[3]);

/**
 * @brief 取状态
 */
void imu_logger_get_status(imu_logger_status_t *out);

/**
 * @brief 删除所有日志文件（释放空间）
 */
esp_err_t imu_logger_erase_all(void);

/**
 * @brief 列出所有日志文件（给界面/串口命令显示）
 *
 * @param names  输出缓冲区数组
 * @param max    最多列几个
 * @return 实际列出的个数
 */
int imu_logger_list(char names[][32], int max);

/**
 * @brief 读一个日志文件的一段数据（给串口导出用）
 *
 * @param name     文件名
 * @param offset   起始字节偏移
 * @param buf      输出缓冲
 * @param len      想读多少字节
 * @param[out] got 实际读到多少字节
 */
esp_err_t imu_logger_read(const char *name, uint32_t offset,
                          uint8_t *buf, uint32_t len, uint32_t *got);

/** @brief 取某个日志文件的大小（字节）；不存在返回 0 */
uint32_t imu_logger_file_size(const char *name);

/**
 * @brief 把当前状态打一行日志（串口命令用）
 */
void imu_logger_dump_status(void);

#ifdef __cplusplus
}
#endif
