file(GLOB APP_SRC "driver/src/*.cpp" "driver/src/*.c" "utils/src/*.c" "utils/src/*.cpp" "ble/*.cpp" "main.cpp")
set(APP_INC "." "driver/inc" "utils/inc" "ble")
idf_component_register(SRCS ${APP_SRC}
                       INCLUDE_DIRS ${APP_INC}
                       REQUIRES esp_wifi esp_event esp_netif nvs_flash driver pthread spi_flash esp_partition bt esp_timer console esp_app_format)
