/**
 * @file qmi8658c.h
 * @brief QMI8658C（QST）六轴 IMU 驱动 + 腕戴计步/距离/卡路里估算
 *
 * 寄存器定义出处：**QMI8658A Datasheet Rev A**，QST Corporation，
 * Document#: 13-52-25。对应表号：
 *   - Table 20  WHO_AM_I / REVISION_ID
 *   - Table 22  CTRL1..CTRL9 配置寄存器
 *   - Table 23  STATUSINT / STATUS0 / STATUS1
 *   - Table 25  数据输出寄存器（AX_L..GZ_H = 0x35..0x40）
 *   - Table 26  Tap / Pedometer 活动检测
 *   - Table 27  RESET 寄存器
 * 本地副本：相关资料/示例工程/.../SensorLib/datasheet/QMI8658A Datasheet Rev A.pdf
 *
 * 设计取舍：
 *   - 热路径全部整数/Q15 定点，不调用 float（ESP32-S3 有 FPU，但中断/任务里
 *     定点更可控，也便于移植）。
 *   - I2C 走 400kHz 共享总线，通过 board 组件挂设备。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#ifndef IMU_HOST_TEST
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "board.h"
#else
/* Host unit-test build: only the step-counter half is exercised, so the
 * ESP-IDF / board dependencies are stubbed (see test/stub/). */
#include "esp_err.h"
#include "board.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、寄存器地址（Datasheet Rev A, Table 22/23/25/27）
 * ================================================================== */

#define QMI8658_ADDR_LOW            0x6B   /* SA0 = 0 —— 本板用这个 */
#define QMI8658_ADDR_HIGH           0x6A   /* SA0 = 1 */

#define QMI8658_REG_WHO_AM_I        0x00   /* 期望 0x05 */
#define QMI8658_REG_REVISION_ID     0x01   /* 默认 0x7C */

#define QMI8658_REG_CTRL1           0x02   /* 串行接口 + 传感器使能 */
#define QMI8658_REG_CTRL2           0x03   /* 加速度：aST aFS<2:0> aODR<3:0> */
#define QMI8658_REG_CTRL3           0x04   /* 陀螺：  gST gFS<2:0> gODR<3:0> */
#define QMI8658_REG_CTRL5           0x06   /* LPF 配置 */
#define QMI8658_REG_CTRL7           0x08   /* 使能传感器 / syncSample */
#define QMI8658_REG_CTRL8           0x09   /* 运动检测控制 */
#define QMI8658_REG_CTRL9           0x0A   /* host command 接口 */

#define QMI8658_REG_CAL1_L          0x0B   /* host 标定 / 命令载荷 */
#define QMI8658_REG_CAL4_H          0x12

#define QMI8658_REG_FIFO_WTM_TH     0x13
#define QMI8658_REG_FIFO_CTRL       0x14
#define QMI8658_REG_FIFO_COUNT      0x15
#define QMI8658_REG_FIFO_STATUS     0x16
#define QMI8658_REG_FIFO_DATA       0x17

#define QMI8658_REG_STATUSINT       0x2D   /* bit7 = CTRL9 命令完成 */
#define QMI8658_REG_STATUS0         0x2E   /* bit0 aDA，bit6 gDA */
#define QMI8658_REG_STATUS1         0x2F

#define QMI8658_REG_TIMESTAMP_L     0x30
#define QMI8658_REG_TEMPERATURE_L   0x33

/* ---- 加速度 + 陀螺数据：16 位二进制补码，小端 ---- */
#define QMI8658_REG_AX_L            0x35
#define QMI8658_REG_AX_H            0x36
#define QMI8658_REG_AY_L            0x37
#define QMI8658_REG_AY_H            0x38
#define QMI8658_REG_AZ_L            0x39
#define QMI8658_REG_AZ_H            0x3A
#define QMI8658_REG_GX_L            0x3B
#define QMI8658_REG_GX_H            0x3C
#define QMI8658_REG_GY_L            0x3D
#define QMI8658_REG_GY_H            0x3E
#define QMI8658_REG_GZ_L            0x3F
#define QMI8658_REG_GZ_H            0x40

