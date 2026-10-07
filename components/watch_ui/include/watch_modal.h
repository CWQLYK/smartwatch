/**
 * @file watch_modal.h
 * @brief 通用子页面容器（模态、可嵌套一层）
 *
 * ## 为什么不直接新建 screen
 *
 * 轮播页是"同一个 screen 里的并排容器"（v0.3.0 改成这样是为了跟手滑动）。
 * 子页面（WiFi / 开发人员选项 / IMU 录制 / 时间校准）**不该参与滑动**，
 * 但也不该各自 `lv_obj_create(NULL)` 造 screen —— 那样：
 *   - 会和轮播的 screen 抢 `lv_scr_act()`，返回时状态容易错乱
 *   - 没法自然继承轮播的层级关系
 *   - 子页面里再开子页（开发人员 → IMU 录制）时栈管理会很乱
 *
 * 所以做成**盖在轮播容器上的普通容器**，自带返回按钮。
 *
 * ## 栈式 API
 *
 * 支持"开发人员选项 → IMU 录制"这种一层嵌套：
 * 每个子页 open 时压栈，返回时弹栈并自动刷上一层。
 * 目前限制最多 `W_MODAL_MAX_DEPTH` 层 —— 240×280 的屏也堆不了更深。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define W_MODAL_MAX_DEPTH   4

/**
 * @brief 子页面构建回调
 *
 * @param parent  挂载父对象（子页面的根容器已经建好，往里放控件即可）
 * @param user    透传指针
 */
typedef void (*w_modal_build_cb_t)(lv_obj_t *parent, void *user);

/**
 * @brief 打开一个子页面
 *
 * @param title   顶部标题（中文，会走中文字库）
 * @param build   构建回调
 * @param user    透传给构建回调
 *
 * @return 0 成功；-1 失败（栈满 / 参数为空）
 *
 * @note 会自动加一个左上角"返回"按钮，点了就弹栈。
 *       子页面里不要再自己做返回键（重复）。
 */
int w_modal_open(const char *title, w_modal_build_cb_t build, void *user);

/**
 * @brief 关闭当前子页面（等价于点"返回"）
 */
void w_modal_close(void);

/** @brief 当前是否有子页面打开 */
bool w_modal_is_open(void);

/** @brief 当前打开了几层 */
int w_modal_depth(void);

/**
 * @brief 刷新当前子页面（重跑它的 build 回调）
 *
 * @note 用于"子页面里的操作改变了自身显示"的场景，
 *       比如录制状态从"空闲"变成"录制中"。比让每个页面自己维护刷新逻辑简单。
 */
void w_modal_refresh(void);

/**
 * @brief 初始化（内部用；由 watch_pages 在建立轮播时调用）
 *
 * @param host  子页面挂到哪个容器上（传轮播的 screen）
 */
void w_modal_init(lv_obj_t *host);

#ifdef __cplusplus
}
#endif
