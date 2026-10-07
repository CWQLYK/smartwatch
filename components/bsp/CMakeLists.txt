# bsp — board support package (LCD / touch / backlight / LVGL port)
# board: owns the shared I2C bus that the touch panel hangs on
idf_component_register(
    SRCS "bsp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_gpio esp_driver_spi esp_driver_ledc board watch_ui
    PRIV_REQUIRES esp_lvgl_port esp_lcd_touch esp_lcd_touch_cst816s
)