#define QMI8658_REG_RST_RESULT      0x4D   /* 复位成功读回 0x80 */
#define QMI8658_REG_TAP_STATUS      0x59
#define QMI8658_REG_STEP_CNT_L      0x5A   /* 片内计步器（24 位） */
#define QMI8658_REG_STEP_CNT_M      0x5B
#define QMI8658_REG_STEP_CNT_H      0x5C
#define QMI8658_REG_RESET           0x60   /* 写 0xB0 触发软复位 */

/* STATUS0 位（Table 23） */
#define QMI8658_STATUS0_aDA         (1u << 0)   /* 加速度新数据可用 */
#define QMI8658_STATUS0_gDA         (1u << 6)   /* 陀螺新数据可用 */

/* RESET 魔数（Table 27） */
#define QMI8658_RESET_MAGIC         0xB0
#define QMI8658_RST_RESULT_OK       0x80

/* CTRL7 位（Table 22） */
#define QMI8658_CTRL7_SYNC_SAMPLE   (1u << 7)
#define QMI8658_CTRL7_DRDY_DIS      (1u << 5)
#define QMI8658_CTRL7_GSN           (1u << 4)
#define QMI8658_CTRL7_gEN           (1u << 1)
#define QMI8658_CTRL7_aEN           (1u << 0)

/* CTRL1 位（Table 22） */
#define QMI8658_CTRL1_SIM_3WIRE     (1u << 7)   /* I2C 必须保持 0 */
#define QMI8658_CTRL1_ADDR_AI       (1u << 6)   /* 1 = 地址自增，突发读必需 */
#define QMI8658_CTRL1_BE            (1u << 5)   /* 1 = 大端读取 */

/* CTRL2 / CTRL3 字段掩码 */
#define QMI8658_ACCEL_FS_MASK       0x70u       /* bits 6:4  aFS<2:0> */
#define QMI8658_ACCEL_ODR_MASK      0x0Fu       /* bits 3:0  aODR<3:0> */
#define QMI8658_ACCEL_ST_MASK       0x80u       /* bit 7     aST */
#define QMI8658_GYRO_FS_MASK        0x70u       /* bits 6:4  gFS<2:0> */
#define QMI8658_GYRO_ODR_MASK       0x0Fu       /* bits 3:0  gODR<3:0> */

/* CTRL5 位 */
#define QMI8658_CTRL5_gLPF_EN       (1u << 4)
#define QMI8658_CTRL5_gLPF_MODE_S   5
#define QMI8658_CTRL5_aLPF_EN       (1u << 0)
#define QMI8658_CTRL5_aLPF_MODE_S   1

/* ---------------- 量程 / ODR 枚举 ---------------- */

/** 加速度量程（CTRL2.aFS<2:0>，Table 22）
 *  灵敏度 = 32768 / (满量程的一半)：±2g→16384 LSB/g，±16g→2048 LSB/g */
typedef enum {
    QMI8658_AFS_2G  = 0,   /* 16384 LSB/g */
    QMI8658_AFS_4G  = 1,   /*  8192 LSB/g */
    QMI8658_AFS_8G  = 2,   /*  4096 LSB/g —— 计步推荐 */
    QMI8658_AFS_16G = 3,   /*  2048 LSB/g */
} qmi8658_afs_t;

/**
 * 加速度 ODR（CTRL2.aODR<3:0>，Table 22）
 *
 * ⚠️ 关键坑：**实际速率取决于陀螺是否使能**。
 *    aODR  | 仅加速度   | 6DOF（aEN=1 且 gEN=1）
 *    0011  |   1000 Hz  |  896.8 Hz
 *    0100  |    500 Hz  |  448.4 Hz
 *    0101  |    250 Hz  |  224.2 Hz
 *    0110  |    125 Hz  |  112.1 Hz
 *    0111  |   62.5 Hz  |   56.05 Hz
 *    1000  |  31.25 Hz  |   28.025 Hz
 *    1100  |    128 Hz  |  N/A（低功耗，陀螺必须关）
 *    1101  |     21 Hz  |  N/A
 *    1110  |     11 Hz  |  N/A
 *    1111  |      3 Hz  |  N/A
 * 计步只开加速度时用 0101(250Hz) 再软件抽到 50Hz，或直接 0110(125Hz)。
 * 若同时开陀螺（6DOF），要按“6DOF”那一列算，否则采样率会差一倍。
 */
