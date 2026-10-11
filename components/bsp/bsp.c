/**
 * @file bsp.c
 * @brief 板级支持包实现 —— 背光 PWM / LCD / 触摸 / LVGL 移植
 *
 * 重构点（v0.2）：
 *   1. I2C 总线不再由 BSP 安装，改成调 board_init() 借共享总线（board 组件持有）
 *   2. 触摸设备改用 esp_lcd_panel_io_i2c 的新 API 挂到该总线上
 *   3. 补上手势灵敏度设置（必须在注册输入设备之前）
 *
 * 屏幕初始化的三个关键调用（少一个就花屏/偏色/偏移，厂家示例同样如此）：
 *   esp_lcd_panel_mirror(panel, true, true);
 *   esp_lcd_panel_set_gap(panel, 0, 20);
 *   esp_lcd_panel_invert_color(panel, true);
 */
#include "bsp.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lvgl_port.h"
/* 只为了 w_screen_notify_activity()：把触摸事件上报给电源模块做息屏计时 */
#include "watch_platform.h"

static const char *TAG = "BSP";

/* 界面任务心跳（见 bsp.h） */
volatile uint32_t g_bsp_lvgl_beat = 0;

/* ==================== 内部状态 ==================== */
static esp_lcd_panel_io_handle_t s_lcd_io      = NULL;
static esp_lcd_panel_handle_t    s_lcd_panel   = NULL;
static esp_lcd_touch_handle_t    s_touch       = NULL;
static lv_disp_t                *s_disp        = NULL;
static lv_indev_t               *s_indev       = NULL;
static int                       s_bl_percent  = 100;

/* ==================== 背光（LEDC PWM）====================
 * ⚠️ 占 LEDC TIMER_0 / CHANNEL_0；蜂鸣器用 TIMER_1 / CHANNEL_1，两者别撞。 */
#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES         LEDC_TIMER_10_BIT   /* 0-1023 */
#define BL_DUTY_MAX         1023

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t tcfg = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz         = 5000,
        /*
         * ★ 第二档 DFS：必须用【晶振】当时钟源。
         *   LEDC_AUTO_CLK 会跟着 APB 频率走，而 DFS 一降频（240→80MHz）背光亮度立刻漂；
         *   晶振 40MHz 与 DFS 无关，亮度恒定。
         *   ⚠️ LEDC 驱动自身**不加 PM 锁**（已核 IDF 源码），所以这一改是必须的，不是可选优化。
         */
        .clk_cfg         = LEDC_USE_XTAL_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc timer");

    const ledc_channel_config_t ccfg = {
        .gpio_num   = BSP_LCD_GPIO_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = BL_DUTY_MAX,
        .hpoint     = 0,
    };
    return ledc_channel_config(&ccfg);
}

esp_err_t bsp_backlight_set(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_bl_percent = percent;

    const uint32_t duty = (uint32_t)((BL_DUTY_MAX * percent) / 100);
    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty),
                        TAG, "set duty");
    return ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

int bsp_backlight_get(void)
{
    return s_bl_percent;
}

/* ==================== LCD ==================== */
static esp_err_t lcd_init(void)
{
    ESP_LOGI(TAG, "初始化 LCD (ST7789V2 %dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);

    const spi_bus_config_t buscfg = {
        .sclk_io_num     = BSP_LCD_GPIO_SCLK,
        .mosi_io_num     = BSP_LCD_GPIO_MOSI,
        .miso_io_num     = GPIO_NUM_NC,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .max_transfer_sz = BSP_LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI 总线初始化失败");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = BSP_LCD_GPIO_DC,
        .cs_gpio_num       = BSP_LCD_GPIO_CS,
        .pclk_hz           = 40 * 1000 * 1000,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                                 &io_config, &s_lcd_io),
                        TAG, "panel IO 创建失败");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_GPIO_RST,
        .color_space    = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_lcd_io, &panel_config, &s_lcd_panel),
                        TAG, "ST7789 驱动创建失败");

    esp_lcd_panel_reset(s_lcd_panel);
    esp_lcd_panel_init(s_lcd_panel);
    esp_lcd_panel_mirror(s_lcd_panel, true, true);       /* 屏幕是镜像安装的 */
    esp_lcd_panel_set_gap(s_lcd_panel, 0, 20);           /* 240x320 控制器，可见区 280 */
    esp_lcd_panel_invert_color(s_lcd_panel, true);       /* 不加颜色是反的 */
    esp_lcd_panel_disp_on_off(s_lcd_panel, true);

    return ESP_OK;
}

/**
 * @brief 面板显示开关（v0.7.1 省电）—— 说明见 bsp.h
 *
 * @note 刻意**不加 delay**：它要能被 `power.c` 的 esp_timer 回调安全调用。
 *       面板睡眠（SLPIN/SLPOUT）是更深的路线，但 IDF 的 ST7789 驱动内部
 *       在 `esp_lcd_panel_disp_sleep()` 里无条件 `vTaskDelay(100)`，本版不用。
 */
