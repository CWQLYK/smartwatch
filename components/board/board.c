/**
 * @file board.c
 * @brief 板级资源表实现 —— 共享 I2C 总线 + 寄存器读写助手
 *
 * 为什么要有这个组件：
 *   板上触摸(0x15) / IMU(0x6B) / RTC(0x51) **共用一条 I2C 总线**
 *   （SCL=GPIO10, SDA=GPIO11），只能安装一次。
 *   如果每个驱动各装一次，后装的会报 ESP_ERR_INVALID_STATE。
 *   所以把总线"所有权"收到 board 组件里，所有驱动都来借。
 */
#include "board.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "BOARD";

/* ==================== 共享 I2C 总线 ==================== */
static i2c_master_bus_handle_t s_i2c_bus = NULL;

esp_err_t board_init(void)
{
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
    return ESP_OK;
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
