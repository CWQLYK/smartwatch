/**
 * @file watch_strings.h
 * @brief 界面文案总表 —— **所有中文都写在这里**
 *
 * 为什么要集中放：
 *   中文字体是"按需抽字"的（见 components/fonts/），
 *   如果有中文散落在各个 .c 文件里，就很容易漏字 → 屏幕上出现豆腐块。
 *   统一放这一个头文件后：
 *
 *       加文案 → 写到这里 → 跑 tools/gen_font.ps1 → 收工
 *
 *   字体生成脚本**只需要扫这一个文件**，永远不会漏。
 *
 * 命名约定：W_STR_<用途>；带 %d 之类参数的直接用 printf 风格。
 */
#pragma once

/* ==================== 字库补字（只为让生成脚本扫到） ====================
 *
 * 这个宏**不在界面上显示**，目的是把"只出现在 app 层日志/串口命令行里"的汉字
 * 也纳入字库扫描范围（生成脚本只扫字符串字面量，
 * 而 watch_strings.h 的中文一律会被收进去）。
 *
 * 为什么需要：串口命令行和启动日志里的中文也要能正常显示 ——
 * 它们用的是同一套字库。漏了就是豆腐块（本项目踩过多次）。
 *
 * ★ v0.6.1 追加 16 个（新增的 console 命令 / 背光淡入日志里用到的字）：
 *     亲优危史各塞它支故核溃说越险隔   ← app_console.c（taskmem/stats/bt/crash）
 *     淡                              ← app.c（背光淡入）
 *   处理流程：写到这里 → 跑 tools/gen_font.ps1 -Force → 三条校验脚本会确认覆盖。
 * ================================================================== */
#define W_STR_FONT_EXTRA  "提示凭亲优危史各塞它支故核溃说越险隔淡"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 页面名 / 标题 ==================== */
/*
 * ★ v0.5.0 页面结构变化：
 *     轮播： 时间 → 运动 → 健康 → 功能列表
 *     （原来第 1 页叫"表盘"，现在叫"时间"；第 2 页原来是"设置"，
 *       现在设置降级成"功能列表"里的一个子页）
 */
#define W_STR_HOME          "时间"
#define W_STR_FUNCTIONS     "功能列表"
#define W_STR_SETTINGS      "设置"
#define W_STR_CLOCK         "时钟"
#define W_STR_HEALTH        "健康"
#define W_STR_SPORT         "运动"

/* ==================== 星期 ==================== */
#define W_STR_WEEK_MON      "周一"
#define W_STR_WEEK_TUE      "周二"
#define W_STR_WEEK_WED      "周三"
#define W_STR_WEEK_THU      "周四"
#define W_STR_WEEK_FRI      "周五"
#define W_STR_WEEK_SAT      "周六"
#define W_STR_WEEK_SUN      "周日"
#define W_STR_WEEK_UNKNOWN  "---"

/**
 * @brief ISO 星期号 -> 中文（1=周一 … 7=周日）
 *
 * @note 写成 `static inline` 放在这个头里，是为了让"表盘"和"时钟 > 时间"
 *       两个页面共用同一份映射 —— 复制两份的话，改文案时一定会漏一处。
 *       （放 watch_ui 的控件层不行：那一层不许依赖 watch_ui_pages 的文案表。）
 */
static inline const char *w_weekday_text(int iso_weekday)
{
    switch (iso_weekday) {
    case 1: return W_STR_WEEK_MON;
    case 2: return W_STR_WEEK_TUE;
    case 3: return W_STR_WEEK_WED;
    case 4: return W_STR_WEEK_THU;
    case 5: return W_STR_WEEK_FRI;
    case 6: return W_STR_WEEK_SAT;
    case 7: return W_STR_WEEK_SUN;
    default: return W_STR_WEEK_UNKNOWN;
    }
}

/* ==================== 日期格式 ====================
 *
 * ⚠️ 这几个字必须写在这里，**不能直接写在 watch_page_home.c 的格式串里**。
 *    原因：中文字库是 tools/gen_font.ps1 按需抽字的，而最容易漏的
 *    就是"嵌在 lv_label_set_text_fmt 里的年月日"——
 *    症状非常隐蔽：编译通过、运行正常，屏幕上只有两个豆腐块。
 *    所以日期这类格式化用字也统一登记在文案表里。
 */
#define W_STR_DATE_FMT      "%d年%d月%d日"
#define W_STR_YEAR          "年"
#define W_STR_MONTH         "月"
#define W_STR_DAY           "日"

