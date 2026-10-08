# 对标 Watch-Player_IDF：功能差距、体验差距与代码优化清单

> 对标对象：`相关资料/示例工程/Watch-Player_IDF`（ESP-IDF v5.5.5 + LVGL 8.3.11，12 个应用、约 1.3 万行）
> 我方：`firmware/smartwatch`（ESP-IDF v5.5.4 + LVGL 8.4，22 个组件、约 1.4 万行）
> 本文所有结论都标了来源（文件:行 或 实测命令输出）。**凡是没验证的，都在第八节单独列为"待核实"。**

---

## 零、先说三句结论

1. **前提要纠正**：这个工程**不是"和当前硬件一模一样"**。它是作者自己的开源 PCB（立创 `coconut-E/smart-watch-multi-function-termi`），引脚图和我们的微雪 1.69 完全不同，而且多了 **SD 卡 / I2S 功放 / 3 个物理按键 / 带电池的 RTC**。所以它的**媒体类功能（音乐/视频/图片/小说/漫画/文件管理）我们一件也搬不过来**。
2. **但它的"框架层"极其值得抄**：部分双缓冲 + 异步 DMA 刷屏、开机防花屏 + 背光淡入、运行时改刷新周期、深睡、串口控制台的 `taskmem/stats/bt`、启动加速配置包——这些与硬件无关，其中**好几项正是我们现在的短板**。
3. **我们不是全面落后**：闹钟、计步（IMU 算法）、IMU 录制、串口命令行、按需抽字的字库流水线、PC 端模拟器 + 自检 + 截图，这些是**它有我们没有的**；电池电量它还停留在"线性插值 + 手调系数"，我们已经用了**非线性放电曲线表**。

顺带一个重要发现：**我们的正式固件很可能在屏幕右下角显示着 `NN FPS / NN% CPU` 的调试浮层**（详见 5.2，来源是 LVGL 源码 + 我们的 sdkconfig，**请在实机上看一眼确认**）。

---

## 一、前提纠正：硬件并不相同

### 1.1 引脚对照（两边都是 ESP32-S3 + 240×280 ST7789，但接法完全不同）

| 功能 | Watch-Player_IDF | 我们（微雪 1.69 V2.1） |
|---|---|---|
| LCD SCLK / MOSI | GPIO12 / 13 | **GPIO6 / 7** |
| LCD DC / CS / RST | GPIO14 / 10 / 11 | **GPIO4 / 5 / 8** |
| LCD 背光 | GPIO9 | **GPIO15** |
| 触摸 I2C SDA/SCL | GPIO3 / 4 | **GPIO11 / 10** |
| 触摸 INT / RST | GPIO2 / —（驱动复位） | **GPIO14 / 13** |
| 外部 RTC | RX8130CE @0x32（**有备份电池**） | PCF85063ATL @0x51（**无备份电池**） |
| IMU | 无 | **QMI8658C @0x6B** |
| SD 卡 | SDMMC 1-bit（D0/CLK/CMD = 16/17/18） | **没有** |
| 音频 | I2S（BCLK/LRCLK/DIN = 47/40/48） | **没有，只有 GPIO42 蜂鸣器** |
| 物理按键 | **3 个**（GPIO6/7/8） | **没有** |
| 外设电源使能 | GPIO5（低有效，屏+SD 共用） | **没有** |
| 充电检测 | GPIO39 | **未在 board.h 登记**（见 8.3） |
| 电池 ADC | GPIO1 | GPIO1（**同一个脚**） |
| 空闲引脚 | 基本没有 | **只有 2 / 3 / 17 / 18** |

来源：`Watch-Player_IDF/main/app/board.h:17-39`、`main/drivers/lcd.c:25-31`；`firmware/smartwatch/components/board/include/board.h:27-68`。

### 1.2 由此得出的取舍

| 它的能力 | 我们能搬吗 | 原因 |
|---|---|---|
| 计算器、日历、天气、2048 | ✅ 可以 | 纯逻辑 + LVGL，只依赖触摸 |
| 环形菜单 / 缩放转场 / 侧滑亮度 / 顶部提示条 | ✅ 可以 | 纯 UI 机制 |
| 异步刷屏、深睡、启动加速、控制台 | ✅ 可以 | 与传感器无关 |
| 壁纸 / 多表盘 | ⚠️ 要自己造"数据通道" | 它靠 SD 卡导入图片；我们没有 SD |
| 音乐 / 视频 / 漫画 / 文件管理 | ❌ 不能 | 硬依赖 SD + I2S |
| 图片浏览器 / 小说阅读 | ⚠️ 改造量大 | 依赖 SD 存内容（我们只有 11MB SPIFFS） |

---

## 二、功能矩阵：谁有什么

### 2.1 我方已有（22 个组件、4 个轮播页 + 6 个模态页）

