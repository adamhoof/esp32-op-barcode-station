idf_component_register(
    SRCS "src/display_device.cpp"
         "src/display_task.cpp"
    INCLUDE_DIRS "include"
    REQUIRES station_common
    PRIV_REQUIRES driver esp_lcd esp_lvgl_port
)
