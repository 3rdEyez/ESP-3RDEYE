#include "sdkconfig.h"
#if CONFIG_SATORI_TRANSPORT_BLE_PRIMARY
#include "ble_server.h"
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

using namespace satori::ble;
namespace {
constexpr char kTag[] = "BLE";
constexpr std::uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;
constexpr std::uint16_t kNoHandle = 0;
constexpr std::size_t kPendingCommands = 12;

// UUID arrays are stored in Bluetooth little-endian order for NimBLE.
static const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(0x00,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
static const ble_uuid128_t kIdentityUuid = BLE_UUID128_INIT(0x01,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
static const ble_uuid128_t kInfoUuid = BLE_UUID128_INIT(0x02,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
static const ble_uuid128_t kRxUuid = BLE_UUID128_INIT(0x03,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
static const ble_uuid128_t kTxUuid = BLE_UUID128_INIT(0x04,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);
static const ble_uuid128_t kStateUuid = BLE_UUID128_INIT(0x05,0x00,0x5a,0x14,0xb2,0x63,0x3e,0x9d,0x14,0x4f,0xb9,0x73,0xa0,0xf6,0x89,0x4d);

struct WorkItem { Outcome outcome{}; bool disconnected{false}; };
Session g_session;
MotionEngine g_motion;
PairingCore g_pairing;
QueueHandle_t g_work_queue = nullptr;
SemaphoreHandle_t g_pairing_mutex = nullptr;
portMUX_TYPE g_session_lock = portMUX_INITIALIZER_UNLOCKED;
BleIdentityData g_identity{};
bool g_has_identity = false, g_event_subscribed = false, g_secure_peer = false;
bool g_identity_corrupt = false;
bool g_ble_started = false;
bool g_pairing_store_ready = false;
std::atomic<std::uint8_t> g_bond_count{0};
std::atomic<bool> g_output_fault{false};
std::atomic<bool> g_pairing_storage_fault{false};
ble_store_write_fn* g_store_write_delegate = nullptr;
std::uint8_t g_address_type = 0;
std::uint16_t g_connection = kNoConnection;
std::uint16_t g_tx_handle = kNoHandle;
std::uint16_t g_state_handle = kNoHandle;
std::uint32_t g_connected_at_ms = 0;
bool g_startup_configured = false;
Target g_startup_target{};
ble_gap_adv_params g_adv_params{};
void StartAdvertising();

bool IsStartupConfigurationValid() {
    auto& config = EspPartitionParam::GetInstance();
    if (!config.IsValid()) return false;
    const bool custom_confirmed = config.GetBoolParam("BLE_STARTUP_CONFIRMED", false);
    constexpr const char* startup_keys[] = {"BLE_STARTUP_CH1", "BLE_STARTUP_CH2", "BLE_STARTUP_CH3"};
    int custom[3] = {1500, 1500, 1500};
    bool any_custom = false, all_custom = true;
    for (int i = 0; i < 3; ++i) {
        const bool present = config.HasParam(startup_keys[i]);
        any_custom |= present;
        all_custom &= present;
        if (present) {
            custom[i] = config.GetIntParam(startup_keys[i], -1);
            if (custom[i] < 500 || custom[i] > 2500) return false;
        }
    }
    Target startup{};
    if (SelectStartupTarget(custom_confirmed, any_custom, all_custom, custom, startup) != StartupProfileResult::Ok)
        return false;
    const int pins[3] = {
        config.GetIntParam("SERVO_PULSE_GPIO_CH1", CONFIG_SERVO_PULSE_GPIO_CH1),
        config.GetIntParam("SERVO_PULSE_GPIO_CH2", CONFIG_SERVO_PULSE_GPIO_CH2),
        config.GetIntParam("SERVO_PULSE_GPIO_CH3", CONFIG_SERVO_PULSE_GPIO_CH3),
    };
    const float default_scale[3] = {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f};
    const float default_offset[3] = {0.0f, 40.0f, -40.0f};
    const float default_zero[3] = {90.0f, 60.0f, 120.0f};
    const float default_min[3] = {45.0f, 0.0f, 120.0f};
    const float default_max[3] = {135.0f, 90.0f, 180.0f};
    const bool default_reverse[3] = {true, true, true};
    for (int i = 0; i < 3; ++i) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) return false;
        for (int j = 0; j < i; ++j) if (pins[i] == pins[j]) return false;
        const std::string suffix = std::to_string(i + 1);
        const float scale = config.GetFloatParam("SERVO_SCALE_CH" + suffix, default_scale[i]);
        const float offset = config.GetFloatParam("SERVO_OFFSET_CH" + suffix, default_offset[i]);
        const float zero = config.GetFloatParam("SERVO_ZEROPOINT_CH" + suffix, default_zero[i]);
        const float min_angle = config.GetFloatParam("SERVO_MIN_ANGLE_CH" + suffix, default_min[i]);
        const float max_angle = config.GetFloatParam("SERVO_MAX_ANGLE_CH" + suffix, default_max[i]);
        (void)config.GetBoolParam("SERVO_IS_REVERSE_CH" + suffix, default_reverse[i]);
        if (!std::isfinite(scale) || !std::isfinite(offset) || !std::isfinite(zero) || !std::isfinite(min_angle) || !std::isfinite(max_angle) ||
            scale <= 0.0f || min_angle < 0.0f || max_angle > 180.0f || min_angle >= max_angle || zero < 0.0f || zero > 180.0f) return false;
    }
    g_startup_target = startup;
    return config.IsValid();
}

PeerIdentity IdentityFromDesc(const ble_gap_conn_desc& desc) {
    PeerIdentity peer{};
    peer.address_type = desc.peer_id_addr.type;
    std::memcpy(peer.address.data(), desc.peer_id_addr.val, peer.address.size());
    return peer;
}
LinkSecurity SecurityFromDesc(const ble_gap_conn_desc& desc) {
    return {desc.sec_state.encrypted != 0, desc.sec_state.authenticated != 0, desc.sec_state.bonded != 0};
}
void PairingLock() { if (g_pairing_mutex) xSemaphoreTake(g_pairing_mutex, portMAX_DELAY); }
void PairingUnlock() { if (g_pairing_mutex) xSemaphoreGive(g_pairing_mutex); }
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
    if (!g_pairing_store_ready || g_pairing_storage_fault.load(std::memory_order_acquire) || !IsSecureBonded(desc)) return false;
    const auto peer = IdentityFromDesc(desc);
    const auto security = SecurityFromDesc(desc);
    PairingLock(); const bool authorized = g_pairing.IsAuthorized(peer, security); PairingUnlock();
    return authorized;
}
int GuardedStoreWrite(int object_type, const ble_store_value* value) {
    if (!g_store_write_delegate) return BLE_HS_ESTORE_FAIL;
    const int rc = g_store_write_delegate(object_type, value);
    if (rc != 0 && (object_type == BLE_STORE_OBJ_TYPE_OUR_SEC || object_type == BLE_STORE_OBJ_TYPE_PEER_SEC)) {
        g_pairing_storage_fault.store(true, std::memory_order_release);
        ESP_LOGE(kTag, "BLE bond persistence failed; pairing and control are disabled until reboot");
    }
    return rc;
}
void Notify(const std::array<std::uint8_t, kFrameSize>& bytes) {
    if (g_connection == kNoConnection || g_tx_handle == kNoHandle || !g_event_subscribed) return;
    struct os_mbuf* om = ble_hs_mbuf_from_flat(bytes.data(), bytes.size());
    if (om) (void)ble_gatts_notify_custom(g_connection, g_tx_handle, om);
}
void NotifySnapshotEvent(std::uint32_t sequence = 0) {
    Snapshot snapshot;
    portENTER_CRITICAL(&g_session_lock); snapshot = g_session.snapshot(); portEXIT_CRITICAL(&g_session_lock);
    Frame request; request.opcode = static_cast<std::uint8_t>(Opcode::AsyncState); request.sequence = sequence;
    Frame event; event.opcode = static_cast<std::uint8_t>(Opcode::AsyncState); event.sequence = sequence; event.token = snapshot.token;
    event.payload[0] = static_cast<std::uint8_t>(Result::Ok); event.payload[1] = static_cast<std::uint8_t>(snapshot.state);
    event.payload[2] = snapshot.last_applied_sequence & 0xff; event.payload[3] = (snapshot.last_applied_sequence >> 8) & 0xff;
    event.payload[4] = (snapshot.last_applied_sequence >> 16) & 0xff; event.payload[5] = (snapshot.last_applied_sequence >> 24) & 0xff;
    event.payload[6] = (snapshot.valid_mask ? 1 : 0) | (snapshot.interpolating_mask ? 2 : 0) |
                       (g_bond_count.load(std::memory_order_relaxed) ? 4 : 0) | (snapshot.token ? 8 : 0);
    event.payload[7] = snapshot.battery_percent; Notify(Encode(event));
}

int GattAccess(std::uint16_t conn_handle, std::uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void*) {
    const ble_uuid_t* uuid = ctxt->chr ? ctxt->chr->uuid : nullptr;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        std::array<std::uint8_t, kFrameSize> bytes{};
        std::array<std::uint8_t, 16> id{};
        std::size_t size = 0;
        if (ble_uuid_cmp(uuid, &kIdentityUuid.u) == 0) {
            if (!g_has_identity) return BLE_ATT_ERR_UNLIKELY;
            id = EncodeIdentity(g_identity.id); return os_mbuf_append(ctxt->om, id.data(), id.size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ble_uuid_cmp(uuid, &kInfoUuid.u) == 0) {
            DeviceInfo info; info.firmware_major = 0; info.firmware_minor = 2; info.firmware_patch = 2;
            info.protocol_minor = 2;
            info.capabilities = 0x5f | kCapabilityPairingCodeManagement | kCapabilitySharedMultiBond;
            info.security_policy = 2;
            bytes = EncodeDeviceInfo(info); size = bytes.size();
        } else if (ble_uuid_cmp(uuid, &kStateUuid.u) == 0) {
            ble_gap_conn_desc desc{};
            if (ble_gap_conn_find(conn_handle, &desc) != 0 || !AuthorizedSecurePeer(desc)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            Snapshot snapshot; portENTER_CRITICAL(&g_session_lock); snapshot = g_session.snapshot(); portEXIT_CRITICAL(&g_session_lock);
            bytes = EncodeSnapshot(snapshot); size = bytes.size();
        } else return BLE_ATT_ERR_READ_NOT_PERMITTED;
        return os_mbuf_append(ctxt->om, bytes.data(), size) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(uuid, &kRxUuid.u) == 0) {
        ble_gap_conn_desc desc{};
        if (ble_gap_conn_find(conn_handle, &desc) != 0 || !AuthorizedSecurePeer(desc)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        if (g_output_fault.load(std::memory_order_acquire)) return BLE_ATT_ERR_UNLIKELY;
        const std::uint16_t packet_size = OS_MBUF_PKTLEN(ctxt->om);
        if (packet_size != kFrameSize) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        std::array<std::uint8_t, kFrameSize> raw{};
        if (os_mbuf_copydata(ctxt->om, 0, raw.size(), raw.data()) != 0) return BLE_ATT_ERR_UNLIKELY;
        const auto token = esp_random();
        Outcome outcome;
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        portENTER_CRITICAL(&g_session_lock);
        outcome = g_session.Handle(raw.data(), raw.size(), now,
                                   g_startup_configured, g_startup_target, token ? token : 1);
        portEXIT_CRITICAL(&g_session_lock);
        if (outcome.result == Result::BadLength) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (outcome.result == Result::NotAuthorized) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        if (outcome.accepted && outcome.action_required) {
            WorkItem item{outcome};
            const auto opcode = static_cast<Opcode>(outcome.request.opcode);
            BaseType_t queued = pdFALSE;
            if (g_work_queue && (opcode == Opcode::Halt || opcode == Opcode::Release)) {
                // Stop commands are barriers: discard stale queued motion before admitting one.
                xQueueReset(g_work_queue);
                queued = xQueueSendToFront(g_work_queue, &item, 0);
            } else if (g_work_queue) {
                queued = xQueueSend(g_work_queue, &item, 0);
            }
            if (queued != pdTRUE) {
                // The state machine has accepted this sequence, so enter fail-safe and do not acknowledge execution.
                portENTER_CRITICAL(&g_session_lock); g_session.Disconnect(); portEXIT_CRITICAL(&g_session_lock);
                g_secure_peer = false;
                if (g_connection != kNoConnection) (void)ble_gap_terminate(g_connection, BLE_ERR_REM_USER_CONN_TERM);
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            if (opcode == Opcode::SetTarget && outcome.has_reply) Notify(outcome.reply);
        } else if (outcome.has_reply) Notify(outcome.reply);
        return 0;
    }
    (void)attr_handle;
    return ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR ? BLE_ATT_ERR_READ_NOT_PERMITTED : BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

static ble_gatt_chr_def kCharacteristics[6]{};
static ble_gatt_svc_def kServices[2]{};
void ConfigureGattTable() {
    kCharacteristics[0].uuid = &kIdentityUuid.u; kCharacteristics[0].access_cb = GattAccess; kCharacteristics[0].flags = BLE_GATT_CHR_F_READ;
    kCharacteristics[1].uuid = &kInfoUuid.u; kCharacteristics[1].access_cb = GattAccess; kCharacteristics[1].flags = BLE_GATT_CHR_F_READ;
    kCharacteristics[2].uuid = &kRxUuid.u; kCharacteristics[2].access_cb = GattAccess;
    kCharacteristics[2].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
    kCharacteristics[3].uuid = &kTxUuid.u; kCharacteristics[3].access_cb = GattAccess;
    kCharacteristics[3].flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                                BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN;
    kCharacteristics[3].val_handle = &g_tx_handle;
    kCharacteristics[4].uuid = &kStateUuid.u; kCharacteristics[4].access_cb = GattAccess;
    kCharacteristics[4].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN;
    kCharacteristics[4].val_handle = &g_state_handle;
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
        if (g_connection != kNoConnection) { ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT); return 0; }
        g_connection = event->connect.conn_handle; g_event_subscribed = false; g_secure_peer = false;
        g_connected_at_ms = esp_timer_get_time() / 1000;
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == g_connection) {
            g_connection = kNoConnection; g_event_subscribed = false; g_secure_peer = false;
            portENTER_CRITICAL(&g_session_lock); g_session.Disconnect(); const auto gen = g_session.generation(); portEXIT_CRITICAL(&g_session_lock);
            WorkItem stop{}; stop.disconnected = true;
            if (g_work_queue) { xQueueReset(g_work_queue); stop.outcome.generation = gen; xQueueSendToFront(g_work_queue, &stop, 0); }
            StartAdvertising();
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        ble_gap_conn_desc desc{};
        if (ble_gap_conn_find(event->subscribe.conn_handle, &desc) != 0 || !AuthorizedSecurePeer(desc)) return 0;
        if (event->subscribe.attr_handle == g_tx_handle) {
            g_event_subscribed = event->subscribe.cur_notify;
            portENTER_CRITICAL(&g_session_lock); g_session.SetSubscribed(g_event_subscribed); portEXIT_CRITICAL(&g_session_lock);
        }
        return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
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
            const bool known = listed && !g_pairing_storage_fault.load(std::memory_order_acquire) &&
                               g_pairing.RegisterSecureBond(peer, SecurityFromDesc(desc)) == PairingResult::Ok;
            PairingUnlock();
            if (!g_has_identity || g_identity_corrupt || !known) {
                (void)ble_gap_terminate(desc.conn_handle, BLE_ERR_AUTH_FAIL); return 0;
            }
            g_bond_count.store(static_cast<std::uint8_t>(count), std::memory_order_release);
            if (!g_secure_peer) {
                g_secure_peer = true;
                portENTER_CRITICAL(&g_session_lock); g_session.Connect(PeerFromDesc(desc), g_event_subscribed, esp_timer_get_time() / 1000); portEXIT_CRITICAL(&g_session_lock);
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // Never discard or replace a previously saved bond automatically.
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        int count = 0;
        if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count) != 0 || count < 0 ||
            !g_pairing_store_ready || g_pairing_storage_fault.load(std::memory_order_acquire) ||
            !g_has_identity || g_identity_corrupt) {
            ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL); return 0;
        }
        PairingLock(); const bool room_available = g_pairing.CanBeginPairing(static_cast<std::size_t>(count)); PairingUnlock();
        if (!room_available) { ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL); return 0; }
        ble_sm_io io{}; io.action = event->passkey.params.action;
        if (io.action != BLE_SM_IOACT_DISP) { ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL); return 0; }
        io.passkey = g_identity.passkey;
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
    if (g_connection != kNoConnection || !g_pairing_store_ready) return;
    ble_hs_adv_fields fields{};
    static const char name[] = "SatoriEye";
    fields.name = reinterpret_cast<const std::uint8_t*>(name); fields.name_len = sizeof(name) - 1; fields.name_is_complete = 1;
    fields.uuids128 = const_cast<ble_uuid128_t*>(&kServiceUuid); fields.num_uuids128 = 1; fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);
    g_adv_params = {}; g_adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; g_adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc = ble_gap_adv_start(g_address_type, nullptr, BLE_HS_FOREVER, &g_adv_params, GapEvent, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(kTag, "BLE advertising restart failed (%d)", rc);
}
bool InitializePairingStore() {
    if (g_pairing_store_ready) return true;
    int bond_count = 0;
    if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bond_count) != 0 || bond_count < 0 ||
        bond_count > static_cast<int>(PairingCore::kMaxBondedPhones)) {
        ESP_LOGE(kTag, "BLE bond store unavailable or exceeds the eight-phone limit; advertising disabled");
        return false;
    }
    if (!g_has_identity && !g_identity_corrupt && bond_count == 0) {
        const auto provision = BleProvisionIdentity(g_identity);
        if (provision == ESP_OK) g_has_identity = true;
        else { ESP_LOGE(kTag, "BLE auto-provision failed: %s", esp_err_to_name(provision)); g_identity_corrupt = true; }
    }
    if (!g_has_identity || g_identity_corrupt) {
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
    std::snprintf(code, sizeof(code), "%06lu", static_cast<unsigned long>(g_identity.passkey));
    PairingCore restored(code);
    for (int i = 0; i < stored_count; ++i) {
        PeerIdentity peer{}; peer.address_type = stored[i].type;
        std::memcpy(peer.address.data(), stored[i].val, peer.address.size());
        if (restored.RestoreBond(peer) != PairingResult::Ok) return false;
    }
    PairingLock(); g_pairing = restored; PairingUnlock();
    g_bond_count.store(static_cast<std::uint8_t>(bond_count), std::memory_order_release);
    g_pairing_store_ready = true;
    return true;
}
void OnSync() {
    if (ble_hs_id_infer_auto(0, &g_address_type) != 0) { ESP_LOGE(kTag, "BLE address setup failed"); return; }
    if (ble_hs_cfg.store_write_cb != GuardedStoreWrite) {
        g_store_write_delegate = ble_hs_cfg.store_write_cb;
        ble_hs_cfg.store_write_cb = GuardedStoreWrite;
    }
    const bool safe = InitializePairingStore();
    if (!safe) { ESP_LOGE(kTag, "BLE identity/bond state is unsafe; advertising remains disabled"); return; }
    StartAdvertising();
}
void HostTask(void*) { nimble_port_run(); nimble_port_freertos_deinit(); }
void OnHostReset(int reason) {
    ESP_LOGW(kTag, "BLE host reset (%d); invalidating live session", reason);
    g_connection = kNoConnection;
    g_event_subscribed = false;
    g_secure_peer = false;
    PairingLock();
    g_pairing_store_ready = false;
    PairingUnlock();
    portENTER_CRITICAL(&g_session_lock);
    g_session.Disconnect();
    const auto generation = g_session.generation();
    portEXIT_CRITICAL(&g_session_lock);
    if (g_work_queue) {
        xQueueReset(g_work_queue);
        WorkItem stop{}; stop.disconnected = true; stop.outcome.generation = generation;
        (void)xQueueSendToFront(g_work_queue, &stop, 0);
    }
}

