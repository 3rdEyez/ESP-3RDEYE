#include "sdkconfig.h"
#if CONFIG_SATORI_TRANSPORT_BLE_PRIMARY
#include "ble_server.h"
#include "ble_server_internal.hpp"
#include "ble_identity.h"
#include "ble_protocol.hpp"
#include "ble_session.hpp"
#include "ble_motion.hpp"
#include "ble_pairing.hpp"
#include "ble_startup_profile.hpp"
#include "servo_group.h"
#include "servor_input_adapter.h"
#include "esp_partition_param.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace satori::ble::internal {
// UUID arrays are stored in Bluetooth little-endian order for NimBLE.
static const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(0x00,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
static const ble_uuid128_t kIdentityUuid = BLE_UUID128_INIT(0x01,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
static const ble_uuid128_t kInfoUuid = BLE_UUID128_INIT(0x02,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
static const ble_uuid128_t kRxUuid = BLE_UUID128_INIT(0x03,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
static const ble_uuid128_t kTxUuid = BLE_UUID128_INIT(0x04,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
static const ble_uuid128_t kStateUuid = BLE_UUID128_INIT(0x05,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
static const ble_uuid128_t kDiagnosticsUuid = BLE_UUID128_INIT(0x06,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
void StartAdvertising();

PeerIdentity IdentityFromDesc(const ble_gap_conn_desc& desc) {
    PeerIdentity peer{};
    peer.address_type = desc.peer_id_addr.type;
    std::memcpy(peer.address.data(), desc.peer_id_addr.val, peer.address.size());
    return peer;
}
LinkSecurity SecurityFromDesc(const ble_gap_conn_desc& desc) {
    return {desc.sec_state.encrypted != 0, desc.sec_state.authenticated != 0, desc.sec_state.bonded != 0};
}
Peer PeerFromDesc(const ble_gap_conn_desc& desc) {
    Peer p{};
    std::memcpy(p.address.data(), desc.peer_id_addr.val, p.address.size());
    p.encrypted = desc.sec_state.encrypted; p.authenticated = desc.sec_state.authenticated; p.bonded = desc.sec_state.bonded;
    return p;
}
bool IsSecureBonded(const ble_gap_conn_desc& desc) {
    return desc.sec_state.encrypted != 0 && desc.sec_state.authenticated != 0 && desc.sec_state.bonded != 0;
}
bool AuthorizedSecurePeer(const ble_gap_conn_desc& desc) {
    if (!g_runtime.pairing_store_ready || g_runtime.pairing_storage_fault.load(std::memory_order_acquire) || !IsSecureBonded(desc)) return false;
    const auto peer = IdentityFromDesc(desc);
    const auto security = SecurityFromDesc(desc);
    PairingLock(); const bool authorized = g_runtime.pairing.IsAuthorized(peer, security); PairingUnlock();
    return authorized;
}
int GuardedStoreWrite(int object_type, const ble_store_value* value) {
    if (!g_runtime.store_write_delegate) return BLE_HS_ESTORE_FAIL;
    const int rc = g_runtime.store_write_delegate(object_type, value);
    if (rc != 0 && (object_type == BLE_STORE_OBJ_TYPE_OUR_SEC || object_type == BLE_STORE_OBJ_TYPE_PEER_SEC)) {
        g_runtime.pairing_storage_fault.store(true, std::memory_order_release);
        RecordFault(BondStorageFault, StopReason::StorageFault);
        ESP_LOGE(kTag, "BLE bond persistence failed; pairing and control are disabled until reboot");
    }
    return rc;
}
void Notify(const std::array<std::uint8_t, kFrameSize>& bytes) {
    const auto connection = g_runtime.connection.load(std::memory_order_acquire);
    if (connection == kNoConnection || g_runtime.tx_handle == kNoHandle || !g_runtime.event_subscribed) return;
    struct os_mbuf* om = ble_hs_mbuf_from_flat(bytes.data(), bytes.size());
    if (!om || ble_gatts_notify_custom(connection, g_runtime.tx_handle, om) != 0) {
        portENTER_CRITICAL(&g_runtime.session_lock);
        IncrementDiagnosticCounter(g_runtime.diagnostics.notification_failure_count);
        portEXIT_CRITICAL(&g_runtime.session_lock);
    }
}
void NotifySnapshotEvent(std::uint32_t sequence) {
    Snapshot snapshot;
    portENTER_CRITICAL(&g_runtime.session_lock); snapshot = g_runtime.session.snapshot(); portEXIT_CRITICAL(&g_runtime.session_lock);
    Frame request; request.opcode = static_cast<std::uint8_t>(Opcode::AsyncState); request.sequence = sequence;
    Frame event; event.opcode = static_cast<std::uint8_t>(Opcode::AsyncState); event.sequence = sequence; event.token = snapshot.token;
    event.payload[0] = static_cast<std::uint8_t>(Result::Ok); event.payload[1] = static_cast<std::uint8_t>(snapshot.state);
    event.payload[2] = snapshot.last_applied_sequence & 0xff; event.payload[3] = (snapshot.last_applied_sequence >> 8) & 0xff;
    event.payload[4] = (snapshot.last_applied_sequence >> 16) & 0xff; event.payload[5] = (snapshot.last_applied_sequence >> 24) & 0xff;
    event.payload[6] = (snapshot.valid_mask ? 1 : 0) | (snapshot.interpolating_mask ? 2 : 0) |
                       (g_runtime.bond_count.load(std::memory_order_relaxed) ? 4 : 0) | (snapshot.token ? 8 : 0);
    event.payload[7] = snapshot.battery_percent; Notify(Encode(event));
}

int GattAccess(std::uint16_t conn_handle, std::uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void*) {
    if (conn_handle != g_runtime.connection.load(std::memory_order_acquire)) return BLE_ATT_ERR_UNLIKELY;
    const ble_uuid_t* uuid = ctxt->chr ? ctxt->chr->uuid : nullptr;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        std::array<std::uint8_t, kFrameSize> bytes{};
        std::array<std::uint8_t, 16> id{};
        std::size_t size = 0;
        if (ble_uuid_cmp(uuid, &kIdentityUuid.u) == 0) {
            if (!g_runtime.has_identity) return BLE_ATT_ERR_UNLIKELY;
            id = EncodeIdentity(ReadIdentity().id); return os_mbuf_append(ctxt->om, id.data(), id.size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ble_uuid_cmp(uuid, &kInfoUuid.u) == 0) {
            DeviceInfo info; info.firmware_major = 0; info.firmware_minor = 2; info.firmware_patch = 5;
            info.protocol_minor = 2;
            info.capabilities = 0x5f | kCapabilityPairingCodeManagement | kCapabilitySharedMultiBond;
            info.security_policy = 2;
            bytes = EncodeDeviceInfo(info); size = bytes.size();
        } else if (ble_uuid_cmp(uuid, &kStateUuid.u) == 0) {
            ble_gap_conn_desc desc{};
            if (ble_gap_conn_find(conn_handle, &desc) != 0 || !AuthorizedSecurePeer(desc)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            Snapshot snapshot; portENTER_CRITICAL(&g_runtime.session_lock); snapshot = g_runtime.session.snapshot(); portEXIT_CRITICAL(&g_runtime.session_lock);
            bytes = EncodeSnapshot(snapshot); size = bytes.size();
        } else if (ble_uuid_cmp(uuid, &kDiagnosticsUuid.u) == 0) {
            ble_gap_conn_desc desc{};
            if (ble_gap_conn_find(conn_handle, &desc) != 0 || !AuthorizedSecurePeer(desc)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            bytes = EncodeDiagnostics(ReadDiagnostics()); size = bytes.size();
        } else return BLE_ATT_ERR_READ_NOT_PERMITTED;
        return os_mbuf_append(ctxt->om, bytes.data(), size) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(uuid, &kRxUuid.u) == 0) {
        ble_gap_conn_desc desc{};
        if (ble_gap_conn_find(conn_handle, &desc) != 0 || !AuthorizedSecurePeer(desc)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        // No control or persistent-management commands before startup confirms.
        if (!g_runtime.boot_control_allowed) return BLE_ATT_ERR_UNLIKELY;
        if (g_runtime.output_fault.load(std::memory_order_acquire)) return BLE_ATT_ERR_UNLIKELY;
        const std::uint16_t packet_size = OS_MBUF_PKTLEN(ctxt->om);
        if (packet_size != kFrameSize) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        std::array<std::uint8_t, kFrameSize> raw{};
        if (os_mbuf_copydata(ctxt->om, 0, raw.size(), raw.data()) != 0) return BLE_ATT_ERR_UNLIKELY;
        const auto token = esp_random();
        Outcome outcome;
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        portENTER_CRITICAL(&g_runtime.session_lock);
        outcome = g_runtime.session.Handle(raw.data(), raw.size(), now,
                                   g_runtime.startup_configured && g_runtime.boot_control_allowed, g_runtime.startup_target, token ? token : 1);
        portEXIT_CRITICAL(&g_runtime.session_lock);
        if (outcome.result == Result::BadLength) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (outcome.result == Result::NotAuthorized) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        if (outcome.accepted && outcome.action_required) {
            WorkItem item{outcome};
            const auto opcode = static_cast<Opcode>(outcome.request.opcode);
            BaseType_t queued = pdFALSE;
            if (g_runtime.work_queue && (opcode == Opcode::Halt || opcode == Opcode::Release)) {
                // Stop commands are barriers: discard stale queued motion before admitting one.
                xQueueReset(g_runtime.work_queue);
                queued = xQueueSendToFront(g_runtime.work_queue, &item, 0);
            } else if (g_runtime.work_queue) {
                queued = xQueueSend(g_runtime.work_queue, &item, 0);
            }
            if (queued != pdTRUE) {
                RecordFault(WorkQueueFault, StopReason::QueueFault);
                // The state machine has accepted this sequence, so enter fail-safe and do not acknowledge execution.
                portENTER_CRITICAL(&g_runtime.session_lock); g_runtime.session.Disconnect(); portEXIT_CRITICAL(&g_runtime.session_lock);
                g_runtime.secure_peer = false;
                if (g_runtime.connection != kNoConnection) (void)ble_gap_terminate(g_runtime.connection, BLE_ERR_REM_USER_CONN_TERM);
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            if (opcode == Opcode::SetTarget && outcome.has_reply) Notify(outcome.reply);
        } else if (outcome.has_reply) Notify(outcome.reply);
        return 0;
    }
    (void)attr_handle;
    return ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR ? BLE_ATT_ERR_READ_NOT_PERMITTED : BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

static ble_gatt_chr_def kCharacteristics[7]{};
static ble_gatt_svc_def kServices[2]{};
void ConfigureGattTable() {
    kCharacteristics[0].uuid = &kIdentityUuid.u; kCharacteristics[0].access_cb = GattAccess; kCharacteristics[0].flags = BLE_GATT_CHR_F_READ;
    kCharacteristics[1].uuid = &kInfoUuid.u; kCharacteristics[1].access_cb = GattAccess; kCharacteristics[1].flags = BLE_GATT_CHR_F_READ;
    kCharacteristics[2].uuid = &kRxUuid.u; kCharacteristics[2].access_cb = GattAccess;
    kCharacteristics[2].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
    kCharacteristics[3].uuid = &kTxUuid.u; kCharacteristics[3].access_cb = GattAccess;
    kCharacteristics[3].flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                                BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN;
    kCharacteristics[3].val_handle = &g_runtime.tx_handle;
    kCharacteristics[4].uuid = &kStateUuid.u; kCharacteristics[4].access_cb = GattAccess;
    kCharacteristics[4].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN;
    kCharacteristics[4].val_handle = &g_runtime.state_handle;
    kCharacteristics[5].uuid = &kDiagnosticsUuid.u; kCharacteristics[5].access_cb = GattAccess;
    kCharacteristics[5].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN;
    kServices[0].type = BLE_GATT_SVC_TYPE_PRIMARY; kServices[0].uuid = &kServiceUuid.u; kServices[0].characteristics = kCharacteristics;
}

int GapEvent(ble_gap_event* event, void*) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            (void)ble_gap_adv_stop();
            StartAdvertising();
            return 0;
        }
        if (g_runtime.connection != kNoConnection) { ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT); return 0; }
        g_runtime.connection = event->connect.conn_handle; g_runtime.event_subscribed = false; g_runtime.secure_peer = false;
        g_runtime.connected_at_ms = esp_timer_get_time() / 1000;
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == g_runtime.connection) {
            portENTER_CRITICAL(&g_runtime.session_lock);
            const bool had_owner = g_runtime.session.snapshot().token != 0;
            portEXIT_CRITICAL(&g_runtime.session_lock);
            if (had_owner) RecordStop(StopReason::LinkLost);
            RecordDisconnect(static_cast<std::uint16_t>(event->disconnect.reason));
            g_runtime.connection = kNoConnection; g_runtime.event_subscribed = false; g_runtime.secure_peer = false;
            portENTER_CRITICAL(&g_runtime.session_lock); g_runtime.session.Disconnect(); const auto gen = g_runtime.session.generation(); portEXIT_CRITICAL(&g_runtime.session_lock);
            WorkItem stop{}; stop.disconnected = true;
            if (g_runtime.work_queue) { xQueueReset(g_runtime.work_queue); stop.outcome.generation = gen; xQueueSendToFront(g_runtime.work_queue, &stop, 0); }
            StartAdvertising();
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        if (event->subscribe.conn_handle != g_runtime.connection.load(std::memory_order_acquire)) return 0;
        ble_gap_conn_desc desc{};
        if (ble_gap_conn_find(event->subscribe.conn_handle, &desc) != 0 || !AuthorizedSecurePeer(desc)) return 0;
        if (event->subscribe.attr_handle == g_runtime.tx_handle) {
            g_runtime.event_subscribed = event->subscribe.cur_notify;
            portENTER_CRITICAL(&g_runtime.session_lock); g_runtime.session.SetSubscribed(g_runtime.event_subscribed); portEXIT_CRITICAL(&g_runtime.session_lock);
        }
        return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.conn_handle != g_runtime.connection.load(std::memory_order_acquire)) return 0;
        ble_gap_conn_desc desc{};
        if (event->enc_change.status == 0 && ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            if (!IsSecureBonded(desc)) {
                (void)ble_gap_terminate(desc.conn_handle, BLE_ERR_AUTH_FAIL);
                return 0;
            }
            const auto peer = IdentityFromDesc(desc);
            int count = 0;
            const auto count_result = ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count);
            std::array<ble_addr_t, PairingCore::kMaxBondedPhones> stored{};
            int stored_count = 0;
            const bool listed = count_result == 0 && count > 0 &&
                count <= static_cast<int>(PairingCore::kMaxBondedPhones) &&
                ble_store_util_bonded_peers(stored.data(), &stored_count, stored.size()) == 0 &&
                stored_count == count && std::any_of(stored.begin(), stored.begin() + stored_count,
                    [&desc](const ble_addr_t& address) {
                        return address.type == desc.peer_id_addr.type &&
                               std::memcmp(address.val, desc.peer_id_addr.val, sizeof(desc.peer_id_addr.val)) == 0;
                    });
            PairingLock();
            const bool known = listed && !g_runtime.pairing_storage_fault.load(std::memory_order_acquire) &&
                               g_runtime.pairing.RegisterSecureBond(peer, SecurityFromDesc(desc)) == PairingResult::Ok;
            PairingUnlock();
            if (!g_runtime.has_identity || g_runtime.identity_corrupt || !known) {
                (void)ble_gap_terminate(desc.conn_handle, BLE_ERR_AUTH_FAIL); return 0;
            }
            g_runtime.bond_count.store(static_cast<std::uint8_t>(count), std::memory_order_release);
            if (!g_runtime.secure_peer) {
                g_runtime.secure_peer = true;
                portENTER_CRITICAL(&g_runtime.session_lock); g_runtime.session.Connect(PeerFromDesc(desc), g_runtime.event_subscribed, esp_timer_get_time() / 1000); portEXIT_CRITICAL(&g_runtime.session_lock);
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // Never discard or replace a previously saved bond automatically.
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if (event->passkey.conn_handle != g_runtime.connection.load(std::memory_order_acquire)) return 0;
        int count = 0;
        if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count) != 0 || count < 0 ||
            !g_runtime.pairing_store_ready || g_runtime.pairing_storage_fault.load(std::memory_order_acquire) ||
            !g_runtime.has_identity || g_runtime.identity_corrupt) {
            ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL); return 0;
        }
        PairingLock(); const bool room_available = g_runtime.pairing.CanBeginPairing(static_cast<std::size_t>(count)); PairingUnlock();
        if (!room_available) { ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL); return 0; }
        ble_sm_io io{}; io.action = event->passkey.params.action;
        if (io.action != BLE_SM_IOACT_DISP) { ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL); return 0; }
        io.passkey = ReadIdentity().passkey;
        if (ble_sm_inject_io(event->passkey.conn_handle, &io) != 0) {
            (void)ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
        }
        return 0;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        StartAdvertising(); return 0;
    default: return 0;
    }
}

void StartAdvertising() {
    if (g_runtime.connection != kNoConnection || !g_runtime.pairing_store_ready) return;
    ble_hs_adv_fields fields{};
    static const char name[] = "SatoriEye";
    fields.name = reinterpret_cast<const std::uint8_t*>(name); fields.name_len = sizeof(name) - 1; fields.name_is_complete = 1;
    fields.uuids128 = const_cast<ble_uuid128_t*>(&kServiceUuid); fields.num_uuids128 = 1; fields.uuids128_is_complete = 1;
    const int fields_rc = ble_gap_adv_set_fields(&fields);
    if (fields_rc != 0) { g_runtime.host_startup_ready = false; ESP_LOGW(kTag, "BLE advertising fields failed (%d)", fields_rc); return; }
    g_runtime.adv_params = {}; g_runtime.adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; g_runtime.adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc = ble_gap_adv_start(g_runtime.address_type, nullptr, BLE_HS_FOREVER, &g_runtime.adv_params, GapEvent, nullptr);
    g_runtime.host_startup_ready = rc == 0 || rc == BLE_HS_EALREADY;
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(kTag, "BLE advertising restart failed (%d)", rc);
}
bool InitializePairingStore() {
    if (g_runtime.pairing_store_ready) return true;
    int bond_count = 0;
    if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bond_count) != 0 || bond_count < 0 ||
        bond_count > static_cast<int>(PairingCore::kMaxBondedPhones)) {
        ESP_LOGE(kTag, "BLE bond store unavailable or exceeds the eight-phone limit; advertising disabled");
        return false;
    }
    if (!g_runtime.has_identity && !g_runtime.identity_corrupt && bond_count == 0) {
#if CONFIG_SATORI_DUAL_OTA_BOOT_CONFIRM
        // Migration must preserve an existing identity, never create credentials.
        ESP_LOGE(kTag, "Existing identity missing; OTA validation will fail closed");
        return false;
#else
        BleIdentityData generated{};
        PairingLock();
        const auto provision = BleProvisionIdentity(generated);
        if (provision == ESP_OK) { g_runtime.identity = generated; g_runtime.has_identity = true; }
        PairingUnlock();
        if (provision != ESP_OK) { ESP_LOGE(kTag, "BLE auto-provision failed: %s", esp_err_to_name(provision)); g_runtime.identity_corrupt = true; }
#endif
    }
    if (!g_runtime.has_identity || g_runtime.identity_corrupt) {
        ESP_LOGW(kTag, "BLE identity unavailable or malformed; advertising disabled pending USB recovery");
        return false;
    }
    ble_addr_t stored[PairingCore::kMaxBondedPhones]{};
    int stored_count = 0;
    if (bond_count > 0 && (ble_store_util_bonded_peers(stored, &stored_count, PairingCore::kMaxBondedPhones) != 0 ||
                           stored_count != bond_count)) {
        ESP_LOGE(kTag, "Could not enumerate all persisted BLE bonds; advertising disabled");
        return false;
    }
    char code[7]{};
    std::snprintf(code, sizeof(code), "%06lu", static_cast<unsigned long>(ReadIdentity().passkey));
    PairingCore restored(code);
    for (int i = 0; i < stored_count; ++i) {
        PeerIdentity peer{}; peer.address_type = stored[i].type;
        std::memcpy(peer.address.data(), stored[i].val, peer.address.size());
        if (restored.RestoreBond(peer) != PairingResult::Ok) return false;
    }
    PairingLock(); g_runtime.pairing = restored; PairingUnlock();
    g_runtime.bond_count.store(static_cast<std::uint8_t>(bond_count), std::memory_order_release);
    g_runtime.pairing_store_ready = true;
    return true;
}
void OnSync() {
    if (ble_hs_id_infer_auto(0, &g_runtime.address_type) != 0) { ESP_LOGE(kTag, "BLE address setup failed"); return; }
    if (ble_hs_cfg.store_write_cb != GuardedStoreWrite) {
        g_runtime.store_write_delegate = ble_hs_cfg.store_write_cb;
        ble_hs_cfg.store_write_cb = GuardedStoreWrite;
    }
    const bool safe = InitializePairingStore();
    if (!safe) { ESP_LOGE(kTag, "BLE identity/bond state is unsafe; advertising remains disabled"); return; }
    StartAdvertising();
}
void HostTask(void*) { nimble_port_run(); nimble_port_freertos_deinit(); }
void OnHostReset(int reason) {
    g_runtime.host_startup_ready = false;
    RecordStop(StopReason::HostReset);
    ESP_LOGW(kTag, "BLE host reset (%d); invalidating live session", reason);
    g_runtime.connection = kNoConnection;
    g_runtime.event_subscribed = false;
    g_runtime.secure_peer = false;
    PairingLock();
    g_runtime.pairing_store_ready = false;
    PairingUnlock();
    portENTER_CRITICAL(&g_runtime.session_lock);
    g_runtime.session.Disconnect();
    const auto generation = g_runtime.session.generation();
    portEXIT_CRITICAL(&g_runtime.session_lock);
    if (g_runtime.work_queue) {
        xQueueReset(g_runtime.work_queue);
        WorkItem stop{}; stop.disconnected = true; stop.outcome.generation = generation;
        (void)xQueueSendToFront(g_runtime.work_queue, &stop, 0);
    }
}


}

