# 智能手表 — B 批（v0.7.0）实施文档

> **状态：待用户审阅，尚未动代码。**
> 基线：固件 **v0.6.1**，git HEAD `d5f4765`，工作区干净。
> 对标参考：`相关资料\示例工程\Watch-Player_IDF`（见 `doc/智能手表-对标参考工程开发指南.md` 的方法论）。
>
> | 项 | 值 |
> |---|---|
> | 本批范围 | **只做"补功能"**；省电第一档拆到 v0.7.1（用户已定：分两批） |
> | 功能清单 | toast → 计算器 → 日历 → 2048 → WiFi 多槽位 → 天气 |
> | 前置验证 | 编译 0 error ✅｜模拟器自检 0 失败 ✅｜实机 v0.6.1 在跑 ✅ |

---

## 〇、法务前提（**动手前必须知道**）

参考工程 **根目录没有 LICENSE 文件**（只有第三方 `components/pngdec/LICENSE`），
其 `main/ui/apps/*.cpp` 也**没有文件头许可声明**。

> **结论：只借鉴思路、结构、参数；不复制任何代码文本，也不复制图像资产。**
> 本项目的 `README.md` "第三方与致谢"表**暂不需要**为它加行（未使用其代码/资产）；
> 若将来确实需要引用其代码，先补许可证确认再加致谢。

**参考工程各功能的"值得抄 / 绝不能抄"速查**（下文每节展开）：

| 功能 | ✅ 抄 | ❌ 别抄 |
|---|---|---|
| 计算器 | 调度场双栈、5×4/19 键布局、配色与尺寸 | 无输入上限、`set_btn_width` 循环、textarea 当纯显示 |
| 日历 | `lv_calendar` + `LV_ALIGN_TOP_MID` 的用法 | 50ms GPIO 轮询退出、缺省英文星期名 |
| 2048 | 指数存储、`combined[][]` 记账、90/10 概率、150ms | 动画当对象、退出靠父容器兜底删 anim、无胜负判定 |
| 天气 | URL 参数、失败降级、任务与 UI 解耦 | 「保存即落盘」式凭据、MMDD 字符串比日期、200KB 位图 |
| WiFi 多槽位 | 5 槽位的产品思路 | **"保存即写 NVS"**（正是我们修掉的 bug）、`nvs_erase_all` |
| toast/转场 | 层次、层次与优先级约定、200ms/1500ms、状态机 | `tip_display_35.c` 字模（版权+子集）、每次新建对象、"清理只挂 `ready_cb`"、截图+canvas 转场 |

---

## 一、已完成的验证（本次接手实测）

| 项 | 命令 | 结果 |
|---|---|---|
| 编译链路 | `build.ps1 build` | **0 error**，镜像 1.68MB，4MB 分区**余 60%** |
| 模拟器自检 | `SIM_SELFTEST=1 lvgl_sim.exe` | **0 项失败，退出码 0** |
| 实机 | `serial_cmd.py COM24 ...` | v0.6.1 在跑；`data_hub` 栈余 **2048**、`taskLVGL` **3296**、`settings_ev` **1032**（与交接文档 §4.7 完全一致） |
| 冷启动耗时 | 复位 → REPL 可用 | **约 5.3 秒**（将来做深睡要重付这个代价） |

### 1.1 本次发现的两处"文档已过时"

1. 交接文档 §10 #2 / §11 说"5.16 自检缺口未补"——**实际已补**
   （`lvgl_sim/src/sim_main.c:936` 模态页开关计数、`:1011` WiFi 扫描中关闭、`:1026` 息屏唤醒）。
2. 优化清单 4.3 说"缺充电状态显示"——**实际已有**（`battery` 命令输出"充电中：否"）。

### 1.2 ★ 实机现状：用户正踩在 §10 #1 那个悬案上

```
wifi status → 状态=连接中  SSID=CWQLYK  RSSI=0  IP=-  已保存凭据: CWQLYK
```

- NVS 里有 `CWQLYK` 的密码，开机自动重连**一直失败**且不自愈；
- 这条**自动重连**路径上 `s_conn_used_saved == false` → **不触发**认证失败清理（交接文档 §10 #3 的"小缺口"）；
- 后果：永远转圈，既不报错也不重新问密码。

