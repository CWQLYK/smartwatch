/**
 * @file watch_page_wifi.c
 * @brief WiFi 连接页 —— 扫描列表 + 小键盘输密码 + 连上自动 SNTP 校时
 *
 * ==================================================================
 * 交互流程（按用户要求）
 * ==================================================================
 *
 * ```
 *   进入页面
 *     ├── 已连上？ → 显示 SSID / IP / 信号 / 对时状态 + [断开] [重新对时]
 *     └── 未连上？ → 自动扫描 → 列表（点一下选中）
 *                      → 若加密：弹小键盘输密码（明文，方便自检）
 *                      → 连上后弹提示："WiFi 耗电大，不用请断开"
 * ```
 *
 * ## 几个刻意的设计
 *
 * 1. **连上后保持连接**（用户要求），但页面顶部常驻一个醒目的
 *    "WiFi 已开，耗电较大" 提示条 —— 用户可以随时断开。
 *
 * 2. **不做无限自动重连**：用户明确要"手动断开"，无脑重连会让断开按钮失效。
 *    只在开机时按保存的凭据重连一次。
 *
 * 3. **扫描是异步的**：点"扫描"后每 300ms 轮询一次结果，
 *    期间界面显示"扫描中…"。同步等会让 UI 卡住几百毫秒。
 *
 * 4. **信号强度用文字而不是数字**：普通用户看不懂 -67 dBm，
 *    但"强/中/弱"一眼就懂。开发者要看数字可以走串口 `wifi`。
 */
#include "watch_pages.h"
#include "watch_style.h"
#include "watch_widgets.h"
#include "watch_strings.h"
#include "watch_platform.h"
#include "watch_modal.h"
#include "watch_keyboard.h"
#include "watch_toast.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "WIFIPAGE";

/* 扫描轮询周期（ms） */
#define SCAN_POLL_MS   300
/* 扫描最长等多久（超过就显示"没扫到"） */
/*
 * 扫描超时。
 *
 * ⚠️ 实测数字（别凭感觉改小）：
 *   - 首次启动 WiFi 协议栈要 **8.1 秒**（全量 RF 校准；
 *     之后走缓存会快很多）
 *   - 扫描本身约 **2.8 秒**
 * 而界面可能触发"按需启动协议栈 + 扫描"，所以总时长可能到 11 秒。
 * 12 秒就卡在边界上 —— 用户很容易看到"扫描超时"。
 * 放到 25 秒，宁可多等一会儿也不要误报超时。
 */
#define SCAN_TIMEOUT_MS 25000

/*
 * SSID 副本的槽位数。
 *
 * ⚠️ 为什么需要静态副本：列表项的回调是**异步**触发的（用户点的时候
 *    扫描结果数组早就出作用域了）。所以要把 SSID 拷到静态存储里，
 *    再把指针交给回调。槽位数只要 >= 屏上能显示的行数即可。
 */
#define WIFI_SSID_STORE  W_WIFI_SCAN_MAX

/**
 * @brief 界面最多列几个"已保存的网络"（v0.7.0 多槽位）
 *
 * @note 要和 `wifi_service.c` 的 `WIFI_SLOT_MAX` 对齐。
 *       这里定 5 是"上限"，实际有几条由 `get_saved_ssid_at()` 返回是否越界决定。
 */
#define WIFI_SAVED_MAX   5

static struct {
    lv_obj_t *list;         /* 列表容器 */
    lv_obj_t *status;       /* 状态文字 */
    lv_timer_t *poll;       /* 扫描轮询定时器 */
    uint32_t  scan_started; /* 扫描开始时刻，用于超时 */
    char      pending_ssid[W_WIFI_SSID_MAX];  /* 正在等密码的 SSID */
    bool      hint_shown;   /* 耗电提示是否已经弹过（同一次连接只弹一次） */

    /*
     * ★ 状态机用的三个字段（修"输完密码跳回扫描列表"时加的）
     */
    w_wifi_state_t last_state;     /* 上一次看到的连接状态，变了就重建页面 */
    bool           scanning;       /* 扫描是否还在进行（UI 自己的标志） */
    bool           ever_scanned;   /* 本次进页面是否已经自动扫过一次 */
    /*
     * 用户是否**主动**点了扫描/重新扫描。
     *
     * 用来区分"用户要扫描"和"只是状态变化导致页面重建" ——
     * 后者不能触发扫描，否则会把"连接失败"等提示覆盖掉。
     */
    bool           rescan_requested;

    /*
     * 已保存密码的那个 SSID（页面构建时从 NVS 读一次）。
     * 用来判断"点这个网络要不要问密码"。
     */
    char           saved_ssid[W_WIFI_SSID_MAX];

    /* 上一次看到的对时过程状态（用来检测变化并重建页面） */
    bool           last_syncing;
    bool           last_sync_failed;
    bool           last_synced;

