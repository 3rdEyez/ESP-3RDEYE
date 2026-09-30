#include "sdkconfig.h"
#if CONFIG_SATORI_TRANSPORT_BLE_PRIMARY
#include "ble_server_internal.hpp"

namespace satori::ble::internal {
Runtime g_runtime;
void PairingLock() { if (g_runtime.pairing_mutex) xSemaphoreTake(g_runtime.pairing_mutex, portMAX_DELAY); }
void PairingUnlock() { if (g_runtime.pairing_mutex) xSemaphoreGive(g_runtime.pairing_mutex); }
BleIdentityData ReadIdentity() { PairingLock(); const auto identity = g_runtime.identity; PairingUnlock(); return identity; }
void UpdatePasskey(std::uint32_t passkey) { PairingLock(); g_runtime.identity.passkey = passkey; PairingUnlock(); }
}
#endif
