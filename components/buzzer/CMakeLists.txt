# buzzer — passive buzzer on GPIO42 (LEDC PWM tone)
idf_component_register(
    SRCS "buzzer.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_driver_ledc esp_timer
)
