/**
 * @file wifi_service.c
 * @brief WiFi 连接 + SNTP 校时实现
 *
 * ## 关键实现点
 *
 * 1. **按需初始化**：WiFi 协议栈吃 50KB+ RAM。开机就初始化纯属浪费
 *    （大部分人一天也不会去点 WiFi 页）。所以拆成
 *    `register_hooks()`（只挂钩子，几乎不要内存）和 `init()`（真正起协议栈）。
 *    用户点进 WiFi 页、或串口下命令时才 init。
 *
 * 2. **事件驱动**：连接结果靠 `WIFI_EVENT` / `IP_EVENT` 回调更新状态，
 *    界面只轮询状态快照，不用阻塞等待。
 *
 * 3. **SNTP 用回调而不是阻塞等待**：`esp_sntp` 是异步的，
 *    对到时在回调里写 RTC —— 这样界面不会卡住。
 *
 * 4. **凭据存独立的 NVS 命名空间**（`wifi`），不放 settings：
 *    settings 组件是"表驱动的 int32 项"，塞字符串进去要改它的设计。
 *    SSID/密码天然是字符串，单独存更干净。
 */
#include "wifi_service.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "WIFI";

#define WIFI_NVS_NS     "wifi"
#define WIFI_NVS_SSID   "ssid"
#define WIFI_NVS_PASS   "pass"

/* SNTP 服务器：国内可达性好的两个 */
#define SNTP_SERVER_1   "ntp.aliyun.com"
#define SNTP_SERVER_2   "cn.pool.ntp.org"

/* ==================== 内部状态 ==================== */
static bool              s_started = false;
static esp_netif_t      *s_netif   = NULL;
static SemaphoreHandle_t s_lock    = NULL;
/*
 * 断开事件信号量。
 *
 * 用来实现"断开 -> 等断开完成 -> 设配置 -> 连接"这个顺序。
 * 不等的话 esp_wifi_set_config() 会被驱动拒绝（"sta is connecting"），
 * 新密码根本生效不了 —— 这是用户实测踩到的坑。
 */
static SemaphoreHandle_t s_disc_sem = NULL;

/* 连接超时后需要在锁外做断开，用它传个信号（见 hook_get_status） */
static volatile bool s_conn_stuck = false;

static wifi_status_t     s_st;
static wifi_ap_t         s_aps[WIFI_SCAN_MAX];
static int               s_ap_count = 0;
static volatile bool     s_scanning = false;

/** SNTP 对时成功后的回调（由 app 层注册，用来写 RTC） */
typedef void (*wifi_time_synced_cb_t)(uint32_t utc_epoch);
static wifi_time_synced_cb_t s_time_cb = NULL;

/* ==================== 锁工具 ==================== */
static inline void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}
static inline void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* ==================== NVS 凭据 ==================== */
static esp_err_t creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, WIFI_NVS_SSID, ssid ? ssid : "");
    nvs_set_str(h, WIFI_NVS_PASS, pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t creds_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;                 /* 没存过 */
    }
    size_t n = ssid_len;
    err = nvs_get_str(h, WIFI_NVS_SSID, ssid, &n);
    if (err == ESP_OK) {
        n = pass_len;
        esp_err_t e2 = nvs_get_str(h, WIFI_NVS_PASS, pass, &n);
        if (e2 != ESP_OK) {
            pass[0] = '\0';         /* 开放网络：没存密码 */
        }
    }
    nvs_close(h);
    return err;
}

static void creds_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ==================== SNTP ==================== */
static void sntp_sync_cb(struct timeval *tv)
{
    if (!tv) {
        return;
    }
    const uint32_t epoch = (uint32_t)tv->tv_sec;

    lock();
    s_st.time_synced  = true;
    s_st.synced_epoch = epoch;
    s_st.synced_count++;
    s_st.syncing      = false;      /* 对时过程结束 */
    s_st.sync_failed  = false;
    unlock();

    ESP_LOGI(TAG, "SNTP 对时成功：UTC %" PRIu32 "（本地按 settings 时区换算）", epoch);

    /*
     * 交给 app 层写 RTC。
     * 这里不直接调 rtc_pcf_*：那会让 wifi_service 依赖 rtc 驱动，
     * 而"对到时间之后干什么"是 app 该决定的事（以后可能还要上报云端）。
     */
    if (s_time_cb) {
        s_time_cb(epoch);
    }
}

/* 对时超时（毫秒）。超过这个时间还没收到响应就报失败。
 * NTP 一般 1~3 秒回来，给 15 秒足够宽松。 */
#define SNTP_TIMEOUT_MS  15000

/*
 * 连接超时（毫秒）。
 *
 * ⚠️ 这个宏是为了修一个**连锁 bug**（用户实测报了三个症状，全是它引起的）：
 *
 *   卡在驱动的 "connecting" 状态时，驱动会拒绝一切操作：
 *       E wifi:sta_scan: STA is connecting, scan are not allowed!
 *       E wifi:sta is connecting, cannot set config
 *   于是：
 *     - 扫描不出 wifi（扫描被拒）
 *     - 输对密码也连不上（配置写不进去）
 *     - "重新对时"没反应（连不上 -> 拿不到 IP -> SNTP 不启动）
 *
 *   原来 esp_wifi_connect() 之后没有任何超时兜底，一旦卡住就**永久卡住**。
 *   15 秒足够正常关联 + DHCP（实测正常 3~5 秒）。
 */
