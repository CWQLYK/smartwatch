/**
 * @file watch_pages.h
 * @brief 页面管理器（跟手轮播）+ 数据源注入接口
 *
 * 两个关键设计：
 *
 * 【1】页面 = 一个小接口，不是一个巨型 switch
 *     每个页面自己声明"叫什么、怎么建、什么时候刷新"，管理器不认识任何具体页面：
 *
 *         static const watch_page_t k_home_page = {
 *             .id = "home", .title = "首页",
 *             .create = home_create, .refresh = home_refresh,
 *         };
 *
 *     加页面 = 新写一个 .c + 在注册表里加一行，**不动框架**。
 *
 * 【2】数据通过**注入的 provider** 拿，而不是直接 include data_hub.h
 *     这样 watch_ui / watch_ui_pages 里**没有一行 ESP-IDF 代码**，
 *     同一套页面代码能直接在 PC 模拟器里跑（模拟器注入一个假数据源）。
 *
 * ==================================================================
 * ⚠️ v0.3 重大结构变化：页面从 "screen" 改成 "普通对象"，滑动改跟手轮播
 * ==================================================================
 *
 * **为什么必须改**（结论逐条对照过本项目自带的 LVGL 8.4 源码，不是猜的）：
 *
 *   1. 想要**跟手**滑动，就要让内容跟着手指实时位移。
 *      最省事、手感最好的做法是用 LVGL 自己的滚动 + 滚动吸附（scroll snap），
 *      而 LVGL 8.4 **有** `lv_obj_set_scroll_snap_x()`（`lv_obj_scroll.h:76`），
 *      松手后的"速度预测 + 吸附动画"是白送的
 *      （`lv_indev_scroll.c:165-172`）。
 *
 *   2. 但滚动吸附要求"相邻页并排在同一个父对象里"。
 *      **screen 之间无法并排**：
 *        · 没有 API 能把 screen 摘下来重挂父对象
 *          —— `lv_obj_remove_from_parent()` 在 8.4 不存在；
 *        · `lv_obj_set_parent(screen, obj)` 会被直接拒绝：
 *          `lv_obj_tree.c:132` → `LV_LOG_WARN("Can't set the parent of a screen"); return;`
 *
 *   3. 也不能"拖到一半再交接给 lv_scr_load_anim"：
 *      `lv_disp.c:257` 会 `lv_anim_del(new_scr, NULL)` 杀掉你喂的动画，
 *      `:261` 强制 `lv_obj_set_pos(new_scr, 0, 0)` 把预热位置清零，
 *      `:313` 入场位移硬编码整屏宽 —— **物理上必然跳变**。
 *
 *   所以：**页面根从 screen 改成普通 `lv_obj`，放进一个可横向滚动的 viewport**。
 *   控制中心（watch_overlay）不受影响，它仍是独立 screen（模态覆盖层，不跟手）。
 *
 * 对写页面的影响：`create()` 多一个 parent 参数，用 `w_page_create(parent)` 造根。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、UI 看到的数据（和设备/驱动完全解耦的纯数据）
 * ================================================================== */

/** @brief 时间 */
typedef struct {
    int  year;      ///< 完整年份
    int  month;     ///< 1-12
    int  day;       ///< 1-31
    int  hour;      ///< 0-23
    int  minute;    ///< 0-59
    int  second;    ///< 0-59
    int  weekday;   ///< 1=周一 … 7=周日
    bool valid;     ///< false → UI 显示 "--:--"
} watch_time_t;

/** @brief 电量 */
typedef struct {
    int  millivolt;   ///< mV；0 = 未知
    int  percent;     ///< 0-100；-1 = 未知
    bool charging;    ///< 是否充电中
    bool present;     ///< 是否有电池
} watch_battery_t;

/** @brief 运动数据（IMU 未接入时 available=false，UI 显示 "--"） */
typedef struct {
    int  steps;
    int  kcal;
    int  meters;
    bool available;
} watch_steps_t;

/** @brief UI 用的状态快照 */
typedef struct {
    watch_time_t    time;
    watch_battery_t battery;
    watch_steps_t   steps;
} watch_state_t;