typedef enum {
    QMI8658_AODR_1000HZ   = 0x03,
    QMI8658_AODR_500HZ    = 0x04,
    QMI8658_AODR_250HZ    = 0x05,
    QMI8658_AODR_125HZ    = 0x06,
    QMI8658_AODR_62_5HZ   = 0x07,
    QMI8658_AODR_31_25HZ  = 0x08,
    QMI8658_AODR_LP_128HZ = 0x0C,   /* 低功耗模式：仅陀螺关闭时可用 */
    QMI8658_AODR_LP_21HZ  = 0x0D,
    QMI8658_AODR_LP_11HZ  = 0x0E,
    QMI8658_AODR_LP_3HZ   = 0x0F,
} qmi8658_aodr_t;

/** 陀螺 ODR（CTRL3.gODR<3:0>，Table 22）：括号内为对应的 6DOF 加速度率 */
typedef enum {
    QMI8658_GODR_7174_4HZ = 0x00,
    QMI8658_GODR_3587_2HZ = 0x01,
    QMI8658_GODR_1793_6HZ = 0x02,
    QMI8658_GODR_896_8HZ  = 0x03,
    QMI8658_GODR_448_4HZ  = 0x04,
    QMI8658_GODR_224_2HZ  = 0x05,
    QMI8658_GODR_112_1HZ  = 0x06,
    QMI8658_GODR_56_05HZ  = 0x07,
    QMI8658_GODR_28_025HZ = 0x08,
} qmi8658_godr_t;

/** LPF 带宽（CTRL5，Table 22）：占 ODR 的百分比 */
typedef enum {
    QMI8658_LPF_2_66PCT  = 0,   /* 2.66% of ODR */
    QMI8658_LPF_3_63PCT  = 1,
    QMI8658_LPF_5_39PCT  = 2,
    QMI8658_LPF_13_37PCT = 3,
} qmi8658_lpf_t;

/** 陀螺量程（CTRL3.gFS<2:0>）：灵敏度 = 32768 / (满量程/2) */
typedef enum {
    QMI8658_GFS_16DPS   = 0,    /* 2048   LSB/dps */
    QMI8658_GFS_32DPS   = 1,    /* 1024   LSB/dps */
    QMI8658_GFS_64DPS   = 2,    /*  512   LSB/dps */
    QMI8658_GFS_128DPS  = 3,    /*  256   LSB/dps */
    QMI8658_GFS_256DPS  = 4,    /*  128   LSB/dps */
    QMI8658_GFS_512DPS  = 5,    /*   64   LSB/dps */
    QMI8658_GFS_1024DPS = 6,    /*   32   LSB/dps */
} qmi8658_gfs_t;

/* ==================================================================
 * 二、计步器参数
 * ================================================================== */

#define STEP_MAX_INTERVALS 8

/**
 * @brief 计步器可调参数
 *
 * ⚠️ 下面 init 里填的是**起始值**，不是标定值。必须上板实测调整，
 *    调整优先级见 step_counter_init() 注释。
 */
