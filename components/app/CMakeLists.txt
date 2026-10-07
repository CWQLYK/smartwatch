# app — assembly layer: data source adapter + live wiring (no UI code here)
# NOTE: CMake does NOT support backslash line continuation, keep lists on one line.
idf_component_register(
    SRCS "app.c" "app_console.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer console bsp board
    PRIV_REQUIRES event_bus settings rtc_pcf85063 battery buzzer data_hub motion imu imu_logger wifi_service power ui_port watch_ui watch_ui_pages
)