> **决定：把 §10 #3 从"顺手修"提升为 v0.7.0 必修项**（见 §六）。

---

## 二、开工前必须知道的"地雷清单"（本次研究新增）

| # | 地雷 | 证据 | 后果 | 对策 |
|---|---|---|---|---|
| **L1** | `creds_erase()` 用 `nvs_erase_all` | `wifi_service.c:124-132`（3 处调用：`:470` 认证失败清理、`:760` 空密码清理、`:1024` `hook_forget`） | 扩成多槽后，**任何一次认证失败会删光全部网络** | 先改成按槽 `nvs_erase_key`，再扩槽 |
| **L2** | `lv_btnmatrix` **只存 map 指针** | `components/lvgl/src/widgets/lv_btnmatrix.c:102` `btnm->map_p = map;` | map 写成栈上数组 → 悬空崩溃 | map 必须 `static const char *` |
| **L3** | `lv_btnmatrix_set_btn_width()` 每次调用**重跑整张 map** | `lv_btnmatrix.c:289-292` | 参考工程那个 `for(i<20)` 循环 = 20 次全量重排（纯浪费） | **照抄时删掉该循环** |
| **L4** | 同屏 z-order 会被模态抢占 | `watch_modal.c:163` 与 `watch_alert.c:152-157` | 挂在同一个 host 上的浮层会被后开的模态盖住 | 要么挂 `lv_layer_top()`（高一层，推荐给 toast），要么照 `watch_alert` 的做法**反复重新宣告** `lv_obj_move_foreground()` |
| **L5** | 我们只有 14/16/20 三档中文字号 | `watch_fonts.h:23-25`；参考工程用 24 | 直接搬 24 号 → 字体缺失/白屏 | 用 `watch_font_cn(20)`；**别加 24 号**（每档约 +100~250KB） |
| **L6** | 新增 enum 到设置表必须加在**末尾** | `settings.h:37-38`（`SETTINGS_BIT()` 掩码按枚举值算） | 插在中间 → 老设置全部错位 | 天气相关设置项一律 append |
| **L7** | `lv_layer_top()` 本身**不**吞触摸，但**建在它上面的默认尺寸子容器会** | `lv_hal_disp.c:225-230` 显式 `lv_obj_clear_flag(top_layer, CLICKABLE)`；而 `lv_obj_create(NULL)` 默认满屏 + CLICKABLE | 没清子容器 CLICKABLE → 整屏点不动（`watch_overlay.c:7-11` 当年记的就是这个，但归因写成了 layer_top） | 在 layer_top 上建容器后**立刻**清 `CLICKABLE`/`CLICK_FOCUSABLE` |
| **L8** | 清理只挂在 `lv_anim` 的 `ready_cb` 上是不可靠的 | `lv_anim.c:137-153`：`lv_anim_del()` **只调 `deleted_cb`、绝不调 `ready_cb`** | 动画被取消/对象被删 → `ready_cb` 永不执行 → 收尾（解锁、复位、重建相邻页）**静默丢失** | 收尾不要只挂 `ready_cb`；或干脆做成"常驻对象"，从根上没有收尾 |

---

## 三、六个功能的落地设计

> 每个功能都按指南 §三 的四步走：① 找它的答案 → ② 三查（硬件/框架/资源）→ ③ 抄结构参数 → ④ 在我们框架里落地。

### 3.1 通用 toast 提示条（**第一个做**）

**为什么第一个做**：后面 5 个功能都要用它做反馈（"已保存""已连接""录制完成"），先做省重复劳动。

#### ★ 先纠正一个我原先的错误判断（已翻源码核实）

我原以为"`lv_layer_top()` 可点击会吞掉整屏触摸"（依据 `watch_overlay.c:7-11` 的注释），
**所以不能用 layer_top**。翻了自己的 LVGL 源码后确认——**这个理解是错的**：

```c
/* components/lvgl/src/hal/lv_hal_disp.c:225-230 */
disp->top_layer = lv_obj_create(NULL); /*Create top layer on the display*/
disp->sys_layer = lv_obj_create(NULL); /*Create sys layer on the display*/
lv_obj_remove_style_all(disp->top_layer);
lv_obj_remove_style_all(disp->sys_layer);
lv_obj_clear_flag(disp->top_layer, LV_OBJ_FLAG_CLICKABLE);   /* ★ 显式清掉 */
lv_obj_clear_flag(disp->sys_layer, LV_OBJ_FLAG_CLICKABLE);   /* ★ 显式清掉 */
```