void ControlTask(void*) {
    TickType_t last = xTaskGetTickCount();
    std::uint32_t motion_generation = 0;
    std::uint32_t last_state_event = 0;
    bool servo_enabled = false;
    bool output_fault = false;
    const auto fail_output = [&]() {
        output_fault = true;
        g_output_fault = true;
        servo_enabled = false;
        g_startup_configured = false; // Latched until reboot and configuration revalidation.
        portENTER_CRITICAL(&g_session_lock);
        g_session.Disconnect();
        const auto stopped_generation = g_session.generation();
        portEXIT_CRITICAL(&g_session_lock);
        g_motion.Halt(stopped_generation);
        motion_generation = stopped_generation;
        if (g_connection != kNoConnection) {
            (void)ble_gap_terminate(g_connection, BLE_ERR_REM_USER_CONN_TERM);
        }
        ESP_LOGE(kTag, "PWM driver fault; output commands disabled until reboot");
    };

    const auto state_now = []() {
        Snapshot state;
        portENTER_CRITICAL(&g_session_lock);
        state = g_session.snapshot();
        portEXIT_CRITICAL(&g_session_lock);
        return state;
    };
    const auto valid_work = [](const Outcome& op) {
        portENTER_CRITICAL(&g_session_lock);
        const bool valid = g_session.connected() &&
            op.generation == g_session.generation() &&
            op.request.token != 0 && op.request.token == g_session.snapshot().token;
        portEXIT_CRITICAL(&g_session_lock);
        return valid;
    };
    const auto write_output = [&]() {
        // Only this task touches MotionEngine or ServoGroup. No output is
        // initialized by pairing, subscribing, claiming or a GAP callback.
        const Target logical{g_motion.issued(), 0};
        const auto angles = LogicalTargetToAngles(logical);
        auto& servos = ServoGroup::GetInstance();
        if (!servos.IsReady()) return false;
        for (int channel = 0; channel < 3; ++channel) {
            if (!servos.SetAngle(channel, angles.value[channel])) return false;
        }
        return true;
    };
    const auto publish_motion = [&](std::uint32_t generation, std::uint32_t sequence) {
        portENTER_CRITICAL(&g_session_lock);
        if (!output_fault && generation == g_session.generation() && g_motion.valid_mask() == 7) {
            g_session.UpdateMotion(g_motion.issued(), g_motion.interpolating_mask(), sequence);
        }
        portEXIT_CRITICAL(&g_session_lock);
    };

    while (true) {
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        if (g_pairing_storage_fault.load(std::memory_order_acquire) && g_connection != kNoConnection) {
            g_secure_peer = false;
            portENTER_CRITICAL(&g_session_lock);
            g_session.Disconnect();
            const auto fault_generation = g_session.generation();
            portEXIT_CRITICAL(&g_session_lock);
            if (g_work_queue) {
                xQueueReset(g_work_queue);
                WorkItem stop{}; stop.disconnected = true; stop.outcome.generation = fault_generation;
                (void)xQueueSendToFront(g_work_queue, &stop, 0);
            }
            (void)ble_gap_terminate(g_connection, BLE_ERR_AUTH_FAIL);
        }
        std::uint32_t generation;
        bool expired;
        portENTER_CRITICAL(&g_session_lock);
        expired = g_session.TickLease(now);
        generation = g_session.generation();
        portEXIT_CRITICAL(&g_session_lock);
        if (motion_generation != generation) {
            g_motion.Halt(generation);
            motion_generation = generation;
            publish_motion(generation, state_now().last_applied_sequence);
        }
        if (expired && g_connection != kNoConnection) {
            (void)ble_gap_terminate(g_connection, BLE_ERR_REM_USER_CONN_TERM);
        }
        if (g_connection != kNoConnection &&
            static_cast<std::uint32_t>(now - g_connected_at_ms) >= 60000) {
            ble_gap_conn_desc desc{};
            if (ble_gap_conn_find(g_connection, &desc) != 0 || !AuthorizedSecurePeer(desc)) {
                (void)ble_gap_terminate(g_connection, BLE_ERR_AUTH_FAIL);
            }
        }

        WorkItem item{};
        Outcome latest_target{};
        bool has_target = false;
        // Bounded work per cycle. SET_TARGET is an accepted latest-value slot;
        // stop barriers change generation in the receiver before we get here.
        for (std::size_t count = 0; count < kPendingCommands &&
             xQueueReceive(g_work_queue, &item, 0) == pdTRUE; ++count) {
            if (item.disconnected) {
                portENTER_CRITICAL(&g_session_lock);
                generation = g_session.generation();
                portEXIT_CRITICAL(&g_session_lock);
                g_motion.Halt(generation);
                motion_generation = generation;
                has_target = false;
                continue;
            }
            const auto& op = item.outcome;
            if (!valid_work(op)) continue;
            const auto opcode = static_cast<Opcode>(op.request.opcode);
            if (opcode == Opcode::SetTarget) {
                latest_target = op;
                has_target = true;
                continue;
            }
            has_target = false;
            if (opcode == Opcode::SetPairingCode) {
                std::uint32_t requested = 0;
                for (int i = 0; i < 4; ++i) requested |= static_cast<std::uint32_t>(op.request.payload[i]) << (8 * i);
                ble_gap_conn_desc desc{};
                PeerIdentity peer{}; LinkSecurity security{};
                const bool have_desc = ble_gap_conn_find(g_connection, &desc) == 0;
                if (have_desc) { peer = IdentityFromDesc(desc); security = SecurityFromDesc(desc); }
                char code[7]{}; std::snprintf(code, sizeof(code), "%06lu", static_cast<unsigned long>(requested));
                PairingLock();
                if (!valid_work(op)) { PairingUnlock(); continue; }
                const auto validated = g_pairing.ValidateCodeChange(peer, security, code);
                esp_err_t persisted = validated == PairingResult::Ok ? BleUpdatePairingCode(requested) : ESP_FAIL;
                const bool code_committed = persisted == ESP_OK && g_pairing.CommitCodeChange() == PairingResult::Ok;
                if (!code_committed) g_pairing.AbortCodeChange();
                PairingUnlock();
                if (code_committed) {
                    g_identity.passkey = requested;
                    std::array<std::uint8_t, kFrameSize> reply{};
                    const auto completed_at = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
                    portENTER_CRITICAL(&g_session_lock);
                    const bool completed = g_session.CompleteAction(op.generation, op.request.sequence, true, completed_at, reply);
                    portEXIT_CRITICAL(&g_session_lock);
                    if (completed) Notify(reply);
                } else {
                    std::array<std::uint8_t, kFrameSize> reply{};
                    portENTER_CRITICAL(&g_session_lock);
                    const bool completed = g_session.FailAction(op.generation, op.request.sequence, Result::InternalError, reply);
                    portEXIT_CRITICAL(&g_session_lock);
                    if (completed) Notify(reply);
                }
                continue;
            }
            if (opcode == Opcode::Arm) {
                if (!g_startup_configured) continue;
                if (g_motion.valid_mask() == 0) {
                    if (!valid_work(op)) continue;
                    g_motion.Initialize(op.target.channels);
                    g_motion.Halt(op.generation);
                    motion_generation = op.generation;
                    if (!write_output()) { fail_output(); continue; }
                    servo_enabled = true;
                }
            } else if (opcode == Opcode::Halt || opcode == Opcode::Release) {
                g_motion.Halt(op.generation);
                motion_generation = op.generation;
            } else {
                continue;
            }
            std::array<std::uint8_t, kFrameSize> reply{};
            const auto completed_at = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
            portENTER_CRITICAL(&g_session_lock);
            const bool completed = g_session.CompleteAction(op.generation, op.request.sequence, true, completed_at, reply);
            portEXIT_CRITICAL(&g_session_lock);
            if (completed) {
                g_motion.SetLastSequence(op.request.sequence);
                publish_motion(op.generation, op.request.sequence);
                Notify(reply);
            }
            // RELEASE's bounded cache window is managed by TickLease, so
            // duplicate RELEASE can recover a lost ACK without blocking ticks.
        }
        if (has_target && servo_enabled && valid_work(latest_target)) {
            if (motion_generation != latest_target.generation) {
                g_motion.Halt(latest_target.generation);
                motion_generation = latest_target.generation;
            }
            g_motion.SetTarget(latest_target.target, now, latest_target.generation);
            if (latest_target.target.transition_ms == 0 && valid_work(latest_target)) {
                if (!write_output()) { fail_output(); continue; }
            }
            std::array<std::uint8_t, kFrameSize> ignored_reply{};
            const auto completed_at = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
            portENTER_CRITICAL(&g_session_lock);
            const bool completed = g_session.CompleteAction(latest_target.generation,
                latest_target.request.sequence, true, completed_at, ignored_reply);
            portEXIT_CRITICAL(&g_session_lock);
            if (completed) {
                g_motion.SetLastSequence(latest_target.request.sequence);
                publish_motion(latest_target.generation, latest_target.request.sequence);
            }
        }
        portENTER_CRITICAL(&g_session_lock);
        generation = g_session.generation();
        portEXIT_CRITICAL(&g_session_lock);
        if (generation != motion_generation) {
            g_motion.Halt(generation);
            motion_generation = generation;
        } else if (servo_enabled && g_motion.Tick(now, generation)) {
            if (write_output()) {
                publish_motion(generation, state_now().last_applied_sequence);
            } else {
                fail_output();
            }
        }
        if (g_connection != kNoConnection && now - last_state_event >= 500) {
            last_state_event = now;
            NotifySnapshotEvent();
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(MotionEngine::kTickMs));
    }
}