/** @brief 快照里哪部分变了 */
#define WATCH_CHANGED_TIME     0x01u
#define WATCH_CHANGED_BATTERY  0x02u
#define WATCH_CHANGED_STEPS    0x04u
#define WATCH_CHANGED_ALL      0x07u

/** @brief 数据变更通知（由 provider 调用） */
typedef void (*watch_state_cb_t)(const watch_state_t *state, uint32_t changed);

/**
 * @brief 数据源接口
 *
 * @note 两个函数都可能在**非 UI 任务**里被调用，
 *       watch_ui 内部已经负责加锁，provider 实现里不要再加 LVGL 锁。
 */
typedef struct {
    /** @brief 取当前快照 */
    void (*get_state)(watch_state_t *out);

    /**
     * @brief 注册"数据变了"回调
     * @return 订阅句柄（>=0），失败返回 -1
     */
    int  (*subscribe)(watch_state_cb_t cb);

    /** @brief 注销回调 */
    void (*unsubscribe)(int handle);
} watch_data_provider_t;

/* ==================================================================
 * 二、页面定义
 * ================================================================== */

struct watch_page_s;

/**
 * @brief 页面定义
 *
 * 生命周期：
 *   create(parent)  轮播需要**预先建好相邻页**，所以页面在窗口重排时创建
 *   refresh(root)   数据变化或页面重新可见时调用，只更新数值
 *   destroy(root)   页面被移出窗口时调用（可 NULL，框架会删掉容器）
 */
typedef struct watch_page_s {
    const char *id;         ///< 唯一标识，如 "home" / "sport"
    const char *title;      ///< 页面标题（中文）

    /**
     * @brief 建界面，返回页面根对象
     * @param parent 页面容器（**必须**把自己的根挂到它下面）
     *
     * @note ⚠️ v0.3 起签名带 parent —— 因为滑动轮播要求"相邻页并排放在
     *       同一个可滚动容器里"，而 LVGL 的 screen 无法被塞进容器
     *       （见文件头说明）。请用 `w_page_create(parent)` 造根。
     */
    lv_obj_t *(*create)(lv_obj_t *parent);

    /** @brief 刷新数值。state 保证非 NULL；changed 见 WATCH_CHANGED_xxx */
    void (*refresh)(lv_obj_t *root, const watch_state_t *state, uint32_t changed);

    /** @brief 页面被移出窗口时清理（可 NULL） */
    void (*destroy)(lv_obj_t *root);
} watch_page_t;

/* ==================================================================
 * 三、管理器接口
 * ================================================================== */

/**
 * @brief 初始化界面（建跟手轮播 + 订阅数据）
 *
 * @param provider 数据源（**必须非 NULL**；模拟器也要给一个）
 * @param home     初始页面（必须已注册）
 * @param unused   历史参数，传 0 即可（v0.3 起轮播没有"栈深"概念）
 *
 * @note 调用前必须先 ui_port_init() 成功，并且调用者要持有 UI 锁。
 */
esp_err_t watch_ui_init(const watch_data_provider_t *provider,
                        const watch_page_t *home,
                        int unused);

/**
 * @brief 注册一个页面（可注册多个）
 * @note 注册表是静态数组，最多 12 个页面
 */
esp_err_t watch_ui_register_page(const watch_page_t *page);

/**
 * @brief 跳到第 index 页（带动画；循环取模）
 */
esp_err_t watch_ui_goto(int index);

/** @brief 下一张（相当于左滑） */
esp_err_t watch_ui_next(void);

/** @brief 上一张（相当于右滑） */
esp_err_t watch_ui_prev(void);

/** @brief 当前居中页的定义 */
const watch_page_t *watch_ui_current(void);

/** @brief 当前居中页的下标 */
int watch_ui_current_index(void);

/** @brief 手动触发当前页刷新 */
void watch_ui_refresh(void);

/** @brief 取当前页的根对象（控件查找/调试用） */
lv_obj_t *watch_ui_current_root(void);

/** @brief 跟手轮播的 screen 对象（首次 lv_scr_load 用） */
lv_obj_t *watch_ui_screen(void);

/** @brief 打一份页面状态日志 */
void watch_ui_dump(void);

/**
 * @brief 调试用：取轮播当前的 scroll_x（-1 表示还没建好）
 *
 * 排查"滑不动"时的关键数字：拖动过程中它应该跟着手指变。
 * 如果它一直是 240 不动，说明滚动根本没被触发。
 */