/* ==================== 表盘指标卡 ==================== */
#define W_STR_STEPS         "步数"
#define W_STR_BPM           "心率"
#define W_STR_BATTERY       "电量"
#define W_STR_CALORIE       "卡路里"
#define W_STR_DISTANCE      "距离"

/* ==================== 控制中心 ==================== */
#define W_STR_BRIGHTNESS    "亮度"
#define W_STR_BRIGHT_HINT   "左右拖动调整屏幕亮度"
#define W_STR_SWIPE_DOWN    "下滑关闭"
#define W_STR_ABOUT         "关于"
#define W_STR_SCREEN_OFF    "息屏"

/* ==================== 占位 / 未实现提示 ==================== */
#define W_STR_NO_DATA       "--"
#define W_STR_NOT_READY     "未接入"
#define W_STR_TIME_UNSET    "时间未校准"
#define W_STR_TIME_HINT     "请用串口命令 set_time 校时"
#define W_STR_IMU_TODO      "计步功能待开发"
#define W_STR_PPG_TODO      "心率需外接 MAX30102"
#define W_STR_SPO2          "血氧"
#define W_STR_PPG           "脉搏波"

/* ==================== 设置页 ==================== */
#define W_STR_VERSION       "版本"
#define W_STR_BUILD         "构建"
#define W_STR_BOARD         "开发板"
#define W_STR_BOARD_NAME    "ESP32-S3 触摸手表 1.69"
#define W_STR_RTC_STATUS    "时钟状态"
#define W_STR_RTC_OK        "走时正常"
#define W_STR_RTC_LOST      "掉电丢失，待校时"

/* ==================== 交互提示 ==================== */
#define W_STR_HINT_SWIPE    "左右滑动切页"
#define W_STR_HINT_UP       "上滑控制中心"

/* ==================== 设置页：可点项（P1 功能 8/9/13 用） ====================
 *
 * ⚠️ 新增中文一律写在这里，别直接写在页面代码里 ——
 *    字库生成脚本会扫这个文件，漏了就是豆腐块（踩过三次）。
 *
 * ★ v0.5.0 删除的三行（连同设置项本身）：
 *     W_STR_WRIST_WAKE  抬腕亮屏  —— 功能取消
 *     W_STR_USER_HEIGHT 身高      —— 步长改为固定值
 *     W_STR_USER_WEIGHT 体重      —— 设置项删除（卡路里用内部固定体重）
 * ================================================================== */
#define W_STR_AUTO_SCREEN   "自动息屏"
#define W_STR_ON            "开"
#define W_STR_OFF           "关"
#define W_STR_SECONDS_SUF   " 秒"
#define W_STR_NEVER         "常亮"
/* 步数目标：设置项仍在（SETTINGS_STEP_GOAL），但 v0.5.0 按要求
 * 从设置页去掉了这一行。字符串留着，以后做"目标达成提醒"时直接接回。 */
#define W_STR_STEP_GOAL     "步数目标"

/* ==================== 开发人员选项 ==================== */
#define W_STR_DEV_TITLE        "开发人员选项"
#define W_STR_DEV_IMU_REC      "IMU 录制"
#define W_STR_DEV_IMU_REC_SUB  "录真实走路数据，回来导出调参"
#define W_STR_DEV_TIME_SET     "时间校准"
#define W_STR_DEV_TIME_SET_SUB "手动设置 UTC 时间"
#define W_STR_DEV_SYSINFO      "系统信息"
#define W_STR_DEV_SYSINFO_SUB  "内存 / 固件 / 存储"
#define W_STR_DEV_HINT         "这些是给开发调试用的功能，普通使用不需要进来。"

/* ==================== IMU 录制页 ==================== */
#define W_STR_REC_START       "开始录制"
#define W_STR_REC_STOP        "停止录制"
#define W_STR_REC_ERASE       "删除全部日志"
#define W_STR_REC_IDLE        "空闲"
#define W_STR_REC_RECORDING   "录制中…"
#define W_STR_REC_UNAVAILABLE "存储不可用，无法录制"
#define W_STR_REC_FRAMES      "帧"
#define W_STR_REC_SECONDS     "秒"
#define W_STR_REC_FILES       "已有日志"
#define W_STR_REC_COUNT_SUF   "个"
#define W_STR_REC_CAPACITY    "存储"
#define W_STR_REC_TIP         "走完先点【停止录制】再拔线，否则会丢一点数据。录制期间不会自动息屏。"

/* ==================== 时间校准页 ==================== */
#define W_STR_TIME_UTC_HINT "本页设置的是 UTC 时间（不是本地时间）。表盘显示时会按设备时区换算。"
#define W_STR_TIME_APPLY    "写入时间"

