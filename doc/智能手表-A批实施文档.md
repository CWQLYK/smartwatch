# A 批实施文档（v0.6.1）——修缺陷 + 零成本收益

> 目的：**这份文档是为了"改不乱、可回滚、每步可验证"**。实施时严格按本文档的顺序和粒度走，不即兴扩大范围。
> 上游依据：[智能手表-对标Watch-Player_IDF与优化清单.md](智能手表-对标Watch-Player_IDF与优化清单.md) 第五节 + 第六节 A 批。
> 版本：`v0.6.1`（A 批 = 修复与配置，不引入新功能；功能类放 B 批 = v0.7.0）

---

## 一、范围

### 做什么（12 项，来自优化清单 A 批）

| 编号 | 事项 | 类型 | 优先级 |
|---|---|---|---|
| A1 | WiFi 页轮询定时器生命周期（**唯一的 P0**） | 代码 | P0 |
| A2 | 时钟服务跨任务竞态（闹钟/倒计时） | 代码 | P1 |
| A3 | `data_hub` 回调改传快照 + 采样任务栈 3072→4096 | 代码 | P1 |
| A4 | `settings` 写 NVS 纳入锁 | 代码 | P1 |
| A5 | 息屏唤醒穿透（第一下触摸被界面吃掉） | 代码 | P1 |
| A6 | 三处"事件回调里同步删对象" → `lv_obj_del_async` | 代码 | P2 |
| A7 | 功能列表行高 52→46（5 行一屏放下，恢复上滑控制中心） | 代码+UI | P2 |
| A8 | 编译改 `-O2` + 关 FPS 浮层 + 启动/内存配置包 | 配置 | P1 |
| A9 | 命令行补 `taskmem` / `stats` / `bt` | 代码 | P1 |
| A10 | coredump 分区 + 任务看门狗 panic + 调试用 `crash` 命令 | 配置+代码 | P1 |
| A11 | 开机"先刷黑 → UI 就绪 → 背光淡入" | 代码 | P2 |
| A12 | 自检补三类缺口（模态页计数 / WiFi 扫描中返回 / 模拟息屏） | 测试 | P2 |

### 明确不做（防止范围蔓延）

| 不做 | 原因 |
|---|---|
| SPI 40→80MHz、`wait_cb` 改造 | 需实机试错，风险独立，放 C 批 |
| 任何省电改动（面板 disp_off / 降刷新 / light sleep） | B 批 |
| 任何新功能（计算器/日历/天气/2048/toast/壁纸） | B 批 |
| 重构（拆 `wifi_page_build()`、轮播"只重建一页"、行高宏收进 watch_style） | 会碰最敏感模块，单独排版本 |
| 改中文文案（除 A7 可能新增的提示外） | 一改就要重跑字库，避免与 A7 混在一起 |
| 顺手格式化 / 改名 / 调注释排版 | **A 批只改文档里列出的点** |

---

## 二、铁律（每条都有历史教训）

1. **一次只动一个主题**：A1~A12 每项独立完成（编译通过 + 自检 0 失败）才进下一项。
2. **每项一个 commit**，message 用 `A1: 修 WiFi 页定时器生命周期` 这种格式，方便单独 revert。
3. **源码只用编辑器工具改，绝不用 shell 回写**。本项目因 PowerShell 回写丢过两次文件（编码变 ANSI、换行被压平）。
4. **改 `sdkconfig.defaults` 之后必须 `clean`**（`build.ps1 clean` 会删掉 `build/` 与 `sdkconfig`），否则新值不生效 —— v0.5.0 的 `LV_ROLLER_INF_PAGES` 就栽在这。
5. **改 LVGL 的 Kconfig 项要同步看模拟器**：`sdkconfig`（设备）与 `lvgl_sim/lv_conf.h`（模拟器）是两套配置，**两边不一致就会出现"模拟器好好的、实机不一样"**（FPS 浮层就是这么漏掉的）。
6. **中文只写进 `watch_strings.h`**（或 `watch_ui` 层的字面量），改完必须跑 `tools/gen_font.ps1 -Force`（**不带 `-Force` 会静默跳过** → 实机豆腐块）。
7. **每步结束跑三条校验**：`tools/check_all_literals.py`、`tools/check_fonts.py`、`tools/check_glyphs.py --all`。
8. **每步记录回滚方式**（本文档已给），任何一步出问题先回滚再分析，不叠加修复。
9. **凡是我这边验不了的，一律写进"实机验收清单"，不自行宣称通过。**
10. **分区表/配置类改动单独成步**，并明确告知你是否需要整片擦除。