**真相是两件事**：
1. **`top_layer` 本身不吞触摸**（LVGL 自己清掉了 CLICKABLE）；
2. 真正吞触摸的是**建在它上面的、默认尺寸的子容器** —— `lv_obj_create(NULL)` 默认**满屏且 CLICKABLE**。

→ **结论：toast 用 `lv_layer_top()` 是对的选择**（参考工程 `ui_manager.cpp:1139` 也是这么做的），
代价只是**必须显式清掉自己容器的 CLICKABLE / CLICK_FOCUSABLE**（参考工程 `:1164-1165` 也是这么做的）。
这条更正也解释了 `watch_overlay.c` 当年为什么改用独立 screen：不是 layer_top 不能用，而是**当时没清子容器的 CLICKABLE**。

#### 挂载点与层级

| | 方案 | 评价 |
|---|---|---|
| **选它** | `lv_layer_top()` + 自己清 CLICKABLE | 高于轮播 screen、也高于挂在轮播 screen 上的模态栈（`watch_modal.c:151`）→ **不会被模态盖住**，省掉 L4 那套"反复重新置顶"的补丁 |
| 不选 | host（轮播 s_scr）上的容器（`watch_alert` 的写法） | 与模态**同层争 z-order**，必须像 `watch_alert.c:152-157` 那样每次 poll 重新 `lv_obj_move_foreground()` 才不被盖住 |

**与提醒层的优先级**：`watch_alert`（闹钟/倒计时）**必须盖住一切**，它挂在轮播 screen 上。
→ 若用户正开着闹钟提醒，toast 显示在它之上会挡住"停止"按钮。
**约定：提醒可见时 toast 直接丢弃**（不是排队）——闹钟响时多一条提示本来也没价值。

#### 结构

```c
/* 新组件 components/watch_ui/watch_toast.c + include/watch_toast.h */
void watch_toast_show(const char *text);      /* 幂等，可在 LVGL 任务里随时调 */
void watch_toast_init(void);                  /* 建一次，常驻复用 */

/* 内部：单例常驻 */
static lv_obj_t *s_toast;   /* 药丸（在 lv_layer_top() 上） */
static lv_obj_t *s_label;
static lv_timer_t *s_hold;  /* 保持计时 */
```

- 容器：`lv_obj_create(lv_layer_top())` → **立刻** `lv_obj_clear_flag(CLICKABLE)` + `CLICK_FOCUSABLE`。
- 药丸：`LV_SIZE_CONTENT` + 最大宽 216、`pad_all = W_PAD_CARD(10)`、`radius = W_RADIUS_CARD(12)`、
  `bg = W_COLOR_CARD`、文字 `W_COLOR_TEXT` + `watch_font_cn(16)`。
  ⚠️ 别照抄参考工程的**215×90 靠左药丸 + 240 宽容器里居中标签**（那有 ≈12px 水平错位）。
- 位置：`LV_ALIGN_TOP_MID`，`y` 从 `-h`（屏外）滑到 `0`。

#### 动画与时长（参考工程实测数值）

| 项 | 参考工程 | 我们的取值 |
|---|---|---|
| 滑入/滑出 | 纯 Y 位移，**200 ms** | 同 200 ms |
| 缓动 | **linear**（它没设 `path_cb`，`lv_anim.c:71` 默认就是 linear）→ **很生硬** | **改 `lv_anim_path_ease_out`** |
| 保持 | **1500 ms**（用 `lv_timer`，不是 anim 回调） | 同 1500 ms |
| 行程 | 115 px（整个容器高） | 药丸自身高度 |

> 可见总时长 ≈ 200（滑入）+ 1500（保持）+ 200（滑出）。

#### 生命周期（**这是 toast 最容易写坏的地方**）

参考工程在这里有一个**真实缺陷，我们必须避开**：

> 它**每次显示都新建对象**、离场动画的 `ready_cb` 里删对象。
> 但 `hide_tip_container()` 一进来就把定时器删了并置 `nullptr`，而对象要等动画 ready 才删。
> 这 200 ms 窗口内再来一条提示 → 走"已存在就早退"的分支：文字改了，但定时器空转、也没有反向滑入动画，
> **而已在跑的离场动画照样在 ≤200ms 后把容器删掉** → 新提示一闪即逝、状态归零、再也不会显示。