/* ==================== 系统信息页 ==================== */
#define W_STR_INFO_FW       "固件版本"
#define W_STR_INFO_HEAP     "空闲内存"
#define W_STR_INFO_PSRAM    "PSRAM"
#define W_STR_INFO_UPTIME   "已运行"
#define W_STR_INFO_STORAGE  "存储占用"

/* ==================== WiFi 页 ==================== */
#define W_STR_WIFI_TITLE        "WiFi 连接"
#define W_STR_WIFI_SCAN         "扫描附近网络"
#define W_STR_WIFI_SCANNING     "扫描中…"
#define W_STR_WIFI_SCAN_TIMEOUT "扫描超时，点下面重新扫"
#define W_STR_WIFI_SCAN_DONE    "选一个网络连接"
#define W_STR_WIFI_RESCAN       "重新扫描"
#define W_STR_WIFI_CANCEL       "取消连接"
#define W_STR_WIFI_CONNECTING_TIP "连上后会自动校准时间。"
#define W_STR_WIFI_PICK_AP      "点一个网络连接"
#define W_STR_WIFI_NO_AP        "没扫到网络，点上面重新扫描"
#define W_STR_WIFI_UNAVAILABLE  "WiFi 不可用（请看串口日志）"
#define W_STR_WIFI_PASSWORD     "输入 WiFi 密码"
#define W_STR_WIFI_CONNECTING   "连接中…"
#define W_STR_WIFI_CONNECTED    "已连接"
#define W_STR_WIFI_FAILED       "连接失败（密码错或信号太差）"
/* v0.5.0：用"已保存的密码"连接失败时，服务会自动清掉那条凭据。
 *        必须告诉用户，否则他会疑惑"我刚存的密码怎么没了"。 */
#define W_STR_WIFI_CREDS_CLEARED "已自动清除保存的密码，请重新点这个网络输入新密码"
#define W_STR_WIFI_IDLE         "未连接"
#define W_STR_WIFI_RECONNECTING "正在用已保存的密码重连…"
#define W_STR_WIFI_DISCONNECT   "断开连接"
#define W_STR_WIFI_FORGET       "取消保存"
#define W_STR_WIFI_RESYNC       "重新对时"
#define W_STR_WIFI_SYNCING      "对时中…"
#define W_STR_WIFI_SYNC_FAILED  "对时失败"
#define W_STR_WIFI_SYNCED       "已对时"
#define W_STR_WIFI_NOT_SYNCED   "未对时"
#define W_STR_WIFI_STRONG       "强"
#define W_STR_WIFI_MEDIUM       "中"
#define W_STR_WIFI_WEAK         "弱"
#define W_STR_WIFI_LOCK         "[锁]"
#define W_STR_WIFI_POWER_WARN   "WiFi 已开，耗电较大"
#define W_STR_WIFI_POWER_HINT   "WiFi 保持连接会比较费电。不用的时候请回到本页点【断开】，可以明显延长续航。"
#define W_STR_OK                "知道了"

/* ==================================================================
 * v0.5.0 新增文案（功能列表页 / 时钟页 / 蜂鸣器测试 / 全局提醒）
 *
 * ⚠️ 只允许出现这几类字符，否则一定是豆腐块：
 *      ASCII 0x20-0x7F
 *      CJK 统一汉字   U+4E00-U+9FFF
 *      中文标点       U+3000-U+303F
 *      全角形式       U+FF00-U+FFEF
 *      省略号 …       U+2026（脚本里 ExtraChars 显式加的）
 *    不要用 ·(00B7) °(00B0) →(2192) ×(00D7) 这些 —— 两边都不沾。
 * ================================================================== */

/* ---------- 通用按钮 ----------
 * ⚠️ "取消""确定""返回""停止"这些**没有**放进本表：
 *    用它们的是 watch_ui 组件里的通用控件（滚轮弹窗 watch_wheel.c、
 *    提醒层 watch_alert.c、模态返回键 watch_modal.c）——
 *    而 watch_ui **不许** include 本文件（依赖方向是反的）。
 *    按项目既有约定，那一层的中文直接写字面量，字库脚本照样会收进去。 */
#define W_STR_START         "开始"
#define W_STR_PAUSE         "暂停"
#define W_STR_RESUME        "继续"
#define W_STR_RESET         "复位"