---

## 三、基线（开工前必须记录）

| 项 | 基线值 | 怎么取 |
|---|---|---|
| 代码回退点 | `v0.6.0` = commit `3184219` | 先把当前未提交的文档改动（`README.md`、`doc/智能手表-项目计划.md`、两份新分析文档）提交，得到一个干净的起点 |
| 固件大小 | image **1,662,253 B**（app 分区 4MB，余约 60%） | `idf.py size` |
| 组件占用 | lvgl 348,590 / fonts 287,562 / net80211 148,972 | `idf.py size-components` |
| 编译警告 | 2 条已知第三方 deprecation（`bsp.c` + `esp_lvgl_port_touch.c`） | `build.ps1 build` |
| 模拟器自检 | 3 套全过（退出码 0） | `SIM_SELFTEST=1` 运行 |
| 字库校验 | 3 个脚本退出码 0；当前 505 字面量字符 / 757 码点 | `tools/*.py` |

> 实施结束时用同样的方法复测，**用数字说明改动的效果**（例如 `-O2` 后 image 变大多少、帧率在模拟器里有无变化）。

---

## 四、实施步骤

### 阶段一 · 代码修复（每步：改 → 编译 → 自检 → 截图/断言）

#### A1【P0】WiFi 页轮询定时器生命周期

- **文件**：`components/watch_ui_pages/pages/watch_page_wifi.c`（约 806 行）
- **问题**（已逐行核实）：`ui.poll` 只在 `poll_cb` 内被删（`:386-389` 状态变化、`:404-407` 扫描超时）；`wifi_page_build()` 开头的 `ui.poll = NULL`（`:632`）只清指针**不杀定时器**；文件里**没有 `LV_EVENT_DELETE` 钩子**。→ 扫描中按返回 ⇒ 定时器继续跑并写已释放的 `ui.status`/`ui.list`。
- **改法**（三步，缺一不可）：
  1. `wifi_page_build()` 开头：`if (ui.poll) { lv_timer_del(ui.poll); ui.poll = NULL; }`
  2. 在 `wifi_page_build(parent, user)` 里给**页面根**挂 `LV_EVENT_DELETE`（照 `watch_page_clock.c:513-518` 现有写法），回调里：删 `ui.poll`、把 `ui.poll/ui.list/ui.status` 置 NULL、`ui.scanning = false`
  3. 注释里那句"别让它悬空"改成说明真实约束（定时器与对象同生命周期）
- **自检**：模拟器加断言"打开 WiFi 页 → 扫描中关闭 → 推进 5 秒 → 不崩 + 定时器数回到开关前"（若模拟器打不开 WiFi 页，见 A12 的说明，改为实机用例）
- **回滚**：`git revert` 单文件
- **风险**：低。**唯一注意**：`LV_EVENT_DELETE` 钩子里不能再调 LVGL 删除类 API（对象正在析构），只做删定时器 + 置指针

#### A2【P1】时钟服务跨任务竞态

- **文件**：`components/watch_ui/watch_clock.c`（495 行）
- **问题**：`watch_clock_on_time()`（`:397-431`）由 `data_hub` 采样任务经 `watch_pages.c:645` 调用（**故意放在拿 LVGL 锁之前**，理由见 `watch_pages.c:637-644`：闹钟按"时:分"比较，每秒必须喂、一次不能漏）；它写 `s_time_ok`/`s_last_hour`/`s_last_min`/`s_al_fired_hhmm`，并通过 `alert_start()`（`:321-330`）写 `s_alert`/`s_alert_ms`/`s_alert_beep`。同时 LVGL 任务的 `clock_timer_cb`（`:80-98`）读写同一批变量。**`watch_ui` 组件内 0 处互斥**。
- **重要澄清**：这条**不是**"从错误的任务调 LVGL"——`alert_start()` 只是改状态，真正的界面刷新在 `watch_alert_poll()`（LVGL 任务）里。所以问题面窄：**一批跨核共享的小整数缺保护**。
- **改法**：在 `watch_clock.c` 加 `static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;`，用 `taskENTER_CRITICAL(&s_mux)/taskEXIT_CRITICAL(&s_mux)` 包住：
  - `watch_clock_on_time()` 里"状态赋值 + `alert_start()`"这一段；
  - 界面侧改状态的地方（停止/静音路径）；
  - 只读访问器（`watch_clock_alerting()`、`watch_clock_alert_kind()`、`watch_clock_alert_ring_done()`、`watch_clock_last_time()`）的读取。
