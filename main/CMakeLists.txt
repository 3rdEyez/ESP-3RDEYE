file(GLOB APP_SRC "driver/src/*.cpp" "driver/src/*.c" "utils/src/*.c" "utils/src/*.cpp" "ble/*.cpp" "ota/*.cpp" "main.cpp")
set(APP_INC "." "driver/inc" "utils/inc" "ble" "ota")
idf_component_register(SRCS ${APP_SRC}
                       INCLUDE_DIRS ${APP_INC}
                       REQUIRES esp_wifi esp_event esp_netif nvs_flash driver pthread spi_flash esp_partition bt esp_timer console esp_app_format app_update esp_http_server mbedtls json bootloader_support)
if(CONFIG_SATORI_WIFI_OTA_PROTOTYPE)
    # Retain the complete infrastructure for a meaningful offline size build.
    # No caller executes this symbol or opens an upgrade window at startup.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,satori_ota_prototype_link_anchor" "-Wl,-u,satori_ota_board_tag")
endif()
