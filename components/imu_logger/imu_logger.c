/**
 * @file imu_logger.c
 * @brief IMU 数据记录器实现
 *
 * ## 设计要点
 *
 * 1. **RAM 缓冲 + 批量落盘**：50Hz 下每帧都写 Flash 会很快写坏（SPIFFS 一个
 *    扇区 4KB，每帧都改元数据意味着每秒几十次擦写）。所以攒够一批再写，
 *    掉电代价是丢最后一个缓冲块（约 2 秒）。
 *
 * 2. **元数据和数据分开存**：`/spiffs/rec_meta.bin` 记录"当前文件、已录帧数"。
 *    如果每次落盘都重写整个文件头，就得为了改 8 个字节搬动 2MB 数据。
 *
 * 3. **采样线程里不做 malloc**：缓冲是静态数组，避免堆碎片和不确定的延迟。
 *
 * 4. **环形覆盖**：文件名递增，写到上限就回头覆盖 `imu_0001.bin`。
 *    这样永远不会"录不上"，代价是早期数据被冲掉（用户已确认可以接受）。
 */
#include "imu_logger.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "IMU_LOG";

/* ==================== 参数 ==================== */
#define LOG_MOUNT_POINT   "/spiffs"
#define LOG_FILE_FMT      LOG_MOUNT_POINT "/imu_%04d.bin"
#define LOG_META_PATH     LOG_MOUNT_POINT "/rec_meta.bin"
#define LOG_MAX_FILES     9999

/*
 * RAM 缓冲。
 *
 * 现在（LOG_FLUSH_EVERY_FRAME=1）每帧直接落盘，缓冲只当"单帧暂存"用。
 * 保留 341 帧的容量是给"以后想改回攒批"留的余地 ——
 * 攒批本身没问题，问题在于**复位后续录会从缓冲边界开始错乱**（已踩过）。
 * 真要改回攒批，必须同时修好续录逻辑（比如续录时把缓冲清空并重新对齐）。
 */
#define LOG_BUF_BYTES     4092                  /* 341 帧，按 12 字节/帧 */
#define LOG_BUF_RECS      (LOG_BUF_BYTES / sizeof(imu_log_rec_t))

/* 元数据魔数（用于判断文件是否是我们写的、版本是否对得上） */
#define LOG_META_MAGIC    0x494D5531u           /* "IMU1" */

/**
 * @brief 落盘策略：**每帧都写盘**
 *
 * ## 为什么从"攒批落盘"改成"每帧落盘"
 *
 * 最初为了省 Flash 擦写寿命，用的是"RAM 攒够 341 帧（约 6.8 秒）再写盘"。
 * 实测发现：**插 USB 会复位芯片**，复位后自动续录时，从缓冲边界
 * （约第 331 帧）开始的数据会**错乱** —— 时间戳变乱值、轴数据错位。
 * 更坑的是校验和照样通过（数据确实写进去了，只是内容是坏的），
 * 所以纯看"校验失败行数=0"完全发现不了。
 *
 * 重估了代价，发现"攒批"根本不值得：
 *   - 数据率只有 50Hz x 12B = **600 B/s**，一帧一写也就 50 次/秒
 *   - 内部 Flash 擦写寿命 10 万次；按每次录 1 小时算，
 *     要把一个扇区写坏需要**连续录几百小时**
 *   - 而攒批的代价是：掉电丢最多 6.8 秒 + 续录边界错乱（已经踩到）
 *
 * 结论：宁可多写几次 Flash，也不要"数据看着有、其实是坏的"。
 * 这类"静默错误数据"对调参是致命的 —— 会把你带到完全错误的方向。
 */
#define LOG_FLUSH_EVERY_FRAME   1

/* ==================== 内部状态 ==================== */
typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t  cur_index;     /* 当前写到第几个文件；-1 = 没在录 */
    uint32_t cur_recs;      /* 当前文件已落盘的帧数 */
    uint32_t total_recs;    /* 本次录制总帧数（含缓冲） */
    uint32_t start_ms;      /* 本次录制起始时间戳 */
    uint32_t last_ms;       /* 最近一帧时间戳（已含 time_offset） */
    /*
     * 跨复位的时间偏移。
     *
     * ⚠️ 必需：时间戳来自 esp_timer_get_time()，**每次开机从 0 重新算**。
     *    所以"录了 6 秒 → 插 USB 复位 → 续录"时，第二段的 t_ms 会从
     *    很小重新开始，直接写进同一个文件就会出现**时间戳倒退**
     *    （实测相邻两帧差 -16805 ms），后处理时整段数据都废了。
     *    续录时把 offset 设成 last_ms - 当前ms + 1，
     *    保证续录后的第一帧比上一帧大，时间戳在整个文件里单调递增。
     */
    uint32_t time_offset;
    uint32_t crc;           /* 简单校验（保留，当前恒 0） */
} log_meta_t;