#define CONN_TIMEOUT_MS  15000

static void sntp_mark_syncing(void)
{
    lock();
    s_st.syncing     = true;
    s_st.sync_failed = false;
    s_st.sync_start  = (uint32_t)(esp_timer_get_time() / 1000);
    unlock();
}

static void sntp_start(void)
{
    /*
     * ⚠️ 幂等检查必须在 sntp_mark_syncing() **之前**。
     *
     * 踩过的坑：把检查写在后面，而 mark_syncing() 已经把 syncing 置成 true，
     * 于是 `if (already) return;` **永远成立** ——
     * 手动点"重新对时"会被直接忽略，表现就是"点了完全没反应"。
     * 改这段的时候务必保持"先检查、后置位"的顺序。
     */
    lock();
    const bool already = s_st.syncing;
    unlock();
    if (already) {
        ESP_LOGI(TAG, "正在对时中，忽略重复请求");
        return;
    }

    /*
     * ★ 先标记"正在对时"再动手。
     *
     * 这一步是让界面能显示"对时中…"的关键：原来点了之后界面上
     * 什么都不变（time_synced 还是上次的 true，文字照样显示"已对时"），
     * 用户根本不知道按钮生效没有。
     */
    sntp_mark_syncing();

    if (esp_sntp_enabled()) {
        /*
         * ★ 彻底重建 SNTP 客户端，而不是用 esp_sntp_restart()。
         *
         * 为什么：实测"开机首次对时能成功，手动点重新对时却 15 秒超时"。
         *       `esp_sntp_restart()` 只重置 lwIP 的 SNTP 状态机，
         *       **不重新解析服务器名**，重启后有时就再也拿不到响应。
         *       stop + init 走的是和"首次成功"完全相同的路径，
         *       行为可预期。
         */
        ESP_LOGI(TAG, "重建 SNTP 客户端（不用 restart）");
        esp_sntp_stop();   /* 没有 esp_sntp_deinit，只有 stop */
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER_1);
    esp_sntp_setservername(1, SNTP_SERVER_2);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP 已启动（%s / %s）", SNTP_SERVER_1, SNTP_SERVER_2);
}

/* ==================== WiFi 事件 ==================== */
static void set_state(wifi_state_t st)
{
    lock();
    s_st.state = st;
    unlock();
}

/*
 * ==================================================================
 * v0.5.0 新增的两个内部标志
 * ==================================================================
 *
 * s_conn_used_saved
 *   本次连接用的密码是**从 NVS 回填的**（而不是用户刚输入的）。
 *   连接失败时要靠它决定"要不要把保存的凭据清掉"—— 用户明确要求：
 *   「如果连接已经保存的 wifi 连接失败，直接自动取消保存，
 *     避免 wifi 密码修改导致无法取消保存 WiFi」。
 *   只有"用的是保存的密码"才清；用户刚手输的密码连不上，
 *   不该把旧的保存记录也无辜删掉（那种情况本来也没保存）。
 *
 * s_user_disconnect
 *   本次断开是**用户主动点的**（断开连接 / 取消保存 / 扫描自救）。
 *   用来避免两个误判：
 *     1. 把用户主动断开当成"连接失败"，界面显示"连接失败"
 *     2. 用户主动断开时触发"自动取消保存"（那会把密码也删了，
 *        而"断开连接"的语义是**保留**密码）
 */
static volatile bool s_conn_used_saved  = false;
static volatile bool s_user_disconnect  = false;

/**
 * @brief 这次失败是不是"认证类"的（值得清掉保存的密码）
 *
 * 为什么要区分（而不是"任何失败都清"）：
 *   走到信号范围外、路由器关机，也会失败，但那些**和密码无关** ——
 *   那时候把密码删掉，回家还得重新输一遍，体验很差。
 *   所以只对"一看就是认证/握手问题"的 reason 自动清理；
 *   纯粹"找不到 AP"的两种 reason 保留凭据。
 *
 * reason 码（ESP-IDF）：
 *   2   AUTH_EXPIRE              认证过期（密码错最常见）
 *   4   ASSOC_EXPIRE             关联过期
 *   15  4WAY_HANDSHAKE_TIMEOUT   四次握手超时（密码错典型）
 *   202 AUTH_FAIL                认证失败
 *   203 ASSOC_FAIL               关联失败
 *   204 HANDSHAKE_TIMEOUT        握手超时
 *   211 NO_AP_FOUND_W_COMPATIBLE_SECURITY  加密方式不兼容（等于连不上）
 *   —— 保留：200 BEACON_TIMEOUT / 201 NO_AP_FOUND（纯粹没找到 AP）
 */
