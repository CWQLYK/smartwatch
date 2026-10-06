# ui_port — platform port layer for UI code (target: ESP-IDF, simulator: PC)
# REQUIRES lvgl is needed because the public header ui_port.h includes lvgl.h
idf_component_register(
    SRCS "ui_port_target.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl bsp
)
