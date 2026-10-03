#pragma once
#include "esp_err.h"
#include "ota_window_protocol.hpp"
#include "lan_window_protocol.hpp"
#include <cstddef>
#include <cstdint>
namespace satori::ota {
struct SessionCredentials { char ssid[33]{};char password[65]{}; };
// All networking work runs outside the NimBLE host callback.
esp_err_t StartWifiOtaControlWorker();
bool SubmitLanOtaCommand(std::uint16_t authenticated_peer,const LanCommand& command);
std::size_t ReadLanOtaStatus(std::uint8_t* output,std::size_t capacity);
bool SubmitWifiOtaCommand(std::uint16_t authenticated_peer,const WindowCommand& command);
std::size_t ReadWifiOtaStatus(std::uint8_t* output,std::size_t capacity);
esp_err_t StartExplicitWifiOta(std::uint16_t authenticated_peer,SessionCredentials& output,std::uint32_t connection_epoch);
void CancelWifiOta();
bool WifiOtaSessionActive();
} // namespace satori::ota