- **硬约束**：临界区里**不许**调 LVGL、蜂鸣器、`ESP_LOGx`（日志会取锁）。`ESP_LOGI(TAG,"闹钟触发…")`（`:429`）必须挪到临界区**外面**。
- **自检**：模拟器里跑"闹钟到点 → 响 → 停止 → 再响"的既有用例，确认行为不变
- **回滚**：单文件 revert
- **风险**：中。临界区必须短（几次赋值），且不能与其他锁嵌套

#### A3【P1】`data_hub` 快照 + 采样任务栈

- **文件**：`components/data_hub/data_hub.c`
- **问题 1**：`notify_locked()`（`:52-68`）先 `xSemaphoreGive(s_lock)` 再调回调，而传下去的是 `&s_state`（活内存）→ 回调（也就是整条 UI 刷新链）执行期间采样任务仍在改它。
  **改法**：在**释放锁之前**做 `device_state_t snap = *st;`，回调改传 `&snap`（结构体几十字节，栈开销可忽略）。
- **问题 2**：`HUB_TASK_STACK` 是 **3072**（`:35`），而 UI 刷新链跑在这个任务上（`watch_pages.c:631-661` → 各页 `refresh`）。
  **改法**：提到 **4096**；真实余量等 A9 的 `taskmem` 上线后在实机上量，再定最终值。
- **顺带**：`:210` 的过期注释（"P1 还没接 IMU"）改掉。
- **自检**：既有全部用例（每次数据变化都会走这条路）
- **回滚**：单文件
- **风险**：低

#### A4【P1】`settings` 写 NVS 纳入锁

- **文件**：`components/settings/settings.c`
- **问题**：`settings_set()`（`:145-179`）的锁只包住缓存比较（`:152-157`），`nvs_set_i32`/`nvs_commit`（`:163-166`）在**锁外**；`s_nvs` 是全局句柄，多任务会并发进来。`settings_reset()`（`:200-209`）与 `settings_reset_all()`（`:216-231`）同样是"锁只包缓存"。
- **改法**：把"比较缓存 → 更新缓存 → `nvs_set_i32` → `nvs_commit`"放进**同一把锁**；`event_bus_publish()`（`:177-179`）保持在锁**外**（订阅者可能回调 `settings_get`，持锁会死锁）。
- **自检**：编译 + 现有用例；实机跑"亮度滑动/开关自动息屏/控制台改设置"混合操作
- **回滚**：单文件
- **风险**：低

#### A5【P1】息屏唤醒穿透