**我们的做法（从结构上消灭这一类 bug）**：

1. **单例 + 常驻复用**：对象只创建一次，**永不删除**（`lv_obj_add_flag(HIDDEN)` 复用）。
   → 没有删除，就没有 UAF、没有"同树两个 `lv_async_call`"、没有"cleanup 写在 `ready_cb` 里被静默吞掉"。
2. **显式状态机**（而不是"有对象就早退"）：`IDLE → SHOWING → HOLDING → HIDING → IDLE`。
3. **`watch_toast_show()` 幂等**：
   - HOLDING 中再来 → 只换文案 + `lv_timer_reset()`（照参考工程正常路径的行为）；
   - **HIDING 中再来** → **反向 `lv_anim_start`**（LVGL 会自动取消同 `(var, exec_cb)` 的旧动画，`lv_anim.c:79-80`）+ 重建计时。
     这正是参考工程缺的那一步。
4. **不用 `lv_async_call` 做删除**（我们踩过"同一棵树排两个 async_call"必崩）。
5. **跨任务调用要持锁**：`watch_toast_show()` 可能被服务层（如天气任务完成）调用 →
   必须走 `ui_port_lock()`，或由页面在自己的 `refresh` 里转达。**绝不能从非 LVGL 任务直接碰 `lv_*`**。

**要点**：参考工程两处"删自己"是安全的（`lv_anim.c:431-438` ready 在 free 前调；`lv_timer.c:201-207` 定时器删自己有保护），
但**不安全的是"清理只挂在 `ready_cb` 上"这个模式**——`lv_anim_del` **只调 `deleted_cb`、绝不调 `ready_cb`**（`lv_anim.c:137-153`）。
我们靠"常驻不删"直接绕开整个问题。

### 3.2 计算器

**参考**：`main/ui/apps/fs_calculator.cpp`（334 行）。**我们要写得比它短且更健壮。**

**算法**：双栈调度场（`+ - → 1`，`* / → 2`）。纯 C 化后零堆分配：

```c
char   expr[40];      /* 表达式，限长（参考工程无上限，是缺陷） */
double vstk[24];      /* 值栈 */
char   ostk[24];      /* 运算符栈 */
```

**★ 一个必须你拍板的语义问题**：参考工程的 `%` 是**后缀百分数、解析时立刻 ÷100**，
所以 `100-10%` = **99.9**（不是 90，也不是 100.1）。

| 选项 | 语义 | 说明 |
|---|---|---|
| A | 照参考：`50%` = 0.5，`100-10%` = 99.9 | 行为明确、实现最简单 |
| B | 商用计算器语义：`100-10%` = 90 | 更符合直觉，但要记录"上一个操作数"，代码复杂度上升 |

**界面参数**（无语义，可直接沿用）：

| 元素 | 参数 |
|---|---|
| 显示区 | 230×70，`LV_ALIGN_TOP_MID(0,5)`，右对齐；**建议用 `lv_label`**（L8） |
| 键盘 | `lv_btnmatrix`，240×200，5 行 19 键：`C ÷ × [隐藏] / 7 8 9 - / 4 5 6 + / 1 2 3 % / 0 . =` |
| 键位尺寸 | 前 4 行每格 **50×30**，末行每格 69×30（LVGL 行内均分） |
| 退格 | 独立 `lv_btn` 48×30 @ `TOP_RIGHT(3,61)` |
| 样式 | pad 8、圆角 12；运算符橙 `LV_PALETTE_ORANGE`、`C` 与占位 `rgb(80,80,80)`、数字 `rgb(45,45,45)` |

**要补齐的健壮性**（参考工程的明确缺陷，逐条修）：

