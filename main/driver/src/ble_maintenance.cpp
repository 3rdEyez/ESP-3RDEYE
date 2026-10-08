#include "sdkconfig.h"
#if CONFIG_SATORI_TRANSPORT_BLE_PRIMARY
#include "ble_server_internal.hpp"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/task.h"
#include "services/gatt/ble_svc_gatt.h"
#include <cstdio>
#include <cstring>

namespace satori::ble::internal {
#if CONFIG_BT_NIMBLE_GATT_CACHING
int GattStatusCommand(int, char**) {
    if (!g_runtime.ble_started || !g_runtime.host_startup_ready) {
        std::printf("GATT database is not ready.\n");
        return 1;
    }
    const ble_uuid16_t uuid = BLE_UUID16_INIT(0x1801);
    std::uint16_t service = 0;
    std::uint8_t hash[16]{};
    if (ble_gatts_find_svc(&uuid.u, &service) != 0 || ble_gatts_calculate_hash(hash) != 0) {
        std::printf("GATT database validation failed.\n");
        return 1;
    }
    // Database Hash is public GATT metadata; this command never reads identity,
    // passkeys, owner records, BLE security keys or network settings.
    std::printf("GATT status: service=%u changed=%u hash=%u database_hash=",
        service, ble_svc_gatt_changed_handle(), ble_svc_gatt_hash_handle());
    for (const auto byte : hash) std::printf("%02x", byte);
    std::printf("\n");
    return 0;
}
#endif
int PairCardCommand(int, char**) {
    BleIdentityData identity{};
    PairingLock();
    const bool loaded = BleLoadIdentity(identity);
    if (loaded) g_runtime.identity = identity;
    PairingUnlock();
    if (!loaded) { std::printf("BLE identity is not provisioned. Connect over USB and run: ble-provision\n"); return 1; }
    std::printf("SatoriEye BLE pairing card (keep private)\nDevice ID: ");
    for (auto byte : identity.id) std::printf("%02x", byte);
    std::printf("\nPasskey: %06lu\n", static_cast<unsigned long>(identity.passkey));
    return 0;
}
int ProvisionCommand(int, char**) {
    BleIdentityData generated{};
    PairingLock();
    const auto err = BleProvisionIdentity(generated);
    if (err == ESP_OK) { g_runtime.identity = generated; g_runtime.has_identity = true; }
    PairingUnlock();
    if (err == ESP_ERR_INVALID_STATE) { std::printf("Identity already exists; use ble-pair-card to retrieve the saved card.\n"); return 1; }
    if (err != ESP_OK) { std::printf("Provisioning failed: %s\n", esp_err_to_name(err)); return 1; }
    return PairCardCommand(0, nullptr);
}
int RecoverBindingCommand(int, char**) {
    if (!g_runtime.ble_started || !g_runtime.pairing_store_ready) {
        std::printf("NimBLE bond storage is unavailable until NVS is repaired and BLE restarts. Use ble-repair-nvs only after making a private backup.\n");
        return 1;
    }
    const auto current_connection = g_runtime.connection.load(std::memory_order_acquire);
    if (current_connection != kNoConnection) {
        g_runtime.secure_peer = false;
        portENTER_CRITICAL(&g_runtime.session_lock); g_runtime.session.Disconnect(); const auto generation = g_runtime.session.generation(); portEXIT_CRITICAL(&g_runtime.session_lock);
        if (g_runtime.work_queue) {
            xQueueReset(g_runtime.work_queue);
            WorkItem stop{}; stop.disconnected = true; stop.outcome.generation = generation;
            (void)xQueueSendToFront(g_runtime.work_queue, &stop, 0);
        }
        (void)ble_gap_terminate(current_connection, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    const int clear_result = ble_store_clear();
    if (clear_result != 0) { std::printf("Could not clear saved BLE bonds (%d); owner remains locked.\n", clear_result); return 1; }
    std::uint32_t rotated_passkey = 0;
    PairingLock();
    const auto rotate_result = BleRotatePasskeyAndClearOwner(rotated_passkey);
    if (rotate_result == ESP_OK) g_runtime.identity.passkey = rotated_passkey;
    PairingUnlock();
    if (rotate_result != ESP_OK) { std::printf("Could not rotate pairing code and clear owner (%s); enrollment remains locked.\n", esp_err_to_name(rotate_result)); return 1; }
    g_runtime.pairing_store_ready = false;
    g_runtime.bond_count.store(0, std::memory_order_release);
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
#if CONFIG_BT_NIMBLE_GATT_CACHING
    esp_console_cmd_t gatt{}; gatt.command = "ble-gatt-status"; gatt.help = "Read public GATT handles and Database Hash"; gatt.func = GattStatusCommand;
    esp_console_cmd_register(&gatt);
#endif
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
} // namespace satori::ble::internal
#endif