| 类别 | 内容 |
|---|---|
| 页面 | 时间 / 运动 / 健康 / 功能列表（轮播 4 页） |
| 模态 | 设置、时钟（时间/秒表/倒计时/**闹钟**）、WiFi 连接、开发人员选项（IMU 录制 / 时间校准 / 系统信息 / 蜂鸣器测试）、游戏（列表 → 小恐龙） |
| 常驻层 | 上滑控制中心（亮度滑块 / 息屏 / 关于）、到点提醒层 |
| 服务 | `data_hub`（单一数据源）、`motion`（IMU 计步）、`imu_logger`（录到 SPIFFS）、`wifi_service`（STA + 扫描 + SNTP 校时）、`settings`（NVS 名字寻址）、`event_bus`、`power`（自动息屏） |
| 工具 | 串口命令行 **16 条**（time/set_time/battery/brightness/settings/reset_settings/status/next/pages/wifi/power/rec/imu_raw/motion/motion_watch/steps_reset） |
| 工程化 | PC 模拟器 + 3 套自检 + 截图、字库按需抽字 + 3 个校验脚本、`build.ps1` |

### 2.2 对方已有而我方没有

| 功能 | 依赖 | 可做性 |
|---|---|---|
| 计算器（调度场算法 + 4×5 键阵） | 无 | ✅ 可直接做 |
| 日历（`lv_calendar`，仅 10 行有效代码） | 无 | ✅ 可直接做（我们 LVGL 已开 `CONFIG_LV_USE_CALENDAR=y`、`LV_USE_CHART=y`） |
| 天气（Open-Meteo 7 天 + 折线 + 图标 + NVS 缓存） | WiFi | ✅ 可做（我们已有 WiFi；`cJSON` 是 IDF 自带） |
| 2048 游戏（含移动/合并动画） | 无 | ✅ 可做（我们有现成的游戏宿主 + 游戏列表） |
| 壁纸 / 多表盘（静态图 + MJPEG 动态壁纸） | SD 导入 | ⚠️ 要自造导入通道 |
| 图片浏览 / 小说阅读 | SD | ⚠️ 要自造内容通道（SPIFFS 够放） |
| 音乐 / 视频 / 漫画 / 文件管理 | SD + I2S | ❌ 需加硬件 |
| 环形菜单 + 图标缩放 + 全屏缩放转场 | 无 | ✅ 可做（视觉升级） |
| 侧滑亮度抽屉（吸附动画） | 无 | ⭕ 我们已有"上滑控制中心"，等价 |
| 顶部提示条 toast（1.5s 自动消失） | 无 | ✅ 可做（复用面很广） |
| 开机"先刷黑 → UI 就绪 → 背光淡入 400ms" | 无 | ✅ 可做（成本极低） |
| **深度睡眠 + 唤醒**（长按电源键关机→按键唤醒） | 它用按键 | ⚠️ 我们得换唤醒源（见 4.4） |
| **WiFi 凭据 5 个槽位**（家里/公司/手机热点） | WiFi | ✅ 可做（我们现在只存 1 组：`wifi_service.c` 的 `WIFI_NVS_SSID`/`WIFI_NVS_PASS`） |
| 电池校准页（改系数 + 实时预览） | 无 | ⭕ 价值低（我们的电量算法已经比它好，见 4.3） |
| 串口控制台 `stats`/`taskmem`/`bt`/`ram`/`psram`/`nvs`/`temp` | 无 | ✅ 补进我们现有命令行（`taskmem` 尤其值钱） |
| coredump 分区 | 无 | ✅ 见 5.5 |
| 运行时改 LVGL 刷新周期 + `lv_refr_now` 手动刷 | 无 | ✅ 见 4.2 |

### 2.3 我方独有（它没有）

- **闹钟**（它只有秒表 + 倒计时，没有闹钟）
- **IMU 计步**（它完全没用 IMU；QMI8658C 驱动是我们自己写的，645 行）
- **IMU 录制到 SPIFFS**（用来回传真实走路数据调算法）
- **PC 模拟器 + 3 套自检 + 截图**（它没有任何自动化测试）
- **字库按需抽字 + 3 个校验脚本**（它是全量 `chinese_24.c` 单文件 **13.2 MB** 源码）
- **单一数据源 `data_hub` + 钩子注入的平台抽象**（它是 `extern` 全局变量满天飞 + Arduino 兼容层）
- **非线性电池放电曲线**（它是线性插值 + 手调系数）

---

## 三、还缺的功能（按"能不能做"分四组）

### 第 1 组：纯软件，现在就能做（建议优先）

| 功能 | 参考它的实现 | 工作量 | 备注 |
|---|---|---|---|
| **计算器** | `fs_calculator.cpp`（334 行，调度场算法双栈） | 小 | 我们纯 C，去掉 STL/异常后约 250 行 |
| **日历** | `fs_calendar.cpp`（76 行） | 极小 | LVGL 日历控件已编译进去，只需配我们的模态框架 + 触摸返回 |
| **天气** | `ui_manager.cpp` 的 `Get_weather()` + `weather.cpp`（173 行渲染） | 中 | 用 `esp_http_client` + IDF 自带 `cJSON`；Open-Meteo **明文 http 即可**（对方就是 `http://api.open-meteo.com`，省掉证书包）；图标建议用 LVGL 图元画，别引位图 |
| **2048** | `fs_game.cpp`（342 行，含动画） | 小 | 我们有 `watch_game.c` 宿主 + 游戏列表页，只需加逻辑 + 列表一行 |
| **开机背光淡入** | `boot_anim.cpp`（400ms，80 级 × 5ms） | 极小 | 配合"先刷黑再开背光"防花屏 |
| **顶部 toast 提示条** | `show_tip_container()`/`hide_tip_container()` | 小 | 可复用到"已保存/已连接/已断开/录制完成"等所有反馈 |
| **多 WiFi 槽位**（3~5 组） | `WiFiSSID1..5`/`WiFiPass1..5` | 小 | 我们 `wifi_service` 现在只存 1 组 |
| **命令行补 `taskmem`/`stats`/`bt`** | `console.c`（381 行，自包含） | 小 | `taskmem` 直接告诉我们每个任务栈该给多少 |
| **页面切换动画** | 缩放/淡入转场 | 中 | 我们现在的翻页是"瞬移" |
| **秒表分段（Lap）** | 它也没有，属增补 | 小 | |
| **倒计时结束蜂鸣** | 它也没有（注释明说没提示音） | 极小 | 我们有蜂鸣器，白捡 |

### 第 2 组：需要一个"数据通道"（这是壁纸/相册/小说的前提）

对方靠 SD 卡当内容来源，**我们没有 SD，但有 11MB SPIFFS + WiFi + 原生 USB**。可选通道：

| 方案 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **A. WiFi 上传页**（推荐） | 设备开 SoftAP 或连家里路由，跑 `esp_http_server`，手机/PC 浏览器上传文件到 SPIFFS | 已有 WiFi 栈；手机就能用；不占引脚 | 要写 HTTP 上传 + 前端页；耗电（缩短续航） |
| **B. 串口上位机** | PC 端 Python 脚本按协议把文件写进 SPIFFS | 最简单、最稳、可脚本化 | 必须插线 |
| **C. USB 大容量设备（MSC）** | 把 flash 分区当 U 盘挂到 PC | 体验最好 | 分区要换成 FAT + 磨损均衡；`tinyusb` + MSC 回调改造量大，且有与固件并发访问的风险 |

**配套关键简化**：壁纸不要用 PNG/JPEG 解码——**在 PC 端就转成裸 RGB565（240×280×2 = 134400 字节）再上传**，设备端零解码器，读进 PSRAM 直接 `lv_img_set_src`。
这正是对方 `wp_header_t`（魔数 `"WP01"` + 134400B 预览）的思路，只不过它把"转换"放在了设备上（靠 pngdec/JPEGDEC），我们放在 PC 上更省事 —— **也绕开了"我们组件全部本地化、不能引 managed 依赖"的限制**。

有了数据通道后，第 2 组功能全部解锁：**多表盘/壁纸、相册浏览、TXT 小说阅读、导出 IMU 日志、备份/恢复设置**。

### 第 3 组：需要加硬件（本批不做，已记入《功能扩展对标与可行性分析》）

音乐/视频/漫画/文件管理器/语音/GPS —— 都是 SD + I2S 或额外模块，本轮不动。

### 第 4 组：不建议做

模仿它的 **MJPEG 动态壁纸**（需 JPEG 解码器 + 三块 134KB PSRAM + 双核流水线，收益只有一个会动的背景）；**漫画 `.cmj` 格式**（还要自己写打包工具）。

---

## 四、体验/行为上要优化的地方（对标它的做法）

### 4.1 显示流水线：我们是"异步刷屏 + 忙等自旋"

**已经对的部分**：我们用 `esp_lvgl_port`，配置 `buffer_size = 240×80`、`double_buffer = 1`（`components/bsp/bsp.c:220-221`），内部走的是**异步**路径：
- `flush_cb` 只调 `esp_lcd_panel_draw_bitmap()` 就返回，**不调** `lv_disp_flush_ready()`（`esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:417-432`，我们 `trans_size=0`）；
- `lv_disp_flush_ready()` 由 SPI 传输完成中断回调触发（同文件 `:339-354`）。

**缺的部分**：我们**没有设 `wait_cb`**（`bsp.c:217-229` 的 `disp_cfg` 里没有，注册后也没补）。而 LVGL 8.4 的局部双缓冲语义是：

```c
/* lv_refr.c:1277-1282 */
bool full_sized = draw_buf->size == hor_res * ver_res;
if(draw_buf->buf1 && draw_buf->buf2 && !full_sized) {
    while(draw_buf->flushing) {
        if(disp_refr->driver->wait_cb) disp_refr->driver->wait_cb(disp_refr->driver);
    }
}
```

我们的缓冲是 1/3 屏（`!full_sized` 成立），所以**每次都在这个循环里空转等待**上一段 DMA 完成 —— 而对方专门写了 `my_disp_wait_cb()` 用信号量 `xSemaphoreTake(..., portMAX_DELAY)` 阻塞，源码注释就一句："**阻塞等待上一段 DMA 完成，避免在 lvgl 任务里忙等空转**"（`ui_core.c:57-64`）。

| 项 | 影响 | 建议改法 | 改动量 | 风险 |
|---|---|---|---|---|
| 缺 `wait_cb` | 每次刷屏在 LVGL 任务里自旋，白烧 CPU/电（对方专门规避） | 给显示驱动补 `wait_cb`：`esp_lcd_panel_io_register_event_callbacks()` 注册自己的完成回调（给信号量 + 调 `lv_disp_flush_ready`），再把 `s_disp->driver->wait_cb` 指向"取信号量" | 小 | 中（要保证 `flush_ready` 每段恰好调一次；需实机验证不撕裂、不卡死） |

> 另一条路：像对方那样**不用 `esp_lvgl_port`，自己写 60 行 LVGL 移植**（`ui_core.c` 只有 160 行），完全掌控 flush/wait/tick。收益是彻底可控，代价是放弃官方组件的维护性。**建议先试补 `wait_cb`，不行再考虑自写移植。**

### 4.2 屏幕 SPI 是 40MHz，对方是 80MHz（可试，但别当成白捡）

- 我们：`bsp.c:107` → `pclk_hz = 40 * 1000 * 1000`
- 对方：`lcd.c:33` → `80 * 1000 * 1000`
- **但微雪官方 ESP-IDF LVGL 示例用的也是 40MHz**（`examples/esp-idf/02_ESP_IDF_ST7789_LVGL/main/main.c:31`）。

算一笔账：整屏 240×280×2 = 134,400 B。@40MHz ≈ **27ms/整屏**（≈37FPS 上限），@80MHz ≈ 13ms。我们现在整屏刷新确实是瓶颈（v0.6.0 文档里记过这个 27ms）。

**建议**：把它当成**一次实验**，不要直接改。做法：临时把 `pclk_hz` 提到 60MHz 编译烧录，用"整屏滚动 + 游戏"看有没有花屏/错位；没问题再试 80MHz。`max_transfer_sz` 现在是 `240*80*2 = 38400`（`bsp.c:99`），正好覆盖一段 flush，这个别动。

| 项 | 收益 | 风险 |
|---|---|---|
| 40 → 60/80MHz | 整屏刷新 27ms → 18/13ms，翻页和动画明显更顺 | **需要实机验证**：ST7789V2 标称上限约 62.5MHz；超频可能花屏、也可能完全正常，取决于我们这块板的走线 |

### 4.3 电池：我们已经比它好，但缺"充电状态"

- 我们：`battery.c:187-206` 用**放电曲线表**做分段线性插值，还有中值滤波（`:48`）和 `adc_cali` 曲线拟合（`:88-96`）；
- 对方：`battery.cpp` 是 `(V-3.3)/(4.15-3.3)` **纯线性** + 手调系数 `batt_cal`（80~120%）+ 一阶 IIR，另外还做了一件聪明事：**充电时内阻压降动态补偿**（解决"插着充电器显示 100%，一拔就掉"）。

结论：**不建议照搬它的"电量校准页"**（对我们没价值）。真正缺的是：
1. **充电状态显示**（我们的界面只有电量百分比）。对方用 GPIO39 采样 PWM 占空比判充电；**我们的板子有没有这个脚还没确认**（见 8.3）。
2. 它的**内阻压降补偿**思路值得抄（如果以后要显示充电中的准确电量）。

### 4.4 功耗：它是"深睡整体断电"，我们是"只关背光"

**现状**（`power.c:5-11` 明确写着"只调 `bsp_backlight_set(0)`。CPU、LVGL、IMU 采样任务全在跑"，这是 v0.5 之前的刻意取舍）。所以息屏时：
- 背光关（省了大头）✅
- LCD 面板**没进 sleep**（还在扫描显示 GRAM，有静态功耗）
- LVGL 仍以 **10ms（100Hz）** 刷新（`sdkconfig`：`CONFIG_LV_DISP_DEF_REFR_PERIOD=10`）
- 触摸仍以 **10ms** 轮询 I2C（`CONFIG_LV_INDEV_DEF_READ_PERIOD=10`）
- `motion` 的 50Hz IMU 采样任务继续跑
- **WiFi 连上就一直连着**（`wifi_service` 没有"闲置断开"）
- 电源轮询定时器 50ms

对方：`CONFIG_PM_ENABLE` 也没开（CPU 恒 240MHz），但它有**深睡**（`board_sleep()`：关背光 → 断外设电源 → 按键唤醒 → `esp_deep_sleep_start()`）。

**我们的难点**：没有物理按键，深睡唤醒源得换。可选：
| 唤醒源 | 可行性 | 说明 |
|---|---|---|
| 触摸 INT（GPIO14）走 `ext1` | ⚠️ 要实测 | 深睡时 I2C 掉电，CST816T 能否在待机下拉低 INT 需验证；对方源码里触摸 INT 只做轮询，**没有用它唤醒过** |
| RTC INT（PCF85063ATL，GPIO39） | ✅ 理论最优 | 支持定时/闹钟中断唤醒 —— 但**对方工程完全没用这颗芯片，没有现成代码**，要自己写 |
| IMU INT（QMI8658C 运动唤醒） | ⚠️ 复杂 | 抬手唤醒可以走这条，但要调阈值 |
| 定时唤醒 `esp_sleep_enable_timer_wakeup` | ✅ 最稳 | 最小可用方案：深睡 N 分钟后醒来刷一次表盘 |

**代价提醒**：**深睡 = 整机重启**（`app_main` 重跑）。我们冷启动目前要等 I2C 自检 + WiFi 起来（WiFi 首次 RF 校准要几秒），所以深睡只适合"用户主动关机/长时间不用"，不适合"息屏 30 秒就深睡"。
**更合适的中间档**：**轻睡（light sleep）**—— RAM 保持、唤醒只要微秒级、不用重新初始化，配合"息屏 + 降刷新率 + WiFi 省电模式"能拿到大部分收益。这一档对方**没有做**，属于我们可以反超的地方。

### 4.5 启动速度：它做了 4 件我们没做的事

| 它的做法 | 我们的现状 | 收益 | 风险 |
|---|---|---|---|
| `CONFIG_SPIRAM_MEMTEST=n` | **`=y`**（每次启动全片写读测试 8MB PSRAM） | 省可观启动时间 | 不再在启动时发现 PSRAM 损坏 |
| `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y` | 未设 | 深睡唤醒跳过镜像校验 | 前提是不开 Secure Boot、不用 OTA 切分区 |
| `CONFIG_BOOTLOADER_COMPILER_OPTIMIZATION_PERF=y` | `=SIZE` | bootloader 快一点 | bootloader 变大 |
| `CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y` | `=INFO` | 省几毫秒 | 开机早期日志没了（他们那边日志走 UART0 看不到；**我们的控制台也是 USB-Serial-JTAG，是否同理需实测**） |
| "先刷黑 → UI 就绪 → 背光淡入" | 没有（背光直接开到目标值） | **感知启动更快**，且不刺眼、不露花屏 | 极低 |

### 4.6 交互细节差距

| 点 | 它 | 我们 | 建议 |
|---|---|---|---|
| 反馈提示 | 顶部滑入提示条，1.5s 自消，`lv_layer_top()` 上 | 只有闹钟提醒层 + WiFi 页内的 popup | 抽一个通用 toast |
| 页面转场 | 全屏缩放动画（300ms，指数缓动 + 圆角裁剪） | 瞬移 | 加淡入/位移，成本小收益大 |
| 菜单 | 环形菜单 + 图标按距中心缩放 1.2/0.8/0.7 | 3 槽轮播 | 可给当前页加"放大量"，不必重做成环形 |
| 亮度 | 侧滑抽屉（带迟滞吸附） | 上滑控制中心滑块 | 等价，不改 |
| 二手准备 | — | 我们多了"功能列表需滚动，上滑控制中心在该页失效" | 见 5.6 |

---

## 五、代码/配置层面需要优化的地方

> 分级：**P0 = 必须改**（真 bug / 明显错误）、**P1 = 应该改**（明白的收益）、**P2 = 可以改**、**P3 = 锦上添花**。
> 每条都给了证据位置。**标注"[需实机确认]"的，请你在设备上按描述看一眼再决定。**
>
> 5.1~5.8 来自"对标参考工程"时发现的差异；**5.9~5.16 来自一轮对我们自己代码的逐行审计**（**5.9 是本次唯一的 P0**）。

### 5.1 【P1】息屏后第一下触摸会"穿透"到界面

**现象（源码级推导）**：屏幕熄灭时点一下屏幕 → 屏幕亮起**同时**，手指下面那个控件会收到一次点击（例如在功能列表上直接打开子页面）。

**证据链**：
1. `power.c:12-21` 的设计说明写得很清楚：息屏后第一次触摸要"把 LVGL 的输入设备临时禁用（`lv_indev_enable(false)`），这一下就不会传到界面"；
2. 但**全工程只有一处调用 `lv_indev_enable`，而且是 `true`**（`power.c:161`），**没有任何地方调 `false`**（全仓 grep 确认）；
3. 触摸上报路径：`bsp.c:151-170` 每次按下都 `data->state = LV_INDEV_STATE_PRESSED`，同时调 `w_screen_notify_activity()`；
4. `power.c:211-219` 的 `hook_activity()` 在屏幕灭着时**只调 `power_screen_on()` 就 return**，没有拦住这一次输入；
5. 想在同一个 tick 里靠 `lv_indev_enable(false)` 拦住也**来不及**：LVGL 8.4 是在**读回调之前**检查 `proc.disabled`（`lv_indev.c:78-79`），本次 PRESSED 仍会被处理。

**建议改法**（在 BSP 触摸读取回调里拦，而不是禁用输入设备）：
```
bsp.c 的 touch_read_cb：
  若 power 侧处于"刚唤醒的屏蔽窗口"（power.c 已有 s_wake_at / W_IGNORE_TOUCH_MS=350ms 这套状态）
    → 上报 RELEASED（不上报 PRESSED、不更新坐标）