| # | 它的缺陷 | 我们的做法 |
|---|---|---|
| 1 | **无输入长度上限** | `expr[40]` 限长 |
| 2 | 无一元负号：`-5+3`、`5*-3`、`5--3` 全 Error | 数字 token 打 `neg` 标志；或按键守卫挡住 |
| 3 | `1..2` 被 `stod` 前缀解析**静默变 1** | 用 `strtod` 但**自己校验**：≥1 位数字、每数最多 1 个 `.`、`errno==ERANGE` 归溢出 |
| 4 | 除零判据 `\|b\|<1e-9` → `1/0.0000000001` 也报错 | 改成严格的 `b == 0.0` |
| 5 | 只查 `isinf` 不查 `NaN`（显示小写 `nan`，清屏名单写的是 `NaN` → 匹配不上） | 两个都查，错误串统一 |
| 6 | Error 后按运算符 → 文本变 `"+"` → 死锁 | 加 `bool just_evaluated`：数字键开新表达式、运算符键从结果续算 |
| 7 | 链式运算**用显示文本再解析**，`%.10g` 出科学计数法后清洗丢 `e` → 静默算错 | 另存 `double last_result` 累加器，**不复用显示文本** |

**测试向量**（固化成自检断言）：`1+2×3=7`、`2+3×4-5=9`、`50%=0.5`、`100-10%=99.9`、
`1/0=Error`、`5+=Error`、`1..2=Error`、超长输入被截断不崩。

**规模**：引擎 120~150 行 + UI 70~90 行 + 回调 60~80 行 ≈ **250~320 行 C**。

### 3.3 日历

**参考**：`main/ui/apps/fs_calendar.cpp`（仅 76 行，**最便宜的一个**）。

- 控件：`lv_calendar_create()`，**220×240**，`LV_ALIGN_TOP_MID(0,12)`。
- 我们的 sdkconfig **已开** `CONFIG_LV_USE_CALENDAR=y` + `HEADER_ARROW` + `HEADER_DROPDOWN`
  → 走 dropdown 头（月份是写死的 `"01".."12"` 数字串）。
- **星期名改成中文**：`lv_calendar_set_day_names()` 有运行时 setter（传 `日一二三四五六`）；
  字体用 `lv_obj_set_style_text_font()` 设日历对象（子对象继承）。
- ⚠️ dropdown 的月份**没有**运行时 setter → 保持数字月份（可接受）；要中文月份只能自画头部。
- 该页的日期文案（`日一二三四五六`）**要登记进 `watch_strings.h`**（否则豆腐块）。

### 3.4 2048

**参考**：`main/ui/apps/fs_game.cpp`（342 行）。**复用我们现成的游戏宿主层**（`watch_game.c` 已通用：全屏外壳、暂停/结算面板、防息屏、闹钟到点自动暂停、最高分存档）。

**抄的算法**（纯逻辑，无生命周期风险）：

- 指数存储 `uint8_t grid[4][4]`（1=2, 2=4…）→ 等值比较就是整数比较；
- `combined[4][4]` 记账，保证**一次移动同一格只合并一次**；
- 方向 `dr/dc` 直走（**不需要**"旋转棋盘"技巧）+ 朝目标边**反序遍历**；
- 新块：90% 出 2 / 10% 出 4；有移动才补 1 块，新局补 2 块；
- 网格 45/5、动画 150ms；配色 `0xfaf8ef / 0xbbada0 / 0xcdc1b4 / 0x776e65` 与色阶。

**★ 我们要比它好的三处**：

1. **动画当数据不当对象**：宿主本来就是帧驱动 + 画布（`watch_game.h:84-88`）。
   每格存 `{src_r, src_c, dst_r, dst_c, t}`，`step(dt)` 推进、`render()` 一次画完。
   → **重开/退出就是 `memset` 一块结构体，没有任何 anim/定时器需要清理**，从根上消除悬空回调。
2. **补动画期输入屏蔽**（`busy` 标志）——参考工程漏了，连划会跳变。
3. **补齐胜负判定**（参考工程完全没有）：无空格且无相邻等值 → 结算；达成 2048 → 胜利。
   交给宿主的结算面板。
4. `pow(2,val)` → **查表** `TILE_VAL[16]`（避免每格每帧浮点调用）。
5. 手势不用 `LV_EVENT_GESTURE`（默认阈值 50px 在 240 宽屏上偏重）；
   宿主转发坐标，自己算 Δ，**阈值 30~40px**。

**规模**：约 250~350 行（不含宿主）。

### 3.5 WiFi 多槽位（3~5 组）

**参考**：`fs_settings.cpp` + `compat/WiFi.cpp`。它的产品思路可抄，**实现细节多处不能抄**。

