#include "sdkconfig.h"
#if CONFIG_SATORI_TRANSPORT_BLE_PRIMARY
#include "ble_server_internal.hpp"
#include "esp_log.h"
#include "esp_timer.h"

namespace satori::ble::internal {
Runtime g_runtime;
Diagnostics ReadDiagnostics() {
    portENTER_CRITICAL(&g_runtime.session_lock);
    auto snapshot = g_runtime.diagnostics;
    portEXIT_CRITICAL(&g_runtime.session_lock);
    snapshot.uptime_seconds = static_cast<std::uint32_t>(esp_timer_get_time() / 1000000);
    return snapshot;
}
void RecordStop(StopReason reason) {
    portENTER_CRITICAL(&g_runtime.session_lock);
    g_runtime.diagnostics.last_stop = reason;
    if (reason == StopReason::LeaseExpired)
        IncrementDiagnosticCounter(g_runtime.diagnostics.lease_expiry_count);
    portEXIT_CRITICAL(&g_runtime.session_lock);
    ESP_LOGI(kTag, "Control stop recorded: reason=%u", static_cast<unsigned>(reason));
}
void RecordFault(std::uint8_t fault, StopReason reason) {
    portENTER_CRITICAL(&g_runtime.session_lock);
    g_runtime.diagnostics.faults |= fault;
    portEXIT_CRITICAL(&g_runtime.session_lock);
    RecordStop(reason);
}
void RecordDisconnect(std::uint16_t reason) {
    portENTER_CRITICAL(&g_runtime.session_lock);
    g_runtime.diagnostics.last_gap_reason = reason;
    if (g_runtime.diagnostics.disconnect_count != UINT32_MAX)
        ++g_runtime.diagnostics.disconnect_count;
    portEXIT_CRITICAL(&g_runtime.session_lock);
    ESP_LOGI(kTag, "BLE disconnected: reason=%u", static_cast<unsigned>(reason));
}
void PairingLock() { if (g_runtime.pairing_mutex) xSemaphoreTake(g_runtime.pairing_mutex, portMAX_DELAY); }
void PairingUnlock() { if (g_runtime.pairing_mutex) xSemaphoreGive(g_runtime.pairing_mutex); }
BleIdentityData ReadIdentity() { PairingLock(); const auto identity = g_runtime.identity; PairingUnlock(); return identity; }
void UpdatePasskey(std::uint32_t passkey) { PairingLock(); g_runtime.identity.passkey = passkey; PairingUnlock(); }
}
#endif
