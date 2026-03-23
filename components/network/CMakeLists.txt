idf_component_register(
    SRCS "src/wifi_service.cpp"
         "src/mqtt_service.cpp"
         "src/ota_task.cpp"
         "src/json_parser.cpp"
    INCLUDE_DIRS "include"
    REQUIRES station_common
    PRIV_REQUIRES esp_wifi esp_netif mqtt esp_https_ota esp_timer
    EMBED_TXTFILES "certs/ca.crt" "certs/client.crt" "certs/client.key"
)
