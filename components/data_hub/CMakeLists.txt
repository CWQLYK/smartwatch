# data_hub — single source of truth for live device data (time/battery/steps)
idf_component_register(
    SRCS "data_hub.c"
    INCLUDE_DIRS "include"
    REQUIRES rtc_pcf85063 battery settings event_bus
    PRIV_REQUIRES esp_timer
)
