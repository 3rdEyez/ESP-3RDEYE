#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
using esp_ota_handle_t = unsigned;
constexpr int ESP_OK = 0;
enum esp_partition_type_t { ESP_PARTITION_TYPE_APP, ESP_PARTITION_TYPE_DATA };
enum esp_partition_subtype_t { ESP_PARTITION_SUBTYPE_APP_OTA_0, ESP_PARTITION_SUBTYPE_APP_OTA_1, ESP_PARTITION_SUBTYPE_OTHER };
struct esp_partition_t { esp_partition_type_t type; esp_partition_subtype_t subtype; std::uint32_t address, size; bool encrypted; };
enum esp_ota_img_states_t { ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID, ESP_OTA_IMG_INVALID };
struct esp_app_desc_t;
const esp_partition_t* esp_ota_get_running_partition();
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*);
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t*);
esp_err_t esp_ota_begin(const esp_partition_t*, std::size_t, esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, std::size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);
esp_err_t esp_ota_get_partition_description(const esp_partition_t*, esp_app_desc_t*);
esp_err_t esp_partition_read(const esp_partition_t*, std::size_t, void*, std::size_t);
