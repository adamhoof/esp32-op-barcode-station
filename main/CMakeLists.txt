idf_component_register(
    SRCS "src/main.cpp"
         "src/control_mode_store.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_netif esp_event station_common barcode display network
)
