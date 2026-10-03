/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <vector>
#include <chrono>
#include <stdio.h>
#include <inttypes.h>
#include <esp_pthread.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "nvs_bootstrap.h"

#include "connect_wifi.h"
#include "servo_group.h"
#include "udp_server.h"
#include "led_controller.h"
#include "esp_thread_helper.h"

#include "esp_partition_param.h"
#if CONFIG_SATORI_TRANSPORT_BLE_PRIMARY
#include "ble_server.h"
#endif

static const char *TAG = "APP MAIN";


void greeting(void)
{
    printf("Hello world!\n");

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

}

extern "C" void app_main(void)
{
    greeting();
    const auto* app = esp_app_get_description();
    ESP_LOGI(TAG, "Boot reset_reason=%u, build=%s, elf_id=%02x%02x%02x%02x",
             static_cast<unsigned>(esp_reset_reason()), app->version,
             app->app_elf_sha256[0], app->app_elf_sha256[1], app->app_elf_sha256[2], app->app_elf_sha256[3]);
#if CONFIG_SATORI_TRANSPORT_BLE_PRIMARY
    const esp_err_t nvs_result = InitializeSharedNvs();
    if (nvs_result != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s). No erase was attempted; use USB maintenance after reviewing storage state.", esp_err_to_name(nvs_result));
        StartBleMaintenanceConsole();
        return;
    }
    ESP_LOGI(TAG, "Starting BLE primary transport (Wi-Fi remains off)");
    const esp_err_t ble_result = StartBlePrimary();
    if (ble_result != ESP_OK) {
        ESP_LOGE(TAG, "BLE service did not start (%s); leaving outputs disabled.", esp_err_to_name(ble_result));
        return;
    }
#else
    const esp_err_t nvs_result = InitializeSharedNvs();
    if (nvs_result != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s); refusing to start legacy UDP output.", esp_err_to_name(nvs_result));
        return;
    }
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    LedController::GetInstance().SetColor(50, 0, 0);
    connect_wifi();
    LedController::GetInstance().SetColor(0, 0, 50);
    int udpPort = EspPartitionParam::GetInstance().GetIntParam("ESP_UDP_PORT", CONFIG_EXAMPLE_PORT);
    EspThreadHelper th;
    th.AddTask([&udpPort] {
        while (true) {
            UdpServer UdpServer(udpPort);
            UdpServer.StartListening();
        }
    }, 8192);

    th.PrintInfo();
#endif
}