```
即把"吃掉第一下"从"事后禁用 indev"改成"源头不报按下"。**改动量小**，但要动 `watch_platform.h` 的钩子（加一个 `should_swallow_touch()`）或让 `power` 暴露一个查询函数。

**验证办法（实机）**：息屏 → 在"功能列表"的某一行上点一下 → 若直接进了子页面，就是这个问题。

### 5.2 【P1】调试用的 FPS/CPU 浮层被编进了正式固件

**证据链（全部来自源码，无需实机即可确认编译期配置）**：
1. 我们的 `sdkconfig`：`CONFIG_LV_USE_PERF_MONITOR=y`、`CONFIG_LV_USE_LABEL=y`（`sdkconfig.defaults:47` 也是这么写的）；
2. LVGL 的 Kconfig → 宏映射确实生效：`components/lvgl/src/lv_conf_internal.h:754-766` 把 `CONFIG_LV_USE_PERF_MONITOR` 映射成 `LV_USE_PERF_MONITOR`，位置默认 `LV_ALIGN_BOTTOM_RIGHT`；
3. `components/lvgl/src/core/lv_refr.c:362-376`：该宏为 1 时会在 `lv_layer_sys()` 上**创建一个半透明黑底标签**，文案 `"%"PRIu32" FPS\n%"PRIu32"%% CPU"`（`:415`），压在所有界面之上。

**为什么模拟器截图里看不到**：`lvgl_sim/lv_conf.h:282` 里 `LV_USE_PERF_MONITOR 0`。**所以截图和实机不一致**——这也是本轮才发现它的原因。

**旁证**：我们的 `doc/官方示例配置对照.md:40` 把 `LV_USE_PERF_MONITOR` 标成"✅ 与官方示例一致"——微雪官方示例确实开着它，但那是**示例**，不是产品固件。

**建议**：`CONFIG_LV_USE_PERF_MONITOR` 改为 `n`（省一点 CPU + 去掉浮层）；真要测帧率时用命令行开关或临时打开。
**改动量**：极小（一行配置）。**风险**：无。
**请在实机看一眼右下角确认**：如果那里本来就没有东西，说明我这条判断错了，我会重新查。

### 5.3 【P1】应用是 `-Og` 编译的，对标工程是 `-O2`

**证据**：
- 我们的 `build/compile_commands.json` 里编译参数含 **`-Og`**；`sdkconfig` 里 `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` + `CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y` + `CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y`；
- 对方 `sdkconfig:604`：`CONFIG_COMPILER_OPTIMIZATION_PERF=y`（**`-O2`**）。

**影响**：LVGL 渲染、`motion` 的 50Hz 滤波、游戏每帧的像素搬运算力都直接受影响 —— 这是**零代码改动**能拿到的最实在的一笔性能。

**建议**：`sdkconfig.defaults` 加 `CONFIG_COMPILER_OPTIMIZATION_PERF=y`（`-O2`），同时 `CONFIG_BOOTLOADER_COMPILER_OPTIMIZATION_PERF=y`。
**代价**：固件会变大一点（现在 image 1.66MB，4MB 分区还剩 60%，完全够）；`-O2` 偶尔会暴露"依赖未定义行为"的老代码，编译一次 + 实机跑一遍就知道。
**改动量**：配置两行。**风险**：低。

### 5.4 【P1】没有 coredump，崩溃只能靠猜

**证据**：`sdkconfig` 是 `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y`；`partitions.csv` 只有 4 个分区（nvs / phy_init / factory / storage），**没有 coredump 分区**。
对方虽然也没打开 `CONFIG_ESP_COREDUMP_*`，但**分区留好了**（`partitions.csv` 最后一行 `coredump ... 0x10000`）。

**为什么对我们特别重要**：按我们的分工，**实机验证全在你那边**。有了 coredump，设备崩溃（看门狗复位、StoreProhibited、LoadProhibited）之后你只要把板子插上跑一句 `idf.py coredump-info`，就能拿到**出错任务 + 调用栈 + 寄存器**，而不是只有一句复位日志。

**建议**：
1. `partitions.csv` **在 flash 尾部追加** `coredump, data, coredump, 0xF10000, 0x40000`（256KB）—— 已用 `idf.py partition-table` 核实：`storage` 结束于 `0xF10000`，尾部空闲 960KiB，**追加不会移动任何现有分区**；
2. `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`、`CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`；
3. 顺手把 `CONFIG_ESP_TASK_WDT_PANIC=y`（现在是关的 → 任务看门狗只打印不复位）。
> ⚠️ **本条已更正**（原写"必须整片擦除再烧"）：那是按"插在 storage 之前"的方案算的，会移动 SPIFFS 导致数据失效。改成**追加在尾部**后，`nvs`/`phy_init`/`factory`/`storage` 的偏移全不变，**普通 `idf.py flash` 即可，不需要整片擦**。详见 [A 批实施文档](智能手表-A批实施文档.md) 的 A10。
**改动量**：小。**风险**：低。

### 5.5 【P2】启动/内存配置包没抄

按 4.5 的表，建议一次性加进 `sdkconfig.defaults`：`CONFIG_SPIRAM_MEMTEST=n`、（可选）`CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y`、`CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y`、`CONFIG_BOOTLOADER_COMPILER_OPTIMIZATION_PERF=y`，以及对方的堆阈值策略 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` / `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`（≤16KB 的分配走内部 RAM，给内部 RAM 留 32KB 底）。
我们**已经一致**的项：`CONFIG_FREERTOS_HZ=1000`、`SPIRAM_MODE_OCT` + `SPIRAM_SPEED_80M`、`ESP_DEFAULT_CPU_FREQ_MHZ=240`。

