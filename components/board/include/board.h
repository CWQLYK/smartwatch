/**
 * @file board.h
 * @brief 板级资源表 —— 所有引脚、I2C 地址、共享总线都收在这里
 *
 * 设计意图（对标 ESP-Brookesia 的「HAL 用 YAML 描述板子」思想）：
 *   全项目**只有这一个文件**知道 GPIO 号和外设地址。
 *   换板子 = 只改这个文件 + bsp.c，驱动/服务/UI 一行不动。
 *
 * 目录位置：components/board/
 *   上层谁需要引脚，就 include "board.h"，不要再硬编码 GPIO 号。
 */
#pragma once

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 一、引脚分配（来自原理图 V2.1）
 * ================================================================== */

/* ---- LCD：ST7789V2，4 线 SPI ---- */
#define BOARD_LCD_GPIO_SCLK     GPIO_NUM_6
#define BOARD_LCD_GPIO_MOSI     GPIO_NUM_7
#define BOARD_LCD_GPIO_RST      GPIO_NUM_8
#define BOARD_LCD_GPIO_DC       GPIO_NUM_4
#define BOARD_LCD_GPIO_CS       GPIO_NUM_5
#define BOARD_LCD_GPIO_BL       GPIO_NUM_15   /* 背光，LEDC PWM 调光 */

/* ---- 共享 I2C 总线：触摸 + IMU + RTC 都挂在这两条线上 ---- */
#define BOARD_I2C_PORT          0             /* I2C_NUM_0 */
#define BOARD_I2C_GPIO_SCL      GPIO_NUM_10
#define BOARD_I2C_GPIO_SDA      GPIO_NUM_11
#define BOARD_I2C_FREQ_HZ       400000        /* 400 kHz */

/* ---- 触摸：CST816T ---- */
#define BOARD_TOUCH_I2C_ADDR    0x15
#define BOARD_TOUCH_GPIO_RST    GPIO_NUM_13
#define BOARD_TOUCH_GPIO_INT    GPIO_NUM_14

/* ---- IMU：QMI8658C（P1 暂未使用，地址先登记，避免以后撞车）---- */
#define BOARD_IMU_I2C_ADDR      0x6B
#define BOARD_IMU_GPIO_INT2     GPIO_NUM_38

/* ---- RTC：PCF85063ATL ---- */
#define BOARD_RTC_I2C_ADDR      0x51
#define BOARD_RTC_GPIO_INT      GPIO_NUM_39

/* ---- 电池电压采样：B+ 经 R3=200K / R7=100K 分压到 GPIO1 ---- */
#define BOARD_BAT_GPIO_ADC      GPIO_NUM_1
#define BOARD_BAT_ADC_UNIT      ADC_UNIT_1
#define BOARD_BAT_ADC_CHANNEL   ADC_CHANNEL_0
#define BOARD_BAT_DIVIDER_NUM   3             /* (R3+R7)/R7 = 300/100 = 3 */
#define BOARD_BAT_DIVIDER_DEN   1

/* ---- 蜂鸣器：GPIO42，PWM 单音（不是喇叭）---- */
#define BOARD_BUZZER_GPIO       GPIO_NUM_42

/* ---- 电源自锁：SYS_EN / SYS_OUT（原理图 V2.1「KEYS」区，已逐线核对）----
 *
 * ★★ 电池供电的**命门**：不把 SYS_EN 拉高，松手就断电。
 *     插 USB 时 VBUS 直接供电，所以只用 USB 开发**永远发现不了**这个问题
 *     （本项目 v0.7.1 之前就是这样，直到插上电池才暴露）。
 *
 * 电路（原理图 V2.1，KEYS 区）：
 *
 *     B+ ──[R21 10K]──┬── Q5(AO3401, P-MOS) 栅极     ← R21 默认把栅极拉到 B+ ⇒ Q5 默认**关断**
 *                     ├── D3(RB521S-30) ── Key2(PWR 键) ── GND
 *                     └── T2(SS8050) 集电极
 *     SYS_EN ──[R22 1K]── T2 基极（并有 R24 10K 下拉到 GND）
 *     T2 发射极 ── GND
 *     B+ ── Q5 源极 ;  Q5 漏极 ── VBAT（系统电源轨）
 *
 *     ⇒ SYS_EN = 高 → T2 导通 → Q5 栅极被拉到 GND → Q5 导通 → 电池接上 VBAT（系统有电）
 *     ⇒ SYS_EN = 低/悬空 → Q5 关断 → 电池供电被切断（此时只有按住 PWR 键才供电）
 *
 *     VCC3V3 ──[R20 10K]──┬── SYS_OUT → GPIO40      ← PWR 键状态：按下时经 D2 拉到低
 *                         └── D2(RB521S-30) ── Key2 ── GND
 *
 * ⚠️ 因此"开机"这个动作在硬件上就是**按住 PWR 键**（经 D3 把 Q5 栅极拉低）；
 *    固件起来后必须立刻拉高 SYS_EN 把电源"自锁"住，这就是 boards 上常说的"长按开机"。
 * ⚠️ 电池供电时**复位/panic 会掉电**（复位期间引脚回到高阻 → Q5 关断 → 断电，
 *    需要再按一次 PWR）。这是该自锁电路的固有特性，不是软件能绕过的。
 * ⚠️ GPIO40/41 在别处**不要再使用**。
 */
