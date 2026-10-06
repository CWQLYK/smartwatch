# event_bus — lightweight publish/subscribe event bus
idf_component_register(
    SRCS "event_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)
