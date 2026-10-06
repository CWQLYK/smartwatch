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

#ifdef __cplusplus
}
#endif
