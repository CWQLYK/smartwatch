/**
 * @file board.c
 * @brief 板级资源表实现 —— 电源自锁 + 共享 I2C 总线 + 寄存器读写助手
 *
 * 为什么要有这个组件：
 *   板上触摸(0x15) / IMU(0x6B) / RTC(0x51) **共用一条 I2C 总线**
 *   （SCL=GPIO10, SDA=GPIO11），只能安装一次。
 *   如果每个驱动各装一次，后装的会报 ESP_ERR_INVALID_STATE。
 *   所以把总线"所有权"收到 board 组件里，所有驱动都来借。
 *
 * ★ v0.7.1：本组件还负责**电源自锁**（`board_power_hold()`）——
 *   插电池才发现"松手就断电"的那个坑，电路与原理见 board.h 里 SYS_EN 那段注释。
 */
#include "board.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "BOARD";

/* ==================== 电源自锁 ==================== */
static bool s_power_held = false;

esp_err_t board_power_hold(void)
{
    if (s_power_held) {
        return ESP_OK;                      /* 幂等 */
    }

    /*
     * ★★ 顺序**不能**反：先把输出寄存器置 1，再使能输出驱动。
     *
     *   反过来的话（先 gpio_config 成输出、再 set_level(1)），切换输出的那一瞬间
     *   引脚会驱动**低**电平 → T2 关断 → Q5 关断 → **电池供电当场被切断**。
     *   之所以能这么写：`gpio_config()` 只切输入/输出使能与上下拉，
     *   **不碰输出数据寄存器**（已核对 IDF v5.5.4 的 `gpio_config()` 实现），
     *   而 `gpio_set_level()` 对"还没配成输出"的脚也安全（只写电平寄存器）。
     */
    esp_err_t err = gpio_set_level(BOARD_SYS_EN_GPIO, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "预置 SYS_EN 电平失败: %s", esp_err_to_name(err));
        return err;
    }

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_SYS_EN_GPIO,
        /*
         * 用 INPUT_OUTPUT 而不是纯 OUTPUT：**为了能回读引脚真实电平**。
         * 只写寄存器"以为拉高了"和"引脚上真的是高"是两件事 ——
         * 回读能证明真的锁上了（插 USB 时也照样能看，方便排查）。
         */
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,    /* 外部已有 R24 10K 下拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SYS_EN(GPIO%d) 配置为输出失败: %s",
                 BOARD_SYS_EN_GPIO, esp_err_to_name(err));
        return err;
    }

    s_power_held = true;

    const int lv = gpio_get_level(BOARD_SYS_EN_GPIO);
    if (lv == 1) {
        ESP_LOGI(TAG, "电源已锁存：SYS_EN(GPIO%d) 拉高并回读=高 ✓"
                      "（电池供电靠它维持；插 USB 时看不出差别）",
                 BOARD_SYS_EN_GPIO);
    } else {
        /* 拉高了却回读为低 —— 引脚被外部拉死或配置没生效，电池供电必然掉电 */
        ESP_LOGE(TAG, "电源锁存失败：SYS_EN(GPIO%d) 写高但回读=%d！"
                      "电池供电会掉电（插 USB 时不影响）",
                 BOARD_SYS_EN_GPIO, lv);
    }
    return ESP_OK;
}

int board_power_hold_level(void)
{
    if (!s_power_held) {
        return -1;                          /* 还没配置 */
    }
    return gpio_get_level(BOARD_SYS_EN_GPIO);
}

/* ==================== 共享 I2C 总线 ==================== */
static i2c_master_bus_handle_t s_i2c_bus = NULL;

esp_err_t board_init(void)
{
    /*
     * ★ 电源自锁必须是**第一个**动作，而且要在这条路径上无条件执行
     *   （不能放在下面的幂等判断之后，也不能因为 I2C 失败而被跳过）：
     *   电池供电时，从复位到 SYS_EN 拉高之间只有 PWR 键在维持供电。
     */
    esp_err_t hold_err = board_power_hold();

    if (s_i2c_bus) {
        return ESP_OK;                      /* 幂等 */
    }

    const i2c_master_bus_config_t cfg = {
        .i2c_port          = BOARD_I2C_PORT,
        .sda_io_num        = BOARD_I2C_GPIO_SDA,
        .scl_io_num        = BOARD_I2C_GPIO_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c_bus), TAG,
                        "共享 I2C 总线初始化失败 (SCL=%d SDA=%d)",
                        BOARD_I2C_GPIO_SCL, BOARD_I2C_GPIO_SDA);

    ESP_LOGI(TAG, "共享 I2C 总线就绪 (port=%d SCL=GPIO%d SDA=GPIO%d %d kHz)",
             BOARD_I2C_PORT, BOARD_I2C_GPIO_SCL, BOARD_I2C_GPIO_SDA,
             BOARD_I2C_FREQ_HZ / 1000);

    /* 电源没锁上是大问题（电池供电会掉电），但别拖垮整个 I2C 初始化，
     * 所以只把它作为返回值上报（I2C 自己的错误优先级更高）。 */
    return hold_err;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c_bus;
}

esp_err_t board_i2c_add_device(uint8_t addr_7bit, uint32_t freq_hz,
                               i2c_master_dev_handle_t *out)
{
    ESP_RETURN_ON_FALSE(s_i2c_bus, ESP_ERR_INVALID_STATE, TAG,
                        "共享 I2C 总线未初始化，请先调用 board_init()");
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out 为空");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr_7bit,
        .scl_speed_hz    = freq_hz,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, out);
}

/* ==================== 寄存器读写助手 ==================== */
esp_err_t board_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                             uint8_t *buf, size_t len)
{
    ESP_RETURN_ON_FALSE(dev && buf && len, ESP_ERR_INVALID_ARG, TAG, "参数非法");
    /* 标准 I2C 寄存器读：先写寄存器号（不发 STOP），再读 len 字节 */
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, 100);
}

esp_err_t board_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                              const uint8_t *buf, size_t len)
{
    ESP_RETURN_ON_FALSE(dev && buf && len, ESP_ERR_INVALID_ARG, TAG, "参数非法");

    uint8_t tmp[8];
    ESP_RETURN_ON_FALSE(len + 1 <= sizeof(tmp), ESP_ERR_INVALID_SIZE, TAG,
                        "单次最多写 %d 字节", (int)sizeof(tmp) - 1);
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);
    return i2c_master_transmit(dev, tmp, len + 1, 100);
}

esp_err_t board_i2c_probe(uint8_t addr_7bit)
{
    ESP_RETURN_ON_FALSE(s_i2c_bus, ESP_ERR_INVALID_STATE, TAG,
                        "共享 I2C 总线未初始化");
    return i2c_master_probe(s_i2c_bus, addr_7bit, 100);
}
