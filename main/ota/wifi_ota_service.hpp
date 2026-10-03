#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
namespace satori::ota {
struct SessionCredentials { char ssid[33]{};char password[65]{}; };
// Local candidate APIs, not exposed via GATT/USB in this stage. A future BLE
// worker must invoke from a non-host-task context following explicit user intent.
// Provisioning public trust requires separate user approval; no root is bundled.
esp_err_t StartExplicitWifiOta(std::uint16_t authenticated_peer,SessionCredentials& output);
void CancelWifiOta(); // async; closes Wi-Fi and keeps motion stopped
bool WifiOtaSessionActive();
} // namespace satori::ota
