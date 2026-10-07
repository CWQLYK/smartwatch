# power — 屏幕开关 / 自动息屏 / 亮屏调度
#
# 为什么单独一个组件：
#   息屏涉及"谁在阻止息屏"（引用计数）、"什么时候该息屏"（触摸超时）、
#   "谁能让它亮"（触摸 / 以后还有抬腕）。这些状态需要一个统一的地方管，
#   摊在 app.c 里会越滚越乱。
idf_component_register(
    SRCS "power.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp settings esp_timer board lvgl watch_ui
    PRIV_REQUIRES data_hub
)
