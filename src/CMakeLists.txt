idf_component_register(
    SRCS
        "main.c"
        "app_state.c"
        "nvs_storage.c"
        "pulse_counter.c"
        "wifi_manager.c"
        "mqtt_service.c"
        "telegram_bot.c"
        "web_server.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    REQUIRES
        nvs_flash
        esp_event
        esp_wifi
        esp_netif
        esp_http_server
        esp_http_client
        esp-tls
        mbedtls
        mqtt
        json
        driver
        esp_timer
)
