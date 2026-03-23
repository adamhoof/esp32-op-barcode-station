idf_component_register(
    SRCS "src/barcode_device.cpp"
         "src/barcode_task.cpp"
    INCLUDE_DIRS "include"
    REQUIRES station_common driver
    PRIV_REQUIRES esp_event
)