**★ 绝对不能抄的一条**：它是 **"点保存就直接写 NVS"**（`fs_settings.cpp:842-848`），
不发起连接、不校验、无回滚 —— **这正是我们 v0.6.1 修掉的 bug**（密码打错也照写，把能用的顶掉）。
我们必须**保留现有的两段式**：`hook_connect` 只暂存 → `STA_CONNECTED` 才落盘（`wifi_service.c:273-299 / 371-399`）。

**NVS 布局（向后兼容）**：

| 键 | 含义 |
|---|---|
| `ssid` / `pass` | **旧键保留**（迁移源） |
| `ssid1..ssid5` / `pass1..pass5` | 新槽位 |
| `slot_cnt` (u8) | 槽位计数 → 兼作"是否已迁移"的标志 |
| `last_ok` (u8) | 上次成功连接的槽号（开机优先试） |

**首启迁移**：`ssid` 非空且 `slot_cnt` 不存在 → 拷进槽 1，写 `slot_cnt=1`。
**命名刻意用 `ssidN` 而非参考工程的 `WiFiSSID%d`**（省 NVS 项 + 降低相似度 + 自有命名）。

**必须改的函数**（按此顺序，**L1 先修**）：

| 顺序 | 位置 | 改什么 |
|---|---|---|
| **0** | `wifi_service.c:124-132` | **`nvs_erase_all` → 按槽 `nvs_erase_key`**（不先修，扩槽即数据全灭） |
| 1 | `wifi_service.c:90-132` | `creds_save/load/erase` 加 slot 参数；新增 `creds_find(ssid,&slot)`、`creds_put`、`creds_del(slot)`、`creds_load_all()` |
| 2 | `wifi_service.c:371-399` | `STA_CONNECTED` 落盘 → 写命中槽/新槽 + 记 `last_ok`；**保留两段式暂存**（最易改坏） |
| 3 | `wifi_service.c:467-476` | 认证失败清理 → **只清当前槽** |
| 4 | `wifi_service.c:657-970` | `hook_connect` 空密码回填 → 改为"按 SSID 查槽"（现在是单条比较 `:701-705`） |
| 5 | `wifi_service.c:1267-1275` | 开机重连 → 按 `last_ok` 优先遍历槽位（**别 5×15s 串行**，单槽超时降到 5~8s） |
| 6 | `wifi_service.c:1103-1111` + `watch_platform.h:39-41,98-118` | `get_saved_ssid` → 支持按索引查（钩子两边同步改） |
| 7 | `watch_page_wifi.c:272, 700-702` | 任一槽命中即直接连；单值缓存改多值查询 |
| 8 | `app_console.c:641-717` | 加 `wifi list`、`wifi forget <SSID\|idx>`；**同时修 §10 #4**（帮助串写了 `forget` 但没实现，而 `hook_forget()` 早就是现成的，`:1014`，还挂在 hooks 上） |

**UI 形态建议**：复用现有"扫描列表 + 点选 + 密码键盘"，**槽位管理放二级页**（列表行 + 删除按钮）。
**不要照搬参考工程的"下拉选槽位 + 覆盖"交互**——它没有删除按钮，清槽只能"选同槽存别的 SSID"。

**风险**：
- 写频次 ×5 会加快 NVS GC → `last_ok` 仅变化时写；
- **必须去重**，否则同一 SSID 占多槽会反复重试；
- 清凭据粒度必须缩到**单槽**，否则一次错密码毁掉全部网络。

### 3.6 天气（**工作量最大，放最后**）

**参考**：`weather.cpp` + `ui_manager.cpp` 的 `Get_weather()`。

**数据源**（照它的参数，我们自己写）：

```
http://api.open-meteo.com/v1/forecast
  ?latitude=%.5f&longitude=%.5f
  &daily=weathercode,temperature_2m_max,temperature_2m_min
  &timezone=Asia%2FShanghai
  &forecast_days=7
```

- **明文 http 即可**（省掉证书包）；显式加 `User-Agent: SmartWatch/1.0`（它没设，便于排查）；
- 显式加 `&forecast_days=7`（它靠默认值，不保险）；
- 它只取 `daily`；我们建议**补 `&current=temperature_2m,weather_code`** 显示实时温度（它的增量空间）；
- 超时：它意图 8s 总超时/5s 连接，但嫁接层把两者合成一个（实际 ≈5s）。
  我们用 `esp_http_client` **显式设 `timeout_ms`**。