- **文件**：`components/watch_ui/include/watch_platform.h`、`components/watch_ui/watch_platform.c`、`components/bsp/bsp.c`、`lvgl_sim`（自检侧）
- **问题**：`power.c:12-21` 的注释描述"靠 `lv_indev_enable(false)` 吃掉息屏后第一下触摸"，但全工程**没有任何地方调 `false`**（只有 `power.c:161` 调 `true`）；而且 LVGL 8.4 在读回调**之前**就检查 `proc.disabled`（`lv_indev.c:78-79`），**事后禁用来不及**。
- **不能这么改**：不能在息屏时直接禁用输入设备 —— 那样 `bsp.c` 的触摸读回调根本不会被调用，**屏幕就再也亮不起来了**（唤醒正是靠这个回调上报的）。
- **改法**（把判断放在"源头"，且放在两边都编译的平台层，便于自检）：
  1. `watch_platform.h` 新增两个接口（不改老函数签名，减少波及面）：
     ```c
     /** @brief 刚发生的那次触摸是不是"只是用来亮屏"、不该交给界面 */
     bool w_screen_should_swallow_touch(void);
     /** @brief 手指抬起：解除屏蔽 */
     void w_touch_settled(void);
     ```
  2. `watch_platform.c`：加 `static bool s_swallow;`；在 `w_screen_notify_activity()` 里先取 `w_screen_is_on()`，若**本来是灭的**（说明这次活动把屏幕叫醒了）→ `s_swallow = true`；两个新接口分别读/清这个标志。
  3. `bsp.c` 的 `touch_read_cb`（`:151-170`）：
     ```
     按下时：先 w_screen_notify_activity()（灭屏时会亮屏）
             再 if (w_screen_should_swallow_touch()) → 上报 RELEASED（不报坐标、不报 PRESSED）
             否则 → 照旧上报 PRESSED + 坐标
     抬起时：w_touch_settled() 然后上报 RELEASED
     ```
     这样"唤醒那一次触摸"从按下到抬手**整段被吞掉**，下一按正常。
  4. `power.c` **不动**（它那套 350ms 窗口保留为兜底；本次不再依赖它）
- **自检**：模拟器加用例"息屏唤醒不穿透"：① 把模拟器的屏幕钩子置成"灭"；② 走与 `bsp.c` 等价的按下路径 → 断言"屏幕变亮 **且当前页没变**"；③ 抬手；④ 再按一次 → 断言"这次生效（能进子页面）"。
  *实施时先确认*：模拟器的屏幕钩子能否被置成"灭"（若不能，先给它加一个 sim 专用开关）以及模拟器现在的触摸注入路径是否等价于 `bsp.c`。
- **回滚**：4 个文件一起 revert（一个 commit）
- **风险**：中。要实机确认三件事：① 息屏后第一下只亮屏、不触发控件；② 亮屏后的下一次点击正常；③ 亮屏状态下点击**完全不受影响**（这是回归重点）

#### A6【P2】三处"事件回调里同步删对象"复核

- **文件**：`components/watch_ui/watch_modal.c`（模态返回）、`components/watch_ui/watch_game.c`（游戏退出），以及审计提到的第三处
- **问题**：LVGL 只在"被删对象**正是**事件 target/current_target"时标记删除（`lv_event.c:147`），**删祖先不命中**，事件系统返回后仍会读该对象的 `parent/flags`。项目自己在 `watch_keyboard.c:261` 写过正确用法的注释，只是没统一。
- **改法**：把这三处的 `lv_obj_del(...)` 改成 `lv_obj_del_async(...)`。
  *实施时逐处确认*：审计列的第三处 `watch_page_wifi.c:386-390` 实际是**在定时器回调里删自己**（`lv_timer_del`）——LVGL 支持这种用法，**不属于本项**，我会在实施时确认后再决定要不要动。
- **自检**：既有"模态反复开关""游戏进出"用例 + 对象/定时器计数断言
- **回滚**：按文件 revert
- **风险**：低（`lv_obj_del_async` 是 LVGL 官方推荐做法）

#### A7【P2】功能列表行高 52→46

- **文件**：`components/watch_ui/watch_widgets.c`（`w_menu_row_create`，`:578-605`）、`components/watch_ui_pages/pages/watch_page_functions.c`（`:50-55` 的常量）
- **为什么是 46 而不是 44**：算一下就知道 —— 可用高度 `280 - 32(标题) = 248`。
  - `行高 44 + 间隙 6`：`5×44 + 4×6 = 244` ✅ 但行内只剩 `44-20(上下内边距) = 24px` 装"16 号标题 + 14 号副标题"，**必然叠字**；
  - `行高 46 + 间隙 4`：`5×46 + 4×4 = 246`，总高 `32+246 = 278 ≤ 280` ✅，配合"行内纵向内边距压到 2px"→ 内容区 `42px`，够放两行。
