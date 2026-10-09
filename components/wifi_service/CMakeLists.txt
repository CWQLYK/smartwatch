# wifi_service — WiFi STA + SNTP 校时（给"连上就自动对时"用）
#
# 为什么单独一个组件：
#   界面层（watch_ui_pages）不许 include ESP-IDF 头文件，否则 PC 模拟器编不过。
#   所以 WiFi 能力全部收在这里，通过**函数钩子**暴露给界面。
#   界面只认 wifi_service_get_hooks() 返回的那几个函数指针。
idf_component_register(
    SRCS "wifi_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi nvs_flash esp_timer lwip lvgl watch_ui settings
)