#define BOARD_SYS_EN_GPIO       GPIO_NUM_41   /* 输出：拉高 = 保持系统供电（电池供电必备） */
#define BOARD_SYS_OUT_GPIO      GPIO_NUM_40   /* 输入：PWR 键状态，低 = 按下（本版尚未使用） */

/* ---- 空闲扩展口（做扩展要省着用）---- */
#define BOARD_GPIO_FREE_1       GPIO_NUM_2
#define BOARD_GPIO_FREE_2       GPIO_NUM_3
#define BOARD_GPIO_FREE_3       GPIO_NUM_17
#define BOARD_GPIO_FREE_4       GPIO_NUM_18

/* ==================================================================
 * 二、板级资源初始化 / 共享总线
 * ================================================================== */

/**
 * @brief 初始化板级公共资源（电源自锁 + 共享 I2C 总线）
 *
 * @note **幂等**，重复调用直接返回 ESP_OK。
 *       bsp_init() 内部会先调它，独立驱动组件也可以自己先调一次保证安全。
 */
esp_err_t board_init(void);

/**
 * @brief 锁存系统电源（电池供电必备）—— 电路见上面 SYS_EN 那段注释
 *
 * 把 `SYS_EN`(GPIO41) 配成输出并**拉高**，让电源自锁电路维持供电。
 * 不调它：插 USB 时一切正常，**拔掉 USB 立刻断电**（只有按住 PWR 键才活着）。
 *
 * @return ESP_OK 成功
 *
 * @note **幂等**；`board_init()` 会**第一个**调它，正常路径不需要手动调。
 * @note 必须**尽早上电执行**：从复位到本函数之间（bootloader 阶段，约 0.2~0.4 秒）
 *       电池供电完全靠用户按住 PWR 键维持 —— 这就是这类板子"要长按开机"的原因。
 * @note 实现上刻意**先置输出电平、再使能输出**（靠 `gpio_config()` 不写输出寄存器这一点），
 *       否则切换瞬间会输出低电平 → 电池供电当场被切断。
 */
esp_err_t board_power_hold(void);

/**
 * @brief 回读 `SYS_EN` 引脚的**真实电平**（排查"电池供电到底锁上没有"用）
 *
 * @return 1 = 高（已锁存）/ 0 = 低（电池供电会掉电）/ -1 = 还没调用 `board_power_hold()`
 *
 * @note 只写寄存器不等于引脚上真的是高电平，所以 `board_power_hold()` 把该脚配成
 *       `GPIO_MODE_INPUT_OUTPUT` 并回读验证；本函数就是那个回读值（插 USB 时也有效）。
 */
int board_power_hold_level(void);

/**
 * @brief 取共享 I2C 总线句柄
 * @return 总线句柄；未初始化返回 NULL
 */
i2c_master_bus_handle_t board_i2c_bus(void);

/**
 * @brief 在共享总线上挂一个 I2C 从设备
 *
 * @param addr_7bit  7 位从机地址（如 0x51）
 * @param freq_hz    该设备的时钟频率，一般传 BOARD_I2C_FREQ_HZ
 * @param[out] out   返回的设备句柄
 *
 * @note 同一个地址重复调用会返回新句柄（底层会告警），请各驱动自己只调一次。
 */
esp_err_t board_i2c_add_device(uint8_t addr_7bit, uint32_t freq_hz,
                               i2c_master_dev_handle_t *out);

/**
 * @brief 读寄存器：先写 1 字节寄存器地址，再读 len 字节
 */
esp_err_t board_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                             uint8_t *buf, size_t len);

/**
 * @brief 写寄存器：写 1 字节寄存器地址 + len 字节数据
 */
esp_err_t board_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                              const uint8_t *buf, size_t len);

/**
 * @brief 探测某个 7 位地址上是否有设备应答（用于启动自检）
 * @return ESP_OK 有应答；ESP_ERR_NOT_FOUND 没有
 */
esp_err_t board_i2c_probe(uint8_t addr_7bit);

#ifdef __cplusplus
}
#endif
