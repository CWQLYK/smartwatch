#pragma once
/* Host stub for the board component. */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#define BOARD_I2C_PORT      0
#define BOARD_I2C_FREQ_HZ   400000
#define BOARD_IMU_I2C_ADDR  0x6B
static inline esp_err_t board_init(void) { return 0; }
static inline i2c_master_bus_handle_t board_i2c_bus(void) { return (void *)1; }
static inline esp_err_t board_i2c_add_device(uint8_t a, uint32_t f, i2c_master_dev_handle_t *o)
{ (void)a; (void)f; *o = (void *)1; return 0; }
static inline esp_err_t board_i2c_read_reg(i2c_master_dev_handle_t d, uint8_t r, uint8_t *b, size_t n)
{ (void)d; (void)r; (void)b; (void)n; return -1; }
static inline esp_err_t board_i2c_write_reg(i2c_master_dev_handle_t d, uint8_t r, const uint8_t *b, size_t n)
{ (void)d; (void)r; (void)b; (void)n; return -1; }
static inline esp_err_t board_i2c_probe(uint8_t a) { (void)a; return -1; }
