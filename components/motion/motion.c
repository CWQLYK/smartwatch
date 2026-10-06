/**
 * @file motion.c
 * @brief 运动服务实现 —— 50Hz 采 IMU，跑计步算法，1Hz 推结果给 data_hub
 *
 * 采样节奏用 `vTaskDelayUntil` 而不是 `vTaskDelay`：
 *   计步算法依赖**采样间隔均匀**（带通滤波器的系数是按固定采样率算的）。
 *   用 vTaskDelay 的话，每次调度抖动都会累积，实际采样率会漂，
 *   滤波器的截止频率跟着偏 —— 表现为"计步忽多忽少"，很难查。
 */
#include "motion.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "qmi8658c.h"
#include "data_hub.h"
#include "settings.h"

static const char *TAG = "MOTION";

/* ==================== 参数 ==================== */
#define MOTION_SAMPLE_HZ     50      /* 采样率：走路信号 0.5-3.5Hz，50Hz 够用 */
#define MOTION_TASK_PRIO     6       /* 比 data_hub(5) 高一点，保证采样不被挤 */
#define MOTION_TASK_STACK    4096
#define MOTION_PUBLISH_MS    1000    /* 推给 data_hub 的节流周期 */

/**
 * @brief 加速度量程
 *
 * ±8g（4096 LSB/g）是计步的推荐档：
 *   走路冲击约 0.15–0.6g，跑步也就 2–3g，±8g 余量足够；
 *   同时分辨率（1/4096 g ≈ 0.24mg）远小于我们要检测的 60mg 门限。
 */
#define MOTION_AFS           QMI8658_AFS_8G

/* ==================== 内部状态 ==================== */
static step_counter_t   s_sc;
static SemaphoreHandle_t s_lock   = NULL;
static TaskHandle_t      s_task   = NULL;
static volatile bool     s_imu_ok = false;

static motion_status_t   s_pub;      /* 最近一次对外公布的状态 */

/* ==================== 用户参数 ==================== */
void motion_apply_user_params(void)
{
    const uint16_t h = (uint16_t)settings_get_or_default(SETTINGS_USER_HEIGHT);
    const uint16_t w = (uint16_t)settings_get_or_default(SETTINGS_USER_WEIGHT);
    /*
     * 性别影响卡路里公式的系数。设置里目前没有这一项（P1 不做），
     * 先按男性系数走 —— 差别约 5%，对"参考值"来说可以接受。
     * 以后加设置项时把这里换成 settings_get_or_default(SETTINGS_USER_SEX) 即可。
     */
    const bool is_male = true;

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    step_counter_set_user(&s_sc, h, w, is_male);
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "用户参数：身高 %u cm，体重 %u kg（性别系数按%s）",
             h, w, is_male ? "男" : "女");
}

