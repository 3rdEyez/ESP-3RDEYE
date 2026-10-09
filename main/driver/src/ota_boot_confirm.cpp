#include "sdkconfig.h"
#include "ota_boot_confirm.h"
#if CONFIG_SATORI_DUAL_OTA_BOOT_CONFIRM
#include "ble_server.h"
#include "boot_health.hpp"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char* kTag = "OTA_BOOT";
static_assert(static_cast<unsigned>(ESP_OTA_IMG_PENDING_VERIFY) ==
              static_cast<unsigned>(satori::ble::BootImageState::PendingVerify));
static_assert(static_cast<unsigned>(ESP_OTA_IMG_VALID) ==
              static_cast<unsigned>(satori::ble::BootImageState::Valid));
bool pending = false; // app_main owns the confirmation lifecycle.
bool PartitionMatches(esp_partition_type_t type, esp_partition_subtype_t subtype,
                      const char* label, std::uint32_t address, std::uint32_t size) {
    const auto* part = esp_partition_find_first(type, subtype, label);
    return part && part->address == address && part->size == size && !part->encrypted;
}
}
bool PrepareOtaBootConfirmation() {
    SetBleBootControlAllowed(false);
    pending = false;
    const auto* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || running->type != ESP_PARTITION_TYPE_APP ||
        (running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
        esp_ota_get_state_partition(running, &state) != ESP_OK) {
        ESP_LOGE(kTag, "No valid OTA boot state; outputs remain gated");
        return false;
    }
    if (!satori::ble::BootImageCanValidate(static_cast<std::uint32_t>(state))) {
        ESP_LOGE(kTag, "Unexpected OTA boot state=%u; outputs remain gated",
                 static_cast<unsigned>(state));
        return false;
    }
    pending = state == ESP_OTA_IMG_PENDING_VERIFY;
    ESP_LOGI(kTag, "running_slot=%u state=%u; no output during validation", running->subtype,
             static_cast<unsigned>(state));
    const bool layout =
        PartitionMatches(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs", 0x9000, 0x6000) &&
        PartitionMatches(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY, "phy_init", 0xf000, 0x1000) &&
        PartitionMatches(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_UNDEFINED, "config", 0x300000, 0x2800) &&
        PartitionMatches(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata", 0x303000, 0x2000) &&
        PartitionMatches(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0", 0x10000, 0x170000) &&
        PartitionMatches(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1", 0x180000, 0x170000);
    if (!layout) { FailOtaBootConfirmation("layout"); return false; }
    return true;
}
void FailOtaBootConfirmation(const char* stage) {
    SetBleBootControlAllowed(false);
    ESP_LOGE(kTag, "Startup validation failed: %s; outputs remain gated", stage);
    if (pending) {
        // Only the official app rollback path writes otadata. No NVS/schema edits.
        const auto rc = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(kTag, "Rollback unavailable (%s); staying stopped", esp_err_to_name(rc));
    }
}
void CompleteOtaBootConfirmation() {
    satori::ble::BootHealthWindow window;
    auto previous = BleControlCycleCount();
    for (std::uint32_t elapsed = 0; ; elapsed += 100) {
        const auto current = BleControlCycleCount();
        const bool healthy = BleStartupHealthy() &&
            heap_caps_get_free_size(MALLOC_CAP_8BIT) >= 32768;
        const auto decision = window.Observe(elapsed, healthy, current != previous);
        previous = current;
        if (decision == satori::ble::BootDecision::Fail) {
            FailOtaBootConfirmation("BLE/config/identity/task/heap timeout"); return;
        }
        if (decision == satori::ble::BootDecision::Confirm) {
            if (pending && esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
                FailOtaBootConfirmation("mark-valid"); return;
            }
            pending = false;
            SetBleBootControlAllowed(true);
            ESP_LOGI(kTag, "Startup validation passed; slot confirmed; no servo self-test performed");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#else
bool PrepareOtaBootConfirmation() { return true; }
void FailOtaBootConfirmation(const char*) {}
void CompleteOtaBootConfirmation() {}
#endif
