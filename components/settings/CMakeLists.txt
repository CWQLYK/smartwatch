# settings — NVS-backed persistent settings service
idf_component_register(
    SRCS "settings.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_driver_gpio
    PRIV_REQUIRES event_bus
)