void StartBleMaintenanceConsole() { satori::ble::internal::StartUsbTools(); }
void SetBleBootControlAllowed(bool allowed) { satori::ble::internal::g_runtime.boot_control_allowed = allowed; }
bool BleOtaPeerStillAuthorized(unsigned short peer) {
    using namespace satori::ble::internal;
    ble_gap_conn_desc desc{};
    return peer==g_runtime.connection.load(std::memory_order_acquire)&&
        ble_gap_conn_find(peer,&desc)==0&&AuthorizedSecurePeer(desc);
}
bool BeginBleOtaMaintenance(unsigned short peer) {
#if CONFIG_SATORI_WIFI_OTA_PROTOTYPE
    using namespace satori::ble::internal;
    if (!g_runtime.boot_control_allowed||!BleStartupHealthy()||!BleOtaPeerStillAuthorized(peer)) return false;
    bool expected=false;
    if (!g_runtime.ota_maintenance_requested.compare_exchange_strong(expected,true)) return false;
    g_runtime.boot_control_allowed=false;
    portENTER_CRITICAL(&g_runtime.session_lock);
    g_runtime.session.Disconnect();
    portEXIT_CRITICAL(&g_runtime.session_lock);
    if (g_runtime.work_queue) xQueueReset(g_runtime.work_queue);
    return true;
#else
    (void)peer;return false;
#endif
}
bool BleOtaMaintenanceStopped() {
    return satori::ble::internal::g_runtime.ota_maintenance_stopped.load(std::memory_order_acquire);
}
unsigned BleControlCycleCount() { return satori::ble::internal::g_runtime.control_cycles.load(std::memory_order_relaxed); }
bool BleStartupHealthy() {
    const auto& runtime = satori::ble::internal::g_runtime;
    return runtime.ble_started && runtime.host_startup_ready && runtime.pairing_store_ready &&
           runtime.has_identity && runtime.startup_configured && !runtime.identity_corrupt &&
           !runtime.output_fault && !runtime.pairing_storage_fault;
}