    /*
     * ★ v0.7.0：toast 用的"上次已经提示过的状态"。
     *
     * 为什么不能直接用 last_state：last_state 在**每次重建时**都会被改写成当前值
     * （wifi_page_build 里也是），所以拿它判断"是不是刚变了"会在重建后丢失边沿。
     * 这里单独记一份只增不减的"已提示过"状态，保证一次状态变化只弹一次 toast。
     */
    w_wifi_state_t toasted_state;
} ui;

/* ==================================================================
 * 工具
 * ================================================================== */

/** @brief 信号强度 → 中文描述 */
static const char *rssi_word(int8_t rssi)
{
    if (rssi >= -60) return W_STR_WIFI_STRONG;
    if (rssi >= -75) return W_STR_WIFI_MEDIUM;
    return W_STR_WIFI_WEAK;
}

static const w_wifi_hooks_t *hooks(void)
{
    return w_platform_wifi();
}

/**
 * @brief 这个 SSID 是不是已经存在某个槽位里（v0.7.0 多槽位）
 *
 * @note 钩子没接（模拟器/旧实现）时退回到"只比 ui.saved_ssid"，
 *       所以这条路上不会因为新钩子缺失而误判。
 */
static bool wifi_ssid_is_saved(const w_wifi_hooks_t *h, const char *ssid)
{
    if (!h || !h->get_saved_ssid_at || !ssid || !ssid[0]) {
        return false;
    }
    char tmp[W_WIFI_SSID_MAX];
    for (int i = 0; i < WIFI_SAVED_MAX; i++) {
        tmp[0] = '\0';
        if (h->get_saved_ssid_at(i, tmp, sizeof(tmp)) != 0) {
            break;                  /* 越界（槽位是连续的）*/
        }
        if (strcmp(tmp, ssid) == 0) {
            return true;
        }
    }
    return false;
}

/** @brief 清空列表 */
static void list_clear(void)
{
    if (ui.list) {
        lv_obj_clean(ui.list);
    }
}

static void set_status(const char *text, lv_color_t color)
{
    if (!ui.status) {
        return;
    }
    w_label_set_text_auto(ui.status, text);
    lv_obj_set_style_text_color(ui.status, color, 0);
}

/* ==================================================================
 * ★ v0.7.0：通用 toast —— WiFi 的状态变化给一条"屏幕顶部一闪而过"的反馈
 *
 * 为什么需要：WiFi 是全项目反馈最弱的一环 —— 连接成功只改一行小字，
 * 失败只有页面内的 popup，而**用户此刻往往已经滑走去看别的页面了**
 * （比如点了连接就去表盘看时间）。toast 挂在 layer_top 上，
 * 不管当前在哪个页面都能看见。
 *
 * 规则：一次状态**变化**只提示一次（用 ui.toasted_state 记边沿）。
 * ================================================================== */
static void toast_state_change(w_wifi_state_t st, const w_wifi_status_t *s)
{
    if (st == ui.toasted_state) {
        return;                     /* 没有变化，或者已经提示过 */
    }
    ui.toasted_state = st;

    switch (st) {
    case W_WIFI_CONNECTED:
        /* 连上是"好消息"，把 SSID 带上，用户一眼知道连的是哪个 */
        watch_toast_show_fmt("%s %s", W_STR_WIFI_CONNECTED,
                             (s && s->ssid[0]) ? s->ssid : "");
        break;

    case W_WIFI_FAILED:
        watch_toast_show(W_STR_WIFI_FAILED);
        break;

    default:
        /* IDLE / CONNECTING 不打扰用户（他正在输密码/等待，页面本身有提示）*/
        break;
    }
}

/* ==================================================================
 * 前向声明
 * ================================================================== */
static void wifi_page_build(lv_obj_t *parent, void *user);
static void show_scan_list(void);
static void show_connected(lv_obj_t *parent, const w_wifi_status_t *st);

/* ==================================================================
 * 连接成功后的"耗电"提示（用户明确要求）
 * ================================================================== */
static void hint_ok_cb(lv_event_t *e)
{
    lv_obj_t *panel = (lv_obj_t *)lv_event_get_user_data(e);
    if (panel) {
        lv_obj_del(panel);      /* 关掉提示，回到 WiFi 页 */
    }
    w_modal_refresh();
}

/**
 * @brief 弹一个居中的提示框
 *
 * @note 做成"盖在当前页面上的小面板"而不是新子页：
 *       子页要压栈，而这个提示只是确认一下，不该影响返回逻辑。
 */