static bool              s_mounted   = false;
static bool              s_recording = false;
static SemaphoreHandle_t s_lock      = NULL;

static log_meta_t        s_meta;
static FILE             *s_fp        = NULL;

/* 采样线程写入、落盘线程读取的缓冲（都在同一个任务里，所以不需要锁保护内容） */
static imu_log_rec_t     s_buf[LOG_BUF_RECS];
static uint32_t          s_buf_n     = 0;

static uint32_t          s_fs_total  = 0;
static uint32_t          s_fs_used   = 0;
static uint32_t          s_file_cnt  = 0;

/* ==================== 工具 ==================== */

/** @brief 生成第 idx 个文件名 */
static void make_name(int idx, char *out, size_t out_len)
{
    snprintf(out, out_len, LOG_FILE_FMT, idx);
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

static uint32_t file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fclose(f);
    return (sz > 0) ? (uint32_t)sz : 0;
}

/**
 * @brief 列出 /spiffs 下所有 imu_NNNN.bin
 *
 * @param idx_out  输出文件名对应的序号数组（可为 NULL）
 * @param max      最多输出多少个
 * @return 找到的日志文件个数
 *
 * ⚠️ **不要用"for i in 1..9999 逐个 fopen() 探测"**（我第一版就是那么写的，
 *    结果开机卡死 4.5 秒并触发任务看门狗）：
 *    - 9999 次 fopen 是 9999 次 Flash 读，SPIFFS 每次都要查目录项
 *    - 更致命的是 **Flash 操作期间不喂任务看门狗**（IDLE0 得不到调度），
 *      实测 4.5 秒内一次都没喂 → task_wdt 每 5 秒报一次，串口命令行起不来
 *    正确做法是用 readdir 真正遍历目录，代价是目录项数量级的。
 */
static int list_imu_files(int *idx_out, int max)
{
    DIR *dir = opendir(LOG_MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "打不开目录 %s", LOG_MOUNT_POINT);
        return 0;
    }

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int idx = 0;
        /* 名字形如 imu_0001.bin；用 %n 确认整串都被匹配上 */
        int consumed = 0;
        if (sscanf(ent->d_name, "imu_%4d.bin%n", &idx, &consumed) == 1 &&
            consumed == (int)strlen(ent->d_name) && idx >= 1) {
            if (idx_out && n < max) {
                idx_out[n] = idx;
            }
            n++;
        }
    }
    closedir(dir);
    return n;
}

/** @brief 统计文件系统用量和日志文件个数 */
static void scan_fs(void)
{
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        s_fs_total = (uint32_t)total;
        s_fs_used  = (uint32_t)used;
    }
    s_file_cnt = (uint32_t)list_imu_files(NULL, 0);
}

/* ==================== 元数据 ==================== */
static esp_err_t meta_save(void)
{
    FILE *f = fopen(LOG_META_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "写元数据失败（打开 %s）", LOG_META_PATH);
        return ESP_FAIL;
    }
    const size_t n = fwrite(&s_meta, 1, sizeof(s_meta), f);
    fclose(f);
    return (n == sizeof(s_meta)) ? ESP_OK : ESP_FAIL;
}

static void meta_clear(void)
{
    memset(&s_meta, 0, sizeof(s_meta));
    s_meta.magic      = LOG_META_MAGIC;
    s_meta.version    = 2;
    s_meta.cur_index  = -1;
    s_meta.cur_recs   = 0;
    s_meta.total_recs = 0;
    s_meta.time_offset = 0;
}

/**
 * @brief 启动时恢复元数据
 *
 * @note 如果上次是**掉电**（没走 stop），元数据里 cur_index >= 0，
 *       我们据此把"上一次没正常收尾的录制"标出来，让用户知道数据可能被截断。
 */
static void meta_load(void)
{
    meta_clear();

    FILE *f = fopen(LOG_META_PATH, "rb");
    if (!f) {
        return;
    }
    log_meta_t m;
    const size_t n = fread(&m, 1, sizeof(m), f);
    fclose(f);

    if (n != sizeof(m) || m.magic != LOG_META_MAGIC || m.version != 2) {
        ESP_LOGW(TAG, "元数据无效（可能首次上电），忽略");
        return;
    }

    s_meta = m;

    if (s_meta.cur_index >= 0) {
        ESP_LOGW(TAG, "上次录制没有正常结束（文件 imu_%04d.bin，已有 %" PRIu32 " 帧），"
                      "稍后会自动续录",
                 (int)s_meta.cur_index, s_meta.cur_recs);
    }
}

