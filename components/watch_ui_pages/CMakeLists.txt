# watch_ui_pages — concrete pages (home / settings / health / sport)
# No ESP-IDF dependency: compiles in the PC simulator too.
idf_component_register(
    SRCS "pages/watch_page_home.c" "pages/watch_page_settings.c" "pages/watch_page_health.c" "pages/watch_page_sport.c" "pages/watch_page_developer.c" "pages/watch_page_wifi.c" "watch_pages_all.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl ui_port watch_ui
)