int PairCardCommand(int, char**) {
    if (!BleLoadIdentity(g_identity)) { std::printf("BLE identity is not provisioned. Connect over USB and run: ble-provision\n"); return 1; }
    std::printf("SatoriEye BLE pairing card (keep private)\nDevice ID: ");
    for (auto byte : g_identity.id) std::printf("%02x", byte);
    std::printf("\nPasskey: %06lu\n", static_cast<unsigned long>(g_identity.passkey));
    return 0;
}
int ProvisionCommand(int, char**) {
    BleIdentityData generated{}; const auto err = BleProvisionIdentity(generated);
    if (err == ESP_ERR_INVALID_STATE) { std::printf("Identity already exists; use ble-pair-card to retrieve the saved card.\n"); return 1; }
    if (err != ESP_OK) { std::printf("Provisioning failed: %s\n", esp_err_to_name(err)); return 1; }
    g_identity = generated; g_has_identity = true;
    return PairCardCommand(0, nullptr);
}
int RecoverBindingCommand(int, char**) {
    if (!g_ble_started || !g_pairing_store_ready) {
        std::printf("NimBLE bond storage is unavailable until NVS is repaired and BLE restarts. Use ble-repair-nvs only after making a private backup.\n");
        return 1;
    }
    const auto current_connection = g_connection;
    if (current_connection != kNoConnection) {
        g_secure_peer = false;
        portENTER_CRITICAL(&g_session_lock); g_session.Disconnect(); const auto generation = g_session.generation(); portEXIT_CRITICAL(&g_session_lock);
        if (g_work_queue) {
            xQueueReset(g_work_queue);
            WorkItem stop{}; stop.disconnected = true; stop.outcome.generation = generation;
            (void)xQueueSendToFront(g_work_queue, &stop, 0);
        }
        (void)ble_gap_terminate(current_connection, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    const int clear_result = ble_store_clear();
    if (clear_result != 0) { std::printf("Could not clear saved BLE bonds (%d); owner remains locked.\n", clear_result); return 1; }
    std::uint32_t rotated_passkey = 0;
    const auto rotate_result = BleRotatePasskeyAndClearOwner(rotated_passkey);
    if (rotate_result != ESP_OK) { std::printf("Could not rotate pairing code and clear owner (%s); enrollment remains locked.\n", esp_err_to_name(rotate_result)); return 1; }
    g_pairing_store_ready = false;
    g_bond_count.store(0, std::memory_order_release);
    g_identity.passkey = rotated_passkey;
    std::printf("Binding reset complete. Pairing code reset to default 123456; rebooting.\n");
    vTaskDelay(pdMS_TO_TICKS(100)); esp_restart(); return 0;
}
int RepairNvsCommand(int argc, char** argv) {
    constexpr const char* token = "--erase-entire-default-nvs-after-backup";
    if (argc != 2 || std::strcmp(argv[1], token) != 0) {
        std::printf("WARNING: this erases every key in default NVS, including Wi-Fi credentials and BLE bonds/identity. Back up first. Type: ble-repair-nvs %s\n", token);
        return 1;
    }
    const auto erase = nvs_flash_erase();
    if (erase != ESP_OK) { std::printf("NVS erase failed: %s\n", esp_err_to_name(erase)); return 1; }
    const auto init = nvs_flash_init();
    if (init != ESP_OK) { std::printf("NVS initialization failed after erase: %s\n", esp_err_to_name(init)); return 1; }
    std::printf("Default NVS erased and initialized; Wi-Fi credentials and BLE identity/bonds are gone. The separate config partition is unchanged. Rebooting.\n");
    vTaskDelay(pdMS_TO_TICKS(100)); esp_restart(); return 0;
}

void RegisterUsbCommands() {
    esp_console_cmd_t provision{}; provision.command = "ble-provision"; provision.help = "Initialize a blank BLE identity with default code 123456"; provision.func = ProvisionCommand;
    esp_console_cmd_register(&provision);
    esp_console_cmd_t show{}; show.command = "ble-pair-card"; show.help = "Display the saved pairing card over this USB maintenance console"; show.func = PairCardCommand;
    esp_console_cmd_register(&show);
    esp_console_cmd_t recover{}; recover.command = "ble-recover-binding"; recover.help = "Clear BLE owner/bonds and reset pairing code to 123456 over USB"; recover.func = RecoverBindingCommand;
    esp_console_cmd_register(&recover);
    esp_console_cmd_t repair{}; repair.command = "ble-repair-nvs"; repair.help = "Explicitly erase default NVS after diagnosing an unrecoverable BLE storage fault"; repair.func = RepairNvsCommand;
    esp_console_cmd_register(&repair);
}
void StartUsbTools() {
    RegisterUsbCommands();
    esp_console_repl_t* repl = nullptr;
    esp_console_dev_usb_serial_jtag_config_t usb = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT(); repl_config.prompt = "SatoriEye> "; repl_config.max_cmdline_length = 256;
    const auto create = esp_console_new_repl_usb_serial_jtag(&usb, &repl_config, &repl);
    if (create != ESP_OK) { ESP_LOGE(kTag, "USB Serial/JTAG maintenance console unavailable: %s", esp_err_to_name(create)); return; }
    const auto start = esp_console_start_repl(repl);
    if (start != ESP_OK) ESP_LOGE(kTag, "USB maintenance REPL failed: %s", esp_err_to_name(start));
}
}

void StartBleMaintenanceConsole() { StartUsbTools(); }

esp_err_t StartBlePrimary() {
    g_has_identity = BleLoadIdentity(g_identity);
    g_identity_corrupt = !g_has_identity && BleIdentityHasAnyMaterial();
    if (g_identity_corrupt)
        ESP_LOGE(kTag, "Partial BLE identity record found; pairing is disabled until USB recovery.");
    if (g_work_queue == nullptr) g_work_queue = xQueueCreate(kPendingCommands, sizeof(WorkItem));
    if (!g_work_queue) return ESP_ERR_NO_MEM;
    g_startup_configured = IsStartupConfigurationValid();
    int result = nimble_port_init(); if (result != ESP_OK) return result;
    g_pairing_mutex = xSemaphoreCreateMutex();
    if (!g_pairing_mutex) { (void)nimble_port_deinit(); return ESP_ERR_NO_MEM; }
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
    g_ble_started = true;
    StartUsbTools();
    return ESP_OK;
}
#endif // CONFIG_SATORI_TRANSPORT_BLE_PRIMARY