static bool reason_is_auth(int reason)
{
    switch (reason) {
    case 2: case 4: case 15: case 202: case 203: case 204: case 211:
        return true;
    default:
        return false;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGD(TAG, "STA 启动");
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "已关联 AP，等 DHCP");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *d =
                (const wifi_event_sta_disconnected_t *)data;
            const int reason = d ? d->reason : -1;
            ESP_LOGW(TAG, "断开（reason=%d）", reason);
            /* 通知正在等"断开完成"的 connect 流程 */
            if (s_disc_sem) {
                xSemaphoreGive(s_disc_sem);
            }

            /* ---------- 情况一：用户主动断开 ---------- */
            if (s_user_disconnect) {
                s_user_disconnect = false;
                /*
                 * ★ 不要把状态改成 FAILED。
                 *   否则用户点"断开连接"之后，界面会显示"连接失败（密码错…）"，
                 *   而实际上这正是他想要的结果。保持 IDLE。
                 */
                ESP_LOGI(TAG, "（用户主动断开，保持未连接状态）");
                lock();
                s_st.state = W_WIFI_IDLE;
                s_st.rssi  = 0;
                s_st.ip[0] = '\0';
                unlock();
                break;
            }

            /* ---------- 情况二：连接尝试失败 ---------- */
            lock();
            s_st.state = W_WIFI_FAILED;
            s_st.rssi  = 0;
            s_st.ip[0] = '\0';
            unlock();

            /*
             * ★ 用"已保存的密码"连接失败 -> 自动清掉保存的凭据。
             *
             * 用户要求的场景：路由器改了 WiFi 密码，手表里存着旧密码，
             * 于是永远连不上；而界面上能做的只有"重新扫描"，
             * 点了还是拿旧密码去连 —— 等于卡死，没法翻身。
             *
             * 清掉之后：界面下次点这个网络会**直接问密码**，就能救回来了。
             *
             * 只清"认证类"失败（见 reason_is_auth 的说明）：
             * 走到没信号的地方导致的失败不该把密码删掉。
             */
            if (s_conn_used_saved && reason_is_auth(reason)) {
                ESP_LOGW(TAG, "用已保存的密码连接失败（reason=%d）-> "
                              "清除保存的凭据，下次会重新询问密码", reason);
                creds_erase();
                lock();
                s_st.auto_reconnect = false;
                s_st.ssid[0]        = '\0';
                s_st.creds_cleared  = true;
                unlock();
            }
            s_conn_used_saved = false;

            /*
             * 注意：**不在这里无限重连**。
             * 用户明确要求"提供手动断开"，无脑重连会和它打架
             * （刚点完断开，它 1 秒后又自己连上，用户会以为按钮坏了）。
             * 自动重连只在开机 init 时做一次。
             */
            break;
        }

        case WIFI_EVENT_SCAN_DONE: {
            uint16_t n = WIFI_SCAN_MAX;
            wifi_ap_record_t *recs = calloc(WIFI_SCAN_MAX, sizeof(wifi_ap_record_t));
            if (!recs) {
                s_scanning = false;
                break;
            }
            if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                lock();
                s_ap_count = 0;
                for (int i = 0; i < n && s_ap_count < WIFI_SCAN_MAX; i++) {
                    /* 隐藏 SSID（空名字）不显示，点了也没用 */
                    if (recs[i].ssid[0] == '\0') {
                        continue;
                    }
                    wifi_ap_t *a = &s_aps[s_ap_count++];
                    strncpy(a->ssid, (const char *)recs[i].ssid, WIFI_SSID_MAX - 1);
                    a->ssid[WIFI_SSID_MAX - 1] = '\0';
                    a->rssi   = recs[i].rssi;
                    a->secure = (recs[i].authmode != WIFI_AUTH_OPEN);

                    /*
                     * 把加密方式和信道也打出来。
                     *
                     * 为什么单独记一行：threshold.authmode 设错会导致认证
                     * 失败，而失败日志只给一个数字（reason=2/202），
                     * 光看数字判断不出"是不是加密方式不匹配"。
                     * 有这行就能一眼对上号（例如 AP 是 WPA3 而阈值写的是
                     * WPA2_PSK，就会一直连不上却看不出原因）。
                     */
                    ESP_LOGI(TAG, "  AP '%s' authmode=%d(%s) ch=%d rssi=%d",
                             (const char *)recs[i].ssid, (int)recs[i].authmode,
                             recs[i].authmode == WIFI_AUTH_OPEN          ? "OPEN" :
                             recs[i].authmode == WIFI_AUTH_WEP           ? "WEP" :
                             recs[i].authmode == WIFI_AUTH_WPA_PSK       ? "WPA" :
                             recs[i].authmode == WIFI_AUTH_WPA2_PSK      ? "WPA2" :
                             recs[i].authmode == WIFI_AUTH_WPA_WPA2_PSK  ? "WPA/WPA2" :
                             recs[i].authmode == WIFI_AUTH_WPA3_PSK      ? "WPA3" :
                             recs[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK ? "WPA2/WPA3" :
                             recs[i].authmode == WIFI_AUTH_WAPI_PSK      ? "WAPI" : "其他",
                             recs[i].primary, recs[i].rssi);
                }
                unlock();
                ESP_LOGI(TAG, "扫描完成：%d 个可用 AP", s_ap_count);
            }
            free(recs);
            s_scanning = false;
            break;
        }

        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        lock();
        s_st.state = W_WIFI_CONNECTED;
        snprintf(s_st.ip, sizeof(s_st.ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_st.time_synced = false;
        s_st.syncing     = false;
        s_st.sync_failed = false;
        unlock();
        ESP_LOGI(TAG, "拿到 IP：%s，开始 SNTP 对时", s_st.ip);

        /* 连上就自动对时（用户要求：连上后自动校时） */
        sntp_start();
    }
}

