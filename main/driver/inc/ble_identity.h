#pragma once
#include <array>
#include <cstdint>
#include "esp_err.h"
#include "ble_protocol.hpp"

struct BleIdentityData {
    std::array<std::uint8_t, 16> id{};
    std::uint32_t passkey{0};
};
struct BleOwnerData {
    std::uint8_t type{0};
    std::array<std::uint8_t, 6> address{};
};

bool BleLoadIdentity(BleIdentityData& out);
bool BleIdentityHasAnyMaterial();
esp_err_t BleProvisionIdentity(BleIdentityData& out);
esp_err_t BleUpdatePairingCode(std::uint32_t code);
bool BleLoadOwner(BleOwnerData& out);
bool BleOwnerHasAnyMaterial();
esp_err_t BleSaveOwner(const BleOwnerData& owner);
esp_err_t BleSaveOwnerAndPairingCode(const BleOwnerData& owner, std::uint32_t code);
esp_err_t BleClearOwner();
esp_err_t BleRotatePasskeyAndClearOwner(std::uint32_t& new_passkey);
