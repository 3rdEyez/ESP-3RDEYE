#include "ble_identity.h"
#include "esp_random.h"
#include "nvs.h"

namespace {
constexpr char kNamespace[] = "satori_ble";
constexpr char kIdentityKey[] = "device_id";
constexpr char kPasskeyKey[] = "passkey";
constexpr char kOwnerKey[] = "owner";
struct OwnerBlob { std::uint8_t type; std::uint8_t address[6]; };
}

bool BleLoadIdentity(BleIdentityData& out) {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    std::size_t size = out.id.size();
    const esp_err_t id_result = nvs_get_blob(handle, kIdentityKey, out.id.data(), &size);
    const esp_err_t key_result = nvs_get_u32(handle, kPasskeyKey, &out.passkey);
    nvs_close(handle);
    return id_result == ESP_OK && size == out.id.size() && key_result == ESP_OK && out.passkey <= 999999;
}

bool BleIdentityHasAnyMaterial() {
    nvs_handle_t handle;
    const auto open_result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (open_result == ESP_ERR_NVS_NOT_FOUND) return false;
    if (open_result != ESP_OK) return true;
    std::size_t size = 0;
    const esp_err_t id_result = nvs_get_blob(handle, kIdentityKey, nullptr, &size);
    std::uint32_t passkey = 0;
    const esp_err_t key_result = nvs_get_u32(handle, kPasskeyKey, &passkey);
    nvs_close(handle);
    return id_result != ESP_ERR_NVS_NOT_FOUND || key_result != ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t BleProvisionIdentity(BleIdentityData& out) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    std::size_t size = 0;
    const auto id_result = nvs_get_blob(handle, kIdentityKey, nullptr, &size);
    std::uint32_t existing_key = 0;
    const auto key_result = nvs_get_u32(handle, kPasskeyKey, &existing_key);
    std::size_t owner_size = 0;
    const auto owner_result = nvs_get_blob(handle, kOwnerKey, nullptr, &owner_size);
    if (id_result != ESP_ERR_NVS_NOT_FOUND || key_result != ESP_ERR_NVS_NOT_FOUND ||
        owner_result != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_ERR_INVALID_STATE; // Any identity, owner or malformed record fails closed.
    }
    esp_fill_random(out.id.data(), out.id.size());
    out.passkey = satori::ble::kBleDefaultPairingCode;
    err = nvs_set_blob(handle, kIdentityKey, out.id.data(), out.id.size());
    if (err == ESP_OK) err = nvs_set_u32(handle, kPasskeyKey, out.passkey);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t BleUpdatePairingCode(std::uint32_t code) {
    if (code > 999999u || code == satori::ble::kBleDefaultPairingCode) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    std::uint8_t id[16]{};
    std::size_t id_size = sizeof(id);
    err = nvs_get_blob(handle, kIdentityKey, id, &id_size);
    if (err == ESP_OK && id_size != sizeof(id)) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) err = nvs_set_u32(handle, kPasskeyKey, code);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool BleLoadOwner(BleOwnerData& out) {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    OwnerBlob blob{}; std::size_t size = sizeof(blob);
    const auto err = nvs_get_blob(handle, kOwnerKey, &blob, &size); nvs_close(handle);
    if (err != ESP_OK || size != sizeof(blob)) return false;
    out.type = blob.type; for (int i = 0; i < 6; ++i) out.address[i] = blob.address[i];
    return true;
}
bool BleOwnerHasAnyMaterial() {
    nvs_handle_t handle;
    const auto open_result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (open_result == ESP_ERR_NVS_NOT_FOUND) return false;
    if (open_result != ESP_OK) return true;
    std::size_t size = 0;
    const auto result = nvs_get_blob(handle, kOwnerKey, nullptr, &size);
    nvs_close(handle);
    return result != ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t BleSaveOwner(const BleOwnerData& owner) {
    nvs_handle_t handle; esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle); if (err != ESP_OK) return err;
    OwnerBlob blob{}; blob.type = owner.type; for (int i = 0; i < 6; ++i) blob.address[i] = owner.address[i];
    err = nvs_set_blob(handle, kOwnerKey, &blob, sizeof(blob)); if (err == ESP_OK) err = nvs_commit(handle); nvs_close(handle); return err;
}
esp_err_t BleSaveOwnerAndPairingCode(const BleOwnerData& owner, std::uint32_t code) {
    if (code > 999999u) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    OwnerBlob blob{}; blob.type = owner.type;
    for (int i = 0; i < 6; ++i) blob.address[i] = owner.address[i];
    err = nvs_set_blob(handle, kOwnerKey, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_set_u32(handle, kPasskeyKey, code);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
esp_err_t BleClearOwner() {
    nvs_handle_t handle; esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle); if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, kOwnerKey); if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t BleRotatePasskeyAndClearOwner(std::uint32_t& new_passkey) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    std::size_t id_size = 0;
    err = nvs_get_blob(handle, kIdentityKey, nullptr, &id_size);
    std::uint8_t id[16]{};
    if (err == ESP_OK && id_size == sizeof(id)) err = nvs_get_blob(handle, kIdentityKey, id, &id_size);
    else if (err == ESP_OK) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) {
        new_passkey = satori::ble::kBleDefaultPairingCode;
        err = nvs_set_u32(handle, kPasskeyKey, new_passkey);
    }
    if (err == ESP_OK) {
        const auto erase = nvs_erase_key(handle, kOwnerKey);
        if (erase != ESP_OK && erase != ESP_ERR_NVS_NOT_FOUND) err = erase;
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
