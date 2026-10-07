/**
 * @file watch_game.h
 * @brief 游戏宿主层 —— 全屏游戏外壳（v0.6.0 新增）
 *
 * ## 为什么游戏不能做成"轮播页"，而必须是盖在最上面的全屏层
 *
 * 轮播页是"同一个 screen 里并排的三个槽位"，而且**每次换页都会把三个槽位
 * 整个 `lv_obj_del_async` 掉重建**（见 watch_pages.c 的 slot_build/slot_destroy）。
 * 游戏状态挂在页面上的话，用户手指一滑，整局就没了。
 *
 * 所以游戏层和模态子页（watch_modal.c）、到点提醒层（watch_alert.c）一样，
 * 是**轮播 screen 上的兄弟容器**：
 *
 *     screen
 *     ├── 轮播视口 s_vp（3 个页面槽）
 *     ├── 模态子页栈
 *     ├── 到点提醒层
 *     └── 游戏层 ← 本文件（打开时 move_foreground 置顶）
 *
 * 顺带解决了一个大麻烦：**它天然挡住所有滑动和手势**。
 * LVGL 的命中测试取最上层可点击对象，游戏层满屏且 CLICKABLE，
 * 手指事件根本到不了轮播视口；手势层（watch_gesture）挂在页面根上（更下层），
 * 也收不到事件。所以"玩游戏时不能左右滑切页/上滑开控制中心"
 * **不需要改手势代码**，是层级自带的。
 *
 * ## 分层：宿主 vs 游戏本体
 *
 * 宿主（本文件，在 watch_ui 组件里）负责**所有游戏都要有的东西**：
 *   全屏层、帧定时器、触摸转发、开始/暂停/结束面板、最高分读写、
 *   防息屏、闹钟到点自动暂停。
 *
 * 游戏本体（在 watch_ui_pages/games/ 里）只实现 `w_game_t` 那几个回调：
 *   物理、碰撞、生成、画到画布上。
 *
 * 这样加第二个游戏 = 写一个文件 + 在游戏列表里注册一行，外壳一分钱不用改。
 *
 * ## 中文为什么是字面量
 *
 * 本文件在 watch_ui 里，而 `watch_strings.h` 在 watch_ui_pages 里 ——
 * 依赖方向是反的。按项目既有约定（watch_modal.c 的"返回"、watch_alert.c 的"停止"、
 * watch_wheel.c 的"取消/确定"），这一层的中文直接写字面量，
 * 字库生成脚本扫字符串字面量，照样能收进字库。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、游戏本体的接口
 * ================================================================== */

/**
 * @brief 一个游戏的完整定义
 *
 * 生命周期：
 *     open()    宿主建好画布后调 init() 一次
 *     开始/重开 调 reset()
 *     每帧      先 step(dt) 推进物理，再 render(canvas) 画进画布
 *     触摸      按/放都走 input()，坐标是**画布坐标**（左上角原点）
 *     退出      调 destroy()
 */
typedef struct {
    const char *id;         /**< 唯一标识，如 "dino" */
    const char *title;      /**< 中文标题（开始/暂停/结束面板上显示） */
    const char *hint;       /**< 玩法提示（开始面板上显示） */
    const char *best_key;   /**< 最高分存哪个设置项（见 watch_pages.h 的 W_SETTING_*） */

    /** @brief 画布尺寸；传 0 表示用整屏（W_SCREEN_W x W_SCREEN_H） */
    lv_coord_t canvas_w;
    lv_coord_t canvas_h;

    /** @brief 建状态。返回 0 成功；非 0 则宿主放弃打开 */
    int  (*init)(void);

    /** @brief 开新一局（分数清零、障碍清空） */
    void (*reset)(void);

    /** @brief 推进一帧。dt_ms 是**实测**间隔，不是定时器周期 */
    void (*step)(uint32_t dt_ms);

    /** @brief 把当前画面画进画布（直接改画布位图，不用 LVGL 的绘图 API） */
    void (*render)(lv_obj_t *canvas);

    /** @brief 触摸：down=true 按下，false 抬起；坐标是画布坐标 */
    void (*input)(int x, int y, bool down);

    /** @brief 当前分数（面板显示用） */
    int  (*score)(void);

    /** @brief 这一局是否结束了 */
    bool (*over)(void);

    /**
     * @brief 本帧至少要重画的起始 y（画布坐标）
     *
     * 为什么需要：整屏重画是 240x280x2B = 134KB，按 40MHz SPI 要 27ms，
     * 帧率直接掉到 35FPS 以下。上面的天空是不动的，只有下面那条带子会变，
     * 所以只需要重画 [dirty_top, canvas_h) 这一段。
     * 分数标签和暂停按钮在 dirty_top 之上，只在数字变化时才重画。
     *
     * 可以传 NULL，那就每帧整屏重画（简单但慢，调不动再优化时用得上）。
     */
    lv_coord_t (*dirty_top)(void);

    /** @brief 退出时释放游戏自己的资源（可 NULL） */
    void (*destroy)(void);
} w_game_t;