/* ==================== 落盘 ==================== */
/**
 * @brief 把 RAM 缓冲里的帧追加到当前日志文件
 *
 * @note 调用者必须保证 s_fp 已打开。
 */
static esp_err_t flush_locked(void)
{
    if (!s_recording || s_buf_n == 0) {
        return ESP_OK;
    }
    if (!s_fp) {
        ESP_LOGE(TAG, "录制中但文件没打开");
        return ESP_FAIL;
    }

    const size_t bytes = s_buf_n * sizeof(imu_log_rec_t);
    const size_t written = fwrite(s_buf, 1, bytes, s_fp);
    if (written != bytes) {
        ESP_LOGE(TAG, "写日志失败（想写 %u 字节，实际 %u）",
                 (unsigned)bytes, (unsigned)written);
        s_buf_n = 0;
        return ESP_FAIL;
    }

    s_meta.cur_recs   += s_buf_n;
    s_meta.total_recs += s_buf_n;
    s_buf_n = 0;

    /* 每次落盘都刷一下元数据，掉电后能知道录到哪了 */
    fflush(s_fp);
    meta_save();
    return ESP_OK;
}

/* ==================== 文件滚动 ==================== */
/**
 * @brief 打开一个新的日志文件；空间不够就先删最旧的
 */
static esp_err_t open_new_file(void)
{
    /* 选一个文件名：接着上次的号往下走，环形回绕 */
    int next = (s_meta.cur_index >= 0) ? (s_meta.cur_index + 1) : 1;
    if (next > LOG_MAX_FILES) {
        next = 1;
        ESP_LOGW(TAG, "文件号回绕到 1，开始覆盖最旧的日志");
    }

    char name[32];
    make_name(next, name, sizeof(name));

    /*
     * 环形覆盖：如果目标文件已存在，删掉它腾空间。
     * 这里不检查"整体剩余空间"，因为 SPIFFS 的剩余空间查询不如
     * "直接删目标文件"来得可靠（也避免算错导致写失败）。
     */
    if (file_exists(name)) {
        remove(name);
        ESP_LOGI(TAG, "覆盖最旧日志 %s", name);
    }

    s_fp = fopen(name, "wb");
    if (!s_fp) {
        /* 可能是空间不足：删两个最旧的重试一次 */
        ESP_LOGW(TAG, "打开 %s 失败，尝试清理空间", name);

        int deleted = 0;
        for (int i = 1; i <= LOG_MAX_FILES && deleted < 2; i++) {
            char old[32];
            make_name(i, old, sizeof(old));
            if (i == next || !file_exists(old)) {
                continue;
            }
            remove(old);
            deleted++;
            ESP_LOGW(TAG, "  删掉 %s", old);
        }

        s_fp = fopen(name, "wb");
        ESP_RETURN_ON_FALSE(s_fp, ESP_FAIL, TAG,
                            "清理后仍无法打开 %s（文件系统满？）", name);
    }

    s_meta.cur_index = next;
    s_meta.cur_recs  = 0;
    meta_save();

    ESP_LOGI(TAG, "开始写入 %s", name);
    return ESP_OK;
}

