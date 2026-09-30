#include "nvs_bootstrap.h"
#include "esp_log.h"
#include "nvs_flash.h"

esp_err_t InitializeSharedNvs() {
    const esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE("NVS", "NVS is not usable (%s); refusing to erase bonds or identity automatically", esp_err_to_name(result));
    }
    return result;
}