/* ==================== 钩子实现 ==================== */
/**
 * @brief 把 WiFi 强制拉回"空闲"状态
 *
 * @note 用于"驱动卡在 connecting"时自救：断开并等断开事件，再复位状态。
 */
static void wifi_force_idle(const char *why)
{
    if (s_disc_sem) {
        xSemaphoreTake(s_disc_sem, 0);
    }
    /*
     * 这是我们自己（超时自救 / 扫描前清理）主动断的，不是连接失败：
     * 打上标志，免得事件回调把状态改成 FAILED、
     * 更免得触发"用保存的密码失败 -> 清凭据"那条逻辑。
     */
    s_user_disconnect = true;
    s_conn_used_saved = false;
    esp_wifi_disconnect();
    if (s_disc_sem) {
        if (xSemaphoreTake(s_disc_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "%s：等断开事件超时", why ? why : "force_idle");
        }
    }
    lock();
    s_st.state      = W_WIFI_IDLE;
    s_st.conn_start = 0;
    s_st.ip[0]      = '\0';
    unlock();
}

static int hook_scan_start(void)
{
    /*
     * ★ 按需启动协议栈（重要，别再退回"直接 return -1"）
     *
     * 原来这里是 `if (!s_started) return -1;`，而 app 启动时**故意不调用**
     * wifi_service_init()（为了省 50KB RAM，约定"等真要用再起"）。
     * 结果用户在设置页点"扫描附近网络"时服务根本没起来，
     * 直接返回失败 —— 表现成**界面点了没反应**（用户实测报的 bug）。
     *
     * 现在改成：谁先要用谁负责启动。这样既保留了"不用 WiFi 就不占 RAM"，
     * 又不会出现"点了没反应"。
     */
    if (!s_started) {
        ESP_LOGI(TAG, "首次使用，按需启动 WiFi 协议栈…");
        if (wifi_service_init() != ESP_OK) {
            ESP_LOGE(TAG, "启动失败，无法扫描");
            return -1;
        }
    }
    if (s_scanning) {
        return 0;                   /* 已在扫，别重复发起 */
    }

    /*
     * ★ 自救：驱动还在 connecting 时，扫描会被直接拒绝
     *   （E wifi:sta_scan: STA is connecting, scan are not allowed!）。
     *   用户实测"点扫描毫无反应"就是这个。
     *   先把它拉回空闲再扫。
     */
    if (s_st.state == W_WIFI_CONNECTING) {
        ESP_LOGW(TAG, "驱动还在连接中，先断开再扫描");
        wifi_force_idle("扫描前清理");
    }
    s_scanning = true;
    lock();
    s_ap_count = 0;
    unlock();

    const wifi_scan_config_t cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,           /* 全信道 */
        .show_hidden = false,       /* 隐藏 AP 不列出来 */
    };
    if (esp_wifi_scan_start(&cfg, false) != ESP_OK) {
        s_scanning = false;
        return -1;
    }
    return 0;
}

static bool hook_scan_busy(void)
{
    return s_scanning;
}

static int hook_get_scan_result(wifi_ap_t *out, int max)
{
    if (!out || max <= 0) {
        return 0;
    }
    lock();
    int n = (s_ap_count < max) ? s_ap_count : max;
    memcpy(out, s_aps, n * sizeof(wifi_ap_t));
    unlock();
    return n;
}