typedef struct {
    uint16_t sample_rate_hz;      /* 喂数据的速率，默认 50 */
    int32_t  peak_thresh_mg;      /* 绝对检测地板（mg） */
    int32_t  start_thresh_mg;     /* 静止态进入门限（mg，滞回上沿） */
    int32_t  end_thresh_mg;       /* 退出运动态门限（mg，滞回下沿） */
    int32_t  sigma_mul_x100;      /* 自适应项：thr = 包络 + k*sigma，k*100 */
    int32_t  crest_min_x100;      /* 峰值/σ 最小比（波峰因数），*100 */
    int32_t  peak_rel_pct;        /* 峰值相对近期最大值的最小百分比 */
    uint16_t refractory_pct;      /* 最小步间隔 = 学习到的步周期 * 该百分比 */
    uint16_t refractory_ms;       /* 最小步间隔绝对下限（ms） */
    uint16_t strong_step_ms;      /* 间隔小于此值需更大峰值（0=禁用） */
    int32_t  strong_peak_mg;      /* 上面那个“更大峰值” */
    uint16_t reversal_pct;        /* 从峰值回落该百分比即确认峰 */
    uint16_t peak_timeout_ms;     /* 峰值最长挂起时间 */
    uint32_t adaptive_reset_ms;   /* 静默多久回到静止态 */
    uint8_t  rhythm_window;       /* 节律窗口步数（3..8） */
    int32_t  rhythm_cv_x1000;     /* 步间隔 CV 上限，*1000（260 = 26%） */
    int32_t  commit_after;        /* 攒够几个间隔才确认计步 */
    int32_t  cadence_min_spm;     /* 合理步频范围 */
    int32_t  cadence_max_spm;
    /* ---- 距离 / 卡路里所需的用户参数 ---- */
    uint16_t user_height_cm;
    uint16_t user_weight_kg;
    uint8_t  user_is_male;
} step_config_t;

/** @brief 计步输出状态 */
typedef struct {
    uint32_t steps;               /* 已确认步数 */
    uint32_t steps_pending;       /* 待确认（节律未过）步数 */
    uint32_t distance_cm;         /* 累计距离，cm */
    uint32_t calories_mkcal;      /* 累计活动热量，毫千卡（整数，避免浮点） */
    int32_t  cadence_spm;         /* 当前步频，步/分 */
    uint8_t  activity;            /* 0 静止 / 1 走路 / 2 跑步 */
    int32_t  stride_cm;           /* 当前步长估计，cm */
} step_status_t;

/** @brief 计步器实例（约 270 字节，可静态分配） */
typedef struct {
    step_config_t cfg;
    step_status_t st;

    int32_t  grav_lp;             /* 重力跟踪（Q10 g） */

    int32_t  bp_x1[2], bp_x2[2], bp_y1[2], bp_y2[2];   /* 带通状态 */

    int32_t  bp_mean;             /* bp 均值 EMA（Q10） */
    int64_t  bp_var_sq;           /* (bp-均值)^2 的 EMA（Q20 g²） */
    int32_t  bp_abs_dc;           /* |bp| 包络 EMA（Q10） */
    uint32_t adapt_n;             /* 统计样本计数 */

    uint8_t  active;              /* 1 = 运动态 */
    uint8_t  fresh;               /* 1 = 用 start_thresh */
    uint8_t  above_cnt;
    uint8_t  activity;
    uint32_t last_step_ms;
    uint32_t last_motion_ms;
    uint32_t ready_ms;            /* 带通建立完成时刻 */
    int32_t  settle_max;

    uint8_t  rising;
    int32_t  peak_cand;
    int32_t  peak_deferred;
    uint32_t peak_cand_ms;
    int32_t  last_bp;
    int32_t  peak_shadow;
    int32_t  peak_shadow_disp;
    uint16_t cadence_period_ms;   /* 学习到的步周期 */
    int32_t  thr_dbg;             /* 最近门限（遥测用） */
    int32_t  ac_dbg;              /* 去重力后信号（遥测用） */
    int32_t  bp_dbg;              /* 带通输出（遥测用） */

    uint16_t intervals[STEP_MAX_INTERVALS];
    uint8_t  n_intervals;
    uint8_t  head;
    int32_t  cadence_lp;

    uint32_t pending;
    int64_t  stride_num;          /* Σ(步长) << 8 */
    int64_t  stride_den;          /* 计数 << 8 */
} step_counter_t;