static void popup(const char *title, const char *body, lv_event_cb_t on_ok)
{
    /* 找当前活动屏幕里最上层的容器 */
    lv_obj_t *host = lv_scr_act();
    if (!host) {
        return;
    }

    lv_obj_t *mask = lv_obj_create(host);
    lv_obj_set_size(mask, W_SCREEN_W, W_SCREEN_H);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_60, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(mask);

    lv_obj_t *box = w_card_create(mask, 0, 0, W_SCREEN_W - 40, 150);
    lv_obj_center(box);

    lv_obj_t *t = w_label_create(box, title, watch_font_cn(16), W_COLOR_ACCENT);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *b = w_label_create(box, body, watch_font_cn(14), W_COLOR_TEXT);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(b, W_SCREEN_W - 72);

    lv_obj_t *ok = lv_btn_create(box);
    lv_obj_set_size(ok, 90, 34);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(ok, 8, 0);
    lv_obj_set_style_bg_color(ok, W_COLOR_ACCENT, 0);
    lv_obj_set_style_shadow_width(ok, 0, 0);
    lv_obj_t *ol = w_label_create(ok, W_STR_OK, watch_font_cn(14), W_COLOR_TEXT);
    lv_obj_center(ol);
    lv_obj_add_event_cb(ok, on_ok ? on_ok : hint_ok_cb,
                        LV_EVENT_CLICKED, mask);
}

/* ==================================================================
 * 密码输入
 * ================================================================== */
static void password_done(const char *text, bool ok, void *user)
{
    (void)user;

    if (!ok || !text || !text[0]) {
        ESP_LOGI(TAG, "取消输入密码");
        w_modal_refresh();
        return;
    }

    const w_wifi_hooks_t *h = hooks();
    if (!h || !h->connect) {
        return;
    }

    ESP_LOGI(TAG, "连接 '%s'（密码 %d 位）", ui.pending_ssid, (int)strlen(text));
    h->connect(ui.pending_ssid, text);

    /* 立刻回显"连接中"，用户不用等下一次刷新 */
    set_status(W_STR_WIFI_CONNECTING, W_COLOR_WARN);
    w_modal_refresh();
}

static void ask_password(const char *ssid)
{
    snprintf(ui.pending_ssid, sizeof(ui.pending_ssid), "%s", ssid);

    /* 键盘挂在当前活动屏幕上（盖住一切） */
    lv_obj_t *host = lv_scr_act();
    if (!host) {
        return;
    }
    w_keyboard_open(host, W_STR_WIFI_PASSWORD, true, "",
                    password_done, NULL);
}

/* 输入 SSID 的情况留给"扫不到隐藏网络"，这里先不做入口 */

/* ==================================================================
 * 列表项
 * ================================================================== */
static void ap_clicked(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    const bool secure = (bool)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (!ssid) {
        return;
    }

    const w_wifi_hooks_t *h = hooks();
    if (!h || !h->connect) {
        return;
    }

    /*
     * ★ 已保存过密码的网络：**直接连，不要再问密码**。
     *
     * 用户实测反馈："我主动断开后重新点那个 WiFi，并没有直接连接，
     * 而是让我重新输密码"。原因就是原来只判断了 `secure`，
     * 从不检查"这个网络是不是已经存过密码"。
     *
     * 传 NULL 表示"用已保存的密码"，服务侧会从 NVS 回填
     * （见 hook_connect 里的 eff_pass 逻辑）。
     *
     * ★ v0.7.0：多槽位之后要**查遍所有槽**，不能只看 `ui.saved_ssid`
     *   （那只是"上次成功那一槽"）—— 否则家里/公司两个网里
     *   只有一个能"免密码直连"，另一个每次都要重输。
     */
    const bool saved = wifi_ssid_is_saved(h, ssid) ||
                       (ui.saved_ssid[0] && strcmp(ui.saved_ssid, ssid) == 0);

    if (secure && !saved) {
        ask_password(ssid);
        return;
    }

    ESP_LOGI(TAG, "连接 '%s'（%s）", ssid, saved ? "用已保存的密码" : "开放网络");
    snprintf(ui.pending_ssid, sizeof(ui.pending_ssid), "%s", ssid);
    h->connect(ssid, saved ? NULL : "");
    set_status(W_STR_WIFI_CONNECTING, W_COLOR_WARN);
    w_modal_refresh();
}