esp_err_t bsp_lcd_display(bool on)
{
    ESP_RETURN_ON_FALSE(s_lcd_panel, ESP_ERR_INVALID_STATE, TAG, "LCD 未初始化");
    return esp_lcd_panel_disp_on_off(s_lcd_panel, on);
}

/**
 * @brief 面板睡眠开关（SLPIN/SLPOUT）—— v0.7.2「1.5 档」；细节见 bsp.h 的说明
 *
 * @note ⚠️ 会阻塞约 100ms（IDF 驱动内部无条件 delay，SLPIN/SLPOUT 都一样）
 *       ⇒ 只能从任务里调（本工程由 `power.c` 的 `pwr_sm` 任务调用）。
 */
esp_err_t bsp_lcd_sleep(bool sleep)
{
    ESP_RETURN_ON_FALSE(s_lcd_panel, ESP_ERR_INVALID_STATE, TAG, "LCD 未初始化");
    return esp_lcd_panel_disp_sleep(s_lcd_panel, sleep);   /* 内部 vTaskDelay(100) */
}

/* ==================== 触摸 ==================== */
/**
 * @brief 触摸读取回调（LVGL 每次采样都会调）
 *
 * ★ 这里同时承担"息屏/亮屏"的一部分职责：
 *   每次检测到按下就上报一次"用户正在操作"。
 *
 * 为什么必须在这里上报，而不是让电源模块自己轮询触摸中断脚：
 *   - LVGL 每 `LV_INDEV_DEF_READ_PERIOD`(**10ms**，见 sdkconfig) 调一次本函数，**一次都不漏**
 *     （本注释曾写 5ms，与 sdkconfig 不符，v0.7.1 改正）
 *   - 而电源模块轮询是 50ms 一次，快速点击很容易被漏掉；
 *     一旦漏掉，息屏计时器就以为没人操作，用户正在点着屏幕时突然息屏
 *     （实测用户遇到的就是这个）
 *
 * 屏幕灭着时 `w_screen_notify_activity()` 会顺手**只亮屏**，
 * 所以唤醒路径也统一收在这里，power 里的轮询只剩兜底作用。
 */
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    uint16_t x = 0, y = 0;
    uint8_t  cnt = 0;

    /* ★ 界面任务心跳：本回调每 10ms 被 LVGL 任务调一次（见 bsp.h 的说明）。
     *   卡死哨兵靠它判断"界面任务是不是停住了"——读一个变量，不碰内核锁。 */
    g_bsp_lvgl_beat++;

    /*
     * ★ v0.7.1 省电门控：息屏 + INT 为高（没人碰）→ 直接报"松开"，**不做 I2C 事务**。
     *
     * 为什么需要：LVGL 每 10ms 调一次本函数，而 `esp_lcd_touch_read_data()` 是一次
     * 真实的 I2C 事务 —— 息屏时也照做 100 次/秒，纯浪费（息屏时本来就没人看屏幕）。
     *
     * 为什么安全：
     *   1. CST816T 是**独立器件**（挂在 I2C 0x15），不受 LCD 面板 DISPOFF 影响；
     *   2. 它的 INT 引脚（GPIO14，低有效）在手指按下时拉低 —— 本项目**已经在依赖
     *      它**（power.c 的 touch_pressed() 就是读这个脚）；
     *   3. 读一个 GPIO 是寄存器级操作，比一次 I2C 事务便宜几个数量级；
     *   4. **亮屏时这道门不生效** → 触摸手感逐字节不变；
     *   5. 有人碰时（INT 低）照旧走完整流程 → `w_screen_notify_activity()` → 亮屏
     *      + 吞掉那一下。**唤醒必须仍然从这里进**（见 power.c 的铁律 #3），
     *      所以这道门只拦"没人碰"，绝不会把唤醒挡掉。
     *
     * 副作用（实机确认过即可）：若某块板的 CST816T 空闲时 INT 不是稳定高电平，
     * 只会退化成"多做几次 I2C 读"（省电收益变小），**不会误触** ——
     * 因为坐标要真的按下才有效。
     */
    if (!w_screen_is_on() && gpio_get_level(BOARD_TOUCH_GPIO_INT) != 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_read_data(tp);
    const bool pressed = esp_lcd_touch_get_coordinates(tp, &x, &y, NULL, &cnt, 1);

    if (pressed && cnt > 0) {
        data->point.x = x;
        data->point.y = y;

        /*
         * 上报用户活动：灭屏时它会把屏幕点亮（见 watch_platform.h）。
         * ★ 必须在问"要不要吞掉"**之前**调 —— 它会顺手置上屏蔽标志。
         */
        w_screen_notify_activity();

        /*
         * ★ v0.6.1：如果这一次触摸只是"把屏幕从灭叫醒"，就**不报按下**。
         *
         *   原来这里无条件报 PRESSED，而 power.c 注释里说的
         *   "用 lv_indev_enable(false) 吃掉第一下"实际没生效
         *   （没有任何地方调过 false，而且 LVGL 是在读回调之前看 disabled 的）
         *   → 用户息屏时点一下，会既亮屏、又误触发手指下面的控件
         *   （实测场景：在功能列表上直接进了子页面）。
         *
         *   坐标照给：LVGL 的 pointer 处理会无条件读 data->point
         *   （lv_indev.c 的 indev_pointer_proc），给真实值最安全。
         */
        if (w_screen_should_swallow_touch()) {
            data->state = LV_INDEV_STATE_RELEASED;
        } else {
            data->state = LV_INDEV_STATE_PRESSED;
        }
    } else {
        /* 抬手：解除"吞掉"状态，下一次触摸正常生效 */
        w_touch_settled();
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static esp_err_t touch_init(void)
{
    ESP_LOGI(TAG, "初始化触摸 (CST816T @0x%02X，共享 I2C)", BSP_TOUCH_I2C_ADDR);

    /* 总线由 board 组件持有（bsp_init 里已经保证初始化过），这里只挂设备 */
    i2c_master_bus_handle_t bus = board_i2c_bus();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG,
                        "共享 I2C 总线不可用，board_init() 没成功？");

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config =
        ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    /*
     * ⚠️ 厂家示例里这个宏没有初始化 scl_speed_hz（结构体该字段是 0），
     *    0 会让 I2C 驱动按默认时钟跑。显式给 400kHz，
     *    让触摸和 RTC/IMU 共享总线时每次事务都短一点。
     */
    tp_io_config.scl_speed_hz = BOARD_I2C_FREQ_HZ;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &tp_io_config, &tp_io),
                        TAG, "触摸 IO 创建失败");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BSP_LCD_H_RES,
        .y_max        = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_TOUCH_GPIO_RST,
        .int_gpio_num = BSP_TOUCH_GPIO_INT,
        .levels       = { .reset = 0, .interrupt = 0 },
        .flags        = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    return esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_touch);
}