/* ==================== 对外接口 ==================== */
esp_err_t imu_logger_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "记录器锁创建失败");
    }

    const esp_vfs_spiffs_conf_t conf = {
        .base_path              = LOG_MOUNT_POINT,
        .partition_label        = "storage",
        .max_files              = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        /* 挂载失败不算致命：手表照常跑，只是录制不可用 */
        ESP_LOGW(TAG, "SPIFFS 挂载失败（%s），IMU 录制功能不可用",
                 esp_err_to_name(err));
        return err;
    }

    s_mounted = true;

    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "存储就绪：%u KB 总容量，已用 %u KB（可录约 %u 分钟）",
                 (unsigned)(total / 1024), (unsigned)(used / 1024),
                 (unsigned)((total - used) / (50 * sizeof(imu_log_rec_t)) / 60));
    }

    meta_load();
    scan_fs();

    /*
     * ★ 自动续录（很重要，别删）
     *
     * 元数据里 cur_index >= 0 表示"上次录制没有正常结束"——也就是掉电/复位了。
     * 这种情况必须**自动接着录**，原因：
     *   插上 USB 线看数据时会触发一次芯片复位（USB-UART CHIP RESET），
     *   而复位会丢掉 RAM 里的"正在录制"标志。如果不自动续录，用户就会遇到
     *   "我明明点了开始录制，走完回来一看是空闲，什么都没录上"——
     *   而且完全不知道为什么（因为看起来命令都执行成功了）。
     *
     * 续录用 "ab"（追加）而不是 "wb"，避免把之前录的数据冲掉。
     */
    if (s_meta.cur_index >= 0) {
        char name[32];
        make_name(s_meta.cur_index, name, sizeof(name));

        s_fp = fopen(name, "ab");
        if (s_fp) {
            s_recording = true;

            /*
             * ⚠️ 复位后 start_ms 还是上一轮的旧值，而 last_ms 会用新时间戳，
             *    直接相减会得到 0 或负数 —— 表现成"明明录了 152 秒却显示 0 秒"。
             *    这里把基准挪到"现在"，时长从重连这一刻重新计。
             */
            const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

            /*
             * 关键：把时间戳基准接到上一次的末尾。
             * 加 1ms 保证严格大于上一帧（避免出现 0 间隔或负间隔）。
             */
            if (s_meta.last_ms >= now) {
                s_meta.time_offset += (s_meta.last_ms - now) + 1;
            }
            s_meta.start_ms = now;
            s_meta.last_ms  = now + s_meta.time_offset;

            /* 缓冲必须是空的，否则会把复位前的残帧写进来（错位的根源） */
            s_buf_n = 0;

            meta_save();

            ESP_LOGW(TAG, "**自动续录** %s（上次录制被复位打断，已有 %" PRIu32 " 帧）",
                     name, s_meta.cur_recs);
            ESP_LOGW(TAG, "不想续录就执行 rec stop（或用 rec erase 清空）");
        } else {
            ESP_LOGE(TAG, "续录失败：打不开 %s，录制处于停滞状态", name);
        }
    }

    ESP_LOGI(TAG, "IMU 记录器就绪（%u 个日志文件%s）",
             (unsigned)s_file_cnt, s_recording ? "，正在续录" : "");
    return ESP_OK;
}

bool imu_logger_available(void)
{
    return s_mounted;
}

esp_err_t imu_logger_start(void)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG,
                        "文件系统没挂载，无法录制");
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "记录器没初始化");

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_recording) {
        xSemaphoreGive(s_lock);
        return ESP_OK;                  /* 幂等 */
    }

    s_buf_n = 0;
    s_meta.total_recs  = 0;
    s_meta.time_offset = 0;          /* 新一次录制，时间戳重新计 */
    s_meta.start_ms    = (uint32_t)(esp_timer_get_time() / 1000);
    s_meta.last_ms     = s_meta.start_ms;

    esp_err_t err = open_new_file();
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }

    s_recording = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "开始录制 IMU（50Hz，每帧 %u 字节）",
             (unsigned)sizeof(imu_log_rec_t));
    return ESP_OK;
}

esp_err_t imu_logger_stop(void)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "记录器没初始化");

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_recording) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    /* ★ 关键：把缓冲里剩的落盘，否则最后几秒白录 */
    flush_locked();

    s_recording    = false;
    s_meta.cur_index = -1;              /* 标记"正常收尾"，下次上电不会报掉电 */
    meta_save();

    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }

    const uint32_t recs = s_meta.total_recs;
    const uint32_t dur  = (s_meta.last_ms - s_meta.start_ms) / 1000;

    xSemaphoreGive(s_lock);

    scan_fs();

    ESP_LOGI(TAG, "停止录制：共 %" PRIu32 " 帧 / 约 %" PRIu32 " 秒 / %" PRIu32 " 字节",
             recs, dur, recs * (uint32_t)sizeof(imu_log_rec_t));
    return ESP_OK;
}