static void show_scan_list(void)
{
    const w_wifi_hooks_t *h = hooks();
    if (!h || !ui.list) {
        return;
    }

    list_clear();

    w_wifi_ap_t aps[W_WIFI_SCAN_MAX];
    const int n = h->get_scan_result ? h->get_scan_result(aps, W_WIFI_SCAN_MAX) : 0;

    if (n == 0) {
        lv_obj_t *l = w_label_create(ui.list, W_STR_WIFI_NO_AP, watch_font_cn(14),
                                     W_COLOR_TEXT_DIM);
        lv_obj_set_width(l, LV_PCT(100));
        return;
    }

    ESP_LOGI(TAG, "列表显示 %d 个 AP", n);

    lv_obj_t *tip = w_label_create(ui.list, W_STR_WIFI_PICK_AP, watch_font_cn(14),
                                   W_COLOR_TEXT_DIM);
    lv_obj_set_width(tip, LV_PCT(100));

    /*
     * ★ v0.7.0：多槽位之后，告诉用户"我已经记住了几个网络"。
     *
     *   为什么要有这行：以前只存一个网，"点已保存的网络不用输密码"
     *   是隐含行为；现在能存 5 个，用户需要知道到底记住了哪些、
     *   还有几个空位（满了会覆盖最旧的那个）。
     */
    if (h->saved_count) {
        const int sc = h->saved_count();
        if (sc > 0) {
            lv_obj_t *sl = w_label_create_auto(ui.list, "", W_FONT_SMALL, 13,
                                               W_COLOR_TEXT_MUTE);
            lv_obj_set_width(sl, LV_PCT(100));
            w_label_set_text_fmt_auto(sl, W_STR_WIFI_SAVED_COUNT_FMT,
                                      sc, WIFI_SAVED_MAX);
        }
    }

    for (int i = 0; i < n; i++) {
        lv_obj_t *row = w_card_create_clickable(ui.list, 0, 0, LV_PCT(100), 42);

        lv_obj_t *s = w_label_create(row, aps[i].ssid, watch_font_cn(14), W_COLOR_TEXT);
        lv_obj_align(s, LV_ALIGN_LEFT_MID, 0, 0);
        /* 长 SSID 要能截断，不然会盖住右边的信号文字 */
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s, 130);

        /* 右边：加密标记 + 信号强度 */
        char right[32];
        snprintf(right, sizeof(right), "%s%s",
                 aps[i].secure ? W_STR_WIFI_LOCK : "", rssi_word(aps[i].rssi));
        lv_obj_t *r = w_label_create(row, right, watch_font_cn(14), W_COLOR_TEXT_DIM);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);

        /*
         * SSID 用**静态副本**而不是栈上的 aps[i].ssid：
         * 回调是异步触发的（用户点的时候 aps[] 早就出作用域了）。
         *
         * ★ v0.6.1：这里原来是 snprintf(..., "%s", aps[i].ssid)。
         *   -O2 下 GCC 直接报错：它无法证明 aps[i].ssid 里一定有结束符，
         *   于是按"字符串可能一直延伸到整个 aps[] 数组末尾"来算，
         *   警告"最多 419 字节写进 33 字节"（-Werror=format-truncation）。
         *   改成定长 memcpy + 显式补 0：语义完全一样（SSID 最多 32 字符），
         *   但边界是编译器能验证的。
         */
        static char ssid_store[WIFI_SSID_STORE][W_WIFI_SSID_MAX];
        memcpy(ssid_store[i % WIFI_SSID_STORE], aps[i].ssid, W_WIFI_SSID_MAX);
        ssid_store[i % WIFI_SSID_STORE][W_WIFI_SSID_MAX - 1] = '\0';

        lv_obj_set_user_data(row, (void *)(intptr_t)aps[i].secure);
        lv_obj_add_event_cb(row, ap_clicked, LV_EVENT_CLICKED,
                            ssid_store[i % WIFI_SSID_STORE]);
    }
}

/* ==================================================================
 * 轮询：同时管"扫描进度"和"连接状态变化"
 *
 * ⚠️ 这个页面修过一个状态机 bug（用户实测报的
 *    "输完密码点完成，又跳回扫描列表，没有进行连接"）：
 *
 *   原因是 `wifi_page_build()` 末尾**无条件**调 `start_scan()`，
 *    而 `start_scan()` 会清空列表并把状态文字改成"扫描中"。
 *    于是密码键盘关闭 → `w_modal_refresh()` 重建页面 → 立刻开始新扫描，
 *    用户看到的就是"跳回扫描列表"，完全无法判断密码有没有被接受。
 *    （连接其实**已经在后台发起了**，只是界面把他甩回了起点。）
 *
 *   现在改成：
 *     - 只有 IDLE / FAILED 才自动扫描
 *     - 记住上一次状态，**状态一变就重建页面**
 *     - CONNECTING 时显示"正在连接 <SSID>"和一个取消按钮
 * ================================================================== */
/**
 * @brief 页面被销毁（返回 / 模态重建）时：带走轮询定时器
 *
 * ⚠️ 本回调是 v0.6.1 补的，修的是一处 **use-after-free**：
 *    轮询定时器原先只在 poll_cb 内部两条路径里被删
 *    （状态变化重建、扫描超时）。如果用户在**扫描进行中**按返回，
 *    没有任何人删它 → 定时器继续跑，去写已经释放的 ui.status / ui.list
 *    → 花屏 / 乱跳 / 随机重启；而且定时器自己也永久泄漏。
 *
 * @note 这里**只**做"删定时器 + 清指针"，绝不碰 LVGL 对象 ——
 *       本回调是在对象析构过程中被调用的，那时候不能再增删控件。
 */
