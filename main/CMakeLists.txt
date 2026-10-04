idf_component_register(
    SRCS "main.c" "wifi_manager.c" "uvc_source.c" "http_stream.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer esp_http_server nvs_flash usb
)
