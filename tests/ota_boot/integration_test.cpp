#include "esp_ota_ops.h"
#include "ota_boot_confirm.h"
#include <cassert>
#include <cstring>
#include <initializer_list>

namespace {
esp_partition_t parts[] = {
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x6000, false},
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY, 0xf000, 0x1000, false},
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_UNDEFINED, 0x300000, 0x2800, false},
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, 0x303000, 0x2000, false},
    {ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x10000, 0x170000, false},
    {ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x180000, 0x170000, false},
};
const char* labels[] = {"nvs", "phy_init", "config", "otadata", "ota_0", "ota_1"};
const esp_partition_t* running = &parts[4];
esp_ota_img_states_t state;
bool allowed, healthy, advance;
unsigned cycles, delays, marks, rollbacks;
int mark_result, state_result;
std::size_t heap;
void Reset(esp_ota_img_states_t initial) {
    state = initial; allowed = true; healthy = true; advance = true;
    cycles = delays = marks = rollbacks = 0;
    mark_result = state_result = 0; heap = 32768; running = &parts[4];
}
}
const esp_partition_t* esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype, const char* label) {
    for (unsigned i = 0; i < 6; ++i)
        if (parts[i].type == type && parts[i].subtype == subtype && std::strcmp(labels[i], label) == 0) return &parts[i];
    return nullptr;
}
const esp_partition_t* esp_ota_get_running_partition() { return running; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* out) { *out = state; return state_result; }
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot() { ++rollbacks; return -1; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback() { ++marks; return mark_result; }
void SetBleBootControlAllowed(bool value) { allowed = value; }
bool BleStartupHealthy() { return healthy; }
unsigned BleControlCycleCount() { return cycles; }
std::size_t heap_caps_get_free_size(unsigned) { return heap; }
void vTaskDelay(unsigned) { ++delays; assert(!allowed); if (advance) ++cycles; }

int main() {
    for (auto initial : {ESP_OTA_IMG_VALID, ESP_OTA_IMG_PENDING_VERIFY}) {
        Reset(initial);
        assert(PrepareOtaBootConfirmation() && !allowed);
        CompleteOtaBootConfirmation();
        assert(allowed && marks == (initial == ESP_OTA_IMG_PENDING_VERIFY ? 1u : 0u));
        assert(rollbacks == 0 && delays == 5);
    }
    for (auto bad : {ESP_OTA_IMG_NEW, ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED}) {
        Reset(bad); assert(!PrepareOtaBootConfirmation() && !allowed && marks == 0 && rollbacks == 0);
    }
    Reset(ESP_OTA_IMG_VALID); running = &parts[0];
    assert(!PrepareOtaBootConfirmation() && !allowed);
    Reset(ESP_OTA_IMG_PENDING_VERIFY); ++parts[2].size;
    assert(!PrepareOtaBootConfirmation() && !allowed && rollbacks == 1); --parts[2].size;
    for (unsigned failure = 0; failure < 3; ++failure) {
        Reset(ESP_OTA_IMG_PENDING_VERIFY); assert(PrepareOtaBootConfirmation());
        if (failure == 0) healthy = false;
        if (failure == 1) advance = false;
        if (failure == 2) heap = 32767;
        CompleteOtaBootConfirmation();
        assert(!allowed && marks == 0 && rollbacks == 1 && delays == 150);
    }
    Reset(ESP_OTA_IMG_PENDING_VERIFY); mark_result = -1; assert(PrepareOtaBootConfirmation());
    CompleteOtaBootConfirmation(); assert(!allowed && marks == 1 && rollbacks == 1);
    Reset(ESP_OTA_IMG_VALID); healthy = false; assert(PrepareOtaBootConfirmation());
    CompleteOtaBootConfirmation(); assert(!allowed && marks == 0 && rollbacks == 0);
    Reset(ESP_OTA_IMG_PENDING_VERIFY); state_result = -1;
    assert(!PrepareOtaBootConfirmation() && !allowed && marks == 0 && rollbacks == 0);
}
