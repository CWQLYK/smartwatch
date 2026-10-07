# watch_ui — UI framework (theme / widgets / pages / gestures / overlay)
# No ESP-IDF dependency on purpose: the same code compiles in the PC simulator.
# fonts: only for the generated Chinese glyph tables (plain LVGL data, no IDF).
idf_component_register(
    SRCS "watch_style.c" "watch_widgets.c" "watch_pages.c" "watch_gesture.c" "watch_overlay.c" "watch_keyboard.c" "watch_platform.c" "watch_modal.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl ui_port fonts
)