/** @brief 一次 IMU 采样 */
typedef struct {
    int16_t ax, ay, az;           /* 原始码值 */
    int16_t gx, gy, gz;           /* 原始码值 */
    float   temp_c;
} qmi8658_sample_t;

/* ==================================================================
 * 三、驱动 API
 * ================================================================== */

/**
 * @brief 初始化 IMU：挂总线 → 校验 WHO_AM_I → 软复位 → 开地址自增
 * @note  幂等保护由调用方负责；重复调用会重复挂设备。
 *        WHO_AM_I 不是 0x05 返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t qmi8658_init(void);

/** @brief 软复位：写 0xB0 到 0x60，轮询 0x4D 等 0x80（手册 §3.3：最长 15ms） */
esp_err_t qmi8658_reset(void);

/** @brief 配置加速度计（CTRL2 一次性整字节写，CTRL5 配 LPF） */
esp_err_t qmi8658_config_accel(qmi8658_afs_t fs, qmi8658_aodr_t odr,
                               bool lpf_en, qmi8658_lpf_t lpf);

/** @brief 配置陀螺（CTRL3 整字节写，CTRL5 配 LPF） */
esp_err_t qmi8658_config_gyro(qmi8658_gfs_t fs, qmi8658_godr_t odr,
                              bool lpf_en, qmi8658_lpf_t lpf);

esp_err_t qmi8658_enable_accel(bool en);   /* CTRL7.aEN */
esp_err_t qmi8658_enable_gyro(bool en);    /* CTRL7.gEN */

/**
 * @brief 突发读 0x35..0x40 共 12 字节（AX_L..GZ_H）
 * @note  依赖 CTRL1.ADDR_AI = 1，否则会一直读到同一个寄存器。
 */
esp_err_t qmi8658_read(qmi8658_sample_t *out);

/** @brief 读 STATUS0（0x2E），bit0 = aDA，bit6 = gDA */
esp_err_t qmi8658_read_status0(uint8_t *status0);

/** @brief 原始值 → mg（定点）：raw * 1000 / lsb_per_g */
static inline int32_t qmi8658_accel_raw_to_mg(int16_t raw, qmi8658_afs_t fs)
{
    /* lsb_per_g = 32768 >> fs  (2g:16384, 4g:8192, 8g:4096, 16g:2048) */
    int32_t lsb = 32768 >> (int32_t)fs;
    return ((int32_t)raw * 1000) / lsb;
}

/** @brief 原始值 → 0.001 dps（定点）：raw * 1000 / lsb_per_dps */
static inline int32_t qmi8658_gyro_raw_to_mdps(int16_t raw, qmi8658_gfs_t fs)
{
    int32_t full = 16 << (int32_t)fs;      /* 16,32,64,...1024 dps */
    int32_t lsb = 32768 / full;            /* 2048,1024,512,...32 */
    return ((int32_t)raw * 1000) / lsb;
}

/* ==================================================================
 * 四、计步 API
 * ================================================================== */

/** @brief 初始化计步器；cfg 传 NULL 用内置起始参数 */
void step_counter_init(step_counter_t *sc, const step_config_t *cfg);

/** @brief 设定（或重启）带通建立窗口：now_ms + 600ms 内不计步 */
void step_counter_settle(step_counter_t *sc, uint32_t now_ms);

/** @brief 设置用户身高/体重/性别（用于步长与卡路里） */
void step_counter_set_user(step_counter_t *sc, uint16_t height_cm,
                           uint16_t weight_kg, bool is_male);

/**
 * @brief 喂一个加速度样本
 * @param accel_mg  三轴加速度，单位 **mg**（用 qmi8658_accel_raw_to_mg 换算）
 * @param now_ms    单调递增毫秒时间戳（esp_timer_get_time()/1000）
 */
void step_counter_update(step_counter_t *sc, const int32_t accel_mg[3],
                         uint32_t now_ms);

/** @brief 取当前状态快照 */
void step_counter_get(const step_counter_t *sc, step_status_t *out);

#ifdef __cplusplus
}
#endif