void imu_logger_feed(uint32_t t_ms, const int32_t accel_mg[3])
{
    /*
     * 快路径：没在录就直接返回。
     * 这是 50Hz 下最常见的分支，所以放在最前面、不加锁、不做任何多余的事。
     */
    if (!s_recording) {
        return;
    }

    imu_log_rec_t r;
    r.t_ms     = t_ms + s_meta.time_offset;   /* 跨复位也保持单调递增 */
    r.ax       = (int16_t)accel_mg[0];
    r.ay       = (int16_t)accel_mg[1];
    r.az       = (int16_t)accel_mg[2];
    r.reserved = 0;

    bool need_flush = false;

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_recording) {
#if LOG_FLUSH_EVERY_FRAME
        /* 每帧直接落盘：掉电/复位最多丢这一帧，续录不会错位 */
        s_buf[0]  = r;
        s_buf_n   = 1;
        s_meta.last_ms = t_ms;
        need_flush = true;
#else
        if (s_buf_n < LOG_BUF_RECS) {
            s_buf[s_buf_n++] = r;
            s_meta.last_ms   = t_ms;
            need_flush = (s_buf_n >= LOG_BUF_RECS);
        }
#endif
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    /*
     * 落盘放在锁外单独再取一次锁：
     * 写盘要几毫秒，握着锁做会把 imu_logger_get_status() 之类的查询拖住。
     * 分两段之后，两段持锁时间都极短。
     */
    if (need_flush) {
        if (s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        flush_locked();
        if (s_lock) {
            xSemaphoreGive(s_lock);
        }
    }
}

void imu_logger_get_status(imu_logger_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    out->mounted   = s_mounted;
    out->recording = s_recording;
    /* 含还在缓冲里没落盘的帧 */
    out->samples   = s_meta.total_recs + s_buf_n;
    out->bytes     = (s_meta.cur_recs + s_buf_n) * (uint32_t)sizeof(imu_log_rec_t);
    out->seconds   = (s_meta.last_ms >= s_meta.start_ms)
                         ? (s_meta.last_ms - s_meta.start_ms) / 1000 : 0;
    if (s_meta.cur_index > 0) {
        make_name(s_meta.cur_index, out->current, sizeof(out->current));
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    out->files       = s_file_cnt;
    out->max_files   = LOG_MAX_FILES;
    out->total_bytes = s_fs_total;
    out->used_bytes  = s_fs_used;
}

esp_err_t imu_logger_erase_all(void)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "文件系统没挂载");

    if (s_recording) {
        ESP_LOGW(TAG, "正在录制，先停止再擦除");
        imu_logger_stop();
    }

    int idx[128];
    const int n = list_imu_files(idx, 128);
    int removed = 0;
    char name[32];
    for (int i = 0; i < n; i++) {
        make_name(idx[i], name, sizeof(name));
        if (remove(name) == 0) {
            removed++;
        }
    }
    remove(LOG_META_PATH);
    meta_clear();
    scan_fs();

    ESP_LOGI(TAG, "已删除 %d 个日志文件", removed);
    return ESP_OK;
}

int imu_logger_list(char names[][32], int max)
{
    if (!names || max <= 0) {
        return 0;
    }

    int idx[128];
    const int n = list_imu_files(idx, 128);
    const int out = (n < max) ? n : max;

    /* 按序号从小到大排序，让 `rec list` 的输出顺序稳定 */
    for (int i = 1; i < out; i++) {
        const int key = idx[i];
        int j = i - 1;
        while (j >= 0 && idx[j] > key) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }

    for (int i = 0; i < out; i++) {
        snprintf(names[i], 32, LOG_FILE_FMT, idx[i]);
    }
    return out;
}

esp_err_t imu_logger_read(const char *name, uint32_t offset,
                          uint8_t *buf, uint32_t len, uint32_t *got)
{
    ESP_RETURN_ON_FALSE(name && buf && got, ESP_ERR_INVALID_ARG, TAG, "参数为空");
    *got = 0;

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    /* 导出前先把缓冲落盘，否则最后几秒读不到 */
    const bool was_rec = s_recording;
    if (was_rec) {
        flush_locked();
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    FILE *f = fopen(name, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_ERR_NOT_FOUND, TAG, "打不开 %s", name);

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    const size_t n = fread(buf, 1, len, f);
    fclose(f);

    *got = (uint32_t)n;
    return ESP_OK;
}

uint32_t imu_logger_file_size(const char *name)
{
    return name ? file_size(name) : 0;
}

void imu_logger_dump_status(void)
{
    imu_logger_status_t st;
    imu_logger_get_status(&st);

    if (!st.mounted) {
        ESP_LOGI(TAG, "存储未挂载，录制功能不可用");
        return;
    }

    ESP_LOGI(TAG, "录制状态：%s", st.recording ? "**进行中**" : "空闲");
    if (st.recording || st.samples) {
        ESP_LOGI(TAG, "  本次：%" PRIu32 " 帧 / %" PRIu32 " 秒 / %" PRIu32 " 字节（%s）",
                 st.samples, st.seconds, st.bytes,
                 st.current[0] ? st.current : "-");
    }
    ESP_LOGI(TAG, "  文件：%" PRIu32 " 个   容量：%" PRIu32 "/%" PRIu32 " KB",
             st.files, st.used_bytes / 1024, st.total_bytes / 1024);
}