int watch_ui_debug_scroll_x(void);

/** @brief 调试用：打印轮播的滚动范围和槽位坐标（查"滑不动"用） */
void watch_ui_debug_dump(void);

/**
 * @brief 调试用：当前页能纵向滚多少像素（0 = 内容不满一屏，不会滚）
 *
 * 排查"某个按钮在屏幕外够不着"时先看这个数：
 * 大于 0 说明页面**应该**能滚；如果实际滚不动，问题在拖动处理而不是布局。
 */
int watch_ui_debug_page_scroll(void);

/**
 * @brief 调试用：按拖动路径滚当前页
 *
 * @param dy **手指位移语义**（和手指拖动一致）：
 *           负值 = 手指往上划 = 看下面的内容；正值 = 手指下划 = 回顶部。
 *           页面已经在顶部时传正值会被边界夹住（纹丝不动，这是对的）。
 * @return 滚完之后还剩多少可滚距离
 *
 * @note v0.6.0 加的。功能列表加到 5 行之后内容超过一屏，
 *       模拟器自检需要确认"真的能滚、第 5 行真的够得着"。
 *       这里走的是和手指拖动**同一条**代码路径（scroll_page_by）。
 */
int watch_ui_debug_scroll_by(int dy);

/** @brief 调试用：当前页已经滚到哪个位置（0 = 顶部） */
int watch_ui_debug_page_scroll_y(void);

/**
 * @brief 调试用：模拟一次触摸的"按下 / 移动 / 抬手"
 *
 * @note 走的是**和真手指完全相同**的处理函数（drag_begin / drag_move / drag_end），
 *       不是另抄一份逻辑 —— 所以自检通过能说明真机行为。
 *
 *       为什么需要它：用户实测报过"在功能列表上下滑动，滑动后会误进某一项"
 *       （触摸到了某一行、滑动之后抬手就进去了）。这个问题只有
 *       "按下 → 移动 → 抬手"这一整条路径才暴露得出来，
 *       而模拟器没法真的注入触摸事件（这个环境的 SendInput 不生效）。
 *
 *       典型用法（自检）：
 *           watch_ui_debug_touch_begin(120, 116);   // 按在「时钟」那一行上
 *           watch_ui_debug_touch_move(120, 176);    // 往下拖 60px（滑列表）
 *           watch_ui_debug_touch_end();             // 抬手
 *           // 此时 w_touch_was_drag() 应为 true，
 *           // 再补一个 LV_EVENT_CLICKED 给那一行，它必须**不**响应
 */
void watch_ui_debug_touch_begin(int x, int y);

/** @brief 调试用：触摸移动到 (x, y) */
void watch_ui_debug_touch_move(int x, int y);

/** @brief 调试用：抬手 */
void watch_ui_debug_touch_end(void);

/**
 * @brief 调试用：取三个槽位里各页面的子对象数（-1 = 空槽）
 *
 * @note 用来确认"当前页"和"槽位里的页"一致 —— 两者会不一致是因为
 *       `watch_ui_goto()` 一次只翻一页、而槽位要等吸附动画跑完才重排，
 *       这时量出来的布局全是错的（v0.6.0 自检在这里栽过一次）。
 */
void watch_ui_debug_slot_page_children(int *out, int max);

/* ==================================================================
 * 四、内置页面（定义在 watch_ui_pages 组件里）
 *
 * ★ v0.5.0 起轮播只有 4 页，顺序 = watch_pages_all.c 里的数组顺序：
 *      时间 → 运动 → 健康 → 功能列表
 *   原来的"设置"不再是轮播页，改成功能列表里的模态子页
 *   （入口 `watch_settings_open()`，声明在下面第五节）。
 * ================================================================== */

/** @brief 时间页（原"表盘"）：时间 / 日期 / 星期 / 电量 / 步数 */
extern const watch_page_t watch_page_home;

/** @brief 运动页：步数 / 距离（卡路里卡 v0.5.0 暂时去掉） */
extern const watch_page_t watch_page_sport;

/** @brief 健康页（P4 做心率，本轮占位） */
extern const watch_page_t watch_page_health;

