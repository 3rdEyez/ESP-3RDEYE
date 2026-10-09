#pragma once
#include "esp_err.h"
#include "esp_partition.h"
enum esp_ota_img_states_t : unsigned {
    ESP_OTA_IMG_NEW = 0, ESP_OTA_IMG_PENDING_VERIFY = 1, ESP_OTA_IMG_VALID = 2,
    ESP_OTA_IMG_INVALID = 3, ESP_OTA_IMG_ABORTED = 4, ESP_OTA_IMG_UNDEFINED = 0xffffffffu,
};
const esp_partition_t* esp_ota_get_running_partition();
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t*);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot();
esp_err_t esp_ota_mark_app_valid_cancel_rollback();