- **现状隐患（顺带修）**：`w_card_create` 用 `W_PAD_CARD = 10`（`watch_style.h:70`），所以现在 52px 行的内容区只有 **32px**，而"16 号标题 + 14 号副标题"约 38px —— **当前其实已经偏挤**，这也是本次必须显式设 `pad_ver` 的原因。
- **改法**：
  1. `watch_widgets.c`：`w_menu_row_create` 里行高从写死的 `52` 改成参数或宏（**建议宏 `W_MENU_ROW_H`，保持函数签名不变**，减少调用点改动），并显式 `lv_obj_set_style_pad_ver(card, 2, 0)`；
  2. `watch_page_functions.c`：`ROW_H 52→46`、`LIST_GAP 6→4`、`LIST_H` 跟着改为 `5*46+4*4 = 246`；
  3. 更新该文件头部的 ASCII 示意图与那句"一屏只能看到前 4 行、第 5 行要往下拖"的说明（**现在 5 行一屏放下，上滑控制中心在该页恢复**）。
- **自检（这条能在模拟器里验）**：模拟器出 `ui-functions.png` 截图 → 我**看图确认不叠字**；并断言该页滚动范围为 0（`watch_ui_debug_page_scroll()` 返回 0）。
- **回滚**：两个文件 revert（或只把常量改回 52/6）
- **风险**：低。**若截图显示叠字**：备选顺序 ① 副标题字号 14→12（要重跑字库）；② 退回 52px + 保留滚动（直接回滚本项）

---

### 阶段二 · 配置批（一次改完，一次 clean）

> ⚠️ 本阶段所有改动都在 `sdkconfig.defaults` 里，**必须 `.\build.ps1 clean` 后重新 build**（否则不生效）。
> 建议**一个 commit 一个主题**，但 `clean` 只做一次（每次 clean 重建约几分钟）。

#### A8 配置批：性能 + 显示 + 启动 + 内存

**文件**：`firmware/smartwatch/sdkconfig.defaults`