### 5.6 【P2】空闲时的功耗与刷新率（附带一个界面小坑）

- `CONFIG_LV_DISP_DEF_REFR_PERIOD=10`（100Hz）、`CONFIG_LV_INDEV_DEF_READ_PERIOD=10`（100Hz 触摸轮询）。对方是 **20ms / 50Hz**。手表场景 50Hz 足够，可以把空闲开销砍一半（LVGL 无脏区时也不会重绘，省的是定时器唤醒 + 触摸 I2C 事务）。
- **界面小坑（已知）**：功能列表页现在装不下 5 行 → 允许滚动 → 该页"上滑开控制中心"失效，且手动滚动没有惯性/回弹（v0.6.0 已记录）。
  这是个**可以彻底解决**的问题：把行高从 52 收到 44（5×44 + 4×6 = 244，加标题 32 = 276 ≤ 280），**5 行一屏放下就不用滚了**，"上滑控制中心"在该页自动恢复。需要你确认 44px 行高可读性够不够（和"游戏列表 52px 行"的选择是同一类取舍）。
- WiFi 连上后一直保持（`wifi_service` 没有闲置断开策略）。当前 UI 已经提示用户"不用请断开"，但**可以做成自动**（例如息屏 5 分钟后自动断开、亮屏重连）。