/** @brief 功能列表页：设置 / 时钟 / WiFi 连接 / 开发人员选项 */
extern const watch_page_t watch_page_functions;

/** @brief 工具页（v0.7.0）：计算器，以后挂日历/天气这类纯工具 */
extern const watch_page_t watch_page_tools;

/** @brief 获取所有内置页面的注册表（NULL 结尾） */
const watch_page_t *const *watch_pages_all(void);

/** @brief 内置页面数量 */
int watch_pages_count(void);

/* ==================================================================
 * 五、平台钩子：亮度读写（控制中心的滑块用）
 *
 * 为什么用钩子而不是直接 include settings.h / bsp.h：
 *   框架层要保持"零 ESP-IDF 依赖"才能在 PC 模拟器里跑。
 *   app 启动时把它接到 settings + bsp_backlight_set()；
 *   模拟器里不接，滑块就自然变成"只改数值不生效"，不影响跑界面。
 * ================================================================== */

/** @brief 读当前亮度 0-100 */
typedef int  (*watch_brightness_get_fn)(void);

/** @brief 设亮度 0-100（返回 esp_err_t 的整数值，0 = 成功） */
typedef int  (*watch_brightness_set_fn)(int percent);

/** @brief 注册亮度读写钩子（两个都要给，或都不给） */
void watch_ui_set_brightness_hooks(watch_brightness_get_fn get_fn,
                                   watch_brightness_set_fn set_fn);

/** @brief 读亮度（钩子没接时返回默认 70） */
int watch_ui_brightness_get(void);

/** @brief 设亮度（钩子没接时什么都不做） */
int watch_ui_brightness_set(int percent);

/* ==================================================================
 * 六、平台钩子：设置项读写（设置页用）
 *
 * 和亮度钩子同样的理由：`watch_ui_pages` 里不许出现 ESP-IDF 头文件
 * （否则 PC 模拟器编不过）。设置页要读写"自动息屏/抬腕亮屏/步数目标/
 * 身高体重"，所以这里开一组按名字寻址的通用钩子：
 *
 *     界面层：watch_ui_setting_get("timeout", 30)
 *     app 层：接到 settings_get_or_default() / settings_set()
 *     模拟器：不接 → 用默认值，界面照跑（改动不落盘而已）
 *
 * ⚠️ 界面层**不要**用字面量，统一走下面的 watch_ui_setting_* 包装函数，
 *    键名集中在那里，改的时候一处就够。
 * ================================================================== */

/** @brief 读设置项：找不到就返回 def */
typedef int (*watch_setting_get_fn)(const char *key, int def);

/** @brief 写设置项（返回 esp_err_t 的整数值，0 = 成功） */
typedef int (*watch_setting_set_fn)(const char *key, int value);

/** @brief 注册设置读写钩子（两个都要给，或都不给） */
void watch_ui_set_setting_hooks(watch_setting_get_fn get_fn,
                                watch_setting_set_fn set_fn);

/**
 * @brief 界面层用的设置项键名
 *
 * @note 这些名字由 app 层映射到真正的 NVS 设置项
 *       （见 components/app/app.c 的 settings 适配）。
 *       加新设置项：这里加一个键名 + app 层加一行映射。
 *
 * ★ v0.5.0 删除了三个键名（连同 settings 里的枚举和 NVS 键一起）：
 *     wrist_wake  —— 抬腕亮屏功能取消（用户明确要求）
 *     height      —— 步长改为固定 50cm/90cm，不再按身高推算
 *     weight      —— 体重设置项删除（卡路里改用内部固定体重 65kg）
 */
#define W_SETTING_AUTO_SCREEN  "auto_screen"   /* 自动息屏秒数，0=常亮 */
#define W_SETTING_STEP_GOAL    "step_goal"     /* 每日步数目标 */

/* ---------- v0.7.0 新增：时区（只读，给日历页算本地日期用）----------
 *
 * 时区本来只在设备侧的 `SETTINGS_TIMEZONE` 里，界面层拿不到。
 * 日历要"UTC epoch → 本地年月日"，所以把它按同一套"按名字寻址"的钩子暴露出来。
 * 单位是 **15 分钟**（+8 区 = 32），和设备侧一致。 */
#define W_SETTING_TZ           "tz"