| 改动 | 从 | 到 | 为什么 | 风险 |
|---|---|---|---|---|
| 编译优化 | `DEBUG`（`-Og`，实测自 `compile_commands.json`） | `CONFIG_COMPILER_OPTIMIZATION_PERF=y`（`-O2`） | 零代码拿到渲染/滤波/游戏性能 | 低（image 会变大，4MB 分区余量充足） |
| FPS 浮层 | `CONFIG_LV_USE_PERF_MONITOR=y` | `=n` | 该宏会在 `lv_layer_sys()` 右下角画 `NN FPS / NN% CPU`（`lv_refr.c:362-376`），属开发用；模拟器本来就是关的（`lvgl_sim/lv_conf.h:282`） | 无（真要看帧率：改回 `y` + clean，或临时用） |
| PSRAM 开机测试 | `CONFIG_SPIRAM_MEMTEST=y` | `=n` | 省掉每次启动对 8MB PSRAM 的全片写读测试 | 不再在启动时发现 PSRAM 损坏 |
| bootloader 深睡校验 | 未设 | `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y` | 深睡唤醒省一次镜像校验 | 前提：不开 Secure Boot、不用 OTA 切分区（我们都没用） |
| bootloader 优化 | `SIZE` | `CONFIG_BOOTLOADER_COMPILER_OPTIMIZATION_PERF=y` | 启动代码快一点 | bootloader 变大 |
| 内部 RAM 分配阈值 | 未设 | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`<br>`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768` | ≤16KB 的分配走内部 RAM；给内部 RAM 留 32KB 底（对标工程的做法） | 低 |
| FreeRTOS 观测（给 A9 用） | 未设 | `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`<br>`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`<br>`CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y` | `taskmem`/`stats` 需要 | 极小的运行时开销 |

**明确不改**：`CONFIG_BOOTLOADER_LOG_LEVEL`（我们的控制台是 USB-Serial-JTAG，**开机日志是能看到的，保留**）；`CONFIG_FREERTOS_HZ`（已是 1000）；`SPIRAM_MODE_OCT`/`SPIRAM_SPEED_80M`（已对）。

- **验收**：`clean` + `build` 0 错误；记录新的 image 大小与警告数；**格式上跑一次真机确认没崩**（你）
- **回滚**：revert `sdkconfig.defaults` + `clean`
- **风险**：低；`-O2` 若引出问题（理论上可能暴露老代码的未定义行为），单独 revert 这一行再 clean

---

### 阶段三 · 设施

#### A9 命令行补 `taskmem` / `stats` / `bt`

- **文件**：`components/app/app_console.c`（现有 16 条命令，注册表在 `:808-824`）
- **做法**：
  - `taskmem [名字]`：遍历任务 → 名称/状态/优先级/核/`uxTaskGetStackHighWaterMark`（栈峰值余量）。**这条最有用**：能告诉我们 `data_hub`(A3)、LVGL、WiFi 各任务栈该给多少
  - `stats`：两次 `uxTaskGetSystemState`（间隔 1s）算各任务 CPU%，**按核归一化**
  - `bt [名字]`：当前任务 backtrace；其他任务用 `vTaskGetSnapshot`（**若链接不到相关 API 就退化为只打印当前任务**，不硬上）
- **依赖**：A8 里那三个 FreeRTOS 配置开关（必须先做 A8）
- **自检**：编译 + 实机敲命令（你）
- **回滚**：单文件
- **风险**：低

#### A10 coredump + 任务看门狗 panic + `crash` 命令

- **文件**：`partitions.csv`、`sdkconfig.defaults`、`components/app/app_console.c`
- **分区表改法（关键：什么都不移动）**：现在 `storage` 是 11M，从 `0x410000` 排到 **`0xF10000`**，而 flash 到 `0x1000000` —— **尾部还有 960KB 空闲**。所以**追加**一个分区即可：
  ```
  coredump,  data, coredump, 0xF10000, 0x40000,
  ```
  （256KB，足够容纳所有任务栈；`nvs`/`phy_init`/`factory`/`storage` 的偏移与大小**全部不变**）
  > ✅ **已用 `idf.py partition-table` 核实**（不是手算）：`nvs 0x9000/24K`、`phy_init 0xf000/4K`、`factory 0x10000/4M`、`storage 0x410000/11M` → storage 结束于 `0xF10000`，剩余 `0xF0000` = 960KiB。追加 256KB 后仍余 704KB。
- **⚠️ 纠正我上一轮文档里的一句话**：优化清单 5.4 写的是"改分区表必须整片擦除"——那是我按"插入到中间"的方案写的。**改成追加到尾部后，不需要整片擦除**，NVS（设置/WiFi 密码）与 SPIFFS（IMU 日志）都不受影响。特此更正。
- **配置**：`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`、`CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`、`CONFIG_ESP_TASK_WDT_PANIC=y`（现在任务看门狗只打印不复位）
- **加调试命令** `crash`：故意触发一次崩溃（用于验证 coredump 通路）。**命令帮助里明确写"会让你重启，仅调试用"**。
- **验收（你来做）**：烧录 → 敲 `crash` → 设备重启 → 在 PC 上跑 `idf.py coredump-info`（或 `esp-coredump info`）→ 能看到出错任务与调用栈。**这是"以后你报崩溃我能查"的前提。**
- **回滚**：revert 三个文件（分区表回滚后需重新烧一次分区表）
- **风险**：低；但**烧录步骤变了**（会写分区表），请照我给的命令烧

#### A11 开机"先刷黑 → UI 就绪 → 背光淡入"

- **文件**：`components/app/app.c`（现在 `:547-551` 直接设目标亮度）
- **做法**：
  1. 保持背光在 LCD 初始化后为 0（`bsp.c` 已是先关背光）；
  2. 在**界面初始化完成之后**（`watch_ui_init()` 之后、`wifi_service_init()` 之前）启动淡入：用 `esp_timer` 每 15ms 把亮度从 0 走到目标值（约 300ms），走完恢复"亮度设置变化时直接设值"的原逻辑；
  3. 淡入期间用户触摸 → 立即跳到目标亮度（不要跟用户抢）；
  4. 亮度曲线沿用现有设置项（**不引入对标工程那套指数映射**，避免与本项目"亮度百分比"语义冲突）。
- **自检**：编译 + 确认亮度设置项行为不变（模拟器里背光是空实现，只能编译级验证）
- **回滚**：单文件
- **风险**：低

---

### 阶段四 · 收尾

#### A12 自检补三类缺口

- **文件**：`lvgl_sim/src/sim_main.c`（现有 3 套自检）
- **做法**：
  1. **"任意模态页 开→关 后对象数/定时器数不变"**（现在只对游戏做了）→ 这个断言就是 A1 的守门员；
  2. **WiFi 页"扫描中返回"**用例（若模拟器打不开 WiFi 页，就把它降级为实机用例并在文档里说明）；
  3. **模拟息屏/唤醒**（配合 A5）。
- **验收**：`SIM_SELFTEST=1` 退出码 0，且**故意回滚 A1 时应能报错**（验证断言真的有效 —— 这条很重要：能抓住 bug 的断言才算断言）

#### 收尾清单

1. `build.ps1 clean` + `build`：0 错误，警告不超过基线的 2 条
2. 模拟器 3 套自检：0 失败；重新出 9 张截图（若有 UI 变化，重点 `ui-functions.png`）
3. 字库三条校验：退出码 0
4. 记录新 image 大小 / 命令数 / 行数变化
5. 写 `doc/version/v0.6.1.md`（按现有版本文档格式：改了什么、为什么、验证了什么、已知限制）
6. 更新 `doc/智能手表-项目计划.md` 状态列 + 本文档打勾
7. 给用户一份**实机验收清单**（下面第六节）

---

## 五、验证矩阵

| 我能验（编译 + 模拟器） | 只能你验（实机） |
|---|---|
| 编译 0 错误、警告不增加 | 息屏唤醒：第一下只亮屏不误触；第二下正常 |
| 模拟器 3+ 套自检 0 失败 | WiFi 页扫描中返回不再异常 |
| 新截图肉眼检查（行高/叠字/布局） | 闹钟/倒计时多次触发与停止 |
| 字库三条校验 | `-O2` 后整体手感（翻页、游戏、菜单响应） |
| `idf.py size` 数字对比 | 启动时间是否有改善（主观即可） |
| 静态检查（无 LVGL 跨任务调用、临界区无日志） | 背光淡入观感 |
| — | `crash` → `coredump-info` 能出栈 |
| — | `taskmem` 输出的各任务栈余量 |

---

## 六、实机验收清单（给你逐条打勾用）

> 烧录建议：先做 A1~A7（代码步）后**普通 `idf.py flash`** 验一轮；配置批（A8）与 coredump（A10）之后再各烧一轮。
> 我需要在实机上确认的最小集合（按重要性）：

1. [ ] **息屏穿透**（A5）：等屏幕自动熄灭 → 点"功能列表"里的**某一行** → 应该**只亮屏、不进子页面**；抬手再点一次 → 这次正常进入
2. [ ] **息屏穿透回归**（A5）：屏幕亮着时，点击/滑动/长按**一切照旧**（这是重点，别改出"第一下总要按两回"的毛病）
3. [ ] **WiFi P0**（A1）：进 WiFi 页 → 点扫描 → **扫描中按返回** → 界面正常、不花屏不乱跳；重复 3 次
4. [ ] **闹钟**（A2）：设一个 1~2 分钟后的闹钟 → 到点响 + 弹提醒 → 点【停止】能停；倒计时同理
5. [ ] **设置持久化**（A4）：改亮度/自动息屏 → 重启 → 设置还在
6. [ ] **功能列表**（A7）：一屏能看到 **5 行**（不用滑）；在该页**上滑能开控制中心**；行内文字不叠
7. [ ] **右下角**（A8）：**没有** `FPS / CPU` 小浮层了
8. [ ] **整体手感**（A8）：翻页、菜单、游戏比 v0.6.0 更跟手（`-O2` 的效果）
9. [ ] **启动**（A8）：开机不刺眼、屏幕不花（背光淡入 A11）
10. [ ] **coredump**（A10）：串口敲 `crash` → 设备重启 → PC 上 `idf.py coredump-info` 出栈
11. [ ] **taskmem**（A9）：串口敲 `taskmem` → 打印各任务栈余量（把输出贴给我，我据此定最终栈大小）

---

## 七、风险与回滚总表

| 步骤 | 主要风险 | 回滚方式 | 是否需要重烧 |
|---|---|---|---|
| A1 | 钩子里误调 LVGL | revert 单文件 | 普通 flash |
| A2 | 临界区过长/嵌套 | revert 单文件 | 普通 flash |
| A3 | 快照导致回调拿到旧数据（语义应一致） | revert 单文件 | 普通 flash |
| A4 | 持锁发布事件会死锁 → **必须锁外发布** | revert 单文件 | 普通 flash |
| A5 | 改出"第一下按不动" | revert 4 文件 | 普通 flash |
| A6 | 异步删除导致时序变化 | revert 单文件 | 普通 flash |
| A7 | 行内叠字 | 常量改回 52/6 | 普通 flash |
| A8 | `-O2` 暴露老代码问题 | 单独 revert 那一行 + clean | 普通 flash |
| A9 | 无 | revert 单文件 | 普通 flash |
| A10 | 分区表写错 → 起不来 | revert + 重烧分区表 | **写分区表**（不需整片擦） |
| A11 | 淡入与用户操作抢亮度 | revert 单文件 | 普通 flash |

**最坏情况**：任何一步把设备搞成起不来 → `partitions.csv`/`sdkconfig.defaults` 回到基线 + `build.ps1 clean` + `idf.py flash`，回到 v0.6.0 状态（这就是第三节要的"干净回退点"）。

---

## 八、预期变更文件清单

| 文件 | 步骤 |
|---|---|
| `components/watch_ui_pages/pages/watch_page_wifi.c` | A1 |
| `components/watch_ui/watch_clock.c` | A2 |
| `components/data_hub/data_hub.c` | A3 |
| `components/settings/settings.c` | A4 |
| `components/watch_ui/include/watch_platform.h`、`components/watch_ui/watch_platform.c` | A5 |
| `components/bsp/bsp.c` | A5 |
| `components/watch_ui/watch_modal.c`、`components/watch_ui/watch_game.c` | A6 |
| `components/watch_ui/watch_widgets.c`、`components/watch_ui_pages/pages/watch_page_functions.c` | A7 |
| `sdkconfig.defaults` | A8 / A10 |
| `components/app/app_console.c` | A9 / A10 |
| `partitions.csv` | A10 |
| `components/app/app.c` | A11 |
| `lvgl_sim/src/sim_main.c`（+ 可能的 sim 侧小接口） | A12 / A5 / A1 |
| `doc/version/v0.6.1.md`（新建）、`doc/智能手表-项目计划.md` | 收尾 |

---

## 九、关键设计决策（为什么这么改）

1. **为什么 A5 不照 `power.c` 注释那样"禁用输入设备"**：因为唤醒本身依赖触摸读回调，禁用后**屏幕永远亮不起来**。必须在"上报给 LVGL 之前"把这一下改成 RELEASED。
2. **为什么 A5 的状态放在 `watch_platform.c` 而不是 `bsp.c`**：模拟器不编译 `bsp.c`，放在平台层才能写自检用例（否则这条只能靠实机肉眼）。
3. **为什么 A2 不用 LVGL 锁**：`watch_pages.c:637-644` 明确写了"每小时:分必须喂到、一次都不能漏"，拿 LVGL 锁会丢掉时间片——所以只对**几个小整数**加临界区，而不是把整条路径纳入 UI 锁。
4. **为什么 A7 是 46 而不是 44**：`280 - 32(标题) - 4×4(间隙) = 5×46`，44 会让行内容区只剩 24px 装不下两行字（现有 `W_PAD_CARD=10` 让 52px 行的内容区也只有 32px，已经很挤）。
5. **为什么 A10 的分区追加在尾部**：`storage` 结束于 `0xF10000`，尾部还有 960KB —— 追加**不移动任何现有分区**，所以设置与日志都不用清。这与"插在中间"的方案差别很大（后者才需要整片擦）。
6. **为什么不一次把所有优化都做掉**：显示链路（`wait_cb`/SPI 超频）和省电改动都需要实机试错，混进"修复批"会让"到底是哪一步引入的问题"无法定位 —— 这正是本文档要防的"越改越烂"。

---

*本文档为实施前计划，实施过程中若有偏离，会在 `doc/version/v0.6.1.md` 里逐条记录"为什么偏离"。*