### 5.7 【P3】注释与配置不一致（小但会误导人）

- `bsp.c:143-145` 写"LVGL 每 `LV_INDEV_DEF_READ_PERIOD`**(5ms)** 调一次本函数"，实际 `sdkconfig` 是 **10ms**。
- `board.h:46` 写 IMU "P1 暂未使用" —— 现在 `motion`/`imu_logger` 都在用它了。
- `power.c:12-21` 描述的"临时禁用输入设备"机制**并未实现**（见 5.1）—— 这条注释会让人以为已经处理好了。

### 5.8 【P3】其它可选项

- **分区表为 OTA 预留**：现在 `factory` 4MB / `storage` 11MB。若将来做 OTA（《功能扩展对标与可行性分析》v0.8 计划），需要 `otadata` + 两个 app 槽（可以 2MB + 2MB 塞进现在的 4MB 里）。**现在不动，只是记着**。
- **Flash 占用画像**（`idf.py size-components` 实测）：
  | 组件 | 合计 | 其中 Flash 数据(.rodata) |
  |---|---|---|
  | `liblvgl.a` | 348,590 B | 170,677 B |
  | `libfonts.a`（3 个中文字号 14/16/20） | 287,562 B | 287,493 B |
  | `libnet80211.a`（WiFi） | 148,972 B | 16,704 B |
  | 整机 image | **1,662,253 B（1.59MB）** | — |
  → 4MB 分区还剩 **60%**，**不缺空间**，所以**不建议为了省 flash 去砍中文字号**（会牺牲观感）。这里列出来只是为了"知道钱花哪了"。
