# battery — battery voltage sampling (GPIO1 / BAT_ADC)
idf_component_register(
    SRCS "battery.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_adc
)
