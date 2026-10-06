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

/* ==================================================================
 * 四、内置页面（定义在 watch_ui_pages 组件里）
 * ================================================================== */

/** @brief 表盘：时间 / 日期 / 星期 / 电量（P1 功能 3、4） */
extern const watch_page_t watch_page_home;

/** @brief 设置页 */
extern const watch_page_t watch_page_settings;

/** @brief 健康页（P4 做心率，本轮占位） */
extern const watch_page_t watch_page_health;

/** @brief 运动页（P1 功能 6/7 计步） */
extern const watch_page_t watch_page_sport;

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
 */
#define W_SETTING_AUTO_SCREEN  "auto_screen"   /* 自动息屏秒数，0=常亮 */
#define W_SETTING_WRIST_WAKE   "wrist_wake"    /* 抬腕亮屏 0/1 */
#define W_SETTING_STEP_GOAL    "step_goal"     /* 每日步数目标 */
#define W_SETTING_HEIGHT       "height"        /* 身高 cm */
#define W_SETTING_WEIGHT       "weight"        /* 体重 kg */

/** @brief 读设置项（钩子没接时返回 def） */
int watch_ui_setting_get(const char *key, int def);

/** @brief 写设置项（钩子没接时什么都不做） */
int watch_ui_setting_set(const char *key, int value);

#ifdef __cplusplus
}
#endif