esp_err_t StartBlePrimary() {
    using namespace satori::ble::internal;
    g_runtime.diagnostics.reset_reason = static_cast<std::uint8_t>(esp_reset_reason()); // Before tasks start.
    g_runtime.has_identity = BleLoadIdentity(g_runtime.identity);
    g_runtime.identity_corrupt = !g_runtime.has_identity && BleIdentityHasAnyMaterial();
    if (g_runtime.identity_corrupt) {
        RecordFault(satori::ble::IdentityFault, satori::ble::StopReason::StorageFault);
        ESP_LOGE(kTag, "Partial BLE identity record found; pairing is disabled until USB recovery.");
    }
    if (g_runtime.work_queue == nullptr) g_runtime.work_queue = xQueueCreate(kPendingCommands, sizeof(WorkItem));
    if (!g_runtime.work_queue) return ESP_ERR_NO_MEM;
    g_runtime.startup_configured = IsStartupConfigurationValid(g_runtime.startup_target);
    if (!g_runtime.startup_configured) {
        g_runtime.diagnostics.faults |= satori::ble::StartupConfigurationFault; // Before tasks start.
        ESP_LOGW(kTag, "Startup configuration invalid; ARM remains disabled");
    }
    int result = nimble_port_init(); if (result != ESP_OK) return result;
    g_runtime.pairing_mutex = xSemaphoreCreateMutex();
    if (!g_runtime.pairing_mutex) { (void)nimble_port_deinit(); return ESP_ERR_NO_MEM; }
    ble_hs_cfg.reset_cb = OnHostReset;
    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.store_status_cb = [](struct ble_store_status_event* event, void*) -> int {
        if (!event) return BLE_HS_EINVAL;
        // A full bond store must fail closed; never auto-evict a saved phone.
        if (event->event_code == BLE_STORE_EVENT_FULL || event->event_code == BLE_STORE_EVENT_OVERFLOW)
            return BLE_HS_ESTORE_CAP;
        return BLE_HS_EUNKNOWN;
    };
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ConfigureGattTable();
    int rc = ble_gatts_count_cfg(kServices);
    if (rc != 0) { (void)nimble_port_deinit(); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(kServices);
    if (rc != 0) { (void)nimble_port_deinit(); return ESP_FAIL; }
    ble_svc_gap_init(); ble_svc_gatt_init();
    ble_svc_gap_device_name_set("SatoriEye");
    TaskHandle_t control_task = nullptr;
    if (xTaskCreate(ControlTask, "ble_control", 6144, nullptr, 9, &control_task) != pdPASS) {
        ESP_LOGE(kTag, "Unable to start sole BLE motion task");
        (void)nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    TaskHandle_t host_task = nullptr;
    if (xTaskCreatePinnedToCore(HostTask, "nimble_host", NIMBLE_HS_STACK_SIZE,
                                nullptr, configMAX_PRIORITIES - 4, &host_task, NIMBLE_CORE) != pdPASS) {
        ESP_LOGE(kTag, "Unable to start NimBLE host task");
        vTaskDelete(control_task);
        (void)nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    g_runtime.ble_started = true;
    StartUsbTools();
    return ESP_OK;
}
#endif // CONFIG_SATORI_TRANSPORT_BLE_PRIMARY