static int hook_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        return -1;
    }
    /* 同上：按需启动，避免"点了没反应" */
    if (!s_started) {
        if (wifi_service_init() != ESP_OK) {
            return -1;
        }
        /*
         * ⚠️ init 里如果保存过凭据会自动发起一次连接。
         *    那种情况下别再重复发起，否则会把刚建立的连接打断。
         */
        if (s_st.state == W_WIFI_CONNECTING || s_st.state == W_WIFI_CONNECTED) {
            return 0;
        }
    }

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);

    /*
     * ★ 密码为空时**回填 NVS 里保存的密码**，绝不能让密码字段空着。
     *
     * 踩过的坑（用户实测："输对密码后连接失败，提示密码错误或信号差"）：
     *   这里原来是 `if (password && password[0]) { 拷贝 }` ——
     *   密码为空就什么都不写，而上面刚 `memset` 过，
     *   于是把**空密码**写进了驱动，覆盖掉原本保存的好密码，
     *   结果就是"密码明明是对的却认证失败"（reason=2）。
     *
     * 什么时候会传空密码：
     *   - 用户点已保存过的网络（界面不该问密码，直接连）
     *   - 串口 `wifi connect <SSID>`（省略密码参数）
     *   这两种情况下正确行为都是"用已保存的密码重连"。
     */
    char eff_pass[WIFI_PASS_MAX] = { 0 };
    bool from_saved = false;            /* 密码是不是从 NVS 回填的 */
    if (password && password[0]) {
        strncpy(eff_pass, password, sizeof(eff_pass) - 1);
    } else {
        /* 只有 SSID 也匹配时才回填，避免把 A 的密码用到 B 上 */
        char saved_ssid[WIFI_SSID_MAX] = { 0 };
        char saved_pass[WIFI_PASS_MAX] = { 0 };
        if (creds_load(saved_ssid, sizeof(saved_ssid),
                       saved_pass, sizeof(saved_pass)) == ESP_OK &&
            saved_ssid[0] && strcmp(saved_ssid, ssid) == 0) {
            strncpy(eff_pass, saved_pass, sizeof(eff_pass) - 1);
            from_saved = true;
            ESP_LOGI(TAG, "使用已保存的密码重连 '%s'（%d 位）",
                     ssid, (int)strlen(eff_pass));
        } else {
            ESP_LOGW(TAG, "'%s' 没有对应的已保存密码，按开放网络处理", ssid);
        }
    }

    /*
     * ==============================================================
     * ★ v0.5.0：拒绝"加密网络 + 空密码"这种**必然失败**的组合
     * ==============================================================
     *
     * 用户实测报的问题："连接 CWQLYK 失败，提示密码错误，
     * 但之前保存的密码是正确的。"
     *
     * 根因是 NVS 里那条记录变成了 `ssid="CWQLYK", pass=""`：
     *   · 界面看"已保存" -> 直接 connect(ssid, NULL)（不问密码）
     *   · 这里回填出空密码 -> cfg.sta.password 是空的
     *   · 驱动按"开放网络"去连一个 WPA2/WPA3 的 AP
     *     -> 认证失败 reason=2 -> 界面显示"密码错误"
     *
     * 而空密码是怎么写进去的（v0.4.0 的遗留路径，本次一并堵上）：
     *   原来无论有没有密码，最后都会 `creds_save(ssid, eff_pass)` ——
     *   eff_pass 为空时就把空串存了进去（下一处注释有详细说明）。
     *
     * 现在的处理：**先判断这个 SSID 在扫描结果里是不是加密的**，
     * 加密 + 没密码 -> 不发起连接，直接把这条坏记录清掉，
     * 并明确告诉用户。这样"点一下就能重新输密码"，不会卡死。
     */
    if (!eff_pass[0]) {
        bool known_secure = false;
        bool known_open   = false;
        lock();
        for (int i = 0; i < s_ap_count; i++) {
            if (strcmp(s_aps[i].ssid, ssid) == 0) {
                known_secure = s_aps[i].secure;
                known_open   = !s_aps[i].secure;
                break;
            }
        }
        unlock();

        if (known_secure) {
            ESP_LOGE(TAG, "'%s' 是加密网络但没有可用密码（保存的凭据已损坏/为空）"
                          " -> 不发起连接，清除这条坏记录，请重新输入密码", ssid);
            creds_erase();
            lock();
            s_st.state          = W_WIFI_FAILED;
            s_st.auto_reconnect = false;
            s_st.ssid[0]        = '\0';
            s_st.creds_cleared  = true;
            unlock();
            return -1;
        }
        if (!known_open) {
            /* 还没扫描过，判断不了 —— 允许试一次，但下面不会把空密码存进 NVS */
            ESP_LOGW(TAG, "'%s' 不在扫描结果里，无法判断是否加密，按开放网络试连", ssid);
        }
    }

    if (eff_pass[0]) {
        strncpy((char *)cfg.sta.password, eff_pass, sizeof(cfg.sta.password) - 1);
    }

    /*
     * threshold.authmode：**不筛**。
     *
     * 这个字段的含义是"只连加密强度不低于此值 的 AP"。
     * 原来在有密码时设成 WIFI_AUTH_WPA2_PSK，遇到 WPA3 或
     * WPA2/WPA3 过渡模式的 AP 会被驱动直接过滤掉（reason=211）。
     * 实测手机热点就是 authmode=7（WPA2/WPA3 过渡），会踩到。
     *
     * 安全性由**密码 + PMF** 保证，不需要靠这个字段筛。
     */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    /*
     * PMF（受保护管理帧）。
     *
     * WPA3 强制要求 PMF；WPA2/WPA3 过渡模式通常"支持但不强制"。
     * 设 capable=true / required=false：两种 AP 都能连，
     * 又不会因为**不声明 PMF 能力**而被过渡模式的热点拒绝。
     *
     * 这是排查"密码正确但认证失败(reason=2)"时最可疑的一项。
     */
    cfg.sta.pmf_cfg.capable  = true;
    cfg.sta.pmf_cfg.required = false;

    /*
     * SAE PWE 派生方式。
     *
     * WPA3 的 SAE 握手有两种 PWE 派生：H2E（新）和 Hunt-and-Peck（老）。
     * 设成 BOTH 先试 H2E 再回退；只设一种的话，
     * 对端恰好只支持另一种时握手会失败（表现同样是认证失败）。
     */
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    lock();
    snprintf(s_st.ssid, sizeof(s_st.ssid), "%s", ssid);
    s_st.state         = W_WIFI_CONNECTING;
    s_st.time_synced   = false;
    s_st.creds_cleared = false;         /* 新一轮尝试，清掉上次的提示 */
    s_st.conn_start    = (uint32_t)(esp_timer_get_time() / 1000);
    unlock();

    /* 记下"这次用的是不是保存的密码" —— 失败时据此决定要不要清凭据 */
    s_conn_used_saved = from_saved;
    s_user_disconnect = false;

    /*
     * ★ 关键：先断开、等断开完成，再设配置。
     *
     * 不这么做的话，如果驱动此刻正在连接（比如开机自动重连还没结束），
     * esp_wifi_set_config() 会直接失败并打印
     *     E wifi:sta is connecting, cannot set config
     * 于是**新密码根本没写进去**，紧接着 esp_wifi_connect() 拿旧配置
     * 去连，认证失败（reason=2），表现成"密码明明对却连不上"。
     * 这是用户实测踩到的坑，别再退回成直接 set_config。
     */
    if (s_disc_sem) {
        xSemaphoreTake(s_disc_sem, 0);      /* 清掉可能残留的信号 */
    }
    /*
     * ★ 这次断开是**我们自己为了改配置而做的**，不是连接失败。
     *   必须打上"主动断开"标志，否则紧接着到达的 STA_DISCONNECTED
     *   会被当成"用保存的密码连接失败"，直接把我们刚要用的凭据清掉 ——
     *   那就会变成"永远在清密码、永远连不上"。
     */
    s_user_disconnect = true;
    esp_wifi_disconnect();
    if (s_disc_sem) {
        /* 等断开事件，最多 2 秒。超时也继续 —— 宁可试一次也不要死等 */
        if (xSemaphoreTake(s_disc_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "等断开事件超时，仍然继续配置");
        }
    }

    esp_err_t cerr = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (cerr != ESP_OK) {
        ESP_LOGE(TAG, "写 WiFi 配置失败: %s", esp_err_to_name(cerr));
        set_state(W_WIFI_FAILED);
        return -1;
    }
    /* 记一行日志，方便排查"密码到底有没有写进去" */
    ESP_LOGI(TAG, "配置已写入：SSID='%s' 密码 %d 位", ssid,
             (int)strlen(eff_pass));
    if (eff_pass[0]) {
        /*
         * 把密码按**字节码**打出来。
         *
         * 为什么不直接打文本：长度不对时，光看文本看不出"末尾多了个
         * 不可见字符"（空格 / CR / Tab 都看不见），而字节码一眼就看出来。
         * 曾经因为多一个字节导致认证一直失败（reason=2），查了很久。
         */
        char hex[WIFI_PASS_MAX * 3 + 1];
        int n = 0;
        for (const unsigned char *p = (const unsigned char *)eff_pass;
             *p && n < (int)sizeof(hex) - 4; p++) {
            n += snprintf(hex + n, sizeof(hex) - n, "%02X ", *p);
        }
        hex[n] = '\0';
        ESP_LOGI(TAG, "密码字节码: %s", hex);
    }

    /*
     * 存下来，下次开机自动重连。
     *
     * ⚠️ 必须存 `eff_pass`（最终生效的那个），不能存入参 `password`。
     *    入参可能是空的（用户点已保存的网络重连、或串口省略密码），
     *    存空串会把 NVS 里原本好好的密码**覆盖掉**，
     *    下次开机就再也自动连不上了。
     *
     * ★ v0.5.0 又补了一层：**空密码根本不写 NVS**。
     *
     *   上面那条规则只管"用哪个变量"，没管"空的时候要不要写" ——
     *   于是在"没有已保存密码、又按开放网络去连"的路径上，
     *   仍然会把 `ssid + ""` 写进去。用户实测就是这么坏的：
     *   NVS 里留下 `CWQLYK + 空密码`，界面看着"已保存"就不再问密码，
     *   而实际连接必然认证失败（reason=2），提示"密码错误"，
     *   用户怎么试都救不回来。
     *
     *   现在的规则：
     *     · 有密码            -> 存（正常路径）
     *     · 没密码 + 确认是开放网络 -> 存 SSID（开放网络本来就没密码，
     *                              留着图个开机自动重连）
     *     · 没密码 + 不确定/是加密的 -> **不写**，并清掉可能存在的坏记录
     *       （"不确定"就是没扫描过、判断不了，宁可不记住也别存坏的）
     */
    if (eff_pass[0]) {
        creds_save(ssid, eff_pass);
        lock();
        s_st.auto_reconnect = true;
        unlock();
    } else {
        bool ap_is_open = false;
        lock();
        for (int i = 0; i < s_ap_count; i++) {
            if (strcmp(s_aps[i].ssid, ssid) == 0 && !s_aps[i].secure) {
                ap_is_open = true;
                break;
            }
        }
        unlock();

        if (ap_is_open) {
            creds_save(ssid, "");
            lock();
            s_st.auto_reconnect = true;
            unlock();
            ESP_LOGI(TAG, "'%s' 是开放网络，只记 SSID 不记密码", ssid);
        } else {
            /* 不清 NVS：万一是"没扫描过"的情况，别误删别的网络的好密码。
             * 只保证不把空密码写进去。 */
            ESP_LOGW(TAG, "'%s' 无密码且未确认为开放网络 -> 不写入 NVS", ssid);
            lock();
            s_st.auto_reconnect = false;
            unlock();
        }
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(err));
        set_state(W_WIFI_FAILED);
        return -1;
    }
    ESP_LOGI(TAG, "正在连接 '%s'…", ssid);
    return 0;
}

