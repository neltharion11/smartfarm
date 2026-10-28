idf_component_register(SRCS "main.c" "sh1106.c" "sht4x.c" "ds18b20.c" "i2c_diag.c" "relay.c"
                              "wifi_mgr.c" "mqtt_app.c" "ble_svc.c" "cmd.c" "rules.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2c esp_driver_gpio
                       PRIV_REQUIRES esp_timer esp_wifi esp_event esp_netif nvs_flash mbedtls bt)