**★ 四处我们要比它好的地方**：

| # | 它的缺陷 | 我们的做法 |
|---|---|---|
| 1 | 缓存过期判定用 **`"MM/DD"` 字符串比较** → **跨月出错**（3 月初会拿 2 月数据） | 用 `wx_fetch_epoch`(uint32) + 设备本地日期判"抓取日 ≠ 今天" |
| 2 | **图标是 10 张 100×100 未压缩 RGB565 位图 = 200KB** | **图元自绘**（`lv_canvas` ~40×40 + 缓存 dsc，每种画一次），6~8 种，**省 200KB 且规避图像资产版权** |
| 3 | `is_day` 所有调用点都传 1 → **两张夜间图标从未被使用**（夜里显示太阳） | 用"云图元组 + 太阳/月亮图元"组合 |
| 4 | 无"正在加载/失败"以外的反馈 | 钩子里给 `fetching` / `fetch_failed`（照 `wifi_status_t` 的 `syncing/sync_failed` 既有教训：**点了按钮必须有反馈**） |

**架构（三层，与现有分层一致）**：

1. `components/weather/`（新组件，平台侧，**可** include ESP-IDF）
   `weather_service.c/h`：任务栈 6144~8192、**固定 CPU0**、优先级 1；
   复用 `wifi_service`（**不要再写一套 SSID 读取**）→ `esp_http_client_perform()` → cJSON → 写 NVS → 发事件 → **deinit WiFi**。
   ⚠️ cJSON 是堆分配，**必须 `cJSON_Delete`**，且每个 `cJSON_GetObjectItem` 都要判 NULL
   （它会因数组长度不一致而静默把缺失值当 0 = "晴"）。
2. `watch_ui_pages/pages/watch_page_weather.c`（UI，**禁止 include ESP-IDF 头**）
   只经钩子拿数据。
3. `watch_ui/watch_weather_icon.c`（图元自绘图标，**不新增位图资源**）

**新增钩子**（照 `watch_platform.h` 现有"结构体一次给完 + 取用必判空"的套路）：

```c
typedef struct {
    int  (*refresh)(void);                        /* 触发抓取，非阻塞 */
    void (*get_status)(w_weather_status_t *out);  /* 快照（服务内加锁） */
} w_weather_hooks_t;
```

**经纬度怎么设（关键设计决策）**：现有设置钩子签名是 **`int`**（`watch_pages.h:355,358`），存不了小数。
→ 用**整数微度**：单位 1e-5 度（上海 = `lat 3123000` / `lon 12147000`），
精度约 1.1m，int32 足够（±180e5 = 18,000,000）。
UI 收 `"31.23000"` → `strtod` → ×100000；显示 `%d.%05d` 还原。
**顺带好处**：整数存 NVS 没有 float 的精度/类型问题。

**规模**：约 1000~1300 行 C（组件 350~450、设置与钩子 60~80、设置页 200~260、
UI 250~320、图标 150~220）。

---

## 四、执行顺序与验证

```
第 1 步  toast                      ← 后面全要用（研究任务收尾后补数值）
第 2 步  §六 的三条顺手修（含 §10 #3 自动重连凭据自愈）  ← 用户正在踩
第 3 步  计算器                      ← 需先定 §3.2 的 % 语义
第 4 步  日历                        ← 最便宜
第 5 步  2048
第 6 步  字库三连 + 自检 + 截图（中途每步都跑）
第 7 步  WiFi 多槽位                 ← 动最敏感模块，单独一轮，先修 L1
第 8 步  天气                        ← 最大，放最后
第 9 步  写 doc/version/v0.7.0.md（最后一节「本版本改动简单说明」）
第 10 步 交用户烧录 + 实机验收
```

**每一步都要过的门（缺一不可）**：
1. `.\build.ps1 build` → 0 error（警告也要看）
2. `SIM_SELFTEST=1 lvgl_sim.exe` → **退出码 0**
3. 涉及中文 → `gen_font.ps1 -Force` + 三条校验（`PYTHONUTF8=1`）
4. 自检**补断言**（能自动验的别留给人眼）；修完 bug 把修复"藏起来"再跑一遍，确认断言会红
5. 实机（烧录 + 手感 + 颜色）只能由用户确认