static void page_delete_cb(lv_event_t *e)
{
    (void)e;

    if (ui.poll) {
        lv_timer_del(ui.poll);
        ui.poll = NULL;
    }
    ui.list     = NULL;
    ui.status   = NULL;
    ui.scanning = false;
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    const w_wifi_hooks_t *h = hooks();
    if (!h) {
        return;
    }

    /* ---------- 1. 连接状态变了？重建页面 ---------- */
    if (h->get_status) {
        w_wifi_status_t st;
        h->get_status(&st);
        /*
         * 除了连接状态，**对时过程状态**变化也要重建：
         * 点了"重新对时"之后界面要立刻显示"对时中…"，
         * 对完要显示"已对时"。
         */
        const bool sync_changed = (st.syncing != ui.last_syncing) ||
                                  (st.sync_failed != ui.last_sync_failed) ||
                                  (st.time_synced != ui.last_synced);
        if (st.state != ui.last_state || sync_changed) {
            ESP_LOGI(TAG, "状态 %d -> %d（对时 %d/%d/%d），重建页面",
                     (int)ui.last_state, (int)st.state,
                     (int)st.syncing, (int)st.sync_failed, (int)st.time_synced);
            /*
             * ★ v0.7.0：toast 要在**改 last_state 之前**判定边沿，
             *   而且它挂在自己的 layer 上，随后那次 w_modal_refresh()
             *   把整页重建也不会影响它。
             */
            toast_state_change(st.state, &st);
            ui.last_state    = st.state;
            ui.last_syncing  = st.syncing;
            ui.last_sync_failed = st.sync_failed;
            ui.last_synced   = st.time_synced;
            /* 重建会重新创建定时器；先删掉自己避免泄漏 */
            if (ui.poll) {
                lv_timer_del(ui.poll);
                ui.poll = NULL;
            }
            w_modal_refresh();
            return;
        }
    }

    /* ---------- 2. 扫描进度 ---------- */
    if (!h->scan_busy || !ui.scanning) {
        return;
    }

    if (h->scan_busy()) {
        if (lv_tick_get() - ui.scan_started > SCAN_TIMEOUT_MS) {
            ui.scanning = false;
            set_status(W_STR_WIFI_SCAN_TIMEOUT, W_COLOR_WARN);
            if (ui.poll) {
                lv_timer_del(ui.poll);
                ui.poll = NULL;
            }
        }
        return;
    }

    /* 扫完了：把结果填进列表 */
    ui.scanning = false;
    set_status(W_STR_WIFI_SCAN_DONE, W_COLOR_TEXT_DIM);
    show_scan_list();

    /*
     * ★ v0.7.0：扫描完成也给一条 toast。
     *   扫描要 2~4 秒，用户很可能已经滑走；toast 会告诉他"可以回来选网络了"。
     */
    watch_toast_show(W_STR_WIFI_SCAN_DONE);
}

/**
 * @brief 起一个轮询定时器（如果还没有）
 *
 * @note 只创建一个实例。页面重建时旧的会被删掉，所以每次进来都会新建一个。
 */
static void ensure_poll(void)
{
    if (!ui.poll) {
        ui.poll = lv_timer_create(poll_cb, SCAN_POLL_MS, NULL);
    }
}

static void start_scan(void)
{
    const w_wifi_hooks_t *h = hooks();
    if (!h || !h->scan_start) {
        set_status(W_STR_WIFI_UNAVAILABLE, W_COLOR_WARN);
        return;
    }

    list_clear();
    set_status(W_STR_WIFI_SCANNING, W_COLOR_WARN);

    if (h->scan_start() != 0) {
        set_status(W_STR_WIFI_UNAVAILABLE, W_COLOR_WARN);
        return;
    }

    ui.scanning     = true;
    ui.scan_started = lv_tick_get();
    ensure_poll();
}

/* ==================================================================
 * 已连接视图
 * ================================================================== */
/** @brief 断开连接（**保留**密码） */
static void disconnect_cb(lv_event_t *e)
{
    (void)e;
    const w_wifi_hooks_t *h = hooks();
    if (h && h->disconnect) {
        h->disconnect();
    }
    /*
     * 这里**不重置 hint_shown**：用户只是"先不连了"，
     * 下次重连不该再弹一遍同样的耗电提示（他早就知道了）。
     */
    ui.last_state = W_WIFI_IDLE;
    w_modal_refresh();
}

/** @brief 断开并清除保存的密码（"取消保存"） */
static void forget_cb(lv_event_t *e)
{
    (void)e;
    const w_wifi_hooks_t *h = hooks();
    if (h && h->forget) {
        h->forget();
    }
    /* 密码删了，下次是全新连接 —— 耗电提示可以再提醒一次 */
    ui.hint_shown = false;
    ui.last_state = W_WIFI_IDLE;
    w_modal_refresh();
}