/* ==================== LVGL ==================== */
static esp_err_t lvgl_init(void)
{
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority      = 4,
        .task_stack         = 6 * 1024,
        .task_affinity      = -1,
        .task_max_sleep_ms  = 500,
        .timer_period_ms    = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port 初始化失败");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = s_lcd_io,
        .panel_handle  = s_lcd_panel,
        .buffer_size   = BSP_LCD_H_RES * 80,
        .double_buffer = 1,
        .hres          = BSP_LCD_H_RES,
        .vres          = BSP_LCD_V_RES,
        .monochrome    = false,
        .rotation      = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags         = { .buff_dma = true },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "添加显示失败");

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.disp    = s_disp;
    indev_drv.read_cb = touch_read_cb;
    indev_drv.user_data = s_touch;

    /*
     * ⚠️ 手势参数必须在这里设 —— lv_indev_drv_register() 会把它们拷进 indev，
     *    注册之后再改 driver 结构体就不生效了（见 bsp.h 的说明）。
     */
    indev_drv.gesture_min_velocity = BSP_GESTURE_MIN_VELOCITY;
    indev_drv.gesture_limit        = BSP_GESTURE_LIMIT;

    s_indev = lv_indev_drv_register(&indev_drv);
    ESP_RETURN_ON_FALSE(s_indev, ESP_FAIL, TAG, "注册触摸输入设备失败");

    return ESP_OK;
}

/* ==================== 对外接口 ==================== */
esp_err_t bsp_init(void)
{
    /* 先把共享 I2C 总线准备好（触摸/IMU/RTC 都靠它），幂等 */
    ESP_RETURN_ON_ERROR(board_init(), TAG, "板级资源初始化失败");

    ESP_ERROR_CHECK(backlight_init());
    ESP_ERROR_CHECK(bsp_backlight_set(100));   /* 先点亮，避免初始化期间黑屏 */

    ESP_RETURN_ON_ERROR(lcd_init(),   TAG, "LCD 初始化失败");
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "触摸初始化失败");
    ESP_RETURN_ON_ERROR(lvgl_init(),  TAG, "LVGL 初始化失败");

    ESP_LOGI(TAG, "板级初始化完成（手势阈值 limit=%d velocity=%d）",
             BSP_GESTURE_LIMIT, BSP_GESTURE_MIN_VELOCITY);
    return ESP_OK;
}

lv_disp_t *bsp_display_get(void) { return s_disp; }
lv_indev_t *bsp_touch_get(void)  { return s_indev; }

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}
