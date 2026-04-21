idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../../shared/include"
    REQUIRES sm16703sp3 esp_driver_gpio esp_driver_uart esp_driver_spi esp_event esp_netif esp_wifi esp_http_server nvs_flash
)