**版本号三处**：`app.c:49` `APP_VERSION_STR`、`watch_page_settings.c:35` `WATCH_FIRMWARE_VERSION`、`doc/version/README.md`。

---

## 五、自检要补的断言（否则新功能是"裸奔"）

| # | 断言 | 抓什么 |
|---|---|---|
| 1 | 打开/关闭每个新模态页后 **对象数与定时器数回到基线** | 对象/定时器泄漏（v0.6.1 的 A12 模式，推广到新页） |
| 2 | 计算器测试向量逐条判值 | 引擎正确性（含 `1..2`、除零、超长输入） |
| 3 | 2048：`reset()` 前后**对象/定时器/画布**数不变 | 我们刻意"动画当数据"的设计是否真的零清理 |
| 4 | toast：连续触发 5 次 → 对象数不变（复用单例） | 重入泄漏 |
| 5 | 天气：钩子返回 `valid=false` 时界面显示占位而非空白 | 降级路径（模拟器无网络） |
| 6 | 日历：切月后不崩、日期文案无豆腐块 | LVGL 控件 + 字库 |

---

## 六、顺手修（v0.7.0 三件，全部已核实）

| # | 项 | 证据 | 改法 |
|---|---|---|---|
| 1 | **★ 自动重连不触发凭据自愈**（用户正在踩！） | `wifi_service.c:823` `s_conn_used_saved = from_saved`，而开机自动重连路径传的 `from_saved=false` | 自动重连也走 `disconnect_event_expected()`；认证失败按需清理该槽 |
| 2 | `wifi forget` 帮助里有、实现没有 | `app_console.c:1024` 帮助串 vs `:715` 用法串；`hook_forget()` 现成（`wifi_service.c:1014`，已挂 hooks `:1135`） | 补一行 `h->forget()` + 补用法串 |
| 3 | `bt` 不支持指定单个任务 | 交接文档 §10 #5；参考工程 `console.c:196-242` **有完整实现** | 照它的 `xTaskGetHandle` + `vTaskSuspendAll` + `vTaskGetSnapshot` + `esp_backtrace_print_from_frame` 思路自写；**只能实机验**（模拟器没有 FreeRTOS 任务） |

---

## 七、需要用户拍板的问题

| # | 问题 | 影响 |
|---|---|---|
| 1 | 计算器 `%` 语义：`100-10%` = **99.9**（照参考）还是 **90**（商用计算器）？ | 决定引擎复杂度 |
| 2 | 天气经纬度：用**整数微度**存 `lat_udeg/lon_udeg` 可以吗？你所在城市（默认给上海 31.23/121.47？） | 决定设置页形态 |
| 3 | 天气的"自动刷新"要不要做？做的话多久一次（耗电）？ | 决定是否加定时任务 |
| 4 | WiFi 槽位数定 **5** 还是 3？（5 与参考工程一致；3 更省 NVS 与遍历时间） | 决定 NVS 布局 |
| 5 | 2048 的胜利目标保留 2048 吗？达成后面板怎么显示（继续玩/结算）？ | 决定结算面板文案 |
| 6 | 闹钟 arm 语义（§4.1：注释说"等下一分钟"、实现"立刻响"、自检断言"立刻响"）要改成哪种？ | 行为变更，改前必须确认 |

---

## 八、本批**不做**的（明确划界，避免范围蔓延）

| 项 | 为什么不做 |
|---|---|
| 省电第一档（息屏真关屏/降刷新率/IMU 降频/校时后断 WiFi） | **用户已定拆到 v0.7.1** |
| 页面转场动画 | 依赖轮播改造（只有 3 槽、切页重建），风险较高，建议单独一批 |
| 数据通道（壁纸/相册/小说） | **用户已定暂不决定** |
| `wait_cb` / SPI 超频 | C 批（显示链路） |
| OTA / 分区表改动 | 独立一批（会移动分区，必须整片擦） |
| BLE 连手机 / AI 语音 / 健康监测 | P2 收尾 / P3 / P4 |

---

*B 批实施文档 · 基线 v0.6.1 · 状态：待用户审阅，未改代码*