/* ---------- v0.5.0 新增：时钟服务要持久化的 5 项 ----------
 *
 * 时钟服务（watch_clock.c）跑在 watch_ui 里，**不能 include settings.h**
 * （那会把 ESP-IDF 依赖带进来，PC 模拟器就编不过了）。
 * 所以它读写 NVS 也走这套"按名字寻址"的钩子，
 * 由 app.c 的 setting_get/setting_set 映射到真正的 SETTINGS_* 枚举。
 */
#define W_SETTING_ALARM_HOUR   "alarm_hour"    /* 闹钟-时 0-23 */
#define W_SETTING_ALARM_MIN    "alarm_min"     /* 闹钟-分 0-59 */
#define W_SETTING_ALARM_EN     "alarm_en"      /* 闹钟开关 0/1 */
#define W_SETTING_CD_PRESET    "cd_preset"     /* 倒计时时长(秒) */
#define W_SETTING_CD_RING      "cd_ring"       /* 倒计时响铃时长(秒) */

/* ---------- v0.6.0 新增：游戏存档 ----------
 *
 * 由游戏宿主（watch_game.c）读写，游戏本体只提供键名
 * （见 w_game_t::best_key）。加新游戏就再加一个键名 + app 层一行映射。 */
#define W_SETTING_DINO_BEST    "dino_best"     /* 小恐龙最高分 */

/* ---------- v0.7.0 新增：2048 最高分 ----------
 *
 * 加一款游戏就再加一个键名 + app 层一行映射（见 watch_page_games.c 的文件头）。*/
#define W_SETTING_2048_BEST    "best_2048"     /* 2048 最高分 */

/* ---------- v0.7.0 新增：天气经纬度（整数微度，见 watch_page_weather_city.c）----------
 *
 * 单位 1e-5 度：上海 = 3123000 / 12147000。
 * 为什么用整数：设置钩子签名是 `int`，而且 NVS 里整数没有 float 的精度问题。*/
#define W_SETTING_WX_LAT       "wx_lat"        /* 纬度，1e-5 度 */
#define W_SETTING_WX_LON       "wx_lon"        /* 经度，1e-5 度 */

/* ---------- v0.7.0 新增：WiFi 开机自动连接 ----------
 *
 * 默认 0（关）。以前是"有凭据就一定自动连"，AP 不在时会一直卡在
 * "连接中"，用户既没让它连也没法退出 → 改成由这个设置项控制。
 * 界面在设置页（watch_page_settings.c），设备侧在 wifi_service_init() 读。 */
#define W_SETTING_WIFI_AUTO    "wifi_auto"     /* 开机自动连 WiFi 0/1 */

/** @brief 读设置项（钩子没接时返回 def） */
int watch_ui_setting_get(const char *key, int def);

/** @brief 写设置项（钩子没接时什么都不做） */
int watch_ui_setting_set(const char *key, int value);


/* ==================================================================
 * 子页面入口（功能列表页 / 设置页调用）
 *
 * 这些**不是轮播页**，而是盖在轮播上的模态子页（见 watch_modal.h）。
 * 声明集中放这里，是为了让页面只 include 一个头文件。
 *
 * ★ v0.5.0 的页面结构：
 *
 *     轮播 4 页：  时间(表盘) → 运动 → 健康 → 功能列表
 *
 *     功能列表
 *     ├── 设置      → watch_settings_open()   （亮度/自动息屏/步数目标/时钟状态）
 *     ├── 时钟      → watch_clock_open()      （标签栏：时间/秒表/倒计时/闹钟）
 *     ├── WiFi 连接 → watch_wifi_open()       （已有）
 *     └── 开发人员选项 → watch_developer_open_menu()（IMU录制/系统信息/蜂鸣器测试）
 *
 * ★ v0.7.0：轮播加了第 5 页「工具」，承载纯工具类功能：
 *
 *     工具
 *     └── 计算器    → watch_calc_open()       （求值逻辑在 calc_engine.c，纯 C）
 * ================================================================== */

/** @brief 打开"设置"子页 */
void watch_settings_open(void);

/** @brief 打开"计算器"子页（工具页进去） */
void watch_calc_open(void);