/* ==================================================================
 * 二、宿主生命周期
 * ================================================================== */

/**
 * @brief 初始化宿主（由 watch_pages.c 在建立轮播后调一次）
 * @param host 挂到哪个容器上（传轮播的 screen）
 */
void watch_game_init(lv_obj_t *host);

/**
 * @brief 打开游戏（全屏）
 * @return 0 成功；-1 失败（没初始化 / 已经在玩 / init 回调失败）
 *
 * @note 打开时宿主会：
 *       · move_foreground 置顶（盖住模态和提醒层之外的一切）
 *       · w_screen_hold_awake(true) —— 玩游戏期间不许自动息屏
 *       · 起一个 33ms 的帧定时器
 */
int w_game_open(const w_game_t *g);

/**
 * @brief 关闭游戏（等价于点"退出"）
 *
 * @note 会顺带刷新底下打开着的模态子页（游戏列表要更新最高分），
 *       并解除"别息屏"。
 */
void w_game_close(void);

/** @brief 当前是否有游戏开着（不管在玩还是暂停） */
bool w_game_is_open(void);

/* ==================================================================
 * 三、自检 / 调试钩子（PC 模拟器自检用，实机不用）
 *
 * 为什么要有：帧定时器是按真实时间跑的，自检里没法一边等一边断言。
 * 所以开一组"手动推帧 + 手动触摸"的口子，让自检能把一局游戏
 * 确定性地跑完（物理、碰撞、结束、存档全都能验证）。
 * ================================================================== */

/** @brief 手动推进 n 帧，每帧 dt_ms；推帧期间内部定时器暂停 */
void w_game_debug_step(int frames, int dt_ms);

/** @brief 模拟按下（画布坐标） */
void w_game_debug_press(int x, int y);

/** @brief 模拟抬起 */
void w_game_debug_release(void);

/** @brief 模拟一次完整点击（按下+抬起） */
void w_game_debug_click(int x, int y);

/** @brief 当前状态：0=开始面板 1=游戏中 2=暂停 3=结束 -1=没开 */
int w_game_debug_state(void);

/** @brief 状态名的中文（日志/自检输出用） */
const char *w_game_debug_state_name(void);

/** @brief 已经推进过多少帧（自检用来确认定时器/手动推帧真的在跑） */
int w_game_debug_frames(void);

/** @brief 当前游戏定义（可能是 NULL） */
const w_game_t *w_game_debug_game(void);

/**
 * @brief 把画布位图整个填成指定索引（自检用：确认画布真的在写）
 * @return 实际写入的像素数
 */
int w_game_debug_fill_canvas(int idx);

/**
 * @brief 等价于用户按了左上角的"暂停"键（自检截图暂停面板用）
 *
 * @note 只在 GS_PLAY 状态下生效，和真按钮行为一致。
 */
void w_game_debug_pause(void);

/**
 * @brief 点当前面板上的主按钮（开始 / 继续 / 再来一次）
 *
 * @return true = 找到了按钮并发出点击事件
 *
 * @note 发的是真正的 `LV_EVENT_CLICKED`，走按钮自己的回调，
 *       **不是**直接把状态改成"游戏中" —— 否则"按钮回调断链"
 *       这种最典型的低级错误，自检是发现不了的。
 */
bool w_game_debug_click_primary(void);

#ifdef __cplusplus
}
#endif
