# rtc_pcf85063 — PCF85063ATL real-time clock driver
# NOTE: component/header deliberately NOT named plain "rtc" — ESP-IDF's
#       esp_hw_support ships a deprecated soc/rtc.h, and `#include "rtc.h"`
#       resolves to that one first. Suffixing the chip name removes the ambiguity.
idf_component_register(
    SRCS "rtc_pcf85063.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_driver_i2c
)