static void resync_cb(lv_event_t *e)
{
    (void)e;
    const w_wifi_hooks_t *h = hooks();
    if (h && h->sync_time) {
        h->sync_time();
    }
    w_modal_refresh();
}

static void rescan_cb(lv_event_t *e)
{
    (void)e;
    /*
     * ⚠️ 这里**不直接调 start_scan()**，而是置个标志让页面重建时去扫。
     *
     * 原因：start_scan() 只动"状态文字"和"列表"，但页面上
     * （FAILED 状态时）还挂着一个"重新扫描"按钮 —— 扫完变成 IDLE 之后
     * 那个按钮就该消失。所以要走一次重建，让页面按新状态重新布局。
     */
    ui.rescan_requested = true;
    ui.last_state      = W_WIFI_IDLE;   /* 假装状态变了，触发重建 */
    w_modal_refresh();
}

static void show_connected(lv_obj_t *parent, const w_wifi_status_t *st)
{
    /* 状态卡 */
    lv_obj_t *card = w_card_create(parent, 0, 0, LV_PCT(100), 96);

    lv_obj_t *s = w_label_create(card, st->ssid, watch_font_cn(16), W_COLOR_ACCENT);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, 0, 0);

    /*
     * 对时状态文字：三态（对时中 / 已对时 / 未对时）。
     *
     * ⚠️ 原来只有"已对时/未对时"两态，点"重新对时"后
     *    time_synced 仍是上一次的 true，文字照样显示"已对时" ——
     *    用户以为按钮没生效（实测反馈"点了没反应"）。
     *    现在加上 syncing 这一态，点下去立刻变"对时中…"。
     */
    const char *sync_txt;
    lv_color_t  sync_col = W_COLOR_TEXT_DIM;
    if (st->syncing) {
        sync_txt = W_STR_WIFI_SYNCING;
        sync_col = W_COLOR_WARN;
    } else if (st->time_synced) {
        sync_txt = W_STR_WIFI_SYNCED;
        sync_col = W_COLOR_ACCENT;
    } else if (st->sync_failed) {
        sync_txt = W_STR_WIFI_SYNC_FAILED;
        sync_col = W_COLOR_WARN;
    } else {
        sync_txt = W_STR_WIFI_NOT_SYNCED;
    }

    char line[64];
    snprintf(line, sizeof(line), "%s  %s  %s",
             st->ip[0] ? st->ip : "-", rssi_word(st->rssi), sync_txt);
    lv_obj_t *l2 = w_label_create(card, line, watch_font_cn(14), sync_col);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 0, 26);

    /* ★ 耗电提醒（用户要求"让客户知道不用WiFi时断开网络"）*/
    lv_obj_t *warn = w_label_create(card, W_STR_WIFI_POWER_WARN, watch_font_cn(14),
                                    W_COLOR_WARN);
    lv_obj_align(warn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, LV_PCT(100));

    /* 按钮 */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);

    /*
     * ---------- 三个按钮 ----------
     *
     *   [重新对时]  [断开连接]  [取消保存]
     *
     * ⚠️ "断开连接"和"取消保存"必须是**两个按钮**（用户明确要求）：
     *     断开连接 = 先不连了，密码留着，下次自动重连
     *     取消保存 = 我不想让这块表再连这个网了，密码删掉
     *   原来只有"断开"（顺带删密码），用户每次"先不连了"
     *   都要重新输一遍密码。
     */
    const char *labels[3] = {
        W_STR_WIFI_RESYNC, W_STR_WIFI_DISCONNECT, W_STR_WIFI_FORGET
    };
    lv_event_cb_t cbs[3] = { resync_cb, disconnect_cb, forget_cb };
    /* 第三个用警告色，提醒它会把密码删掉 */
    const lv_color_t cols[3] = { W_COLOR_CARD, W_COLOR_CARD, W_COLOR_WARN };

    const lv_coord_t bw = 70;       /* 70x3 + 5x2 = 220，屏幕宽 240 放得下 */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_btn_create(row);
        lv_obj_set_size(b, bw, 40);
        lv_obj_set_pos(b, i * (bw + 5), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_bg_color(b, cols[i], 0);
        lv_obj_t *bl = w_label_create(b, labels[i], watch_font_cn(14), W_COLOR_TEXT);
        lv_obj_center(bl);
        lv_obj_add_event_cb(b, cbs[i], LV_EVENT_CLICKED, NULL);
    }
}

/* ==================================================================
 * 页面构建
 * ================================================================== */
/** @brief 造一个整宽按钮 */
static lv_obj_t *wide_btn(lv_obj_t *parent, const char *text, lv_color_t bg,
                          lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, LV_PCT(100), 44);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_t *l = w_label_create(b, text, watch_font_cn(16), W_COLOR_TEXT);
    lv_obj_center(l);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    }
    return b;
}

