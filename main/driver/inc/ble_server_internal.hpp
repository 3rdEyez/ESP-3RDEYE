#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "ble_identity.h"
#include "ble_diagnostics.hpp"
#include "ble_motion.hpp"
#include "ble_pairing.hpp"
#include "ble_protocol.hpp"
#include "ble_session.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"

namespace satori::ble::internal {

constexpr char kTag[] = "BLE";
constexpr std::uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;
constexpr std::uint16_t kNoHandle = 0;
constexpr std::size_t kPendingCommands = 12;

struct WorkItem {
    Outcome outcome{};
    bool disconnected{false};
};

// Cross-task scalar state is atomic. Session state is protected by session_lock;
// PairingCore and mutable identity data are protected by pairing_mutex.
// MotionEngine belongs only to ControlTask after task startup.
struct Runtime {
    Session session{};
    Diagnostics diagnostics{}; // Protected by session_lock; never persisted.
    MotionEngine motion{};
    PairingCore pairing{};
    QueueHandle_t work_queue{nullptr};
    SemaphoreHandle_t pairing_mutex{nullptr};
    portMUX_TYPE session_lock = portMUX_INITIALIZER_UNLOCKED;
    BleIdentityData identity{};
    std::atomic<bool> has_identity{false};
    std::atomic<bool> event_subscribed{false};
    std::atomic<bool> secure_peer{false};
    std::atomic<bool> identity_corrupt{false};
    std::atomic<bool> ble_started{false};
    std::atomic<bool> boot_control_allowed{true};
    std::atomic<bool> host_startup_ready{false};
    std::atomic<bool> ota_maintenance_requested{false};
    std::atomic<bool> ota_maintenance_stopped{false};
    std::atomic<std::uint32_t> control_cycles{0};
    std::atomic<bool> pairing_store_ready{false};
    std::atomic<std::uint8_t> bond_count{0};
    std::atomic<bool> output_fault{false};
    std::atomic<bool> pairing_storage_fault{false};
    ble_store_write_fn* store_write_delegate{nullptr};
    std::uint8_t address_type{0}; // NimBLE host task only.
    std::atomic<std::uint16_t> connection{kNoConnection};
    std::uint16_t tx_handle{kNoHandle}; // Set before host/task startup.
    std::uint16_t state_handle{kNoHandle};
    std::atomic<std::uint32_t> connected_at_ms{0};
    std::atomic<bool> startup_configured{false};
    Target startup_target{}; // Set before host/task startup.
    ble_gap_adv_params adv_params{}; // NimBLE host task only.
};

extern Runtime g_runtime;

void PairingLock();
void PairingUnlock();
BleIdentityData ReadIdentity();
void UpdatePasskey(std::uint32_t passkey);
PeerIdentity IdentityFromDesc(const ble_gap_conn_desc& desc);
LinkSecurity SecurityFromDesc(const ble_gap_conn_desc& desc);
bool AuthorizedSecurePeer(const ble_gap_conn_desc& desc);
void Notify(const std::array<std::uint8_t, kFrameSize>& bytes);
void NotifySnapshotEvent(std::uint32_t sequence = 0);
Diagnostics ReadDiagnostics();
void RecordStop(StopReason reason);
void RecordFault(std::uint8_t fault, StopReason reason);
void RecordDisconnect(std::uint16_t reason);
bool IsStartupConfigurationValid(Target& startup);
void ControlTask(void*);
void StartUsbTools();

} // namespace satori::ble::internal