/* ==================== 采样任务 ==================== */
static void motion_task(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(1000 / MOTION_SAMPLE_HZ);
    TickType_t last_wake    = xTaskGetTickCount();
    uint32_t   pub_acc      = 0;

    ESP_LOGI(TAG, "运动采样任务启动（%d Hz）", MOTION_SAMPLE_HZ);

    for (;;) {
        vTaskDelayUntil(&last_wake, period);

        qmi8658_sample_t smp;
        if (qmi8658_read(&smp) != ESP_OK) {
            continue;                   /* 单次读失败不致命，下一拍再来 */
        }

        /*
         * 原始码值 → mg。
         * 驱动给的是 int16 原始值，换算要带上量程（LSB/g 与量程有关），
         * 所以用驱动提供的 qmi8658_accel_raw_to_mg()，别自己乘系数。
         */
        const int32_t accel_mg[3] = {
            qmi8658_accel_raw_to_mg(smp.ax, MOTION_AFS),
            qmi8658_accel_raw_to_mg(smp.ay, MOTION_AFS),
            qmi8658_accel_raw_to_mg(smp.az, MOTION_AFS),
        };

        if (s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        step_counter_update(&s_sc, accel_mg,
                            (uint32_t)(esp_timer_get_time() / 1000));
        if (s_lock) {
            xSemaphoreGive(s_lock);
        }

        /*
         * 节流对外公布：步数每秒变一次就够界面用了。
         * 50Hz 全推的话 UI 每秒重画 50 次，纯浪费电。
         */
        pub_acc += (1000 / MOTION_SAMPLE_HZ);
        if (pub_acc < MOTION_PUBLISH_MS) {
            continue;
        }
        pub_acc = 0;

        motion_status_t st;
        motion_get_status(&st);

        /* 只有真的变了才通知，避免没走路时也每秒刷一次界面 */
        static uint32_t last_steps = UINT32_MAX;
        if (st.steps != last_steps) {
            last_steps = st.steps;
            data_hub_set_steps((int)st.steps, (int)st.kcal, (int)st.distance_m);
        }

        /* 把状态也存一份，串口命令随时能查 */
        s_pub = st;
    }
}

/* ==================== 对外接口 ==================== */
esp_err_t motion_init(void)
{
    if (s_task) {
        return ESP_OK;                  /* 幂等 */
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "运动服务锁创建失败");
    }

    /* 默认参数（合成模型扫出来的起始值，未用真实腕部数据标定） */
    step_counter_init(&s_sc, NULL);

    /* 把用户身高体重喂进去（影响步长/卡路里） */
    motion_apply_user_params();

    /*
     * IMU 初始化失败**不算错误** —— 手表其余功能照常，
     * 步数会一直是 "--"（data_hub 里 steps.available=false）。
     */
    esp_err_t err = qmi8658_init();
    if (err != ESP_OK) {
        s_imu_ok = false;
        ESP_LOGW(TAG, "IMU 初始化失败（%s），步数将显示为 --", esp_err_to_name(err));
        return ESP_OK;
    }

    /*
     * 加速度计：±8g + 62.5Hz ODR + 开 LPF
     *
     * ⚠️ ODR 有两个坑（imu 组件文档里写了）：
     *   1. 同一个 ODR 编码在"只开加速度"和"6DOF"下实际速率不同
     *      （6DOF 下 0x07 是 50Hz 量级，单加速度下是 62.5Hz）。
     *      我们陀螺关掉了，所以取 62.5Hz 这一档最接近目标。
     *   2. 采样率必须和 step_counter 配置里的 sample_rate_hz 对得上，
     *      否则带通滤波器的截止频率会偏，计步会忽多忽少。
     */
    ESP_RETURN_ON_ERROR(qmi8658_config_accel(MOTION_AFS, QMI8658_AODR_62_5HZ,
                                             true, QMI8658_LPF_2_66PCT),
                        TAG, "配置加速度计失败");
    ESP_RETURN_ON_ERROR(qmi8658_enable_accel(true), TAG, "使能加速度计失败");
    /* 计步只用加速度，陀螺关掉省电 */
    (void)qmi8658_enable_gyro(false);

    s_imu_ok = true;

    /*
     * 先把"IMU 已就绪、当前 0 步"推给 data_hub。
     *
     * ⚠️ 不做这一步的话，界面上的步数会一直是 "--"：
     *    采样任务只在**步数变化**时才推送（省得每秒白刷界面），
     *    而没人走路时步数永远是 0 → 永远不推送 → data_hub 里
     *    steps.available 一直是 false → 界面显示占位符。
     *    第一次推送之后 available 才变 true，界面才开始显示 "0"。
     */
    data_hub_set_steps(0, 0, 0);

    if (xTaskCreate(motion_task, "motion", MOTION_TASK_STACK, NULL,
                    MOTION_TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "运动任务创建失败");
    }

    ESP_LOGI(TAG, "运动服务就绪（%d Hz 采样，计步阈值 %" PRId32 "mg）",
             MOTION_SAMPLE_HZ, s_sc.cfg.peak_thresh_mg);
    return ESP_OK;
}

void motion_get_status(motion_status_t *out)
{
    if (!out) {
        return;
    }

    step_status_t st = { 0 };
    int32_t sig = 0, thr = 0;

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    step_counter_get(&s_sc, &st);
    sig = s_sc.ac_dbg;
    thr = s_sc.thr_dbg;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    memset(out, 0, sizeof(*out));
    out->imu_ok        = s_imu_ok;
    out->running       = (s_task != NULL);
    out->steps         = st.steps;
    out->steps_pending = st.steps_pending;
    out->cadence_spm   = st.cadence_spm;
    out->activity      = st.activity;
    out->stride_cm     = st.stride_cm;
    out->distance_m    = st.distance_cm / 100;
    /* 毫千卡 → 千卡（整数） */
    out->kcal          = st.calories_mkcal / 1000;
    out->sig_mg        = sig;
    out->thr_mg        = thr;
}

void motion_reset_steps(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /*
     * 只清步数相关状态，配置和用户参数保留。
     * step_counter_init(NULL) 会重置成默认配置，那样会把用户身高体重也清掉，
     * 所以这里手动清计数部分。
     */
    s_sc.st.steps          = 0;
    s_sc.st.steps_pending  = 0;
    s_sc.st.distance_cm    = 0;
    s_sc.st.calories_mkcal = 0;
    xSemaphoreGive(s_lock);

    data_hub_set_steps(0, 0, 0);
    ESP_LOGI(TAG, "步数已清零");
}

void motion_dump_telemetry(void)
{
    motion_status_t st;
    motion_get_status(&st);

    /* 逗号分隔，方便直接粘进表格做调参 */
    ESP_LOGI(TAG, "TELEM,%u,%d,%d,%d,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32,
             (unsigned)(esp_timer_get_time() / 1000),
             (int)st.activity, (int)st.sig_mg, (int)st.thr_mg,
             st.cadence_spm, st.steps, st.steps_pending, st.distance_m);
}