/**
 * @brief 断开连接（**保留**密码）
 *
 * @note 密码留着，所以下次进 WiFi 页或下次开机还能自动重连。
 *       要彻底"忘掉这个网"用 hook_forget()。
 *       这两个是**不同的用户意图**，不能合成一个按钮：
 *       合成之后用户每次"先不连了"都得重输密码。
 */
static int hook_disconnect(void)
{
    if (!s_started) {
        return -1;
    }
    /*
     * ★ 先打"主动断开"标志再断。
     *   否则 STA_DISCONNECTED 事件会被当成"连接失败"处理：
     *   状态变 FAILED（界面显示"连接失败（密码错或信号太差）"），
     *   而且如果这次连接用的是保存的密码，还会触发"自动取消保存"——
     *   可用户点的是"断开连接"，语义是**保留**密码。
     */
    s_user_disconnect = true;
    s_conn_used_saved = false;
    esp_wifi_disconnect();

    lock();
    s_st.state       = W_WIFI_IDLE;
    s_st.ip[0]       = '\0';
    s_st.rssi        = 0;
    s_st.time_synced = false;
    s_st.syncing     = false;
    s_st.sync_failed = false;
    s_st.conn_start  = 0;
    /* auto_reconnect 和 ssid 都保留 —— 密码还在 */
    unlock();

    ESP_LOGI(TAG, "已断开（密码保留，下次仍可自动重连）");
    return 0;
}