static void cancel_connect_cb(lv_event_t *e)
{
    (void)e;
    const w_wifi_hooks_t *h = hooks();
    if (h && h->disconnect) {
        h->disconnect();
    }
    ui.last_state = W_WIFI_IDLE;
    w_modal_refresh();
}

static void wifi_page_build(lv_obj_t *parent, void *user)
{
    (void)user;

    /*
     * ★ 先把"页面销毁时要删轮询定时器"挂上（v0.6.1 修 P0）。
     *
     *   原来这里只有 `ui.poll = NULL` —— 那只是清自己的指针，
     *   定时器本身没人杀。于是"扫描中按返回"会留下一个继续往
     *   已释放对象里写的定时器，详见 page_delete_cb 的说明。
     *
     *   用 remove + add 两步是为了 parent 被复用时不会重复挂同一个回调
     *   （page_delete_cb 本身是幂等的，重复挂也无害，但没必要）。
     */
    lv_obj_remove_event_cb(parent, page_delete_cb);
    lv_obj_add_event_cb(parent, page_delete_cb, LV_EVENT_DELETE, NULL);

    /* 重建时旧定时器一律先杀掉：它引用的对象马上就要被删掉了 */
    if (ui.poll) {
        lv_timer_del(ui.poll);
        ui.poll = NULL;
    }

    /* 对象指针的真正维护者是 page_delete_cb 和下面的重建流程，这里清空只是兜底 */
    ui.list   = NULL;
    ui.status = NULL;
    ui.scanning = false;

    const w_wifi_hooks_t *h = hooks();
    if (!h || !h->get_status) {
        lv_obj_t *l = w_label_create(parent, W_STR_WIFI_UNAVAILABLE,
                                     watch_font_cn(16), W_COLOR_WARN);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        return;
    }

    w_wifi_status_t st;
    h->get_status(&st);

    /* 读一次已保存的 SSID，供 ap_clicked 判断"要不要问密码" */
    ui.saved_ssid[0] = '\0';
    if (h->get_saved_ssid) {
        h->get_saved_ssid(ui.saved_ssid, sizeof(ui.saved_ssid));
    }

    ui.last_state       = st.state;
    ui.last_syncing     = st.syncing;
    ui.last_sync_failed = st.sync_failed;
    ui.last_synced      = st.time_synced;

    /*
     * ★ v0.7.0：toast 的"边沿基准"。
     *
     * 进页面时就把当前状态记为"已提示过" —— 否则"本来就已连上"的用户
     * 一进 WiFi 页就会收到一条"已连接"的 toast，那是**旧闻不是新闻**。
     * 只有进页面**之后**发生的状态变化才值得提醒。
     */
    ui.toasted_state    = st.state;

    /* 状态行 */
    ui.status = w_label_create_auto(parent, "", watch_font_cn(14), 14,
                                    W_COLOR_TEXT_DIM);
    lv_obj_set_width(ui.status, LV_PCT(100));
    lv_label_set_long_mode(ui.status, LV_LABEL_LONG_WRAP);

    /*
     * ---------- 列表容器（必须建在 switch 之前）----------
     *
     * ⚠️ 这里踩过一个坑（用户实测报的"连接失败后扫描不出 wifi"）：
     *    原来列表建在 switch **之后**，而 FAILED 分支里有 `return` ——
     *    于是连接失败后 `ui.list` 是 NULL，
     *    `show_scan_list()` 开头 `if (!ui.list) return;` 直接返回，
     *    用户点"重新扫描"扫到了也**没地方显示**。
     *
     * 教训：只要有分支会提前 return，**公共资源就得在 return 之前建好**，
     *       不能指望"执行流一定会走到后面那段"。
     */
    ui.list = lv_obj_create(parent);
    lv_obj_set_size(ui.list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ui.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.list, 0, 0);
    lv_obj_set_style_pad_all(ui.list, 0, 0);
    lv_obj_set_style_pad_row(ui.list, 5, 0);
    lv_obj_set_flex_flow(ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(ui.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ui.list, LV_DIR_NONE);

    switch (st.state) {
    case W_WIFI_CONNECTED:
        /*
         * 已连上：显示 SSID / IP / 对时状态 + 耗电提示 + 断开/重新对时。
         * 这里**不建扫描列表**，也就不可能"跳回扫描"。
         */
        set_status(W_STR_WIFI_CONNECTED, W_COLOR_ACCENT);
        show_connected(parent, &st);

        if (!ui.hint_shown) {
            ui.hint_shown = true;
            popup(W_STR_WIFI_CONNECTED, W_STR_WIFI_POWER_HINT, NULL);
        }
        /* 已连接也要轮询：对时是异步的，结果要能自动反映到界面 */
        ensure_poll();
        return;                     /* ★ 已连上：到此为止 */

    case W_WIFI_CONNECTING: {
        /*
         * 连接中：明确显示"正在连接 XXX" + 取消按钮。
         *
         * ⚠️ 这一段是修 bug 加的。原来这里只改了个状态文字，
         *    但函数末尾无条件 start_scan()，把状态文字又覆盖成"扫描中"，
         *    用户完全看不出密码有没有被接受。
         *    现在 CONNECTING 直接 return，**绝不触发扫描**。
         */
        char line[64];
        snprintf(line, sizeof(line), "%s %s",
                 W_STR_WIFI_CONNECTING,
                 st.ssid[0] ? st.ssid : (ui.pending_ssid[0] ? ui.pending_ssid : ""));
        w_label_set_text_auto(ui.status, line);
        lv_obj_set_style_text_color(ui.status, W_COLOR_WARN, 0);

        lv_obj_t *tip = w_label_create(parent, W_STR_WIFI_CONNECTING_TIP,
                                       watch_font_cn(14), W_COLOR_TEXT_DIM);
        lv_obj_set_width(tip, LV_PCT(100));
        lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);

        wide_btn(parent, W_STR_WIFI_CANCEL, W_COLOR_CARD, cancel_connect_cb);

        ensure_poll();              /* 状态一变就自动重建 */
        return;                     /* ★ 连接中：绝不触发扫描 */
    }

    case W_WIFI_FAILED:
        /*
         * 连接失败：明确告诉用户原因，并给一个"重新扫描"。
         *
         * ⚠️ 这里**不能 return** —— 下面还要填充列表（ui.list 已在
         *    switch 之前建好）。之前就是因为这里 return 掉了，
         *    表现成"失败之后怎么点都扫不出 wifi"。
         */
        set_status(W_STR_WIFI_FAILED, W_COLOR_WARN);
        /*
         * ★ v0.5.0：如果这次失败用的是"已保存的密码"，服务已经把那条
         *   凭据自动清掉了（用户要求：避免路由器改了密码之后卡死）。
         *   必须把这件事说出来，否则用户会疑惑"我刚存的密码呢"。
         *   清掉之后，重新扫再点这个网络就会**直接问密码**，能救回来。
         */
        if (st.creds_cleared) {
            lv_obj_t *note = w_label_create_auto(parent, W_STR_WIFI_CREDS_CLEARED,
                                                 W_FONT_SMALL, 14, W_COLOR_ACCENT);
            lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(note, LV_PCT(100));
        }
        wide_btn(parent, W_STR_WIFI_RESCAN, W_COLOR_CARD, rescan_cb);
        break;

    default:
        /* 空闲：显示"扫描"按钮（不自动扫，让用户明确触发） */
        set_status(st.auto_reconnect ? W_STR_WIFI_RECONNECTING : W_STR_WIFI_IDLE,
                   W_COLOR_TEXT_DIM);
        wide_btn(parent, W_STR_WIFI_SCAN, W_COLOR_CARD, rescan_cb);
        break;
    }

    /*
     * ---------- 填充列表 ----------
     *
     * 这里要区分两种情况，**不能混着写**（混着写就是
     * "连接失败后看不到失败提示"那个 bug）：
     *
     *   A) 用户主动点了"扫描 / 重新扫描"
     *      -> 清掉失败提示，去扫描
     *   B) 只是状态变化导致的重建（比如刚变成 FAILED）
     *      -> **保留状态文字**，只把已有列表填回去
     *
     * ⚠️ 原来的写法是「if (!ever_scanned) 扫 else 填列表 + set_status(SCAN_DONE)」，
     *    末尾那句 set_status 会把 FAILED 分支刚设好的"连接失败"
     *    **又覆盖成"选一个网络连接"** —— 用户就看不到失败原因了。
     */
    if (ui.rescan_requested) {
        ui.rescan_requested = false;
        ui.ever_scanned     = true;
        list_clear();                   /* 清掉旧结果，免得看着像没扫 */
        start_scan();                   /* 会自己设状态文字 */
    } else if (ui.ever_scanned) {
        show_scan_list();               /* 只填列表，**不动状态文字** */
    } else {
        ui.ever_scanned = true;
        start_scan();                   /* 首次进页面，自动扫一次 */
    }
}

/* ==================================================================
 * 入口
 * ================================================================== */
void watch_wifi_open(void)
{
    /*
     * 每次打开页面都从"干净状态"开始，但**保留 hint_shown** ——
     * 耗电提示是"每次连接只提醒一次"，不是"每次开页面都提醒"。
     * （断开时会把它清掉，所以下次连上还会再提醒一次。）
     */
    const bool keep_hint = ui.hint_shown;
    memset(&ui, 0, sizeof(ui));
    ui.hint_shown = keep_hint;

    w_modal_open(W_STR_WIFI_TITLE, wifi_page_build, NULL);
}