/** @brief 打开"日历"子页（工具页进去）—— LVGL 自带 lv_calendar，最便宜的一个 */
void watch_calendar_open(void);

/** @brief 打开"天气"子页（工具页进去）—— 数据经 w_platform_weather() 钩子来 */
void watch_weather_open(void);

/** @brief 打开"城市坐标"子页（天气页里再进去，二级） */
void watch_weather_city_open(void);

/* ---------- 计算器的自检接口（v0.7.0）----------
 *
 * 求值逻辑本身由 `calc_engine.c` 的测试向量覆盖（纯函数，34 条）。
 * 下面两个是给自检驱动**界面**用的：按键怎么拼表达式、显示有没有更新，
 * 这两件事只有真的按一遍才知道。
 *
 * @note 只给 PC 模拟器自检用；页面没打开时返回 NULL。
 */
lv_obj_t *watch_calc_debug_btnmatrix(void);
const char *watch_calc_debug_expr_text(void);

/** @brief 打开"游戏"列表子页（功能列表第 5 行进去） */
void watch_games_open(void);

/** @brief 打开"时钟"子页（内含 4 个标签） */
void watch_clock_open(void);

/**
 * @brief 调试/自检用：切换时钟页的标签（0=时间 1=秒表 2=倒计时 3=闹钟）
 *
 * @return true = 切换成功（时钟页开着且下标合法）
 *
 * @note 时钟页没开时返回 false。给 PC 模拟器自检用来把 4 个标签的
 *       构建 + 渲染都跑一遍 —— 这是新 UI 里最容易出问题的部分。
 */
bool watch_clock_debug_select_tab(int tab);

/** @brief 打开"手动校时"子页（时钟 > 时间 里进入；填 UTC） */
void watch_time_set_open(void);

/** @brief 打开"开发人员选项"菜单 */
void watch_developer_open_menu(void);

/**
 * @brief 直接打开"蜂鸣器测试"子页
 *
 * @note 菜单行点进去走的是同一个入口。单独导出是为了：
 *       · PC 模拟器自检可以直接把它开起来截图
 *       · 以后串口加个 `buzzer` 测试命令也方便
 */
void watch_developer_open_buzzer(void);

/**
 * @brief 取当前 UTC 时间戳（界面层拿时间的唯一途径）
 *
 * @return Unix 时间戳；RTC 没校过时返回 0
 *
 * @note 界面层不许用 time.h，所以时间统一从这里问。
 *       ⚠️ 口径提醒：RTC 里存的是**墙上时间**（见 data_hub.c 顶部约定），
 *          所以这个函数算出来的其实是"把墙上时间当成 UTC 表示的 epoch"。
 *          闹钟/倒计时比较时间请直接用 `watch_state_t.time` 的
 *          hour/minute（同样是墙上时间），别跟这里绕。
 */
uint32_t watch_ui_current_epoch(void);

/**
 * @brief 当前时间是否已校准（RTC 有没有掉过电）
 *
 * @note 模态子页（设置/时钟…）**拿不到** `watch_state_t` ——
 *       数据变更回调只发给轮播页。但它们要显示"时钟状态"，
 *       所以这里开一个直接查询的口子。
 *
 * @return true = 时间有效；false = 未校准（界面应显示"掉电丢失，待校时"）
 */
bool watch_ui_time_valid(void);

/**
 * @brief 取当前墙上时间的完整快照（模态子页用）
 *
 * @param out 输出；`out->valid == false` 表示 RTC 未校准
 * @return true = 时间有效
 *
 * @note 模态子页拿不到 watch_state_t（数据变更回调只发给轮播页），
 *       但"时钟 > 时间"栏要显示年月日时分秒，所以开这个口子。
 */
bool watch_ui_now(watch_time_t *out);

/** @brief 打开"WiFi 连接"页 */
void watch_wifi_open(void);

/**
 * @brief 由 app 层注入系统信息（界面层拿不到 esp_* API）
 *
 * @param fw      固件版本
 * @param heap    空闲内存（已格式化，如 "8.5 MB"）
 * @param psram   PSRAM（已格式化）
 * @param uptime  运行时长（已格式化）
 */
void watch_developer_set_sysinfo(const char *fw, const char *heap,
                                 const char *psram, const char *uptime);

#ifdef __cplusplus
}
#endif