/** @brief 断开并清除保存的密码（"取消保存"） */
static int hook_forget(void)
{
    if (!s_started) {
        return -1;
    }
    s_user_disconnect = true;       /* 同上：这是用户主动行为，不是失败 */
    s_conn_used_saved = false;
    esp_wifi_disconnect();
    creds_erase();

    lock();
    s_st.state          = W_WIFI_IDLE;
    s_st.auto_reconnect = false;
    s_st.ip[0]          = '\0';
    s_st.ssid[0]        = '\0';
    s_st.rssi           = 0;
    s_st.time_synced    = false;
    s_st.syncing        = false;
    s_st.sync_failed    = false;
    s_st.conn_start     = 0;
    s_st.creds_cleared  = false;    /* 是我们自己清的，不用提示"已自动清除" */
    unlock();

    ESP_LOGI(TAG, "已断开并清除保存的密码");
    return 0;
}

static void hook_get_status(wifi_status_t *out)
{
    if (!out) {
        return;
    }
    lock();
    /*
     * 连接超时判定。
     *
     * 卡在 CONNECTING 超过 CONN_TIMEOUT_MS 就标记需要复位 ——
     * 否则驱动会一直拒绝扫描和设配置（用户实测三个症状全是这个引起的）。
     */
    if (s_st.state == W_WIFI_CONNECTING && s_st.conn_start) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now - s_st.conn_start) > CONN_TIMEOUT_MS) {
            ESP_LOGW(TAG, "连接超时（%d 秒），准备复位 WiFi 状态",
                     CONN_TIMEOUT_MS / 1000);
            s_st.state      = W_WIFI_FAILED;
            s_st.conn_start = 0;
            s_conn_stuck    = true;     /* 真正的断开在锁外做 */
        }
    }

    /*
     * 对时超时判定放在这里（读状态时顺手算）。
     * 不开额外定时器：这个状态本来就是"界面要看的时候才需要准确"，
     * 轮询频率（300ms）远高于超时精度要求。
     */
    if (s_st.syncing && s_st.sync_start) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_ms - s_st.sync_start) > SNTP_TIMEOUT_MS) {
            s_st.syncing     = false;
            s_st.sync_failed = true;
            ESP_LOGW(TAG, "SNTP 对时超时（%d 秒没收到响应）", SNTP_TIMEOUT_MS / 1000);
        }
    }
    const bool stuck = s_conn_stuck;
    s_conn_stuck = false;
    *out = s_st;
    unlock();

    /*
     * ⚠️ 断开动作必须在**锁外**做：wifi_force_idle 内部也要拿锁，
     *    持锁调用会自死锁。
     */
    if (stuck) {
        wifi_force_idle("连接超时清理");
    }

    /* 信号强度要实时问，状态里的可能过期 */
    if (s_st.state == W_WIFI_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}

static void hook_get_saved_ssid(char *out, int max)
{
    if (!out || max <= 0) {
        return;
    }
    out[0] = '\0';
    char pass[WIFI_PASS_MAX];
    creds_load(out, max, pass, sizeof(pass));
}

static int hook_sync_time(void)
{
    if (!s_started || s_st.state != W_WIFI_CONNECTED) {
        return -1;
    }
    /* 正在对时就当作成功（幂等），避免界面反复点造成一串请求 */
    lock();
    const bool already = s_st.syncing;
    unlock();
    if (already) {
        return 0;
    }
    sntp_start();
    return 0;
}

