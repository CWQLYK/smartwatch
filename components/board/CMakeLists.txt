# board — board-level resource table (pins / shared I2C bus)
# esp_adc is public: board.h exposes the ADC unit/channel macros for the battery driver
idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio esp_adc
)