- `idf.py size` 报 `IRAM 16384 / 16384 = 100%`。**这条我没搞清含义（可能只是 size 工具对固定别名段的报法），不作为结论**，列入第八节待核实。（更可信的数据是 DIRAM：用 167,627 / 341,760 = **49%**，还剩约 170KB 内部 RAM。）

### 5.9 【P0】WiFi 页的轮询定时器：关闭页面不删定时器 → 写已释放内存

**这是我本轮唯一认定的 P0**（已逐行核实，不是推测）：

- `watch_page_wifi.c:423-428` 的 `ensure_poll()` 建轮询定时器，被 `:448`、`:701`、`:727` 三处调用（进页面自动扫描、重扫、状态变化）；
- 全文件的 `lv_timer_del` **只有两处，都在 `poll_cb` 自己内部**（`:386-389` 状态变化时删自己、`:404-407` 扫描超时时删自己）；
- `:632` 的 `ui.poll = NULL` 只是**清自己的指针**，没有杀定时器（注释写的是"别让它悬空"，但悬空的恰恰是定时器）；
- **整个文件没有 `LV_EVENT_DELETE` 钩子**，`w_modal_close()` 也不会去删页面的私有定时器。

**后果**：用户在**扫描进行中**按返回（扫描要 2~4 秒，很容易发生）→ 页面对象被销毁，但定时器还活着 → 下一次 `poll_cb` 走到超时分支 → `set_status()` / `show_scan_list()` 去改**已经释放的 `ui.status` / `ui.list`** → use-after-free（花屏、日志乱、随机重启）。即使没崩，定时器也永久泄漏、永远在跑。

**建议改法**：
1. `wifi_page_build()` 开头改成 `if (ui.poll) { lv_timer_del(ui.poll); ui.poll = NULL; }`；
2. 给页面根挂 `LV_EVENT_DELETE`（照 `watch_page_clock.c` 现有的写法），在钩子里删定时器 + 把 `ui.list/ui.status/ui.poll` 全部置 NULL。

**改动量**：小。**风险**：低。
**顺带**：这个缺陷本可以被自检抓住 —— 见 5.16。

### 5.10 【P1】时钟服务（闹钟/倒计时）没有跨任务保护

**核实路径**：`watch_pages.c:645` 调 `watch_clock_on_time(&state->time)` —— 位置在 `:651` 的 `if (!ui_port_lock(0))` **之前**，而它是被 `data_hub` 的采样任务回调进来的（`:658`）。
同时 `watch_clock.c` 的 `clock_timer_cb`（LVGL 任务）也在读写同一批静态状态：`s_alert` / `s_alert_ms` / `s_alert_beep` / `s_time_ok` / `s_al_fired_hhmm`。
`watch_ui` 组件里 **0 处** `xSemaphore`/`portMUX`（全组件 grep）。

**后果**：偶发"闹钟/倒计时响不停"、"点了【停止】没停"、"同一分钟响两遍或一遍都不响"。这类问题**极难复现**，最容易被当成"偶发"放过。

**建议改法**：给这几个小整数加一个 `portMUX_TYPE` 临界区（临界区里**绝不能**碰 LVGL 和蜂鸣器，只改状态）；或者更干净：到点判定只投递一个事件，所有状态变更统一收敛到 LVGL 任务里做。

**改动量**：中。**风险**：中（要么不动，要么仔细做）。

### 5.11 【P1】`data_hub` 把"活内存指针"交给回调 + UI 刷新跑在 3KB 栈的采样任务上

两条相关的问题（`data_hub.c`）：
1. `notify_locked()` 是**先释放锁再回调**，而传下去的是 `&s_state`（`:93`/`:135`/`:358`）——回调执行期间采样任务仍在改这块内存，UI 可能读到"只更新了一半"的状态（表盘偶发错值、日期与时间不同步）。
   **改法**：锁内做栈拷贝 `device_state_t snap = s_state;` 再把 `&snap` 传出去（结构体只有几十字节）。改动量小。
2. `HUB_TASK_STACK` 是 **3072**（`:34-35`），而整条 UI 刷新链跑在这个任务上（`watch_pages.c:631-661` → 各页 `refresh` → LVGL 调用）。
   **改法**：先提到 4096~5120，并在 `main.c` 心跳里打印 `uxTaskGetStackHighWaterMark`，用数据决定最终值（这也是为什么建议补控制台 `taskmem`，见第 1 组）。

**改动量**：小。**风险**：低。

### 5.12 【P1】`settings` 写 NVS 在锁外