static const wifi_hooks_t s_hooks = {
    .scan_start       = hook_scan_start,
    .scan_busy        = hook_scan_busy,
    .get_scan_result  = hook_get_scan_result,
    .connect          = hook_connect,
    .disconnect       = hook_disconnect,
    .forget           = hook_forget,
    .get_status       = hook_get_status,
    .get_saved_ssid   = hook_get_saved_ssid,
    .sync_time        = hook_sync_time,
};

const wifi_hooks_t *wifi_service_get_hooks(void)
{
    return &s_hooks;
}

const wifi_hooks_t *wifi_service_hooks(void)
{
    return &s_hooks;
}

bool wifi_service_started(void)
{
    return s_started;
}

/**
 * @brief 彻底重置 WiFi 驱动（deinit + init），并清掉保存的凭据
 *
 * ## 为什么需要这个
 *
 * 调试中发现：连接失败若干次之后，驱动内部的认证状态会"变脏" ——
 * 之后**即使密码正确也一直 reason=2**；而**断电重启后一次就能连上**。
 * 说明确实有内存态残留（关联状态 / PMK 缓存之类）。
 *
 * `esp_wifi_disconnect()` 只是断开，清不掉这些；
 * 所以要 `esp_wifi_deinit()` + 重新 `init` 才能回到干净状态。
 *
 * 顺带把 NVS 里的凭据也删掉，让用户从"全新连接"开始。
 */
esp_err_t wifi_service_reset(void)
{
    ESP_LOGW(TAG, "重置 WiFi 驱动（deinit + init）…");

    if (s_started) {
        s_user_disconnect = true;       /* 主动重置，不是连接失败 */
        s_conn_used_saved = false;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_wifi_stop();
        esp_wifi_deinit();
        s_started = false;
    }

    /* 清掉保存的凭据，避免再自动重连到旧配置 */
    creds_erase();
    lock();
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = W_WIFI_IDLE;
    unlock();

    /* 重新初始化（内部会重新注册事件、启动 STA、并尝试自动重连） */
    return wifi_service_init();
}

void wifi_service_register_hooks(void)
{
    /* 钩子表是静态的，这里只是个语义化的占位：调用它表示"界面可以用了" */
    ESP_LOGD(TAG, "钩子已就绪（WiFi 协议栈尚未启动）");
}

/* ==================== 初始化 ==================== */
esp_err_t wifi_service_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "锁创建失败");
    }
    if (!s_disc_sem) {
        s_disc_sem = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_disc_sem, ESP_ERR_NO_MEM, TAG, "断开信号量创建失败");
    }

    /* NVS 可能已经被别的组件初始化过（settings 会初始化），重复初始化返回
     * ESP_ERR_NVS_NO_FREE_PAGES / INVALID_STATE 都算正常，忽略 */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK && nvs_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(nvs_err));
        return nvs_err;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif 初始化失败");

    /* 事件循环可能已被别的组件创建（返回 INVALID_STATE），忽略 */
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }

    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "创建 netif 失败");
    }

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init 失败");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID,
                            wifi_event_handler, NULL, NULL),
                        TAG, "注册 WiFi 事件失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP,
                            wifi_event_handler, NULL, NULL),
                        TAG, "注册 IP 事件失败");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "设置 STA 模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "启动 WiFi 失败");

    s_started = true;
    memset(&s_st, 0, sizeof(s_st));
    s_st.available = true;
    s_st.state     = W_WIFI_IDLE;

    ESP_LOGI(TAG, "WiFi 服务就绪");

    /* ---------- 开机自动重连 ---------- */
    char ssid[WIFI_SSID_MAX], pass[WIFI_PASS_MAX];
    if (creds_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK && ssid[0]) {
        ESP_LOGI(TAG, "发现已保存的网络 '%s'，自动重连", ssid);
        lock();
        s_st.auto_reconnect = true;
        unlock();
        hook_connect(ssid, pass);
    }

    return ESP_OK;
}

void wifi_service_set_time_cb(void (*cb)(uint32_t utc_epoch))
{
    s_time_cb = cb;
}

void wifi_service_dump(void)
{
    wifi_status_t st;
    hook_get_status(&st);

    static const char *names[] = { "空闲", "连接中", "已连接", "失败" };
    ESP_LOGI(TAG, "WiFi: %s  状态=%s", st.available ? "可用" : "**未启动**",
             names[st.state <= W_WIFI_FAILED ? st.state : 0]);
    if (st.ssid[0]) {
        ESP_LOGI(TAG, "  SSID=%s  RSSI=%d dBm  IP=%s",
                 st.ssid, st.rssi, st.ip[0] ? st.ip : "-");
    }
    ESP_LOGI(TAG, "  自动重连=%s  SNTP=%s（成功 %d 次）",
             st.auto_reconnect ? "是" : "否",
             st.time_synced ? "已对时" : "未对时", st.synced_count);

    char saved[WIFI_SSID_MAX];
    hook_get_saved_ssid(saved, sizeof(saved));
    ESP_LOGI(TAG, "  已保存凭据: %s", saved[0] ? saved : "（无）");
}
