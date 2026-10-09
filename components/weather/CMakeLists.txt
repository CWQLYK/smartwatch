# weather — Open-Meteo 拉取 + JSON 解析 + NVS 缓存
#
# 为什么单独一个组件：
#   界面层（watch_ui_pages）不许 include ESP-IDF 头文件，否则 PC 模拟器编不过。
#   所以 HTTP / JSON / NVS 全收在这里，界面只经 watch_platform.h 的钩子拿快照。
#
# REQUIRES 里要有 esp_http_client 和 json（IDF 自带 cJSON）。
idf_component_register(
    SRCS "weather_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json nvs_flash esp_timer freertos
)
