#pragma once
#include <cstdint>
enum esp_partition_type_t { ESP_PARTITION_TYPE_APP, ESP_PARTITION_TYPE_DATA };
enum esp_partition_subtype_t {
    ESP_PARTITION_SUBTYPE_DATA_NVS, ESP_PARTITION_SUBTYPE_DATA_PHY,
    ESP_PARTITION_SUBTYPE_DATA_UNDEFINED, ESP_PARTITION_SUBTYPE_DATA_OTA,
    ESP_PARTITION_SUBTYPE_APP_OTA_0, ESP_PARTITION_SUBTYPE_APP_OTA_1,
};
struct esp_partition_t {
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    std::uint32_t address, size;
    bool encrypted;
};
const esp_partition_t* esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char*);
