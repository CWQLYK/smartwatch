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
 * 这个宏**不在界面上显示**，目的是把"只出现在 app 层日志/提示里"的汉字
 * 也纳入字库扫描范围（生成脚本只扫字符串字面量，
 * 而 watch_strings.h 的中文一律会被收进去）。
 *
 * 为什么需要：串口命令行和启动日志里的中文也要能正常显示 ——
 * 它们用的是同一套字库。漏了就是豆腐块（本项目踩过多次）。
 * ================================================================== */
#define W_STR_FONT_EXTRA  "提示凭"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 页面名 / 标题 ==================== */
#define W_STR_HOME          "表盘"
#define W_STR_SETTINGS      "设置"
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

/* ==================== 设置页：可点项（P1 功能 8/9/12/13 用） ====================
 *
 * ⚠️ 新增中文一律写在这里，别直接写在页面代码里 ——
 *    字库生成脚本会扫这个文件，漏了就是豆腐块（踩过三次）。
 * ================================================================== */
#define W_STR_AUTO_SCREEN   "自动息屏"
#define W_STR_WRIST_WAKE    "抬腕亮屏"
#define W_STR_ON            "开"
#define W_STR_OFF           "关"
#define W_STR_SECONDS_SUF   " 秒"
#define W_STR_NEVER         "常亮"
#define W_STR_STEP_GOAL     "步数目标"
#define W_STR_USER_HEIGHT   "身高"
#define W_STR_USER_WEIGHT   "体重"

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

#ifdef __cplusplus
}
#endif