/* ---------- 功能列表页：4 行的副标题 ---------- */
#define W_STR_FUNC_SET_SUB     "亮度 / 息屏 / 显示"
#define W_STR_FUNC_CLOCK_SUB   "秒表 / 倒计时 / 闹钟"
#define W_STR_FUNC_WIFI_SUB    "连接网络并自动校时"
#define W_STR_FUNC_DEV_SUB     "录制 / 系统信息 / 蜂鸣器"

/* ---------- 时钟页：标签栏 ----------
 * 用字符串字面量拼接（C 的相邻字面量会自动接上），
 * 这样字库脚本按引号逐个匹配也能把每个标签的字都收进去。 */
#define W_STR_TAB_TIME      "时间"
#define W_STR_TAB_SW        "秒表"
#define W_STR_TAB_CD        "倒计时"
#define W_STR_TAB_ALARM     "闹钟"
#define W_STR_TABBAR        W_STR_TAB_TIME "\n" W_STR_TAB_SW "\n" \
                            W_STR_TAB_CD "\n" W_STR_TAB_ALARM

/* ---------- 时钟 > 时间 ---------- */
#define W_STR_MANUAL_SET    "手动校时"

/* ---------- 时钟 > 秒表 ---------- */
#define W_STR_SW_HINT       "点【开始】计时，可暂停后继续"

/* ---------- 时钟 > 倒计时 ---------- */
#define W_STR_CD_DURATION   "时长"
#define W_STR_CD_CUSTOM     "定时"
#define W_STR_CD_RING       "响铃时长"
#define W_STR_CD_SET_TITLE  "自定义定时"
#define W_STR_CD_RING_TITLE "响铃时长"
#define W_STR_CD_HINT       "点【时长】循环切换预设，或点【定时】自定义"
#define W_STR_UNIT_HOUR     "时"
#define W_STR_UNIT_MIN      "分"
#define W_STR_UNIT_SEC      "秒"

/* ---------- 时钟 > 闹钟 ---------- */
#define W_STR_ALARM_TIME    "闹钟时间"
#define W_STR_ALARM_ENABLE  "启用闹钟"
#define W_STR_ALARM_TITLE   "设置闹钟"
#define W_STR_ALARM_HINT    "每天重复。到点会响铃并弹提示，点【停止】结束。"
/* ⚠️ RTC 没有备份电池，掉电后时间是 --:--，此时闹钟必须不生效 */
#define W_STR_ALARM_NO_TIME "时间未校准，闹钟不会生效（请先连 WiFi 对时）"

/* ---------- 开发人员选项 > 蜂鸣器测试 ---------- */
#define W_STR_DEV_BUZZER       "蜂鸣器测试"
#define W_STR_DEV_BUZZER_SUB   "按住响，松开停"
#define W_STR_BUZZER_HOLD      "按住试听"
#define W_STR_BUZZER_HINT      "按住上面的按钮，蜂鸣器持续发声；松开手指立即停止。"

/* ---------- 全局到点提醒 ----------
 * 注意：提醒层本身（watch_alert.c）里的中文是字面量，原因见上面"通用按钮"。
 * 这里只留**页面侧**要用的。 */
#define W_STR_ALARM_TITLE_PAGE  "闹钟"

/* ==================================================================
 * v0.6.0 新增：游戏
 *
 * 层级：功能列表 → 游戏 → 游戏列表 → 全屏小恐龙
 * ⚠️ 游戏宿主（watch_ui/watch_game.c）里的"暂停/继续/重新开始/退出/开始/
 *    返回/已暂停/游戏结束/本局得分/最高分"是**字面量**（原因同"通用按钮"），
 *    不要为了统一把它们也搬到这里 —— 那一层 include 不到本文件。
 * ================================================================== */

/** @brief 功能列表第 5 行 + 游戏列表页标题 */
#define W_STR_GAMES             "游戏"
#define W_STR_FUNC_GAMES_SUB    "小恐龙，玩一局"

/** @brief 游戏列表：一款游戏一行 */
#define W_STR_GAME_DINO         "小恐龙"
#define W_STR_GAME_DINO_HINT    "点屏幕跳跃，按住跳更高"
#define W_STR_GAME_BEST_FMT     "最高分 %d"
#define W_STR_GAME_LIST_HINT    "选一个游戏开始。玩的时候点左上角可以暂停或退出。"
#define W_STR_GAME_NO_MORE      "更多游戏还在做"

/** @brief 开发人员选项里的游戏自检入口（占了"蜂鸣器测试"旁边？没有，见下）
 *
 * 这里**故意不加**串口/开发菜单入口：游戏的调试入口在模拟器自检里，
 * 实机上普通用户能点到的路径就够了，开发人员选项保持现状。 */

#ifdef __cplusplus
}
#endif
