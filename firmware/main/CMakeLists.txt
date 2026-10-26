if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    message(FATAL_ERROR "main/secrets.h is missing: copy main/secrets.example.h to main/secrets.h and fill it in")
endif()

idf_component_register(SRCS "app_main.c" "status.c" "eth_dhcps.c" "wifi.c" "tv.c" "api.c" "switch.c" "button.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_eth esp_netif esp_wifi esp_event nvs_flash esp_http_server
                                     json driver mbedtls lwip esp_http_client)