`settings.c` 里"取值夹取 + 写 NVS + `nvs_commit`"**不在同一把锁内**（取锁 → 释放 → 才写），而 `s_nvs` 句柄是全局的、多任务会并发进来（UI 改亮度、控制台改设置、`wifi_service` 写校时状态）。
**后果**：偶发写失败（`ESP_ERR_NVS_*`）、极端情况下键值错乱。
**改法**：把这几个动作放进同一把锁；`settings_reset*` 同理。**改动量**：小。**风险**：低。

### 5.13 【P2】息屏没有真正"关屏"（省电第一档，收益最快）

核实：全工程 `esp_sleep*` / `esp_pm_*` / `esp_lcd_panel_disp_off` **零调用**。所以息屏后 **LCD 面板仍在被 LVGL 刷**（每帧最多 240×80×2 字节走 SPI），CPU 固定 240MHz。
**按性价比排序的改法**：① 息屏时 `esp_lcd_panel_disp_off()` + 放慢/暂停 LVGL 刷新（改动小、风险低）→ ② 息屏时 IMU 采样 50Hz 降到 10~25Hz → ③ 校时成功后自动断 WiFi → ④ `CONFIG_PM_ENABLE` + DFS → ⑤ light sleep（收益最大、风险最高）。
**建议先只做 ①②③**，做完实机对比续航再决定要不要 ④⑤。

### 5.14 【P2】另外三处"小浪费/隐性风险"

| 位置 | 现象 | 建议 |
|---|---|---|
| `watch_game.c:432-450` | 游戏帧定时器在 READY/PAUSE/OVER 时直接 return，但周期仍是 33ms → 暂停面板上每秒空转 30 次 | 非 `GS_PLAY` 时把周期改成 200~500ms（照 `watch_clock.c` 的做法） |
| `watch_pages.c:420` | 拖动时**每个 PRESSING 事件**都走 `lv_obj_scroll_by_bounded()`，它内部第一步就 `lv_obj_update_layout()` → 整棵页面子树重算布局 | 改成"按下借一次 SCROLLABLE 标记、抬手还一次"，PRESSING 里直接 `lv_obj_scroll_by()` |
| `watch_modal.c` / `watch_game.c` / `watch_page_wifi.c` 三处 | 在事件回调里**同步删除祖先对象**（LVGL 只在"被删对象正是事件 target"时才标记，删祖先不命中） | 统一改 `lv_obj_del_async()`（`watch_keyboard.c` 里已经有正确写法的注释） |

### 5.15 【P3】注释与实现不符（会误导后面每一次改动）

| 位置 | 问题 |
|---|---|
| `watch_clock.c:125-141` | 注释说"开机设成已触发状态 → 刚开机不会立刻吵起来"、"`alarm_arm()` 让闹钟等下一分钟"，实现却是设 `-1`（**允许立刻触发**）。→ 把闹钟设成当前这一分钟会**立刻响**。**建议改实现**（符合注释意图） |
| `watch_gesture.c:267-269` | 注释称"`lv_obj_add_event_cb()` 会自动加 CLICKABLE"——LVGL 8.4 没有这个行为，实际是 `watch_widgets.c:59` 显式加的 |
| `watch_pages.c:269-274` | 先 `clear_flag(EVENT_BUBBLE)`，紧接着 `mark_subtree()` 又加回来 → 前者是死代码，注释与实现相反 |
| `watch_pages.c:369` | 注释提到 `s_page_scrollable`，该变量不存在（真名 `s_page_scroll_limit`） |
| `data_hub.c:210`/`:403` | 注释说"P1 还没接 IMU""未接入"——IMU 从 v0.3.0 就接上了 |
| `bsp.c:125-130` | 6 个 `esp_lcd_panel_*` 返回值**全部未检查**（初始化失败会静默白屏）→ 至少加 `ESP_RETURN_ON_ERROR` |
| `watch_clock.c:467-495` | `fmt_stopwatch/fmt_mmss` 在缓冲不足时直接 return、**不写 `'\0'`** → 调用方可能打印未初始化内容 |
| `build.ps1:30` | `$ErrorActionPreference="Continue"` 且各分支不检查退出码 → **编译失败也可能以 0 退出**（自动化会误判成功） |

### 5.16 【P2】自检没有覆盖到的三类场景（所以才漏掉了 5.9）

模拟器自检已经覆盖轮播/时钟/子页/游戏/滚动/误触，但**没有**：
1. **"任意模态页 开→关 之后，对象数与定时器数不变"**（现在只对"游戏"做了这个前后对比）→ 加上它，5.9 会被自动抓住；
2. **WiFi 页"扫描中返回"**用例（`w_modal_close()` → 推进 5 秒 → 断言不崩 + 定时器归零）；
3. **息屏→触摸唤醒**（模拟器注入的屏幕钩子恒为"亮"，`power.c` 的屏蔽期永远测不到）。

另外两处流水线漏洞：`tools/check_all_literals.py` 的扫描目录**硬编码三个名字**（新组件里的中文会被漏掉）；`gen_font.ps1` 不带 `-Force` 会静默跳过 → 建议加"字库文件比 `watch_strings.h` 旧就报错"。

---

## 六、建议的执行顺序

### A 批：先止血 + 零成本收益（预计半天~一天）
1. **修 P0**：WiFi 页轮询定时器生命周期（5.9）—— 目前唯一确认"会写已释放内存"的缺陷
2. 编译改 `-O2`（5.3）—— 立竿见影
3. 关掉 FPS 浮层（5.2）
4. 修"息屏唤醒穿透"（5.1）
5. 加 coredump + 任务看门狗 panic（5.4）—— 之后你报的崩溃我才能查到现场
6. 时钟服务跨任务竞态（5.10）—— 闹钟"该响不响 / 响不停"的根因
7. `data_hub` 回调改传快照 + 采样任务栈提到 4096（5.11）
8. `settings` 写 NVS 纳入锁（5.12）
9. 启动/内存配置包（5.5）+ "先刷黑 → 背光淡入"（4.5）
10. 行高 52→44，去掉功能列表滚动（5.6，需你点头）
11. 命令行补 `taskmem` / `stats` / `bt`（第 1 组，顺便量出各任务栈的真实余量）
12. 三处 `lv_obj_del` → `lv_obj_del_async`（5.14）

