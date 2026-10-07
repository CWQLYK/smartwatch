# watch_ui — UI framework (theme / widgets / pages / gestures / overlay / clock)
# No ESP-IDF dependency on purpose: the same code compiles in the PC simulator.
# fonts: only for the generated Chinese glyph tables (plain LVGL data, no IDF).
#
# ⚠️ 千万别把 settings / nvs_flash 之类加进 REQUIRES：
#    watch_clock.c 需要读写 NVS 时走的是**按名字寻址的设置钩子**
#    （watch_ui_setting_get/set，由 app.c 注入），不是直接 include settings.h。
#    一旦加了依赖，PC 模拟器立刻编不过 —— 而模拟器是改界面效率的关键。
#
# NOTE: CMake does NOT support backslash line continuation, keep the list on one line.
idf_component_register(
    SRCS "watch_style.c" "watch_widgets.c" "watch_pages.c" "watch_gesture.c" "watch_overlay.c" "watch_keyboard.c" "watch_platform.c" "watch_modal.c" "watch_wheel.c" "watch_clock.c" "watch_alert.c" "watch_gfx.c" "watch_game.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl ui_port fonts
)
