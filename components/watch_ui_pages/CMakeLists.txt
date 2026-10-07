# watch_ui_pages — concrete pages
#   carousel : time(home) / sport / health / functions
#   modals   : settings / clock / timeset / developer / wifi
# No ESP-IDF dependency: compiles in the PC simulator too.
# NOTE: CMake does NOT support backslash line continuation, keep the list on one line.
idf_component_register(
    SRCS "pages/watch_page_home.c" "pages/watch_page_sport.c" "pages/watch_page_health.c" "pages/watch_page_functions.c" "pages/watch_page_settings.c" "pages/watch_page_clock.c" "pages/watch_page_timeset.c" "pages/watch_page_developer.c" "pages/watch_page_wifi.c" "watch_pages_all.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl ui_port watch_ui
)