### B 批：省电第一档 + 补功能
13. 息屏真正关屏：`esp_lcd_panel_disp_off` + 放慢刷新 + IMU 降频 + 校时后自动断 WiFi（5.13 的 ①②③）
14. 计算器 → 日历 → 2048（三个都便宜，2048 复用现有游戏框架）
15. 通用 toast 提示条（后面所有功能都用得上）
16. 天气（HTTP + cJSON + 图标；需要你定经纬度怎么设）
17. WiFi 多槽位（3~5 组）
18. 页面切换动画
19. 自检补三类场景（5.16）—— 把 5.9 这类问题变成"自检能抓"

### C 批：要你决策的大件
20. 数据通道（WiFi 上传页 / 串口上位机 / USB MSC）→ 决定壁纸、相册、小说能不能做
21. 功耗再往上走（DFS / 轻睡 / 深睡）
22. SPI 超频实验（40 → 60/80MHz）
23. `wait_cb` 改造（4.1）或自写 LVGL 移植
24. 轮播"只重建一页"改造 + 拆 `wifi_page_build()`（结构债，动最敏感模块，建议单独一个版本）

---

## 七、需要你拍板的问题

1. **A 批是否现在就做？** 其中第 5 项（coredump）需要**整片擦除 + 重新烧录**，请确认方便。
2. **5.9（WiFi 页 P0）**：我建议无论如何先修 —— 复现步骤：进 WiFi 页 → 点扫描 → 扫描还没结束时按返回 → 之后界面可能花屏/乱跳。
3. **5.1（息屏穿透）**：请实机复现确认（息屏后点"功能列表"某一行，看是否直接进子页面）。
4. **5.2（FPS 浮层）**：请看一眼实机**右下角**，有没有 `NN FPS / NN% CPU` 的小黑框。
5. **功能列表行高**：52 → 44px（5 行一屏放下、恢复"上滑控制中心"）你接受吗？还是保留 52px 继续滚动？
6. **数据通道选哪个**（WiFi 上传 / 串口上位机 / USB MSC）？这决定壁纸和相册要不要做。
7. **省电**做到哪一档：先做 5.13 的 ①②③（纯软件、低风险），还是直接评估 light sleep？
8. **B 批功能优先级**：计算器 / 日历 / 2048 / 天气 / toast，你想先看到哪个？

---

## 八、诚实声明：没验证的、可能错的

| # | 事项 | 状态 |
|---|---|---|
| 1 | FPS 浮层是否**真的**显示在实机屏上 | 编译期配置链已确证；**"肉眼可见"未验证**（模拟器截图看不到≠实机没有）。请实机确认 |
| 2 | 息屏穿透是否**真的**能在实机上复现 | 源码级推导（缺 `lv_indev_enable(false)` + LVGL 检查顺序）；**两条独立分析路径得出同一结论**，但未实机复现 |
| 2b | 5.9 WiFi 页 P0 | 已逐行核实（`lv_timer_del` 只在自己内部、无 `LV_EVENT_DELETE` 钩子）；**未实机复现崩溃** |
| 2c | 5.10 时钟竞态 | 已核实调用位置与任务归属；属"时序相关"，**无法静态证明一定发生** |
| 2d | 5.11/5.12/5.14 的其余项 | 来自逐行审计，**未实机验证**；改动前我会先复看代码 |
| 3 | 我们板子有没有**充电检测脚** | `board.h` 未登记；需查原理图 PDF（`相关资料/示例工程/esp32-s3-touch-lcd-1.69/hardware/schematics/`） |
| 4 | SPI 能不能跑 60/80MHz | 微雪官方示例用 40MHz；对方用 80MHz（不同 PCB）。**必须实机试** |
| 5 | `-O2` 会不会引出问题 | ESP-IDF 官方支持的选项，风险低，但需实机跑一遍 |
| 6 | `wait_cb` 改造后会不会撕裂/卡死 | 需实机验证；对方那套（含 `trans_queue_depth=1`）与我们（`=10`）不同 |
| 7 | 深睡下触摸 INT / RTC INT 能不能唤醒 | 未验证；对方工程没有可参考实现 |
| 8 | 天气 API 用明文 http 是否长期可用 | 对方代码用 `http://api.open-meteo.com`；**是否仍支持 http 需联网确认** |
| 9 | `idf.py size` 报的 `IRAM 100%` 含义 | **未搞清**，不作为结论 |
| 10 | 本轮**没有烧录、没有跑模拟器**，只做了静态代码分析 + `idf.py size` | — |

---

## 九、本轮对标的方法与出处

- 参考工程侧：3 个只读分析任务分别通读了它的**应用层 7 个文件**（≈4,909 行）、**媒体/框架 12 个文件**（≈8,324 行）、**系统/驱动/构建层**（lcd/touch/sd/board/battery/boot_anim/rtc/watchdog/console/lv_conf/sdkconfig/CMakeLists），合计覆盖它约 1.3 万行源码的绝大部分。
- 我方侧：直接读了 `board.h`、`bsp.c`、`power.c`、`main.c`、`app.c`、`watch_page_functions.c`、`watch_strings.h`、`wifi_service.c`、`app_console.c`、`battery.c`、`watch_page_wifi.c`、`watch_pages.c`、`sdkconfig`、`sdkconfig.defaults`、`partitions.csv`、`ui_port_target.c`，并核对了 `lvgl` / `esp_lvgl_port` 的**源码实现**（`lv_refr.c`、`lv_indev.c`、`esp_lvgl_port_disp.c`、`lv_conf_internal.h`）。
- 另做了一轮**逐行代码审计**（覆盖 `watch_pages.c` 全文、`watch_clock.c`、`power.c`、`bsp.c`、`settings.c`、`data_hub.c`、`motion.c`、`watch_page_wifi.c`、`watch_game.c`、`watch_widgets.c` 等，含对 LVGL 8.4 源码的交叉验证），第 5.9~5.16 节即其结论。
- 实测命令：`idf.py size` / `idf.py size-components`（只读，未烧录）。
- 过程中纠正的两个前提：① 该工程硬件与我们**不同**；② 我们**已经有** WiFi 连接页 + SNTP 校时 + 串口命令行（上一轮文档里对"联网那半基本为空"的描述需要限定为"**与手机/云的那半**"）。

---

*本文基于对 `Watch-Player_IDF` 与 `firmware/smartwatch` 的静态分析撰写，未做烧录验证。*
